#!/usr/bin/env python3
"""有限宿主编排离线回归；opaque fixture 不构成签名、设备或 R6 资格。"""

import hashlib
import json
import os
from pathlib import Path
import stat
import sys
import tempfile
import threading
import types
import unittest
import uuid
from unittest import mock

import business_event
import frp_ota
import native_lifecycle_run as lifecycle


DEVICE = "22222222-2222-4222-8222-222222222222"
BOOT = "33333333-3333-4333-8333-333333333333"
TARGET = "esp32c3/esp_base"
KEY = bytes(range(32))


class Clock:
    def __init__(self):
        self.now = 10.0
        self.lock = threading.Lock()

    def __call__(self):
        with self.lock:
            return self.now

    def advance(self, amount):
        with self.lock:
            self.now += amount
            return self.now


class PauseAfterUnlock:
    """让较早采样的线程在释放真实锁后暂停，确定性覆盖调度交错。"""
    def __init__(self):
        self.lock = threading.Lock()
        self.sampled, self.resume = threading.Event(), threading.Event()

    def __enter__(self):
        self.lock.acquire()

    def __exit__(self, *args):
        self.lock.release()
        if threading.current_thread().name == "host-earlier-sample":
            self.sampled.set()
            if not self.resume.wait(2):
                raise RuntimeError("宿主采样调度屏障未释放")


class BlockingSink:
    """真实文件写入前阻塞首条记录，暴露整条持久化与关闭的线程次序。"""
    def __init__(self, output):
        self.output = output
        self.descriptor = output.fileno()
        self.entered, self.release = threading.Event(), threading.Event()
        self.steps = []

    def write(self, value):
        self.steps.append(("write", threading.current_thread().name))
        if not self.entered.is_set():
            self.entered.set()
            if not self.release.wait(5):
                raise RuntimeError("blocking sink fixture 未释放")
        return self.output.write(value)

    def flush(self):
        self.steps.append(("flush", threading.current_thread().name))
        return self.output.flush()

    def fileno(self):
        return self.output.fileno()

    def close(self):
        self.steps.append(("close", threading.current_thread().name))
        return self.output.close()

    @property
    def closed(self):
        return self.output.closed


class JournalConcurrencyTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)

    def blocked_pair(self, operation):
        journal = lifecycle.Journal(Path(self.directory.name) / operation)
        sink = BlockingSink(journal.output)
        journal.output = sink
        self.addCleanup(journal.close)
        errors = []
        second_started, second_finished = threading.Event(), threading.Event()
        def first():
            try:
                journal.record("first")
            except BaseException as error:
                errors.append(error)
        def second():
            second_started.set()
            try:
                if operation == "record":
                    journal.record("second")
                elif operation == "finish":
                    journal.finish({"state": "interrupted"})
                else:
                    journal.close()
            except BaseException as error:
                errors.append(error)
            finally:
                second_finished.set()
        writer = threading.Thread(target=first, name="journal-first")
        follower = threading.Thread(target=second, name="journal-second")
        real_fsync = os.fsync
        def fsync(descriptor):
            if descriptor == sink.descriptor:
                sink.steps.append(("fsync", threading.current_thread().name))
            return real_fsync(descriptor)
        with mock.patch.object(lifecycle.os, "fsync", side_effect=fsync):
            writer.start()
            try:
                self.assertTrue(sink.entered.wait(1))
                follower.start()
                self.assertTrue(second_started.wait(1))
                self.assertFalse(second_finished.wait(0.05))
                self.assertEqual(sink.steps, [("write", "journal-first")])
                self.assertFalse(sink.closed)
            finally:
                sink.release.set()
                writer.join(2)
                if follower.ident is not None:
                    follower.join(2)
        self.assertFalse(writer.is_alive())
        self.assertFalse(follower.is_alive())
        self.assertEqual(errors, [])
        return journal, sink

    def test_two_real_threads_serialize_complete_record_write_flush_fsync(self):
        journal, sink = self.blocked_pair("record")
        self.assertEqual(sink.steps, [(action, name) for name in ("journal-first", "journal-second")
                                    for action in ("write", "flush", "fsync")])
        values = [json.loads(line) for line in (journal.directory / "observations.jsonl").read_text().splitlines()]
        self.assertEqual([value["kind"] for value in values], ["first", "second"])

    def test_finish_and_close_wait_for_active_record_without_deadlock_or_closed_write(self):
        for operation in ("finish", "close"):
            with self.subTest(operation=operation):
                journal, sink = self.blocked_pair(operation)
                expected = [(action, "journal-first") for action in ("write", "flush", "fsync")]
                if operation == "finish":
                    expected += [(action, "journal-second") for action in ("write", "flush", "fsync")]
                    self.assertEqual(json.loads((journal.directory / "summary.json").read_text()), {"state": "interrupted"})
                expected += [("close", "journal-second")]
                self.assertEqual(sink.steps, expected)
                self.assertTrue(sink.closed)


