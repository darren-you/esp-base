"""只用离线日志验证丢失、损坏、并发观测边界；不访问设备。"""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

SCRIPT = Path(__file__).resolve().parents[1] / "analyze_flash_phases.py"
spec = importlib.util.spec_from_file_location("analyze_flash_phases", SCRIPT)
tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tool)
BOOT = "11111111-2222-4333-8444-555555555555"
OP = "66666666-7777-4888-9999-aaaaaaaaaaaa"
REQ = "bbbbbbbb-cccc-4ddd-8eee-ffffffffffff"


def phase(name, boundary, start, end, count, *, result=None, valid=1, uncertain=0):
    return (f"ESP_BASE_OTA_PHASE boot_id={BOOT} operation_id={OP} request_id={REQ} phase={name} "
            f"boundary={boundary} snapshot_started_us={start} snapshot_finished_us={end} "
            f"result={result or ('pending' if boundary == 'begin' else 'ok')} stage_uncertain={uncertain} "
            f"ota_completed_claim_count={count} config_completed_claim_count=3 "
            f"ota_counters_valid={valid} config_counters_valid=1\n")


def flash(start, end, count, *, valid=1, held=15, consumer="ota"):
    return (f"ESP_BASE_FLASH_IO boot_id={BOOT} snapshot_started_us={start} snapshot_finished_us={end} "
            f"consumer={consumer} completed_claim_count={count} acquire_failed_count=0 release_failed_count=0 "
            f"max_wait_us=5 max_held_upper_bound_us={held} counters_valid={valid} "
            "aggregate_storage_bytes=56 snapshot_copy_bytes=48\n")


def output(start, finish):
    return (f"ESP_BASE_FLASH_IO_OUTPUT boot_id={BOOT} started_us={start} finished_us={finish} "
            "scope=snapshot_and_two_rows_excluding_this_line\n")


def snapshot(start, end, count, *, valid=1):
    return (flash(start, end, count, valid=valid) + flash(start, end, 3, consumer="config") + output(start, end + 3))


def failed_transfer():
    return ("I (3) normal unrelated UART output\n" + snapshot(5, 7, 4) +
            phase("retire_inactive", "begin", 10, 12, 4) + phase("retire_inactive", "end", 20, 22, 6) +
            phase("prepare", "begin", 30, 32, 6) + snapshot(50, 52, 8) +
            phase("prepare", "end", 100, 102, 10, result="ota_download_failed") +
            phase("retire_cleanup", "begin", 110, 112, 10) + phase("retire_cleanup", "end", 120, 122, 12) +
            phase("prepare", "terminal", 130, 132, 12, result="ota_download_failed") + snapshot(140, 142, 13))


class FlashPhaseTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)

    def path(self, text, name="uart.log"):
        path = self.root / name
        path.write_bytes(text.encode() if isinstance(text, str) else text)
        return path

    def analyze(self, text):
        return tool.analyze([self.path(text)], BOOT, OP, REQ)

    def rejects(self, text):
        with self.assertRaises(ValueError):
            self.analyze(text)

    def test_failure_and_cleanup_remain_outer_observations_not_success(self):
        raw = failed_transfer()
        path = self.path(raw)
        original = path.stat()
        result = tool.analyze([path], BOOT, OP, REQ)
        self.assertEqual(result["inputs"][0]["sha256"], hashlib.sha256(raw.encode()).hexdigest())
        self.assertEqual(path.read_text(), raw)
        self.assertEqual(path.stat().st_mtime_ns, original.st_mtime_ns)
        prepare = result["phases"][1]
        self.assertEqual(prepare["status"], "observed")
        self.assertEqual(prepare["result"], "ota_download_failed")
        self.assertEqual(prepare["published_completed_claim_delta"], {"ota": 4, "config": 0})
        self.assertEqual(prepare["boundary_snapshot_separation_us"], {"minimum": 68, "maximum": 72})
        self.assertEqual(prepare["inside_snapshot_indices"], [1])
        self.assertEqual(result["phases"][2]["status"], "missing")
        self.assertEqual(result["records"][result["terminal_record_index"]]["phase"], "prepare")
        for key in ("r5_qualified", "r6_qualified", "internal_erase_verify_timing_proven",
                    "unfinished_holder_bound_proven", "whole_interval_worst_case_proven"):
            self.assertFalse(result[key])

    def test_missing_complete_rows_and_missing_phase_boundaries_are_explicit(self):
        result = self.analyze(flash(1, 2, 1) + phase("prepare", "end", 10, 12, 3, result="ota_download_failed"))
        self.assertEqual(result["snapshots"][0]["status"], "missing")
        self.assertEqual(result["snapshots"][0]["missing"], ["config", "output"])
        self.assertEqual(result["phases"][1]["missing"], ["begin"])
        self.assertIsNone(result["phases"][1]["published_completed_claim_delta"])
        self.assertEqual(result["terminal_observation"], "missing")

    def test_output_without_rows_is_missing_not_a_synthetic_snapshot(self):
        result = self.analyze(output(10, 20))
        self.assertEqual(result["snapshots"][0]["missing"], ["ota", "config"])
        self.assertIsNone(result["snapshots"][0]["finished_us"])

    def test_cross_task_print_order_can_reverse_snapshot_order(self):
        # Flash snapshot happened first, but a phase print won stdout first.
        result = self.analyze(phase("prepare", "begin", 30, 31, 3) + snapshot(10, 12, 2) +
                              phase("prepare", "end", 40, 41, 4))
        self.assertTrue(result["valid"])
        self.assertEqual(result["phases"][1]["published_completed_claim_delta"]["ota"], 1)

    def test_overlapping_snapshot_times_do_not_invent_a_counter_order(self):
        result = self.analyze(snapshot(10, 30, 4) + phase("prepare", "begin", 20, 22, 3) +
                              phase("prepare", "end", 40, 42, 5))
        self.assertGreater(result["overlapping_snapshot_order_counts"]["ota"], 0)
        self.assertEqual(result["phases"][1]["boundary_overlapping_snapshot_indices"], [0])

    def test_determined_counter_rollback_rejected_even_if_print_order_reversed(self):
        self.rejects(phase("prepare", "begin", 30, 31, 1) + snapshot(10, 12, 2))

    def test_invalid_history_remains_uncertain_and_cannot_recover(self):
        result = self.analyze(phase("prepare", "begin", 10, 12, 2, valid=0) +
                              snapshot(20, 22, 3, valid=0) + phase("prepare", "end", 30, 32, 4, valid=0))
        self.assertEqual(result["phases"][1]["status"], "uncertain")
        self.assertEqual(result["snapshots"][0]["status"], "uncertain")
        self.assertIsNone(result["phases"][1]["published_completed_claim_delta"])
        self.rejects(snapshot(1, 2, 1, valid=0) + phase("prepare", "begin", 10, 12, 2))

    def test_all_identities_are_required_and_crossing_is_rejected(self):
        raw = failed_transfer()
        for value in (BOOT, OP, REQ):
            with self.subTest(value=value):
                self.rejects(raw.replace(value, "12345678-1234-4234-8234-123456789abc", 1))
        with self.assertRaises(ValueError):
            tool.analyze([self.path(raw)], "bad", OP, REQ)

    def test_unknown_duplicate_reordered_and_missing_fields_are_rejected(self):
        raw = flash(1, 2, 1)
        for changed in (raw.replace("consumer=ota", "consumer=other"),
                        raw.replace("counters_valid=1", "counters_valid=1 counters_valid=1"),
                        raw.replace("max_wait_us=5 max_held_upper_bound_us=15", "max_held_upper_bound_us=15 max_wait_us=5"),
                        raw.replace("max_wait_us=5 ", ""), raw.replace("ESP_BASE_FLASH_IO", "ESP_BASE_FLASH_UNKNOWN")):
            with self.subTest(changed=changed):
                self.rejects(changed)

    def test_corrupted_prefix_non_ascii_and_truncated_lines_rejected(self):
        for changed in ("prefix " + flash(1, 2, 1), flash(1, 2, 1)[:-1],
                        flash(1, 2, 1) + "unrelated truncated tail", flash(1, 2, 1).replace("consumer=ota", "consumer=é")):
            with self.subTest(changed=changed):
                self.rejects(changed)
        part = flash(1, 2, 1)
        with self.assertRaises(ValueError):
            tool.analyze([self.path(part[:40], "part1"), self.path(part[40:], "part2")], BOOT, OP, REQ)

    def test_integer_bool_clock_cost_and_release_invariants(self):
        raw = flash(1, 2, 1)
        for before, after in (("max_wait_us=5", "max_wait_us=-1"), ("max_wait_us=5", "max_wait_us=05"),
            ("max_wait_us=5", "max_wait_us=4294967296"), ("counters_valid=1", "counters_valid=2"),
            ("snapshot_started_us=1", "snapshot_started_us=3"), ("snapshot_copy_bytes=48", "snapshot_copy_bytes=49"),
            ("aggregate_storage_bytes=56", "aggregate_storage_bytes=47"),
            ("release_failed_count=0", "release_failed_count=1"), ("completed_claim_count=1", "completed_claim_count=0")):
            with self.subTest(after=after):
                self.rejects(raw.replace(before, after))
        self.rejects(raw + flash(5, 6, 2).replace("aggregate_storage_bytes=56", "aggregate_storage_bytes=52"))

    def test_duplicate_rows_snapshot_disagreement_and_early_output_rejected(self):
        self.rejects(flash(1, 2, 1) * 2)
        self.rejects(flash(1, 2, 1) + flash(1, 3, 1, consumer="config"))
        self.rejects(flash(1, 3, 1) + output(1, 2))
        self.rejects(output(1, 2) * 2)

    def test_phase_order_clock_and_post_terminal_rejected(self):
        self.rejects(phase("prepare", "end", 20, 22, 2) + phase("prepare", "begin", 30, 32, 3))
        self.rejects(phase("prepare", "begin", 20, 22, 2) + phase("prepare", "end", 21, 25, 3))
        self.rejects(phase("select", "begin", 20, 22, 2) + phase("prepare", "end", 30, 32, 3))
        self.rejects(phase("prepare", "begin", 20, 22, 2) * 2)
        self.rejects(phase("select", "terminal", 20, 22, 2) + phase("select", "end", 30, 32, 3))
        self.rejects(phase("prepare", "terminal", 20, 22, 2))

    def test_terminal_only_and_valid_crlf_are_not_full_phase_coverage(self):
        result = self.analyze(phase("receipt_load", "terminal", 10, 12, 2,
                                    result="ota_boot_state_unknown", uncertain=1).replace("\n", "\r\n"))
        self.assertEqual(result["terminal_observation"], "uncertain")
        self.assertTrue(all(p["status"] == "missing" for p in result["phases"]))

    def test_non_regular_symlink_and_duplicate_inode_rejected(self):
        raw = self.path(failed_transfer())
        link = self.root / "link"
        link.symlink_to(raw)
        fifo = self.root / "fifo"
        os.mkfifo(fifo)
        for paths in ([link], [fifo], [raw, raw]):
            with self.subTest(paths=paths), self.assertRaises((OSError, ValueError)):
                tool.analyze(paths, BOOT, OP, REQ)
        hard = self.root / "hard"
        os.link(raw, hard)
        with self.assertRaises(ValueError):
            tool.analyze([raw, hard], BOOT, OP, REQ)

    def test_changing_input_is_rejected(self):
        path = self.path(failed_transfer())
        parse = tool.parse
        changed = False
        def mutate(raw):
            nonlocal changed
            if not changed:
                changed = True
                with path.open("ab") as stream:
                    stream.write(b"appended during read\n")
            return parse(raw)
        with mock.patch.object(tool, "parse", mutate), self.assertRaisesRegex(ValueError, "读取期间"):
            tool.analyze([path], BOOT, OP, REQ)

    def test_path_replacement_while_descriptor_is_stable_is_rejected(self):
        path = self.path(failed_transfer())
        parse = tool.parse
        replaced = False
        def replace(raw):
            nonlocal replaced
            if not replaced:
                replaced = True
                path.rename(self.root / "old")
                path.write_text(failed_transfer())
            return parse(raw)
        with mock.patch.object(tool, "parse", replace), self.assertRaisesRegex(ValueError, "读取期间"):
            tool.analyze([path], BOOT, OP, REQ)

    def test_bounds_no_observations_and_cli_exit_contract(self):
        self.rejects("ordinary log only\n")
        self.rejects("x" * (tool.MAX_LINE + 1) + "\n")
        with mock.patch.object(tool, "MAX_BYTES", 10):
            self.rejects(failed_transfer())
        with mock.patch.object(tool, "MAX_RECORDS", 1):
            self.rejects(failed_transfer())
        path = self.path(flash(1, 2, 1))
        command = [sys.executable, "-B", str(SCRIPT), "--boot-id", BOOT, "--operation-id", OP,
                   "--request-id", REQ, "--uart-log", str(path), "--json"]
        result = subprocess.run(command, check=False, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0)
        self.assertFalse(json.loads(result.stdout)["r5_qualified"])
        path.write_text(flash(1, 2, 1)[:-1])
        result = subprocess.run(command, check=False, capture_output=True, text=True)
        self.assertEqual(result.returncode, 2)
        self.assertFalse(json.loads(result.stdout)["valid"])


if __name__ == "__main__":
    unittest.main()
