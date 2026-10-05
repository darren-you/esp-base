import tempfile
import unittest
from pathlib import Path
from capacity_observation import observe


def uart(heap=16384, largest=24576, control=1024, task=1024, captured=1):
    return f"ESP_BASE_LAB_MEMORY uptime_ms=10 free_bytes=32768 min_bytes={heap} largest_bytes={largest} control_stack_min_bytes={control}\nESP_BASE_LAB_TASKS uptime_ms=10 expected=1 captured={captured} workspace_bytes=256\nESP_BASE_LAB_TASK uptime_ms=10 task=base_control task_number=1 minimum_stack_bytes={task} priority=5 state=0\n"


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
        self.assertTrue(self.read(uart(), "esp32")["observed_heap_gate_passed"])
        self.assertFalse(self.read(uart(heap=16383), "esp32")["observed_heap_gate_passed"])
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

    def test_empty_or_malformed(self):
        self.assertFalse(self.read("")["observed_heap_gate_passed"])
        result = self.read(uart() + "ESP_BASE_LAB_MEMORY uptime_ms=11 free_bytes=\n")
        self.assertEqual(result["malformed_resource_lines"], 1)
        self.assertFalse(result["observed_heap_gate_passed"])


if __name__ == "__main__":
    unittest.main()