class FrpFixture:
    def __init__(self, args, clock):
        self.args, self.clock = args, clock
        self.boot = BOOT
        self.image_name = "a"
        self.slot = "ota_0"
        self.count = 0
        self.sequence = 0
        self.operations = []
        self.worker_done = threading.Event()
        self.active_sessions = 0
        self.fail_operation = None
        self.interrupt_operation = None
        self.wrong_slot = False
        self.foreign_firmware = False
        self.late_business = False
        self.business_release = threading.Event()
        self.expected_boot_id = BOOT
        self.last_ota_command_attempt_id = None
        self.last_ota_admission_confirmed_id = None
        self.query_operations = []
        self.read_error = None
        self.read_response = None

    def envelope(self, result, state="succeeded"):
        return {"protocol_version": 1, "device_id": DEVICE, "boot_id": self.boot,
                "request_id": str(uuid.uuid4()), "state": state,
                "error_code": None if state == "succeeded" else "storage_uncertain", "result": result}

    def status(self):
        if self.expected_boot_id != self.boot:
            raise lifecycle.RunInterrupted("fixture 非预期 boot")
        return self.envelope({"uptime_ms": 1000, "capabilities": {
            "wifi": "connected", "mqtt": "ready", "frp": "ready", "config": "ready", "ota": "ready"}})

    def command(self, command, **kwargs):
        if command == "firmware.status":
            name = self.image_name
            return self.envelope({"firmware_sha256": "ff" * 32 if self.foreign_firmware else getattr(self.args, f"image_{name}_sha256"),
                "image_size_bytes": getattr(self.args, f"image_{name}_size_bytes"), "target": TARGET, "ota_slot": self.slot})
        if command in {"business.status", "business.pause", "business.resume"}:
            return self.envelope({"byte_count": self.count,
                "state": "paused" if command == "business.pause" else "idle", "window_deadline_uptime_ms": 0})
        raise AssertionError(command)

    def ota_start(self, operation_id, path, target, *, progress, **kwargs):
        destination = "c" if self.image_name == "a" else "a"
        if Path(path) != getattr(self.args, f"image_{destination}_file") or target != TARGET:
            raise AssertionError("必须交替升级另一份冻结镜像")
        self.operations.append((operation_id, destination))
        self.last_ota_command_attempt_id = operation_id
        self.last_ota_admission_confirmed_id = operation_id
        self.worker_done.clear()
        size = getattr(self.args, f"image_{destination}_size_bytes")
        self.clock.advance(0.1)
        progress(4096, size)
        if self.late_business:
            self.clock.advance(0.1)
            progress(size, size)
            self.business_release.set()
        if not self.worker_done.wait(2):
            raise AssertionError("有限业务线程未被非阻塞 progress 唤醒")
        if not self.late_business:
            self.clock.advance(0.1)
            progress(size, size)
        if len(self.operations) == self.interrupt_operation:
            raise KeyboardInterrupt()
        if len(self.operations) == self.fail_operation:
            return self.envelope(None, "unknown")
        old_slot = self.slot
        self.slot = "ota_1" if old_slot == "ota_0" else "ota_0"
        self.image_name = destination
        self.boot = str(uuid.uuid4())
        self.count = self.sequence = 0
        return self.envelope({"operation_id": operation_id,
            "sha256": getattr(self.args, f"image_{destination}_sha256"), "image_size_bytes": size,
            "target": target, "target_slot": old_slot if self.wrong_slot else self.slot})

    def result(self, operation_id, **expected):
        self.query_operations.append((operation_id, expected))
        if self.read_error is not None:
            raise self.read_error
        if self.read_response is not None:
            return self.read_response
        if not expected:
            destination = next((name for identifier, name in self.operations if identifier == operation_id),
                               "c" if self.image_name == "a" else "a")
            expected = {"expected_sha256": getattr(self.args, f"image_{destination}_sha256"),
                "expected_size": getattr(self.args, f"image_{destination}_size_bytes"), "expected_target": TARGET}
        name = self.image_name
        if getattr(self.args, f"image_{name}_sha256") == expected["expected_sha256"]:
            return self.envelope({"operation_id": operation_id, "sha256": expected["expected_sha256"],
                "image_size_bytes": expected["expected_size"], "target": expected["expected_target"],
                "target_slot": self.slot})
        return self.envelope(None, "unknown")


class SessionFixture:
    client = None

    def __init__(self, args, device_id, boot_id, key, *, clock):
        self.boot_id, self.clock = boot_id, clock
        self.cancelled = threading.Event()
        self.closed = False
        self.last_attempt = None

    def open(self):
        self.client.active_sessions += 1
        return self

    def publish_maximum(self, count):
        if self.client.boot != self.boot_id or count != self.client.count:
            raise lifecycle.RunInterrupted("fixture boot/count 错绑")
        if self.client.late_business and threading.current_thread().name == "base-finite-business":
            if not self.client.business_release.wait(2):
                raise AssertionError("迟到业务 fixture 未释放")
        started = self.clock.advance(0.1)
        self.client.count += lifecycle.EVENT_INCREMENT
        self.client.sequence += 1
        value = {"device_id": DEVICE, "boot_id": self.boot_id,
            "event_sequence": self.client.sequence, "event_sha256": hashlib.sha256(lifecycle.MAX_EVENT).hexdigest(),
            "event_size_bytes": len(lifecycle.MAX_EVENT), "frame_size_bytes": 4096,
            "business_result": self.client.count, "published_monotonic_seconds": started,
            "completed_reported_monotonic_seconds": self.clock.advance(0.1), "reported": {}}
        self.last_attempt = {"device_id": DEVICE, "boot_id": self.boot_id,
            "event_sequence": self.client.sequence, "event_sha256": value["event_sha256"],
            "event_size_bytes": len(lifecycle.MAX_EVENT), "expected_business_result": self.client.count,
            "published_monotonic_seconds": started}
        if threading.current_thread().name == "base-finite-business":
            self.client.worker_done.set()
        return value

    def close(self):
        if not self.closed:
            self.client.active_sessions -= 1
            self.closed = True


class LifecycleTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.serial = 0
        self.args = types.SimpleNamespace(mode="cycles-100", device_id=DEVICE, initial_boot_id=BOOT,
            ota_result_timeout_seconds=120, ota_interval_seconds=43200, business_interval_seconds=60)
        for name, raw in (("a", b"a" * 8192), ("c", b"c" * 9472)):
            path = self.root / f"opaque-{name}.bin"
            self.private(path, raw)
            setattr(self.args, f"image_{name}_file", path)
            setattr(self.args, f"image_{name}_sha256", hashlib.sha256(raw).hexdigest())
            setattr(self.args, f"image_{name}_size_bytes", len(raw))
            setattr(self.args, f"image_{name}_target", TARGET)
        self.args.r5_evidence_file = self.root / "r5-original"
        # Even this deliberately misleading JSON is bound as opaque bytes only.
        self.private(self.args.r5_evidence_file, b'{"qualified":true,"r5_passed":true}')
        self.args.r5_evidence_sha256 = hashlib.sha256(self.args.r5_evidence_file.read_bytes()).hexdigest()
        self.args.capacity_log_file = self.root / "capacity-original"
        self.private(self.args.capacity_log_file, b"external capacity original\n")

    def private(self, path, raw):
        path.write_bytes(raw)
        path.chmod(0o600)

    def runner(self):
        self.serial += 1
        self.clock = Clock()
        self.client = FrpFixture(self.args, self.clock)
        SessionFixture.client = self.client
        journal = lifecycle.Journal(self.root / f"run-{self.serial}", self.clock)
        self.addCleanup(journal.close)
        return lifecycle.LifecycleRun(self.args, self.client, KEY, journal,
            session_factory=SessionFixture, clock=self.clock, sleep=self.clock.advance, wall_clock=self.clock)

    def records(self, runner):
        return [json.loads(line) for line in (runner.journal.directory / "observations.jsonl").read_text().splitlines()]

    def test_single_100_cycle_run_alternates_exact_pair_and_never_claims_r6(self):
        runner = self.runner()
        result = runner.run()
        self.assertEqual((result["state"], result["completed_cycles"], result["completed_business_events"]),
                         ("completed", 100, 300))
        operations = self.client.operations
        self.assertEqual([name for _, name in operations], ["c", "a"] * 50)
        self.assertEqual(len({operation for operation, _ in operations}), 100)
        self.assertFalse(result["qualified"])
        self.assertFalse(result["r6_passed"])
        records = self.records(runner)
        self.assertEqual(len([item for item in records if item["kind"] == "cycle_completed"]), 100)
        overlaps = [item for item in records if item["kind"] == "ota_upload_observed"]
        self.assertTrue(all(item["business_observation_overlap"] for item in overlaps))
        self.assertTrue(all(item["overlap_end_monotonic_seconds"] > item["overlap_start_monotonic_seconds"] for item in overlaps))
        self.assertEqual(self.client.active_sessions, 0)
        self.assertFalse(any(thread.name == "base-finite-business" for thread in threading.enumerate()))
        for filename in ("observations.jsonl", "summary.json"):
            self.assertEqual(stat.S_IMODE((runner.journal.directory / filename).stat().st_mode), 0o600)
        self.assertEqual(stat.S_IMODE(runner.journal.directory.stat().st_mode), 0o700)

    def test_unknown_third_operation_stops_without_new_id_or_reupload(self):
        runner = self.runner()
        self.client.fail_operation = 3
        result = runner.run()
        self.assertEqual((result["state"], result["completed_cycles"]), ("interrupted", 2))
        self.assertEqual(len(self.client.operations), 3)
        self.assertEqual(result["original_operation_ids"], [item[0] for item in self.client.operations])
        self.assertFalse(result["observation_completed"])
        self.assertEqual(self.client.active_sessions, 0)
        self.assertEqual(result["capacity_log_end"]["sha256"], result["capacity_log_start"]["sha256"])
        self.assertEqual([item[0] for item in self.client.query_operations], [self.client.operations[-1][0]])
        self.assertTrue(any(item["kind"] == "ota_result_observed" and item["response"]["state"] == "unknown"
                            for item in self.records(runner)))

    def test_keyboard_interrupt_keeps_original_id_and_releases_threads(self):
        runner = self.runner()
        self.client.interrupt_operation = 1
        result = runner.run()
        self.assertEqual(result["state"], "interrupted")
        self.assertEqual(result["error"]["type"], "KeyboardInterrupt")
        self.assertEqual(len(result["original_operation_ids"]), 1)
        self.assertEqual(self.client.active_sessions, 0)
        self.assertEqual([item[0] for item in self.client.query_operations], result["original_operation_ids"])

    def test_thread_start_failure_closes_session_without_submitting_ota(self):
        runner = self.runner()
        with mock.patch.object(threading.Thread, "start", side_effect=RuntimeError("fixture thread start failed")):
            result = runner.run()
        self.assertEqual(result["state"], "interrupted")
        self.assertEqual(self.client.operations, [])
        self.assertEqual(len(result["original_operation_ids"]), 1)
        self.assertEqual(self.client.active_sessions, 0)
        self.assertEqual(self.client.query_operations, [])
        skipped = next(item for item in self.records(runner) if item["kind"] == "ota_exception_read_skipped")
        self.assertEqual(skipped["boundary"], {"reserved": True, "attempted_command": False,
            "admission_confirmed": False, "scope": "ota_start_command_call_and_running_reply; transmitted_bytes_not_observed"})

    def test_scheduled_but_late_business_is_recorded_without_overlap_claim(self):
        runner = self.runner()
        self.client.late_business = True
        runner.cycle()
        overlap = next(item for item in self.records(runner) if item["kind"] == "ota_upload_observed")
        self.assertFalse(overlap["business_observation_overlap"])
        self.assertIsNone(overlap["overlap_start_monotonic_seconds"])
        self.assertIsNone(overlap["overlap_end_monotonic_seconds"])
        self.assertFalse(overlap["device_instantaneous_peak_verified"])
        self.assertEqual(self.client.active_sessions, 0)

    def test_foreign_running_firmware_or_unexpected_boot_prevents_any_ota(self):
        for fault in ("foreign_firmware", "unexpected_boot"):
            with self.subTest(fault=fault):
                runner = self.runner()
                if fault == "foreign_firmware":
                    self.client.foreign_firmware = True
                else:
                    self.client.boot = str(uuid.uuid4())
                result = runner.run()
                self.assertEqual(result["state"], "interrupted")
                self.assertEqual(self.client.operations, [])

    def test_same_pair_or_r5_hash_mismatch_is_rejected_before_network(self):
        self.args.r5_evidence_sha256 = "11" * 32
        with self.assertRaisesRegex(ValueError, "R5 原件"):
            runner = self.runner()
        self.args.r5_evidence_sha256 = hashlib.sha256(self.args.r5_evidence_file.read_bytes()).hexdigest()
        self.args.image_c_file = self.args.image_a_file
        self.args.image_c_sha256 = self.args.image_a_sha256
        self.args.image_c_size_bytes = self.args.image_a_size_bytes
        with self.assertRaisesRegex(ValueError, "两份不同"):
            self.runner()

    def test_wrong_terminal_slot_is_not_completed(self):
        runner = self.runner()
        self.client.wrong_slot = True
        result = runner.run()
        self.assertEqual((result["state"], result["completed_cycles"]), ("interrupted", 0))
        self.assertEqual(len(self.client.operations), 1)

    def test_malformed_progress_never_becomes_overlap_proof(self):
        runner = self.runner()
        def wrong(operation, path, target, *, progress, **kwargs):
            progress(4096, 1)
        self.client.ota_start = wrong
        result = runner.run()
        self.assertEqual(result["state"], "interrupted")
        self.assertFalse(any(item["kind"] == "ota_upload_observed" for item in self.records(runner)))
        self.assertEqual(self.client.active_sessions, 0)

    def test_72h_is_one_fixed_run_and_short_interruption_cannot_be_accumulated(self):
        self.args.mode = "soak-72h"
        runner = self.runner()
        def cycle():
            runner.cycles += 1
            self.clock.advance(5)
        def event():
            runner.events += 1
            self.clock.advance(1)
        runner.cycle, runner.event = cycle, event
        result = runner.run()
        self.assertEqual(result["required_continuous_seconds"], 259200)
        self.assertGreaterEqual(result["elapsed_seconds"], 259200)
        self.assertEqual(result["state"], "completed")
        first_run_id = result["run_id"]
        short = self.runner()
        def interrupted():
            self.clock.advance(3600)
            raise KeyboardInterrupt()
        short.cycle = interrupted
        partial = short.run()
        self.assertNotEqual(partial["run_id"], first_run_id)
        self.assertEqual((partial["state"], partial["elapsed_seconds"]), ("interrupted", 3600))

    def test_72h_wake_after_long_host_pause_is_interrupted_even_past_deadline(self):
        self.args.mode = "soak-72h"
        runner = self.runner()
        def cycle():
            runner.cycles += 1
            self.clock.advance(1)
        def suspended_sleep(amount):
            self.clock.advance(lifecycle.SOAK_SECONDS + 1)
        runner.cycle, runner.sleep = cycle, suspended_sleep
        result = runner.run()
        self.assertGreater(result["elapsed_seconds"], lifecycle.SOAK_SECONDS)
        self.assertEqual(result["state"], "interrupted")
        self.assertFalse(result["observation_completed"])
        self.assertEqual(result["allowed_host_progress_gap_seconds"], 10)
        self.assertGreater(result["maximum_host_progress_gap_seconds"], 10)
        self.assertIn("宿主观察", result["error"]["message"])

    def test_independent_wall_sleep_and_time_jumps_interrupt_without_extending_duration(self):
        self.args.mode = "soak-72h"
        for jump in (3600, 5, -5):
            with self.subTest(wall_jump_seconds=jump):
                runner = self.runner()
                offset = [0]
                runner.wall_clock = lambda: self.clock() + offset[0]
                def cycle():
                    runner.cycles += 1
                    self.clock.advance(1)
                def sleep(amount):
                    offset[0] += jump
                    self.clock.advance(0.1)
                runner.cycle, runner.sleep = cycle, sleep
                result = runner.run()
                self.assertEqual(result["state"], "interrupted")
                self.assertFalse(result["observation_completed"])
                self.assertAlmostEqual(result["elapsed_seconds"], 1.1)
                anomaly = next(item for item in self.records(runner) if item["kind"] == "host_clock_anomaly")
                self.assertAlmostEqual(anomaly["discrepancy_seconds"], jump)
                self.assertEqual(result["allowed_clock_delta_discrepancy_seconds"], 1)

    def test_negative_monotonic_gap_is_an_explicit_clock_interruption(self):
        self.args.mode = "soak-72h"
        runner = self.runner()
        def cycle():
            runner.cycles += 1
            self.clock.advance(1)
        runner.cycle = cycle
        runner.sleep = lambda amount: self.clock.advance(-2)
        result = runner.run()
        self.assertEqual(result["state"], "interrupted")
        anomaly = next(item for item in self.records(runner) if item["kind"] == "host_clock_anomaly")
        self.assertLess(anomaly["monotonic_delta_seconds"], 0)
        self.assertFalse(result["observation_completed"])

    def test_two_real_threads_do_not_turn_normal_monotonic_progress_into_negative_gap(self):
        runner = self.runner()
        runner.clock = runner.wall_clock = lifecycle.time.monotonic
        runner.host_last_progress = runner.clock()
        runner.clock_sample_monotonic = runner.host_last_progress
        runner.clock_sample_wall = runner.wall_clock()
        runner.clock_sample_lock = barrier = PauseAfterUnlock()
        errors = []
        def earlier():
            try:
                runner.observe_host("earlier_sample", record=True)
            except BaseException as error:
                errors.append(error)
        thread = threading.Thread(target=earlier, name="host-earlier-sample")
        thread.start()
        try:
            self.assertTrue(barrier.sampled.wait(1))
            runner.observe_host("later_sample", record=True)
        finally:
            barrier.resume.set()
            thread.join(2)
        self.assertFalse(thread.is_alive())
        self.assertEqual(errors, [])
        records = self.records(runner)
        self.assertEqual(len(records), 2)
        self.assertTrue(all(item["kind"] == "host_progress_checked" and item["gap_seconds"] >= 0
                            for item in records))
        self.assertFalse(any(item["kind"] == "host_clock_anomaly" for item in records))

    def test_host_progress_and_clock_anomaly_records_release_clock_lock_before_journal(self):
        runner = self.runner()
        original = runner.journal.record
        def record(kind, **value):
            acquired = runner.clock_sample_lock.acquire(blocking=False)
            self.assertTrue(acquired, "Journal记录不能持有clock锁")
            if acquired:
                runner.clock_sample_lock.release()
            original(kind, **value)
        with mock.patch.object(runner.journal, "record", side_effect=record):
            self.clock.advance(1)
            runner.observe_host("normal_progress", record=True)
            self.clock.advance(-2)
            with self.assertRaises(lifecycle.RunInterrupted):
                runner.observe_host("actual_clock_rollback")
        self.assertEqual([item["kind"] for item in self.records(runner)],
                         ["host_progress_checked", "host_clock_anomaly"])

    def test_wall_sleep_inside_expected_ota_window_is_not_reset_or_ignored(self):
        runner = self.runner()
        offset = [0]
        runner.wall_clock = lambda: self.clock() + offset[0]
        original = self.client.ota_start
        def sleep_during_ota(*args, **kwargs):
            offset[0] += 3600
            return original(*args, **kwargs)
        self.client.ota_start = sleep_during_ota
        result = runner.run()
        self.assertEqual((result["state"], result["completed_cycles"]), ("interrupted", 0))
        anomaly = next(item for item in self.records(runner) if item["kind"] == "host_clock_anomaly")
        self.assertEqual(anomaly["stage"], "ota_upload_progress")
        self.assertEqual(len(self.client.query_operations), 1)

    def test_progress_runtime_and_unknown_exceptions_read_original_once_without_outcome_change(self):
        for fault in ("progress", "unknown", "post_boot"):
            with self.subTest(fault=fault):
                runner = self.runner()
                original = self.client.ota_start
                if fault == "progress":
                    def failed(operation_id, path, target, *, progress, **kwargs):
                        self.client.last_ota_command_attempt_id = operation_id
                        self.client.last_ota_admission_confirmed_id = operation_id
                        self.client.operations.append((operation_id, "c"))
                        progress(4096, 1)
                    self.client.ota_start = failed
                elif fault == "unknown":
                    def failed(operation_id, *args, **kwargs):
                        original(operation_id, *args, **kwargs)
                        raise frp_ota.UnknownOperation(operation_id, "fixture original ID lost")
                    self.client.ota_start = failed
                else:
                    def failed(*args, **kwargs):
                        value = original(*args, **kwargs)
                        self.client.foreign_firmware = True
                        return value
                    self.client.ota_start = failed
                result = runner.run()
                self.assertEqual(result["state"], "interrupted")
                self.assertEqual(result["completed_cycles"], 0)
                self.assertEqual([item[0] for item in self.client.query_operations], result["original_operation_ids"])
                self.assertEqual(len(self.client.operations), 1)
                observed = next(item for item in self.records(runner) if item["kind"] == "ota_exception_original_id_read")
                self.assertFalse(observed["changes_run_outcome"])
                self.assertEqual(observed["primary_error_type"], "UnknownOperation" if fault == "unknown" else "RunInterrupted")
                self.assertEqual(observed["response"]["state"], "unknown" if fault == "progress" else "succeeded")
                self.assertFalse(result["qualified"])

    def test_original_read_failures_never_mask_keyboard_interrupt(self):
        for failure in (OSError("fixture read timeout"), KeyboardInterrupt(), "device", "boot", "receipt"):
            with self.subTest(failure=str(failure)):
                runner = self.runner()
                self.client.interrupt_operation = 1
                if isinstance(failure, BaseException):
                    self.client.read_error = failure
                else:
                    value = self.client.envelope(None, "unknown")
                    if failure == "device":
                        value["device_id"] = str(uuid.uuid4())
                    elif failure == "boot":
                        value["boot_id"] = str(uuid.uuid4())
                    else:
                        value["result"] = {"operation_id": "foreign-receipt"}
                    self.client.read_response = value
                result = runner.run()
                self.assertEqual(result["error"]["type"], "KeyboardInterrupt")
                self.assertEqual(result["state"], "interrupted")
                self.assertEqual(len(self.client.query_operations), 1)
                if isinstance(failure, BaseException):
                    self.assertTrue(any(item["kind"] == "ota_exception_original_id_read_failed" for item in self.records(runner)))
                else:
                    actual = next(item for item in self.records(runner) if item["kind"] == "ota_exception_original_id_response_observed")
                    verdict = next(item for item in self.records(runner) if item["kind"] == "ota_exception_original_id_read")
                    self.assertEqual(actual["response"], self.client.read_response)
                    self.assertFalse(verdict["identity_verified"])

    def test_new_boot_pending_or_failed_response_is_retained_with_separate_receipt_verdict(self):
        for state, receipt in (("running", False), ("failed", True), ("unknown", False)):
            with self.subTest(state=state):
                runner = self.runner()
                self.client.interrupt_operation = 1
                original = self.client.result
                def read(operation_id, **expected):
                    image = runner.active_operation["image"]
                    expected = {"expected_sha256": image["sha256"], "expected_size": image["size_bytes"],
                                "expected_target": image["target"]}
                    result = {"operation_id": operation_id, "sha256": expected["expected_sha256"],
                        "image_size_bytes": expected["expected_size"], "target": expected["expected_target"],
                        "target_slot": "ota_1"} if receipt else None
                    value = self.client.envelope(result, state)
                    value["boot_id"] = str(uuid.uuid4())
                    self.client.read_response = value
                    return original(operation_id, **expected)
                self.client.result = read
                result = runner.run()
                self.assertEqual((result["state"], result["error"]["type"]), ("interrupted", "KeyboardInterrupt"))
                actual = next(item for item in self.records(runner) if item["kind"] == "ota_exception_original_id_response_observed")
                verdict = next(item for item in self.records(runner) if item["kind"] == "ota_exception_original_id_read")
                self.assertEqual(actual["response"]["state"], state)
                self.assertEqual(verdict["boot_relation"], "new")
                self.assertEqual(verdict["identity_verified"], receipt)
                self.assertEqual(verdict["receipt_identity_verified"], receipt)
                self.assertFalse(verdict["changes_run_outcome"])
                self.assertFalse(result["qualified"])

    def test_real_frp_result_preserves_authenticated_non_candidate_receipt_for_uncertain_verdict(self):
        runner = self.runner()
        operation_id = str(uuid.uuid4())
        client = lifecycle.BoundFrpClient("http://fixture.example.invalid", DEVICE, KEY)
        client.last_ota_command_attempt_id = operation_id
        runner.client = client
        image = runner.images[1]
        value = self.client.envelope({"operation_id": operation_id, "sha256": "ff" * 32,
            "image_size_bytes": image["size_bytes"], "target": TARGET, "target_slot": "ota_1"}, "failed")
        operation = {"operation_id": operation_id, "source_boot_id": BOOT, "image": image, "target_slot": "ota_1"}
        with mock.patch.object(client, "command", return_value=value) as read:
            runner.reconcile_interrupted_operation(operation, KeyboardInterrupt())
        read.assert_called_once_with("ota.result", parameters={"operation_id": operation_id}, deadline=None)
        actual = next(item for item in self.records(runner) if item["kind"] == "ota_exception_original_id_response_observed")
        verdict = next(item for item in self.records(runner) if item["kind"] == "ota_exception_original_id_read")
        self.assertEqual(actual["response"], value)
        self.assertFalse(verdict["identity_verified"])
        self.assertEqual(verdict["identity_verdict"], "uncertain")
        self.assertFalse(verdict["changes_run_outcome"])

    def test_independent_business_runtime_failure_interrupts_progress_then_reads_original(self):
        runner = self.runner()
        failed = threading.Event()
        original_publish = SessionFixture.publish_maximum
        def publish(session, count):
            if threading.current_thread().name == "base-finite-business":
                failed.set()
                raise lifecycle.RunInterrupted("fixture business failed")
            return original_publish(session, count)
        def upload(operation_id, path, target, *, progress, **kwargs):
            self.client.last_ota_command_attempt_id = operation_id
            self.client.last_ota_admission_confirmed_id = operation_id
            self.client.operations.append((operation_id, "c"))
            progress(4096, self.args.image_c_size_bytes)
            self.assertTrue(failed.wait(1))
            for thread in threading.enumerate():
                if thread.name == "base-finite-business":
                    thread.join(1)
            progress(self.args.image_c_size_bytes, self.args.image_c_size_bytes)
        self.client.ota_start = upload
        with mock.patch.object(SessionFixture, "publish_maximum", publish):
            result = runner.run()
        self.assertEqual(result["error"]["type"], "RunInterrupted")
        self.assertIn("上传期间业务", result["error"]["message"])
        self.assertEqual([item[0] for item in self.client.query_operations], result["original_operation_ids"])
        self.assertEqual(len(self.client.operations), 1)
        self.assertEqual(self.client.active_sessions, 0)

    def test_attempted_command_without_admission_is_read_once_and_not_called_submitted(self):
        runner = self.runner()
        def attempted(operation_id, *args, **kwargs):
            self.client.last_ota_command_attempt_id = operation_id
            raise OSError("fixture command connection failure")
        self.client.ota_start = attempted
        result = runner.run()
        self.assertEqual(result["state"], "interrupted")
        self.assertEqual(self.client.operations, [])
        self.assertEqual(len(self.client.query_operations), 1)
        observed = next(item for item in self.records(runner) if item["kind"] == "ota_exception_original_id_read")
        self.assertTrue(observed["boundary"]["attempted_command"])
        self.assertFalse(observed["boundary"]["admission_confirmed"])

    def test_real_bound_command_records_only_attempt_then_verified_running_reply(self):
        client = lifecycle.BoundFrpClient("http://example.invalid", DEVICE, KEY)
        operation_id = str(uuid.uuid4())
        image = {"sha256": "11" * 32, "size_bytes": 8192, "target": TARGET}
        client.expected_image = image
        parameters = {"operation_id": operation_id, "sha256": image["sha256"],
            "image_size_bytes": image["size_bytes"], "target": TARGET}
        with mock.patch.object(frp_ota.FrpClient, "command", side_effect=OSError("fixture before send")):
            with self.assertRaises(OSError):
                client.command("ota.start", parameters=parameters)
        self.assertEqual(client.last_ota_command_attempt_id, operation_id)
        self.assertIsNone(client.last_ota_admission_confirmed_id)
        with mock.patch.object(frp_ota.FrpClient, "command", return_value={"state": "running", "result": None}):
            client.command("ota.start", parameters=parameters)
        self.assertEqual(client.last_ota_admission_confirmed_id, operation_id)

    def test_expected_ota_window_is_recorded_and_over_budget_cannot_complete(self):
        runner = self.runner()
        original = self.client.ota_start
        def slow(*args, **kwargs):
            result = original(*args, **kwargs)
            self.clock.advance(382 + runner.args.ota_result_timeout_seconds)
            return result
        self.client.ota_start = slow
        result = runner.run()
        self.assertEqual((result["state"], result["completed_cycles"]), ("interrupted", 0))
        self.assertEqual(result["expected_ota_window_count"], 1)
        window = next(item for item in self.records(runner) if item["kind"] == "expected_ota_window_closed")
        self.assertGreater(window["elapsed_seconds"], window["allowed_seconds"])
        self.assertFalse(window["device_continuity_verified"])
        self.assertEqual(len(result["original_operation_ids"]), 1)

    def test_main_preflight_failure_keeps_run_monotonic_summary(self):
        self.args.frp_management_key_env = "LIFECYCLE_TEST_FRP_KEY"
        self.args.business_management_key_file = self.root / "key"
        self.args.credentials_file = self.root / "account"
        self.args.ca_file = self.root / "ca"
        self.args.endpoint = "http://fixture.example.invalid"
        self.args.output_directory = self.root / "preflight-run"
        paho, mqtt, client = (types.ModuleType(name) for name in ("paho", "paho.mqtt", "paho.mqtt.client"))
        paho.mqtt, mqtt.client = mqtt, client
        with mock.patch.dict(os.environ, {self.args.frp_management_key_env: KEY.hex()}), \
                mock.patch.dict(sys.modules, {"paho": paho, "paho.mqtt": mqtt, "paho.mqtt.client": client}), \
                mock.patch.object(lifecycle, "arguments", return_value=self.args), \
                mock.patch.object(lifecycle.business_event, "private_key", return_value=KEY), \
                mock.patch.object(lifecycle.publisher, "credentials", return_value={}), \
                mock.patch.object(lifecycle, "file_snapshot", return_value={}), \
                mock.patch.object(lifecycle, "LifecycleRun", side_effect=ValueError("opaque preflight failure")), \
                mock.patch("builtins.print"):
            self.assertEqual(lifecycle.main([]), 1)
        value = json.loads((self.args.output_directory / "summary.json").read_text())
        self.assertEqual(value["state"], "interrupted")
        self.assertGreaterEqual(value["ended_monotonic_seconds"], value["started_monotonic_seconds"])
        self.assertGreaterEqual(value["elapsed_seconds"], 0)
        self.assertEqual(value["original_operation_ids"], [])
        self.assertFalse(value["observation_completed"])
        self.assertEqual(str(uuid.UUID(value["run_id"])), value["run_id"])

    def test_capacity_prefix_rewrite_is_rejected_without_qualification(self):
        self.args.mode = "soak-72h"
        runner = self.runner()
        def cycle():
            runner.cycles += 1
            self.args.capacity_log_file.write_bytes(b"rewritten capacity original\n")
            self.clock.advance(1)
            raise KeyboardInterrupt()
        runner.cycle = cycle
        result = runner.run()
        self.assertEqual(result["state"], "interrupted")
        failed = next(item for item in self.records(runner) if item["kind"] == "external_capacity_binding_failed")
        self.assertIn("前缀", failed["error"]["message"])

    def test_final_evidence_binding_checks_host_clocks_in_both_run_modes(self):
        for mode in ("cycles-100", "soak-72h"):
            for fault in (None, "progress_gap", "monotonic_rollback", "wall_sleep"):
                with self.subTest(mode=mode, fault=fault):
                    self.args.mode = mode
                    runner = self.runner()
                    offset = [0]
                    runner.wall_clock = lambda: self.clock() + offset[0]
                    original = lifecycle.file_snapshot
                    def snapshot(path, **kwargs):
                        value = original(path, **kwargs)
                        if Path(path) == self.args.capacity_log_file and kwargs.get("prefix_bytes") is not None:
                            if fault == "progress_gap":
                                self.clock.advance(20)
                            elif fault == "monotonic_rollback":
                                self.clock.advance(-200)
                            elif fault == "wall_sleep":
                                offset[0] += 3600
                        return value
                    # 只缩短软件模式的循环期限；真实run/cycle及OTA窗口接线不替换。
                    with mock.patch.object(lifecycle, "SOAK_SECONDS", 1), \
                            mock.patch.object(lifecycle, "file_snapshot", side_effect=snapshot), \
                            mock.patch.object(runner, "observe_host", wraps=runner.observe_host) as observed:
                        result = runner.run()
                    observed.assert_any_call("run_final_boundary")
                    self.assertFalse(result["qualified"])
                    self.assertFalse(result["r6_passed"])
                    if fault is None:
                        self.assertEqual(result["state"], "completed")
                        self.assertTrue(result["observation_completed"])
                        self.assertIsNone(result["error"])
                        self.assertLessEqual(result["maximum_host_progress_gap_seconds"], 10)
                    else:
                        self.assertEqual(result["state"], "interrupted")
                        self.assertFalse(result["observation_completed"])
                        self.assertEqual(result["error"]["type"], "RunInterrupted")
                        if fault == "monotonic_rollback":
                            self.assertLess(result["elapsed_seconds"], 0)
                        if fault != "progress_gap":
                            anomaly = next(item for item in self.records(runner)
                                           if item["kind"] == "host_clock_anomaly")
                            self.assertEqual(anomaly["stage"], "run_final_boundary")

    def test_final_host_failure_after_interruption_retains_primary_error(self):
        for record_failure in (False, True):
            with self.subTest(secondary_journal_failure=record_failure):
                runner = self.runner()
                self.client.interrupt_operation = 1
                offset = [0]
                runner.wall_clock = lambda: self.clock() + offset[0]
                original = lifecycle.file_snapshot
                def snapshot(path, **kwargs):
                    value = original(path, **kwargs)
                    if Path(path) == self.args.capacity_log_file and kwargs.get("prefix_bytes") is not None:
                        offset[0] += 3600
                    return value
                original_record = runner.journal.record
                def record(kind, **value):
                    if record_failure and kind == "final_host_observation_failed_after_interruption":
                        raise OSError("software secondary Journal failure")
                    original_record(kind, **value)
                with mock.patch.object(lifecycle, "file_snapshot", side_effect=snapshot), \
                        mock.patch.object(runner.journal, "record", side_effect=record):
                    result = runner.run()
                self.assertEqual(result["state"], "interrupted")
                self.assertEqual(result["error"], {"type": "KeyboardInterrupt", "message": ""})
                self.assertFalse(result["observation_completed"])
                self.assertFalse(result["qualified"])
                self.assertFalse(result["r6_passed"])
                self.assertEqual(len(self.client.query_operations), 1)
                records = self.records(runner)
                anomaly = next(item for item in records if item["kind"] == "host_clock_anomaly")
                self.assertEqual(anomaly["stage"], "run_final_boundary")
                if not record_failure:
                    secondary = next(item for item in records
                                     if item["kind"] == "final_host_observation_failed_after_interruption")
                    self.assertEqual(secondary["error"]["type"], "RunInterrupted")
                    self.assertEqual(secondary["primary_error_type"], "KeyboardInterrupt")
                    self.assertFalse(secondary["changes_run_outcome"])

    def test_final_checked_sample_ends_observation_before_later_clock_changes(self):
        for mode in ("cycles-100", "soak-72h"):
            for fault in ("progress_gap", "monotonic_rollback", "wall_jump"):
                with self.subTest(mode=mode, after_boundary=fault):
                    self.args.mode = mode
                    runner = self.runner()
                    offset = [0]
                    runner.wall_clock = lambda: self.clock() + offset[0]
                    original = runner.observe_host
                    boundary = {}
                    def observe(stage, **kwargs):
                        value = original(stage, **kwargs)
                        if stage == "run_final_boundary":
                            boundary["returned"] = value
                            boundary["sample"] = runner.clock_sample_monotonic
                            if fault == "progress_gap":
                                self.clock.advance(20)
                            elif fault == "monotonic_rollback":
                                self.clock.advance(-200)
                            else:
                                offset[0] += 3600
                        return value
                    # 观察终点之后的注入不属于观察窗，不声称被检测。
                    with mock.patch.object(lifecycle, "SOAK_SECONDS", 1), \
                            mock.patch.object(runner, "observe_host", side_effect=observe):
                        result = runner.run()
                    self.assertEqual(boundary["returned"], boundary["sample"])
                    self.assertEqual(result["ended_monotonic_seconds"], boundary["sample"])
                    self.assertEqual(result["elapsed_seconds"],
                                     boundary["sample"] - result["started_monotonic_seconds"])
                    self.assertEqual(result["state"], "completed")
                    self.assertTrue(result["observation_completed"])
                    self.assertIsNone(result["error"])
                    self.assertFalse(result["qualified"])
                    self.assertFalse(result["r6_passed"])
                    self.assertEqual(result["maximum_clock_delta_discrepancy_seconds"], 0)
                    self.assertFalse(any(item["kind"] == "host_clock_anomaly" for item in self.records(runner)))

    def test_final_clock_failure_uses_raw_checked_sample_before_error_metadata(self):
        for mode in ("cycles-100", "soak-72h"):
            with self.subTest(mode=mode):
                self.args.mode = mode
                runner = self.runner()
                original = lifecycle.file_snapshot
                def snapshot(path, **kwargs):
                    value = original(path, **kwargs)
                    if Path(path) == self.args.capacity_log_file and kwargs.get("prefix_bytes") is not None:
                        self.clock.advance(-200)
                    return value
                original_record = runner.journal.record
                boundary = {}
                def record(kind, **value):
                    if kind == "interrupted":
                        boundary["sample"] = runner.clock_sample_monotonic
                        self.clock.advance(300)
                    original_record(kind, **value)
                with mock.patch.object(lifecycle, "SOAK_SECONDS", 1), \
                        mock.patch.object(lifecycle, "file_snapshot", side_effect=snapshot), \
                        mock.patch.object(runner.journal, "record", side_effect=record):
                    result = runner.run()
                self.assertEqual(result["ended_monotonic_seconds"], boundary["sample"])
                self.assertLess(result["elapsed_seconds"], 0)
                self.assertEqual(result["state"], "interrupted")
                self.assertFalse(result["observation_completed"])
                self.assertEqual(result["error"]["type"], "RunInterrupted")
                self.assertFalse(result["qualified"])
                self.assertFalse(result["r6_passed"])

    def test_final_sampling_failure_reports_last_recorded_host_sample(self):
        for mode in ("cycles-100", "soak-72h"):
            for clock_name in ("clock", "wall_clock"):
                for error_type in (OSError, KeyboardInterrupt):
                    with self.subTest(mode=mode, clock=clock_name, error=error_type.__name__):
                        self.args.mode = mode
                        runner = self.runner()
                        original = lifecycle.file_snapshot
                        original_clock = getattr(runner, clock_name)
                        boundary = {}
                        def snapshot(path, **kwargs):
                            value = original(path, **kwargs)
                            if Path(path) == self.args.capacity_log_file and kwargs.get("prefix_bytes") is not None:
                                boundary["sample"] = runner.clock_sample_monotonic
                                self.clock.advance(1)
                                boundary["fail"] = True
                            return value
                        def sample():
                            if boundary.get("fail"):
                                raise error_type("software final sample failure")
                            return original_clock()
                        sample.advance = self.clock.advance
                        # 最后一次采样未提交时，只能保留之前已记录的原始时刻。
                        with mock.patch.object(lifecycle, "SOAK_SECONDS", 1), \
                                mock.patch.object(lifecycle, "file_snapshot", side_effect=snapshot), \
                                mock.patch.object(runner, clock_name, new=sample):
                            result = runner.run()
                        self.assertEqual(result["ended_monotonic_seconds"], boundary["sample"])
                        self.assertEqual(result["observation_end_scope"], "last_recorded_host_clock_sample")
                        self.assertEqual(result["state"], "interrupted")
                        self.assertEqual(result["error"], {"type": error_type.__name__,
                                                           "message": "software final sample failure"})
                        self.assertFalse(result["observation_completed"])
                        self.assertFalse(result["qualified"])
                        self.assertFalse(result["r6_passed"])

    def test_bound_frp_checks_actual_hashed_bytes_before_any_write(self):
        client = lifecycle.BoundFrpClient("http://example.invalid", DEVICE, KEY)
        client.expected_image = {"sha256": "11" * 32, "size_bytes": 8192, "target": TARGET}
        with mock.patch.object(frp_ota.FrpClient, "command") as parent:
            with self.assertRaises(lifecycle.RunInterrupted):
                client.command("ota.start", parameters={"sha256": "22" * 32,
                    "image_size_bytes": 8192, "target": TARGET})
            parent.assert_not_called()


