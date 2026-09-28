#!/usr/bin/env python3
"""与固件 HMAC 验证共享的业务事件 wire 向量。"""

import importlib.util
import pathlib
import unittest


SCRIPT = pathlib.Path(__file__).with_name("product_event.py")
SPEC = importlib.util.spec_from_file_location("product_event", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class ProductEventFrameTest(unittest.TestCase):
    def test_firmware_vector_and_bounds(self) -> None:
        device_id = "22222222-2222-4222-8222-222222222222"
        boot_id = "33333333-3333-4333-8333-333333333333"
        frame = MODULE.event_frame(bytes(range(32)), device_id, boot_id, "11" * 32,
                                   1, b"\x01\x02\x03")
        self.assertEqual(frame.split(b"\n", 1)[0],
                         b"825d3ce7ac4f69135bd409f47516840047e0f6470cd49279ec1a2e65475aaedd")
        self.assertEqual(len(frame), 206)
        with self.assertRaises(ValueError):
            MODULE.event_frame(bytes(range(32)), device_id, boot_id, "11" * 32,
                               0, b"\x01")
        with self.assertRaises(ValueError):
            MODULE.event_frame(bytes(range(32)), device_id, boot_id, "11" * 32,
                               1, b"\x01" * 4096)


if __name__ == "__main__":
    unittest.main()
