#!/usr/bin/env python3
"""用真实 POSIX 伪终端验证 USB 示例的字节传输与背压期限。"""
import importlib.util
import os
from pathlib import Path
import pty
import select
import termios
import time
import unittest

spec = importlib.util.spec_from_file_location("device_control", Path(__file__).with_name("device-control.py"))
control = importlib.util.module_from_spec(spec)
spec.loader.exec_module(control)


class SerialTransportTests(unittest.TestCase):
    def setUp(self):
        self.master, self.slave = pty.openpty()
        options = termios.tcgetattr(self.slave)
        options[2] |= termios.HUPCL
        termios.tcsetattr(self.slave, termios.TCSANOW, options)
        self.port = control.USBSerialPort(os.ttyname(self.slave))

    def tearDown(self):
        self.port.close()
        os.close(self.slave)
        os.close(self.master)

    def testRawDuplexPreservesBytesAndDisablesHangup(self):
        self.assertEqual(termios.tcgetattr(self.port.fd)[2] & termios.HUPCL, 0)
        payload = bytes(range(256))
        os.write(self.master, payload)
        actual = bytearray()
        deadline = time.monotonic() + 2
        while len(actual) < len(payload) and time.monotonic() < deadline:
            actual.extend(self.port.read(len(payload) - len(actual)))
        self.assertEqual(actual, payload)
        self.assertEqual(self.port.write(payload), len(payload))
        self.assertTrue(select.select([self.master], [], [], 2)[0])
        self.assertEqual(os.read(self.master, len(payload)), payload)

    def testBackpressureEndsAsUnknownWithinBound(self):
        started = time.monotonic()
        with self.assertRaisesRegex(TimeoutError, "unknown"):
            self.port.write(b"x" * (4 * 1024 * 1024))
        self.assertLess(time.monotonic() - started, 2.5)


if __name__ == "__main__":
    unittest.main()