def reported(sequence=0, completed=None, *, boot=BOOT, digest=None, result=None):
    return json.dumps({"protocol_version": 1, "device_id": DEVICE, "boot_id": boot,
        "uptime_ms": 1000, "revision": 1, "wifi_state": "connected", "time_ready": True, "frp_state": "ready",
        "last_accepted_event_sequence": sequence, "last_completed_event_sequence": completed,
        "last_completed_event_sha256": digest, "last_event_outcome": "succeeded" if completed else "none",
        "last_business_result": result}).encode()


class PahoFixture:
    mode = "complete"
    instances = []

    def __init__(self, *args, **kwargs):
        if kwargs.get("reconnect_on_failure") is not False:
            raise AssertionError("不得自动重连")
        self.instances.append(self)
        self.published = []
        self.closed = False

    def username_pw_set(self, *args):
        pass

    def tls_set(self, **kwargs):
        self.ca = kwargs["ca_certs"]

    def connect(self, *args, **kwargs):
        pass

    def loop_start(self):
        self.on_connect(self, None, None, types.SimpleNamespace(is_failure=False), None)

    def message(self, topic, payload, retain=False):
        self.on_message(self, None, types.SimpleNamespace(topic=topic, payload=payload, qos=1, retain=retain))

    def subscribe(self, topic, qos):
        self.on_subscribe(self, None, 1, [types.SimpleNamespace(value=1)], None)
        self.message(topic, reported())
        return 0, 1

    def publish(self, topic, payload, qos, retain):
        self.published.append((topic, payload, qos, retain))
        digest = hashlib.sha256(lifecycle.MAX_EVENT).hexdigest()
        value = reported(1, 1, digest=digest, result=lifecycle.EVENT_INCREMENT)
        if self.mode == "wrong_boot":
            value = reported(1, 1, boot=str(uuid.uuid4()), digest=digest, result=lifecycle.EVENT_INCREMENT)
        elif self.mode == "wrong_count":
            value = reported(1, 1, digest=digest, result=1)
        if self.mode == "puback_only":
            self.on_disconnect(self, None, None, None, None)
        else:
            self.message(topic.removesuffix("/event") + "/reported", value, self.mode == "retained")
            if self.mode == "retained":
                self.on_disconnect(self, None, None, None, None)
        return types.SimpleNamespace(rc=0, is_published=lambda: True)

    def disconnect(self):
        self.closed = True

    def loop_stop(self):
        pass


class BusinessSessionTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        root = Path(self.directory.name)
        account = root / "account"
        account.write_text('{"username":"fixture","password":"public-fixture"}')
        account.chmod(0o600)
        self.args = types.SimpleNamespace(credentials_file=account, ca_file=root / "ca",
                                        mqtt_host="broker.example.invalid", mqtt_port=8883)
        module = types.ModuleType("paho.mqtt.client")
        module.Client = PahoFixture
        module.MQTTv311 = 4
        module.MQTT_ERR_SUCCESS = 0
        module.CallbackAPIVersion = types.SimpleNamespace(VERSION2=2)
        mqtt_module, paho_module = types.ModuleType("paho.mqtt"), types.ModuleType("paho")
        mqtt_module.client, paho_module.mqtt = module, mqtt_module
        patch = mock.patch.dict(sys.modules, {"paho": paho_module, "paho.mqtt": mqtt_module, "paho.mqtt.client": module})
        patch.start()
        self.addCleanup(patch.stop)
        PahoFixture.instances = []

    def test_real_helper_checks_complete_frame_and_exact_device_business_result(self):
        PahoFixture.mode = "complete"
        session = lifecycle.BusinessSession(self.args, DEVICE, BOOT, KEY)
        try:
            session.open()
            value = session.publish_maximum(0)
            self.assertEqual(value["business_result"], 3923)
            client = PahoFixture.instances[-1]
            self.assertEqual(client.connect_timeout, 5)
            self.assertEqual(client.ca, str(self.args.ca_file))
            self.assertEqual(len(client.published), 1)
            topic, payload, qos, retain = client.published[0]
            self.assertEqual((topic, len(payload), qos, retain), (f"esp-base/{DEVICE}/event", 4096, 1, False))
            self.assertEqual(payload, business_event.event_frame(KEY, DEVICE, BOOT, 1, lifecycle.MAX_EVENT))
        finally:
            session.close()
        self.assertTrue(client.closed)

    def test_puback_retained_wrong_boot_or_wrong_business_count_never_complete_or_resend(self):
        for mode in ("puback_only", "retained", "wrong_boot", "wrong_count"):
            with self.subTest(mode=mode):
                PahoFixture.mode = mode
                session = lifecycle.BusinessSession(self.args, DEVICE, BOOT, KEY)
                try:
                    session.open()
                    with self.assertRaises((ValueError, lifecycle.RunInterrupted)):
                        session.publish_maximum(0)
                finally:
                    session.close()
                self.assertEqual(len(PahoFixture.instances[-1].published), 1)
                self.assertTrue(PahoFixture.instances[-1].closed)


if __name__ == "__main__":
    unittest.main()
