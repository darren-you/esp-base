"""正式连续统计帧的解析、保守下界与不授资格回归；不访问设备。"""
import importlib.util
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

TOOLS = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("analyze_capacity", TOOLS / "analyze_capacity.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
analyze = module.analyze
BOOT = "11111111-2222-4333-8444-555555555555"
LOCK = "a" * 64
BYTE_CAPS = (1 << 11) | (1 << 2) | (1 << 1) | (1 << 3) | (1 << 12)
IRAM_CAPS = (1 << 11) | (1 << 13) | (1 << 1) | 1


def fixture(number=1, uptime=10, free_min=30000, largest_min=26000, stack_min=1200,
            phase="periodic", no_final=False):
    # 合成值保持真实 region 物理大小与统计不变量；不授软件假件实体资格。
    common = {"boot_id": BOOT, "frame": number}
    begin = dict(common, schema=1, phase=phase, uptime_ms=uptime, sdk_lock_sha256=LOCK, task_limit=32)

    def region(start, caps, early, free, largest):
        return dict(common, start=start, end=start + 65536, caps0=caps, caps1=0, caps2=0,
                    alias_start=0, alias_end=0, alias_inverted=0, available_at_heap_init=early,
                    free_bytes=free + 500, min_free_bytes=free, largest_request_bytes=largest + 100,
                    min_largest_request_bytes=largest, allocator_metadata_bytes=100)

    regions = [region(0x10000, BYTE_CAPS, 1, free_min, largest_min),
               region(0x20000, BYTE_CAPS, 0, 52000, 50000),
               region(0x30000, IRAM_CAPS, 1, 16000, 10000)]
    tasks = [dict(common, instance=index + 1, name_hex=b"same_worker".ljust(16, b"\0").hex(),
                  minimum_stack_bytes=(stack_min if no_final and index == 0 else 1400 + index * 200), state=index)
             for index in range(2)]
    domains = []
    for caps in module.DOMAIN_CAPS:
        matching = [row for row in regions if row["available_at_heap_init"] and
                    (row["caps0"] & caps) == caps]
        domains.append(dict(common, caps=caps, alignment_bytes=4,
                            minimum_free_lower_bound_bytes=sum(row["min_free_bytes"] for row in matching),
                            largest_request_lower_bound_bytes=max((row["min_largest_request_bytes"] for row in matching), default=0),
                            history="region_minima_conservative_bound"))
    end = dict(common, regions=3, allocated_tasks=2, created_instances=2 if no_final else 3,
               finalized_instances=0 if no_final else 1, completed_stack_min_bytes=0 if no_final else stack_min,
               worst_completed_instance=0 if no_final else 3, minimum_stack_bytes=stack_min,
               task_snapshot_complete=1, counters_valid=1, facts_valid=1, workspace_bytes=256,
               observation_cost_added_back=0)
    return {"BEGIN": [begin], "REGION": regions, "TASK": tasks, "DOMAIN": domains, "END": [end]}


def row(kind, values):
    def value(key):
        result = values[key]
        return f"{result:08x}" if key in module.HEX_FIELDS else str(result)
    return "ESP_BASE_CAPACITY_" + kind + " " + " ".join(key + "=" + value(key) for key in module.FIELDS[kind]) + "\n"


def render(frame):
    return "".join(row(kind, values) for kind, rows in frame.items() for values in rows)


def cleanup_sequence(final_min=1100):
    first = fixture(no_final=True, stack_min=1400)
    first["TASK"][1]["minimum_stack_bytes"] = 1400
    gap = fixture(2, 5010, no_final=True, stack_min=1400)
    gap["TASK"].pop()
    gap["END"][0].update(allocated_tasks=1, task_snapshot_complete=0)
    after = fixture(3, 10010, stack_min=final_min)
    after["TASK"].pop()
    after["END"][0].update(created_instances=2, allocated_tasks=1, worst_completed_instance=2)
    return first, gap, after


class AnalyzeCapacityTest(unittest.TestCase):
    def read(self, *contents, target="esp32", boot=BOOT, lock=LOCK):
        with tempfile.TemporaryDirectory() as directory:
            paths = []
            for index, content in enumerate(contents):
                path = Path(directory) / f"uart-{index}.log"
                path.write_text(content)
                paths.append(path)
            result = analyze(paths, target, boot, lock)
            for path, content in zip(paths, contents):
                self.assertEqual(path.read_text(), content)
            return result

    def reject(self, text):
        with self.assertRaises(ValueError):
            self.read(text)

    def test_complete_frames_prove_begin_only_and_never_r5_r6_or_next_request(self):
        result = self.read("SDK startup\n" + render(fixture()), render(fixture(2, 5010)) + "panic / reset after readout\n")
        self.assertTrue(result["valid"])
        self.assertTrue(result["numeric_gates_passed"])
        self.assertEqual(result["proved_until_begin_uptime_ms"], 5010)
        self.assertEqual(result["metrics_source"], "continuous_sdk_statistics_read_by_complete_frames")
        self.assertTrue(result["current_region_values_are_sequential"])
        self.assertFalse(result["observation_cost_added_back"])
        for key in ("r5_qualified", "r6_qualified", "next_maximum_legal_request_verified"):
            self.assertFalse(result[key])
        self.assertEqual(len(result["inputs"]), 2)

    def test_numeric_thresholds_do_not_grant_missing_qualification(self):
        result = self.read(render(fixture(largest_min=24576, stack_min=1024)))
        self.assertTrue(result["numeric_gates_passed"])
        for args, key in (({"free_min": 16383, "largest_min": 16000}, "heap_gate_passed"),
                          ({"largest_min": 24575}, "largest_gate_passed"),
                          ({"stack_min": 1023}, "stack_gate_passed")):
            with self.subTest(args=args):
                self.assertFalse(self.read(render(fixture(**args)))["numeric_gates"][key])

    def test_late_region_does_not_raise_either_domain_lower_bound(self):
        result = self.read(render(fixture()))
        self.assertEqual(result["frames"][0]["minimum_free_lower_bound_bytes"], 30000)
        self.assertEqual(result["frames"][0]["largest_request_lower_bound_bytes"], 26000)
        for key in ("minimum_free_lower_bound_bytes", "largest_request_lower_bound_bytes"):
            bad = fixture()
            bad["DOMAIN"][0][key] += 1
            self.reject(render(bad))

    def test_priority_union_and_alias_never_double_count_physical_heap(self):
        good = fixture()
        region = good["REGION"][0]
        region["caps0"] = (1 << 2) | (1 << 12)
        region["caps1"] = (1 << 11) | (1 << 3) | (1 << 1)
        region["alias_start"], region["alias_end"], region["alias_inverted"] = 0x700000, 0x710000, 1
        self.assertTrue(self.read(render(good))["numeric_gates_passed"])
        good["REGION"][1]["start"] = region["start"]
        good["REGION"][1]["end"] = region["end"]
        self.reject(render(good))

    def test_iram_is_distinct_and_zero_unavailable_c3_domain_is_not_a_numeric_gate(self):
        good = fixture()
        iram = good["REGION"][2]
        good["REGION"].remove(iram)
        good["END"][0]["regions"] = 2
        good["DOMAIN"][1]["minimum_free_lower_bound_bytes"] -= 16000
        good["DOMAIN"][-1]["minimum_free_lower_bound_bytes"] = 0
        good["DOMAIN"][-1]["largest_request_lower_bound_bytes"] = 0
        result = self.read(render(good), target="esp32c3")
        self.assertTrue(result["numeric_gates_passed"])
        self.assertFalse(result["next_maximum_legal_request_verified"])

    def test_missing_rows_counts_or_end_and_cross_file_partial_frames_reject(self):
        good = fixture()
        for kind in ("REGION", "TASK", "DOMAIN", "END"):
            bad = fixture()
            bad[kind].pop()
            with self.subTest(kind=kind):
                self.reject(render(bad))
        text = render(good)
        split = text.index("ESP_BASE_CAPACITY_TASK")
        with self.assertRaises(ValueError):
            self.read(text[:split], text[split:])

    def test_boot_sdk_and_schema_bindings_are_exact(self):
        for kind, key, value in (("TASK", "boot_id", "66666666-2222-4333-8444-555555555555"),
                                 ("BEGIN", "sdk_lock_sha256", "b" * 64), ("BEGIN", "schema", 2)):
            bad = fixture()
            bad[kind][0][key] = value
            self.reject(render(bad))
        self.reject(render(fixture()) + render(fixture()).replace(BOOT, "66666666-2222-4333-8444-555555555555"))
        with self.assertRaises(ValueError):
            self.read(render(fixture()), target="guess-target")

    def test_duplicate_skipped_reversed_or_reset_following_frames_reject(self):
        for second in (fixture(1, 5010), fixture(3, 5010), fixture(2, 9)):
            self.reject(render(fixture()) + render(second))
        self.reject(render(fixture(phase="before_reset")) + render(fixture(2, 5010)))

    def test_unknown_duplicate_domain_wrong_alignment_or_history_reject(self):
        for key, value in (("caps", 1), ("caps", module.DOMAIN_CAPS[0]), ("alignment_bytes", 32),
                           ("history", "sampled_largest")):
            bad = fixture()
            bad["DOMAIN"][1][key] = value
            self.reject(render(bad))

    def test_row_order_fields_encodings_and_truncation_are_strict(self):
        text = render(fixture())
        rows = text.splitlines(keepends=True)
        broken = ("".join(rows[:1] + rows[4:5] + rows[1:4] + rows[5:]),
                  text.replace(" schema=1 boot_id=", " boot_id="),
                  text.replace(" task_limit=32", " task_limit=32 extra=1"),
                  text.replace(" task_limit=32", " task_limit=-1"),
                  text.replace(" frame=1", " frame=4294967296", 1),
                  text.replace(" caps=", " caps=F", 1),
                  "prefix " + text, text.rstrip("\n"),
                  text + "ESP_BASE_CAPACITY_UNKNOWN boot_id=" + BOOT + "\n",
                  rows[0] + rows[0] + "".join(rows[1:]))
        for item in broken:
            self.reject(item)

    def test_incomplete_invalid_or_saturated_counters_reject(self):
        for key, value in (("task_snapshot_complete", 0), ("counters_valid", 0), ("facts_valid", 0),
                           ("created_instances", 4), ("finalized_instances", 4),
                           ("created_instances", module.UINT32_MAX), ("observation_cost_added_back", 1)):
            bad = fixture()
            bad["END"][0][key] = value
            self.reject(render(bad))
        self.reject(render(fixture(module.UINT32_MAX)))

    def test_final_summary_empty_sentinel_and_all_instance_minimum_are_verified(self):
        self.assertTrue(self.read(render(fixture(no_final=True)))["numeric_gates_passed"])
        for key, value in (("minimum_stack_bytes", 9999), ("worst_completed_instance", 1),
                           ("completed_stack_min_bytes", 9999)):
            bad = fixture()
            bad["END"][0][key] = value
            self.reject(render(bad))
        bad = fixture(no_final=True)
        bad["END"][0]["completed_stack_min_bytes"] = 1
        self.reject(render(bad))

    def test_duplicate_task_instances_do_not_merge_same_names(self):
        self.assertTrue(self.read(render(fixture()))["valid"])
        bad = fixture()
        bad["TASK"][1]["instance"] = 1
        self.reject(render(bad))

    def test_region_identity_minimum_and_task_counter_regressions_reject(self):
        changes = (("REGION", 0, "min_free_bytes", 30001), ("REGION", 0, "caps2", 1),
                   ("TASK", 0, "minimum_stack_bytes", 1500), ("TASK", 0, "instance", 4),
                   ("TASK", 0, "name_hex", b"new_name".ljust(16, b"\0").hex()),
                   ("END", 0, "worst_completed_instance", 4))
        for kind, index, key, value in changes:
            bad = fixture(2, 5010)
            bad[kind][index][key] = value
            self.reject(render(fixture()) + render(bad))

    def test_same_named_new_instances_and_final_capture_close_without_reusing_identity(self):
        later = fixture(2, 5010, stack_min=1100)
        later["TASK"][0]["instance"] = 2
        later["TASK"][1]["instance"] = 5
        later["END"][0].update(created_instances=4, finalized_instances=2, worst_completed_instance=1)
        self.assertTrue(self.read(render(fixture()) + render(later))["numeric_gates_passed"])
        reused = fixture(3, 10010, stack_min=1000)
        reused["TASK"][0]["instance"] = 2
        reused["TASK"][1]["instance"] = 1
        reused["END"][0].update(created_instances=5, finalized_instances=3, worst_completed_instance=6)
        self.reject(render(fixture()) + render(later) + render(reused))

    def test_normal_cleanup_gap_is_retained_then_later_final_hwm_closes_prefix(self):
        for minimum, passed in ((1100, True), (896, False)):
            with self.subTest(final_min=minimum):
                first, gap, after = cleanup_sequence(minimum)
                result = self.read(render(first) + render(gap) + render(after))
                self.assertTrue(result["valid"])
                self.assertEqual(len(result["frames"]), 3)
                self.assertEqual(result["incomplete_task_readouts"], 1)
                self.assertIsNone(result["frames"][1]["proved_until_begin_uptime_ms"])
                self.assertIsNone(result["frames"][1]["minimum_stack_bytes"])
                self.assertEqual(result["frames"][1]["reported_minimum_stack_bytes"], 1400)
                self.assertEqual(result["proved_until_begin_uptime_ms"], 10010)
                self.assertEqual(result["frames"][-1]["minimum_stack_bytes"], minimum)
                self.assertEqual(result["numeric_gates_passed"], passed)
                self.assertTrue(result["task_history_closed_at_latest_readout"])
                self.assertFalse(result["r5_qualified"])
                self.assertFalse(result["r6_qualified"])

    def test_incomplete_overall_minimum_can_rise_without_becoming_a_task_anchor(self):
        first, gap, after = cleanup_sequence()
        first["TASK"][1]["minimum_stack_bytes"] = 1100
        first["END"][0]["minimum_stack_bytes"] = 1100
        result = self.read(render(first) + render(gap) + render(after))
        self.assertEqual([f["reported_minimum_stack_bytes"] for f in result["frames"]], [1100, 1400, 1100])
        self.assertTrue(result["numeric_gates_passed"])
        after["END"][0].update(minimum_stack_bytes=1200, completed_stack_min_bytes=1200)
        self.reject(render(first) + render(gap) + render(after))

    def test_last_gap_and_incomplete_before_reset_never_extend_the_cutoff(self):
        first, gap, after = cleanup_sequence()
        for phase in ("periodic", "before_reset"):
            gap["BEGIN"][0]["phase"] = phase
            result = self.read(render(first) + render(gap))
            self.assertEqual(result["proved_until_begin_uptime_ms"], 10)
            self.assertEqual(result["latest_readout_begin_uptime_ms"], 5010)
            self.assertFalse(result["task_history_closed_at_latest_readout"])
            self.assertIsNone(result["frames"][-1]["proved_until_begin_uptime_ms"])
            self.assertFalse(result["r5_qualified"])
            self.assertFalse(result["r6_qualified"])
        self.reject(render(first) + render(gap) + render(after))

    def test_without_any_complete_snapshot_no_combined_proof_is_granted(self):
        _, gap, after = cleanup_sequence()
        result = self.read(render(gap))
        self.assertTrue(result["valid"])
        self.assertIsNone(result["proved_until_begin_uptime_ms"])
        self.assertFalse(result["numeric_gates_passed"])
        self.assertFalse(any(result["numeric_gates"].values()))
        recovered = self.read(render(gap) + render(after))
        self.assertEqual(recovered["proved_until_begin_uptime_ms"], 10010)
        self.assertTrue(recovered["numeric_gates_passed"])

    def test_capacity_zero_capture_does_not_retire_live_instances_and_later_final_min_covers_hidden_exits(self):
        first = fixture(no_final=True, stack_min=1400)
        gap = fixture(2, 5010, no_final=True)
        gap["TASK"] = []
        gap["END"][0].update(created_instances=33, allocated_tasks=0, minimum_stack_bytes=0,
                             task_snapshot_complete=0)
        after = fixture(3, 10010, stack_min=1100)
        after["END"][0].update(created_instances=33, finalized_instances=31, worst_completed_instance=3)
        result = self.read(render(first) + render(gap) + render(after))
        self.assertTrue(result["numeric_gates_passed"])
        self.assertEqual(result["frames"][1]["task_snapshot_issue"], "task_list_capacity_not_complete")
        self.assertEqual(result["frames"][-1]["allocated_tasks"], 2)
        self.assertEqual(result["frames"][-1]["finalized_instances"], 31)

    def test_gap_does_not_hide_corruption_missing_rows_invalid_flags_or_saturation(self):
        first, gap, after = cleanup_sequence()
        prefix, suffix = render(first), render(after)
        for key, value in (("counters_valid", 0), ("facts_valid", 0), ("task_snapshot_complete", 1),
                           ("created_instances", module.UINT32_MAX), ("finalized_instances", 3)):
            bad = cleanup_sequence()[1]
            bad["END"][0][key] = value
            self.reject(prefix + render(bad) + suffix)
        bad = cleanup_sequence()[1]
        bad["TASK"] = []
        self.reject(prefix + render(bad) + suffix)
        bad["END"][0].update(allocated_tasks=0, minimum_stack_bytes=0)
        self.reject(prefix + render(bad) + suffix)  # 两个实例无法触发官方 32 项列表容量拒绝。
        bad = cleanup_sequence()[1]
        bad["DOMAIN"].pop()
        self.reject(prefix + render(bad) + suffix)
        bad = cleanup_sequence()[1]
        bad["BEGIN"][0]["sdk_lock_sha256"] = "b" * 64
        self.reject(prefix + render(bad) + suffix)
        bad["BEGIN"][0]["sdk_lock_sha256"] = LOCK
        bad["TASK"][0]["boot_id"] = "66666666-2222-4333-8444-555555555555"
        self.reject(prefix + render(bad) + suffix)

    def test_gap_counters_and_regions_still_have_continuous_history(self):
        first, gap, after = cleanup_sequence()
        for kind, index, key, value in (("REGION", 0, "min_free_bytes", 30001),
                                       ("REGION", 1, "available_at_heap_init", 1),
                                       ("END", 0, "workspace_bytes", 512)):
            bad = cleanup_sequence()[1]
            bad[kind][index][key] = value
            self.reject(render(first) + render(bad) + render(after))
        after["END"][0]["created_instances"] = 1
        self.reject(render(first) + render(gap) + render(after))

    def test_explicit_finalized_worst_instance_cannot_reappear_after_a_gap(self):
        first = fixture()
        gap = fixture(2, 5010, stack_min=1100)
        gap["TASK"].pop()
        gap["END"][0].update(created_instances=4, finalized_instances=2, allocated_tasks=1,
                             task_snapshot_complete=0, worst_completed_instance=4)
        after = fixture(3, 10010, stack_min=1050)
        after["TASK"][1]["instance"] = 5
        after["END"][0].update(created_instances=5, finalized_instances=3, worst_completed_instance=2)
        self.assertTrue(self.read(render(first) + render(gap) + render(after))["numeric_gates_passed"])
        # 不依赖不足列表的缺席；gap 的显式 finalized-worst=4 已证明该实例停止并最终记录。
        after["TASK"][1]["instance"] = 4
        self.reject(render(first) + render(gap) + render(after))

    def test_region_disappearance_and_late_birth_claims_reject(self):
        bad = fixture(2, 5010)
        bad["REGION"].pop(1)  # 晚出生不贡献下界，域数值不变，也仍必须完整报告。
        bad["END"][0]["regions"] = 2
        self.reject(render(fixture()) + render(bad))
        first = fixture()
        first["REGION"].pop(1)
        first["END"][0]["regions"] = 2
        self.assertTrue(self.read(render(first) + render(fixture(2, 5010)))["valid"])
        bad = fixture(2, 5010)
        bad["REGION"][1]["available_at_heap_init"] = 1
        for domain in bad["DOMAIN"][:4]:
            domain["minimum_free_lower_bound_bytes"] += 52000
            domain["largest_request_lower_bound_bytes"] = 50000
        self.reject(render(first) + render(bad))

    def test_region_invariants_alias_and_unknown_state_reject(self):
        for key, value in (("min_free_bytes", 40000), ("alias_start", 1), ("alias_inverted", 1),
                           ("allocator_metadata_bytes", 0), ("end", 0x10001)):
            bad = fixture()
            bad["REGION"][0][key] = value
            self.reject(render(bad))
        for key, value in (("state", 5), ("name_hex", "z0"), ("instance", 0)):
            bad = fixture()
            bad["TASK"][0][key] = value
            self.reject(render(bad))

    def test_cli_invalid_json_has_no_qualification(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "empty.log"
            path.write_text("")
            result = subprocess.run([sys.executable, str(TOOLS / "analyze_capacity.py"), "--target", "esp32",
                                     "--boot-id", BOOT, "--sdk-lock-sha256", LOCK,
                                     "--uart-log", str(path), "--json"], capture_output=True, text=True)
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stderr, "")
        body = json.loads(result.stdout)
        self.assertFalse(body["valid"])
        self.assertFalse(body["r5_qualified"])
        self.assertFalse(body["r6_qualified"])

    def test_nonregular_and_symlink_inputs_reject_before_read(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "uart.log"
            path.write_text(render(fixture()))
            link = Path(directory) / "linked.log"
            link.symlink_to(path)
            with self.assertRaises(OSError):
                analyze([link], "esp32", BOOT, LOCK)
            with self.assertRaises((ValueError, OSError)):
                analyze([Path(directory)], "esp32", BOOT, LOCK)


class ProvidedRequestFitTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.lock = module.REQUEST_RECIPE_INPUTS["sdk-lock.json"]
        self.uart = self.root / "raw.uart"
        self.evidence_path = self.root / "request-evidence.json"
        signed = self.reference("signed.bin", b"synthetic complete signed fixture")
        config = self.reference("config.json", b'{"private_key":"never echo this fixture"}')
        source = self.reference("consumer.c", b"heap_caps_malloc(size, caps);\n")
        self.evidence = {"schema_version": 1, "target": "esp32", "boot_id": BOOT,
                         "sdk_lock_sha256": self.lock, "allocator_recipe": module.REQUEST_RECIPE,
                         "identity": {"signed_firmware": signed, "config": config, "sources": [source]},
                         "requests": [{"id": "request", "size_bytes": 25600,
                                       "caps": module.INTERNAL_8BIT, "alignment_bytes": 4}]}

    def reference(self, name, raw):
        path = self.root / name
        path.write_bytes(raw)
        return {"path": str(path), "sha256": hashlib.sha256(raw).hexdigest()}

    def read(self, *frames):
        self.uart.write_text("".join(render(frame).replace(LOCK, self.lock) for frame in (frames or [fixture()])))
        self.evidence_path.write_text(json.dumps(self.evidence))
        return analyze([self.uart], "esp32", BOOT, self.lock, self.evidence_path)

    def status(self, **changes):
        signed = self.evidence["identity"]["signed_firmware"]
        row = {"protocol_version": 1, "device_id": "66666666-2222-4333-8444-555555555555",
               "boot_id": BOOT, "request_id": "77777777-2222-4333-8444-555555555555",
               "state": "succeeded", "error_code": None,
               "result": {"firmware_sha256": signed["sha256"],
                          "image_size_bytes": Path(signed["path"]).stat().st_size,
                          "target": "esp32/esp_base", "ota_slot": "ota_0"}}
        for key, value in changes.items():
            (row["result"] if key in row["result"] else row)[key] = value
        self.evidence["identity"]["firmware_status_uart"] = self.reference("status.uart", (json.dumps(row) + "\n").encode())

    def test_optional_input_preserves_old_output_and_never_closes_global_qualification(self):
        result = self.read()
        plain = analyze([self.uart], "esp32", BOOT, self.lock)
        self.assertNotIn("provided_request_fit", plain)
        self.assertEqual({key: value for key, value in result.items() if key != "provided_request_fit"}, plain)
        fit = result["provided_request_fit"]
        self.assertTrue(fit["all_provided_requests_fit"])
        self.assertEqual(fit["weakest_margin_bytes"], 400)
        self.assertFalse(fit["identity"]["fully_bound"])
        self.assertTrue(fit["identity"]["unbound_reasons"])
        self.assertTrue(fit["next_request_unclosed_reasons"])
        self.assertNotIn("never echo", json.dumps(result))
        for key in ("next_maximum_legal_request_verified", "r5_qualified", "r6_qualified"):
            self.assertFalse(result[key])

    def test_higher_alignment_search_overhead_changes_a_raw_payload_fit_to_unknown(self):
        self.evidence["requests"] = [dict(self.evidence["requests"][0], size_bytes=26000, alignment_bytes=1),
                                     dict(self.evidence["requests"][0], id="aligned", size_bytes=26000, alignment_bytes=16)]
        fit = self.read()["provided_request_fit"]
        plain, aligned = fit["requests"]
        self.assertEqual((plain["effective_alignment_bytes"], plain["search_size_bytes"], plain["status"]), (4, 26000, "fit"))
        self.assertEqual((aligned["search_size_bytes"], aligned["margin_bytes"], aligned["status"]), (26032, -32, "unknown"))
        self.assertEqual(fit["weakest_request_ids"], ["aligned"])
        self.assertFalse(fit["all_provided_requests_fit"])
        self.evidence["requests"][0].update(size_bytes=1)
        self.assertEqual(self.read()["provided_request_fit"]["requests"][0]["search_size_bytes"], 12)

    def test_full_caps_priority_union_and_late_birth_are_used_instead_of_weaker_domains(self):
        frame = fixture()
        # 更强请求只能使用 IRAM，不能借 INTERNAL|32BIT 域的 DRAM 下界。
        self.evidence["requests"][0].update(size_bytes=10001, caps=(1 << 11) | (1 << 13) | (1 << 1))
        iram = frame["REGION"][2]
        iram["caps0"], iram["caps1"] = (1 << 13) | 1, (1 << 11) | (1 << 1)
        fit = self.read(frame)["provided_request_fit"]["requests"][0]
        self.assertEqual(fit["matching_early_region_starts"], [0x30000])
        self.assertEqual(fit["largest_request_lower_bound_bytes"], 10000)
        self.assertEqual(fit["search_size_bytes"], 10004)
        self.assertEqual(fit["status"], "unknown")
        self.evidence["requests"][0].update(size_bytes=40000, caps=module.INTERNAL_8BIT)
        fit = self.read()["provided_request_fit"]["requests"][0]
        self.assertEqual(fit["largest_request_lower_bound_bytes"], 26000)
        self.assertEqual(fit["matching_early_region_starts"], [0x10000])
        self.assertEqual(fit["status"], "unknown")

    def test_exec_hardware_caps_and_integer_overflow_remain_unknown(self):
        for caps in (IRAM_CAPS, module.INTERNAL_8BIT | (1 << 3), (1 << 17), 1 << 31):
            with self.subTest(caps=caps):
                self.evidence["requests"][0].update(size_bytes=1, caps=caps)
                row = self.read()["provided_request_fit"]["requests"][0]
                self.assertEqual(row["status"], "unknown")
                self.assertEqual(row["reason"], "exec_or_hardware_or_other_caps_path_not_modeled")
                self.assertIsNone(row["search_size_bytes"])
        self.evidence["requests"][0].update(size_bytes=module.UINT32_MAX, caps=module.INTERNAL_8BIT)
        row = self.read()["provided_request_fit"]["requests"][0]
        self.assertEqual(row["reason"], "target_size_overflow")
        self.assertIsNone(row["margin_bytes"])

    def test_incomplete_tail_cannot_extend_request_fit_or_retroactively_replace_the_closed_frame(self):
        first, gap, _ = cleanup_sequence()
        gap["REGION"][0].update(min_free_bytes=16000, min_largest_request_bytes=12000,
                                 free_bytes=16500, largest_request_bytes=12100)
        for domain in gap["DOMAIN"][:4]:
            domain.update(minimum_free_lower_bound_bytes=16000 + (16000 if domain["caps"] == (1 << 11) | (1 << 1) else 0),
                          largest_request_lower_bound_bytes=12000)
        fit = self.read(first, gap)["provided_request_fit"]
        self.assertEqual(fit["proved_until_begin_uptime_ms"], 10)
        self.assertEqual(fit["requests"][0]["largest_request_lower_bound_bytes"], 26000)
        self.assertEqual(fit["requests"][0]["status"], "fit")
        _, gap, _ = cleanup_sequence()
        fit = self.read(gap)["provided_request_fit"]
        self.assertIsNone(fit["proved_until_begin_uptime_ms"])
        self.assertEqual(fit["requests"][0]["reason"], "no_complete_task_readout")
        self.assertIsNone(fit["weakest_margin_bytes"])

    def test_same_boot_status_checks_full_signed_digest_size_and_target_but_config_sources_stay_unbound(self):
        self.status()
        identity = self.read()["provided_request_fit"]["identity"]
        self.assertEqual(identity["firmware_status"]["status"], "same_boot_target_and_signed_image_observed")
        self.assertFalse(identity["fully_bound"])
        for changes in ({"boot_id": "88888888-2222-4333-8444-555555555555"},
                        {"target": "esp32c3/esp_base"}, {"firmware_sha256": "b" * 64},
                        {"image_size_bytes": 1}, {"image_size_bytes": True}, {"state": "unknown"}):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                self.status(**changes)
                self.read()

    def test_identity_binding_schema_boolean_numeric_and_fake_coverage_claims_reject(self):
        original = json.dumps(self.evidence)
        changes = [(None, "target", "esp32c3"), (None, "boot_id", "88888888-2222-4333-8444-555555555555"),
                   (None, "sdk_lock_sha256", "b" * 64), (None, "allocator_recipe", "caller-formula"),
                   (None, "maximum_verified", True), ("identity", "coverage_complete", True),
                   ("request", "size_bytes", True), ("request", "alignment_bytes", 3),
                   ("request", "alignment_bytes", 0), ("request", "caps", 0),
                   ("request", "caps", module.UINT32_MAX + 1), ("request", "search_size_bytes", 1)]
        for parent, key, value in changes:
            self.evidence = json.loads(original)
            owner = self.evidence["requests"][0] if parent == "request" else self.evidence[parent] if parent else self.evidence
            owner[key] = value
            with self.subTest(parent=parent, key=key), self.assertRaises(ValueError):
                self.read()
        self.evidence = json.loads(original)
        self.evidence["requests"].append(dict(self.evidence["requests"][0]))
        with self.assertRaises(ValueError):
            self.read()
        self.evidence["requests"] = []
        with self.assertRaises(ValueError):
            self.read()
        self.evidence = json.loads(original)
        self.read()
        self.evidence_path.write_text(original.replace('"schema_version": 1', '"schema_version": 1, "schema_version": 1'))
        with self.assertRaises(ValueError):
            analyze([self.uart], "esp32", BOOT, self.lock, self.evidence_path)

    def test_tampered_nonregular_symlink_and_relative_identity_references_reject(self):
        original = json.dumps(self.evidence)
        for key in ("signed_firmware", "config", "sources"):
            self.evidence = json.loads(original)
            reference = self.evidence["identity"][key][0] if key == "sources" else self.evidence["identity"][key]
            reference["sha256"] = "b" * 64
            with self.subTest(key=key), self.assertRaises(ValueError):
                self.read()
        self.evidence = json.loads(original)
        source = self.evidence["identity"]["sources"][0]
        link = self.root / "linked.c"
        link.symlink_to(source["path"])
        source["path"] = str(link)
        with self.assertRaises(OSError):
            self.read()
        fifo = self.root / "fifo.c"
        os.mkfifo(fifo)
        source["path"] = str(fifo)
        with self.assertRaises(ValueError):
            self.read()
        source["path"] = "consumer.c"
        with self.assertRaises(ValueError):
            self.read()

    def test_changed_local_allocator_recipe_is_rejected_instead_of_accepting_a_caller_formula(self):
        read_file = module._file_fact

        def changed_recipe(path, **kwargs):
            fact, raw = read_file(path, **kwargs)
            if path == module.ROOT / "tools/sdk-patches/capacity-tlsf.patch":
                fact["sha256"] = "b" * 64
            return fact, raw

        with mock.patch.object(module, "_file_fact", side_effect=changed_recipe), self.assertRaises(ValueError):
            self.read()

    def test_non_utf8_excessive_nesting_and_prefixed_or_truncated_status_are_rejected(self):
        self.read()
        for raw in (json.dumps(self.evidence).encode("utf-16"), b"[" * 2000 + b"]" * 2000,
                    b"\xef\xbb\xbf" + json.dumps(self.evidence).encode()):
            self.evidence_path.write_bytes(raw)
            with self.assertRaises(ValueError):
                analyze([self.uart], "esp32", BOOT, self.lock, self.evidence_path)
        self.status()
        reference = self.evidence["identity"]["firmware_status_uart"]
        raw = Path(reference["path"]).read_bytes()
        for changed in (b"log: " + raw, raw.rstrip(b"\n")):
            self.evidence["identity"]["firmware_status_uart"] = self.reference("status.uart", changed)
            with self.assertRaises(ValueError):
                self.read()

    def test_cli_exposes_provided_fit_separately_and_numeric_success_never_grants_unknown_request(self):
        self.evidence["requests"][0]["size_bytes"] = 40000
        self.read()
        args = [sys.executable, str(TOOLS / "analyze_capacity.py"), "--target", "esp32", "--boot-id", BOOT,
                "--sdk-lock-sha256", self.lock, "--uart-log", str(self.uart),
                "--request-evidence", str(self.evidence_path)]
        result = subprocess.run(args + ["--json"], capture_output=True, text=True)
        self.assertEqual((result.returncode, result.stderr), (0, ""))
        body = json.loads(result.stdout)
        self.assertEqual(body["provided_request_fit"]["requests"][0]["status"], "unknown")
        self.assertFalse(body["provided_request_fit"]["all_provided_requests_fit"])
        self.assertFalse(body["next_maximum_legal_request_verified"])
        self.assertNotIn("never echo", result.stdout)
        text = subprocess.run(args, capture_output=True, text=True)
        self.assertEqual((text.returncode, text.stderr), (0, ""))
        self.assertIn("unknown / margin -14000 B", text.stdout)
        self.evidence_path.write_text('{"maximum_verified":true}')
        rejected = subprocess.run(args + ["--json"], capture_output=True, text=True)
        self.assertEqual((rejected.returncode, rejected.stderr), (1, ""))
        self.assertFalse(json.loads(rejected.stdout)["valid"])


if __name__ == "__main__":
    unittest.main()
