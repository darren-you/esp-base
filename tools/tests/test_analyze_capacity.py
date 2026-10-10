"""正式连续统计帧的解析、保守下界与不授资格回归；不访问设备。"""
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

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


class AnalyzeCapacityTest(unittest.TestCase):
    def read(self, *contents, target="esp32", boot=BOOT, lock=LOCK):
        with tempfile.TemporaryDirectory() as directory:
            paths = []
            for index, content in enumerate(contents):
                path = Path(directory) / f"uart-{index}.log"
                path.write_text(content)
                paths.append(path)
            return analyze(paths, target, boot, lock)

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


if __name__ == "__main__":
    unittest.main()
