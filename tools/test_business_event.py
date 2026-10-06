#!/usr/bin/env python3
"""与固件 HMAC 验证共享的业务事件 wire 向量。"""

import importlib.util
import pathlib
import unittest


SCRIPT = pathlib.Path(__file__).with_name("business_event.py")
SPEC = importlib.util.spec_from_file_location("business_event", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class BusinessEventFrameTest(unittest.TestCase):
    def test_firmware_vector_and_bounds(self) -> None:
        device_id = "22222222-2222-4222-8222-222222222222"
        boot_id = "33333333-3333-4333-8333-333333333333"
        frame = MODULE.event_frame(bytes(range(32)), device_id, boot_id,
                                   1, b"\x01\x02\x03")
        self.assertEqual(frame.split(b"\n", 1)[0],
                         b"9fb0774308f1438407d8a298468d0c9d067c76897447a2c1d0618fe5429cab8c")
        self.assertEqual(len(frame), 175)
        maximum = MODULE.event_frame(bytes(range(32)), device_id, boot_id,
                                     2, b"\x01" + bytes(3923))
        self.assertEqual(len(maximum), 4096)
        with self.assertRaises(ValueError):
            MODULE.event_frame(bytes(range(32)), device_id, boot_id,
                               2, b"\x01" + bytes(3924))
        with self.assertRaises(ValueError):
            MODULE.event_frame(bytes(range(32)), device_id, boot_id,
                               0, b"\x01")
        with self.assertRaises(ValueError):
            MODULE.event_frame(bytes(range(32)), device_id, boot_id,
                               1, b"\x01" * 4096)


if __name__ == "__main__":
    unittest.main()
