import tempfile
import unittest
from pathlib import Path
from capacity_observation import observe


def observer_start(target="esp32c3"):
    # 与生成器SOURCE实际puts声明同形；只用于软件边界输入，不升级历史现场资格。
    return (f"ESP_BASE_CAPACITY_OBSERVER LAB_ONLY target={target} period_ms=5000"
            " task_limit=32 exit_limit=64 other_capability_domains=unmeasured"
            " worker_exit_coverage=ota_before_done_and_normal_task_cleanup"
            " uncaptured_exits=reset_panic_before_cleanup non_task_stacks=unmeasured"
            " largest_history=unmeasured full_peak_qualification=0"
            " frozen_signed_image_qualification=0 observation_cost_added_back=0\n")


def uart(heap=16384, largest=24576, control=1024, task=1024, captured=1, target="esp32c3"):
    return observer_start(target) + f"ESP_BASE_LAB_MEMORY uptime_ms=10 free_bytes=32768 min_bytes={heap} largest_bytes={largest} control_stack_min_bytes={control}\nESP_BASE_LAB_TASKS uptime_ms=10 expected=1 captured={captured} workspace_bytes=256\nESP_BASE_LAB_TASK uptime_ms=10 task=base_control task_number=1 minimum_stack_bytes={task} priority=5 state=0\n"


class CapacityObservationTest(unittest.TestCase):
    def read(self, text, target="esp32c3"):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "uart.log"
            path.write_text(text)
            return observe([path], target)

    def test_target_boundary(self):
        c3 = self.read(uart())
        self.assertTrue(c3["observed_heap_gate_passed"])
        self.assertTrue(c3["observed_largest_gate_passed"])
        self.assertTrue(c3["observed_stack_gate_passed"])
        self.assertTrue(self.read(uart(target="esp32"), "esp32")["observed_heap_gate_passed"])
        self.assertFalse(self.read(uart(heap=16383, target="esp32"), "esp32")["observed_heap_gate_passed"])
        self.assertFalse(c3["full_peak_or_native_lifecycle_qualification"])

    def test_heap_fail(self):
        self.assertFalse(self.read(uart(heap=16383))["observed_heap_gate_passed"])

    def test_largest_fail(self):
        self.assertFalse(self.read(uart(largest=24575))["observed_largest_gate_passed"])

    def test_stack_fail(self):
        self.assertFalse(self.read(uart(task=1023))["observed_stack_gate_passed"])
        self.assertFalse(self.read(uart(control=1023))["observed_stack_gate_passed"])

    def test_incomplete_snapshot(self):
        result = self.read(uart().replace("expected=1 captured=1", "expected=2 captured=2"))
        self.assertEqual(result["incomplete_task_snapshots"], 1)
        self.assertFalse(result["observed_stack_gate_passed"])

    def test_duplicate_task(self):
        text = uart() + uart().splitlines()[-1] + "\n"
        result = self.read(text)
        self.assertEqual(result["malformed_resource_lines"], 1)
        self.assertFalse(result["observed_stack_gate_passed"])

    def test_native_task_name_with_spaces(self):
        result = self.read(uart().replace("task=base_control", "task=Tmr Svc"))
        self.assertEqual(result["complete_task_snapshots"], 1)
        self.assertEqual(result["malformed_resource_lines"], 0)
        self.assertTrue(result["observed_stack_gate_passed"])

    def test_separate_logs_do_not_complete_each_others_task_frame(self):
        with tempfile.TemporaryDirectory() as tmp:
            first, second = Path(tmp) / "first.log", Path(tmp) / "second.log"
            first.write_text(uart().replace("expected=1 captured=1", "expected=2 captured=2"))
            second.write_text(uart().splitlines()[-1].replace("task_number=1", "task_number=2") + "\n")
            result = observe([first, second], "esp32c3")
        self.assertEqual(result["complete_task_snapshots"], 0)
        self.assertEqual(result["incomplete_task_snapshots"], 1)
        self.assertEqual(result["malformed_resource_lines"], 1)
        self.assertFalse(result["observed_stack_gate_passed"])

    def test_empty_or_malformed(self):
        self.assertFalse(self.read("")["observed_heap_gate_passed"])
        result = self.read(uart() + "ESP_BASE_LAB_MEMORY uptime_ms=11 free_bytes=\n")
        self.assertEqual(result["malformed_resource_lines"], 1)
        self.assertFalse(result["observed_heap_gate_passed"])

    def test_missing_wrong_target_or_repeated_start_never_passes_any_observed_gate(self):
        cases = (uart().split("\n", 1)[1], uart(target="esp32"),
                 uart() + observer_start(), uart() + uart())
        for text in cases:
            with self.subTest(input=text):
                result = self.read(text)
                self.assertFalse(result["observed_heap_gate_passed"])
                self.assertFalse(result["observed_largest_gate_passed"])
                self.assertFalse(result["observed_stack_gate_passed"])
                self.assertFalse(result["full_peak_or_native_lifecycle_qualification"])

    def test_uptime_regression_in_memory_header_or_task_disqualifies_whole_observation(self):
        earlier = uart().replace("uptime_ms=10", "uptime_ms=50000")
        rows = uart().splitlines(keepends=True)[1:]
        for later in ("".join(rows), "".join(rows[1:]), rows[-1]):
            with self.subTest(later=later):
                result = self.read(earlier + later)
                self.assertFalse(result["observed_heap_gate_passed"])
                self.assertFalse(result["observed_largest_gate_passed"])
                self.assertFalse(result["observed_stack_gate_passed"])

    def test_fixture_start_matches_real_generator_declared_format(self):
        import re
        from prepare_capacity_observer import SOURCE
        statement = re.search(r'puts\(("ESP_BASE_CAPACITY_OBSERVER.*?);', SOURCE, re.DOTALL).group(1)
        actual = "".join(re.findall(r'"([^"\n]*)"', statement)).replace("@@TARGET@@", "esp32c3")
        self.assertEqual(observer_start().rstrip("\n"), actual)

    def test_sequential_logs_share_one_start_and_two_boots_never_combine(self):
        with tempfile.TemporaryDirectory() as tmp:
            first, second = Path(tmp) / "first.log", Path(tmp) / "second.log"
            first.write_text(uart())
            continuation = uart().split("\n", 1)[1].replace("uptime_ms=10", "uptime_ms=5010")
            second.write_text(continuation)
            result = observe([first, second], "esp32c3")
            self.assertTrue(result["observed_stack_gate_passed"])
            self.assertEqual(result["observer_start_declarations"], 1)
            self.assertEqual(result["complete_task_snapshots"], 2)
            second.write_text(uart())
            result = observe([first, second], "esp32c3")
            self.assertEqual(result["observer_start_declarations"], 2)
            self.assertFalse(result["observed_heap_gate_passed"])
            self.assertFalse(result["observed_largest_gate_passed"])
            self.assertFalse(result["observed_stack_gate_passed"])
            self.assertEqual(first.read_text(), uart())
            self.assertEqual(second.read_text(), uart())


if __name__ == "__main__":
    unittest.main()
