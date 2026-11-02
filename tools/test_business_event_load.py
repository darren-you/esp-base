#!/usr/bin/env python3
"""无网络/串口的状态机与假 MQTT 客户端回归。"""

import copy
import hashlib
import json
from pathlib import Path
import tempfile
import time
import types
import unittest
from unittest import mock

import business_event_load as load
import business_event_publish as single

DEVICE = "22222222-2222-4222-8222-222222222222"
BOOT = "33333333-3333-4333-8333-333333333333"
RUN = "44444444-4444-4444-8444-444444444444"
OPERATION = "55555555-5555-4555-8555-555555555555"


def report(sequence=0, *, start=0, count=0, uptime=1000):
    return {"protocol_version": 1, "device_id": DEVICE, "boot_id": BOOT,
            "uptime_ms": uptime, "revision": 7, "wifi_state": "connected", "time_ready": True,
            "last_accepted_event_sequence": sequence,
            "last_completed_event_sequence": sequence or None,
            "last_completed_event_sha256": load.sha(load.event_bytes(RUN, sequence)) if sequence else None,
            "last_event_outcome": "succeeded" if sequence else "none",
            "last_business_result": count + load.DATA_BYTES * (sequence - start) if sequence else None}


def raw(value):
    return json.dumps(value).encode()


def initial(sequence=0, count=0):
    return {"business_status": {"protocol_version": 1, "device_id": DEVICE, "boot_id": BOOT,
            "request_id": OPERATION, "state": "succeeded", "error_code": None,
            "result": {"byte_count": count, "state": "idle", "window_deadline_uptime_ms": 0}},
            "reported": report(sequence, start=sequence, count=count)}


def private(path, value):
    path.write_bytes(value)
    path.chmod(0o600)


class Clock:
    def __init__(self): self.now = 0.0
    def __call__(self): return self.now


class FakeSocket:
    def __init__(self): self.closed = False
    def shutdown(self, _how): self.closed = True
    def close(self): self.closed = True
    def version(self): return "TLSv1.2"
    def cipher(self): return ("test-only-cipher", "TLSv1.2", 128)
    def getpeercert(self, binary_form): return b"test-only-certificate"


class FakeClient:
    def __init__(self, owner, *args, **kwargs):
        owner.assertEqual(kwargs["reconnect_on_failure"], False)
        owner.assertEqual(kwargs["clean_session"], True)
        owner.assertNotEqual(kwargs["client_id"], DEVICE)
        self.owner, self.pending = owner, []
        self.sock, self.connected, self.disconnected = FakeSocket(), False, False
        self.sent = []
        owner.client = self

    def max_inflight_messages_set(self, n): self.owner.assertEqual(n, 4)
    def max_queued_messages_set(self, n): self.owner.assertEqual(n, 8)
    def username_pw_set(self, *args): pass
    def tls_set_context(self, context): self.owner.assertIs(context, self.owner.tls_context)
    def connect(self, *args, **kwargs):
        if self.owner.mode == "slow_connect": self.owner.clock.now = 40
        return 0
    def socket(self): return self.sock
    def disconnect(self): self.disconnected = True

    def message(self, value):
        self.on_message(self, None, types.SimpleNamespace(
            topic=f"esp-base/{DEVICE}/reported", payload=raw(value), qos=1, retain=False))

    def subscribe(self, topic, qos):
        self.owner.assertEqual((topic, qos), (f"esp-base/{DEVICE}/reported", 1))
        self.on_subscribe(self, None, 1, [types.SimpleNamespace(value=1)], None)
        value = report()
        if self.owner.mode == "stale_baseline": value = report(1)
        self.message(value)
        return 0, 1

    def publish(self, topic, frame, qos, retain):
        self.owner.assertFalse(self.sock.closed)
        self.owner.assertEqual((topic, len(frame), qos, retain), (f"esp-base/{DEVICE}/event", 4096, 1, False))
        sequence = len(self.sent) + 1
        self.owner.assertEqual((self.owner.args.output / f"frames/{sequence}.bin").read_bytes(), frame)
        events = (self.owner.args.output / "events.jsonl").read_text()
        self.owner.assertIn('"kind":"publish_intent"', events)
        self.sent.append(frame)
        mid = sequence + 1
        self.pending.append((sequence, mid))
        if self.owner.mode == "stop_after_publish":
            private(self.owner.args.stop_gate, raw({"schema_version": 1, **load.identity(self.owner.args), "action": "stop"}))
        if self.owner.mode == "submit_raises":
            raise OSError("private simulated failure text must not be recorded")
        return types.SimpleNamespace(rc=0, mid=mid)

    def loop(self, timeout):
        self.owner.clock.now += timeout
        if not self.connected:
            self.connected = True
            self.on_connect(self, None, None, types.SimpleNamespace(is_failure=False), None)
        while self.pending:
            sequence, mid = self.pending.pop(0)
            mode = self.owner.mode
            if mode == "disconnect":
                self.on_disconnect(self, None, None, None, None)
                return 0
            if mode == "queue_overflow":
                for _ in range(257): self.message(report(sequence, uptime=1000 + sequence))
                return 0
            if mode == "late_receipts": self.owner.clock.now = 16
            value = report(sequence, uptime=1000 + sequence)
            if mode == "boot_changed": value["boot_id"] = RUN
            if mode == "bad_count": value["last_business_result"] += 1
            if mode == "no_progress": value = report(uptime=1000 + sequence)
            if mode == "reported_before_puback": self.message(value)
            if mode != "puback_missing":
                self.on_publish(self, None, mid, types.SimpleNamespace(is_failure=False), None)
            if mode != "reported_before_puback": self.message(value)
            if mode == "duplicate_old_report": self.message(report())
            if mode == "queued_receipts_consumed_late": self.owner.clock.now = 16
        return 0


class LedgerTests(unittest.TestCase):
    def ledger(self, sequence=0, count=0, limit=20):
        before, count = load.baseline(raw(initial(sequence, count)), DEVICE, BOOT, limit)
        state = load.Ledger(RUN, DEVICE, BOOT, before, count, limit, load.MAX_UNCONFIRMED)
        state.reported(raw(before))
        return state

    def test_maximum_frame_and_count_units(self):
        event = load.event_bytes(RUN, 1)
        self.assertEqual((event[0], len(event) - 1), (1, 3923))
        self.assertNotEqual(event, load.event_bytes(RUN, 2))
        frame = load.wire.event_frame(bytes(range(32)), DEVICE, BOOT, 1, event)
        self.assertEqual(len(frame), 4096)

    def test_nonzero_baseline_and_coalesced_prefix(self):
        state = self.ledger(9, 12345)
        for seq in (10, 11, 12):
            self.assertEqual(state.attempt(0), seq)
            state.submitted(seq, seq)
            state.ack(seq)
        state.reported(raw(report(12, start=9, count=12345, uptime=2000)))
        self.assertTrue(state.settled())
        self.assertEqual(state.summary()["confirmed_through_sequence"], 12)
        self.assertFalse(state.summary()["per_event_receipts"])
        self.assertFalse(state.summary()["exclusive_writer_independently_proven"])

    def test_known_old_report_does_not_regress_or_confirm_new_work(self):
        state = self.ledger()
        old = raw(report(uptime=1500))
        state.reported(old)
        state.attempt(0)
        state.submitted(1, 1)
        state.ack(1)
        state.reported(raw(report(1, uptime=2000)))
        state.attempt(1)
        state.submitted(2, 2)
        state.ack(2)
        self.assertIsNone(state.reported(old))
        self.assertEqual((state.uptime, state.confirmed), (2000, 1))
        self.assertTrue(state.expired(16))
        self.assertEqual(state.summary()["unknown_tail"], [2, 2])
        self.assertEqual(state.summary()["duplicate_reports_ignored"], 1)

    def test_unseen_stale_or_changed_report_still_rejected(self):
        state = self.ledger()
        state.reported(raw(report(uptime=2000)))
        for value in (report(uptime=1500), {**report(), "revision": 8}):
            with self.subTest(value=value), self.assertRaisesRegex(load.Rejected, "reported_revision_or_clock_changed"):
                state.reported(raw(value))
        self.assertEqual(state.duplicate_reports, 0)

    def test_duplicate_history_is_bounded_and_does_not_extend_on_replay(self):
        state = self.ledger()
        oldest = raw(report())
        for uptime in range(1001, 1001 + load.MAX_REPORT_HISTORY):
            state.reported(raw(report(uptime=uptime)))
        self.assertEqual(len(state.report_history), load.MAX_REPORT_HISTORY)
        with self.assertRaisesRegex(load.Rejected, "reported_revision_or_clock_changed"):
            state.reported(oldest)
        known = raw(report(uptime=1001))
        history = list(state.report_history)
        self.assertIsNone(state.reported(known))
        self.assertEqual(list(state.report_history), history)

    def test_sequence_alone_does_not_accept_business_failure(self):
        state = self.ledger()
        state.attempt(0)
        state.submitted(1, 2)
        for changes in ({"last_business_result": 1}, {"last_event_outcome": "business_failed", "last_business_result": -2},
                        {"last_completed_event_sha256": "ff" * 32}, {"last_completed_event_sequence": None},
                        {"last_accepted_event_sequence": 2}, {"revision": 8}, {"uptime_ms": 999}):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                state.reported(raw({**report(1, uptime=2000), **changes}))
        self.assertEqual(state.confirmed, 0)

    def test_overflow_and_paused_baselines_rejected(self):
        for value in (initial(0, 1), initial(load.wire.MAX_SEQUENCE, 1), initial(1, load.INT32_MAX)):
            with self.assertRaises(ValueError): load.baseline(raw(value), DEVICE, BOOT, 2)
        value = initial()
        value["business_status"]["result"]["state"] = "paused"
        with self.assertRaises(ValueError): load.baseline(raw(value), DEVICE, BOOT, 2)

    def test_broker_ack_does_not_confirm_and_no_progress_does_not_refresh_deadline(self):
        state = self.ledger()
        state.attempt(0)
        state.submitted(1, 2)
        state.ack(2)
        state.reported(raw(report(uptime=3000)))
        self.assertFalse(state.settled())
        self.assertTrue(state.expired(15))
        self.assertEqual(state.summary()["unknown_tail"], [1, 1])
        self.assertEqual(state.summary()["broker_acked_only"], [1])

    def test_device_confirmation_releases_configured_window_not_puback(self):
        before, count = load.baseline(raw(initial()), DEVICE, BOOT, 10)
        for window in (1, 2, 3):
            with self.subTest(window=window):
                state = load.Ledger(RUN, DEVICE, BOOT, before, count, 10, window)
                state.reported(raw(before))
                for seq in range(1, window + 1):
                    state.attempt(0)
                    state.submitted(seq, seq)
                    state.ack(seq)
                self.assertFalse(state.can_send())
                state.reported(raw(report(uptime=2000)))
                self.assertFalse(state.can_send())
                state.reported(raw(report(window, uptime=3000)))
                self.assertTrue(state.can_send())
                self.assertEqual(state.attempt(4), window + 1)

    def test_default_window_and_invalid_window(self):
        before, count = load.baseline(raw(initial()), DEVICE, BOOT, 10)
        state = load.Ledger(RUN, DEVICE, BOOT, before, count, 10)
        self.assertEqual(state.max_unconfirmed, 1)
        for value in (0, -1, 129, True, 1.0):
            with self.subTest(value=value), self.assertRaises(ValueError):
                load.Ledger(RUN, DEVICE, BOOT, before, count, 10, value)

    def test_independent_bounded_inflight_and_unconfirmed_windows(self):
        state = self.ledger(limit=200)
        for seq in range(1, 5):
            state.attempt(0)
            state.submitted(seq, seq)
        self.assertFalse(state.can_send())
        for seq in range(1, 5): state.ack(seq)
        for seq in range(5, 129):
            state.attempt(0)
            state.submitted(seq, seq)
            state.ack(seq)
        self.assertFalse(state.can_send())
        state.stop("unknown", unknown=True)
        with self.assertRaises(ValueError): state.attempt(1)
        with self.assertRaises(ValueError): state.reported(raw(report(128, uptime=3000)))


class RunnerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        root = Path(self.temp.name)
        self.clock, self.mode, self.client = Clock(), "normal", None
        self.tls_context = object()
        tls_patch = mock.patch.object(load.ssl, "create_default_context", return_value=self.tls_context)
        self.tls_factory = tls_patch.start()
        self.addCleanup(tls_patch.stop)
        self.args = types.SimpleNamespace(host="broker.example.test", port=8883, device_id=DEVICE, boot_id=BOOT,
            run_id=RUN, operation_id=OPERATION, ca_file=root / "ca.pem", credentials_file=root / "credentials.json",
            management_key_file=root / "key", baseline_file=root / "baseline.json", start_gate=root / "start.json",
            stop_gate=root / "stop.json", output=root / "evidence", events=3, rate=20, duration_seconds=30, max_unconfirmed=1)
        private(self.args.ca_file, b"test-only CA")
        private(self.args.credentials_file, raw({"username": "esp_base_control_" + DEVICE, "password": "private-test-password"}))
        private(self.args.management_key_file, bytes(range(32)).hex().encode())
        private(self.args.baseline_file, raw(initial()))
        private(self.args.start_gate, raw({"schema_version": 1, **load.identity(self.args), "action": "start"}))
        self.mqtt = types.SimpleNamespace(Client=lambda *a, **kw: FakeClient(self, *a, **kw),
            MQTTv311=4, CallbackAPIVersion=types.SimpleNamespace(VERSION2=2))
        actual_lease = load.local_lease
        patch = mock.patch.object(load, "local_lease", side_effect=lambda device: actual_lease(device, root / "leases"))
        patch.start()
        self.addCleanup(patch.stop)

    def run_fake(self):
        value = load.run(self.args, self.mqtt, clock=self.clock)
        self.assertEqual(json.loads((self.args.output / "summary.json").read_bytes()), value)
        files = [p for p in self.args.output.rglob("*") if p.is_file()]
        self.assertLessEqual(sum(p.stat().st_size for p in files), load.MAX_OUTPUT)
        self.assertTrue(all(p.stat().st_mode & 0o777 == 0o600 for p in files))
        self.assertNotIn(b"private-test-password", b"".join(p.read_bytes() for p in files))
        return value

    def test_normal_run_closes_actual_count_and_keeps_frames(self):
        value = self.run_fake()
        self.assertTrue(value["requested_count_completed"])
        self.assertEqual((value["attempted_count"], value["confirmed_through_sequence"]), (3, 3))
        self.assertIsNone(value["unknown_tail"])
        self.assertFalse(value["r5_qualified"])
        self.tls_factory.assert_called_once_with(cadata="test-only CA")

    def test_reported_before_publisher_puback_is_safe(self):
        self.mode = "reported_before_puback"
        self.assertTrue(self.run_fake()["requested_count_completed"])

    def test_qos1_old_duplicate_is_logged_without_interrupting_run(self):
        self.mode = "duplicate_old_report"
        value = self.run_fake()
        self.assertTrue(value["requested_count_completed"])
        self.assertEqual(value["duplicate_reports_ignored"], 3)
        records = [json.loads(line) for line in (self.args.output / "events.jsonl").read_bytes().splitlines()]
        self.assertEqual(sum(r["kind"] == "reported_duplicate_ignored" for r in records), 3)

    def test_stop_gate_drains_without_filling_remaining_count(self):
        self.mode = "stop_after_publish"
        value = self.run_fake()
        self.assertEqual((len(self.client.sent), value["stop_reason"]), (1, "stop_gate"))
        self.assertTrue(value["all_attempts_reconciled"])
        self.assertFalse(value["requested_count_completed"])

    def test_boot_change_aborts_transport_and_keeps_unknown_tail(self):
        self.mode = "boot_changed"
        value = self.run_fake()
        self.assertEqual(len(self.client.sent), 1)
        self.assertTrue(self.client.sock.closed)
        self.assertFalse(self.client.disconnected)
        self.assertEqual(value["unknown_tail"], [1, 1])
        records = [json.loads(line) for line in (self.args.output / "events.jsonl").read_bytes().splitlines()]
        received = [load.base64.b64decode(r["payload_base64"]) for r in records if r["kind"] == "reported_received"]
        self.assertEqual(json.loads(received[-1])["boot_id"], RUN)

    def test_disconnect_does_not_resend_or_reconnect(self):
        self.mode = "disconnect"
        value = self.run_fake()
        self.assertEqual((len(self.client.sent), value["error"]), (1, "disconnected"))
        self.assertTrue(self.client.sock.closed)

    def test_callback_overflow_freezes_prefix_without_more_publish(self):
        self.mode = "queue_overflow"
        value = self.run_fake()
        self.assertEqual((len(self.client.sent), value["confirmed_through_sequence"]), (1, 0))
        self.assertEqual(value["error"], "observer_queue_overflow")

    def test_stale_baseline_or_wrong_gate_prevents_any_publish(self):
        self.mode = "stale_baseline"
        value = self.run_fake()
        self.assertEqual(value["attempted_count"], 0)
        self.assertEqual(self.client.sent, [])

    def test_gate_identity_mismatch_prevents_publish(self):
        private(self.args.start_gate, raw({"schema_version": 1, **load.identity(self.args), "operation_id": BOOT, "action": "start"}))
        value = self.run_fake()
        self.assertEqual((value["attempted_count"], value["error"]), (0, "gate_mismatch"))

    def test_gate_boolean_schema_rejected(self):
        private(self.args.start_gate, raw({"schema_version": True, **load.identity(self.args), "action": "start"}))
        with self.assertRaises(ValueError): load.gate(self.args.start_gate, self.args, "start")

    def test_stop_arriving_during_intent_persistence_prevents_publish(self):
        original = load.Evidence.record
        def stop_at_intent(evidence, kind, **value):
            original(evidence, kind, **value)
            if kind == "publish_intent":
                private(self.args.stop_gate, raw({"schema_version": 1, **load.identity(self.args), "action": "stop"}))
        with mock.patch.object(load.Evidence, "record", stop_at_intent):
            result = self.run_fake()
        self.assertEqual(result["attempted_count"], 0)
        self.assertEqual(self.client.sent, [])

    def test_deadline_during_intent_persistence_prevents_publish(self):
        original = load.Evidence.record
        def delay_at_intent(evidence, kind, **value):
            original(evidence, kind, **value)
            if kind == "publish_intent": self.clock.now = 20
        with mock.patch.object(load.Evidence, "record", delay_at_intent):
            result = self.run_fake()
        self.assertEqual((result["attempted_count"], result["stop_reason"]), (0, "duration_limit"))
        self.assertEqual(self.client.sent, [])

    def test_interrupt_during_intent_persistence_prevents_publish(self):
        original = load.Evidence.record
        def interrupt_at_intent(evidence, kind, **value):
            original(evidence, kind, **value)
            if kind == "publish_intent": load.signal.raise_signal(load.signal.SIGINT)
        with mock.patch.object(load.Evidence, "record", interrupt_at_intent):
            result = self.run_fake()
        self.assertEqual((result["attempted_count"], result["error"]), (0, "interrupted"))
        self.assertEqual(self.client.sent, [])

    def test_existing_alarm_is_not_overridden(self):
        with mock.patch.object(load.signal, "getitimer", return_value=(7.0, 0.0)), \
             mock.patch.object(load.signal, "setitimer") as timer:
            result = self.run_fake()
        timer.assert_not_called()
        self.assertEqual((result["attempted_count"], result["error"]), (0, "existing_alarm_rejected"))

    def test_missing_start_gate_is_bounded_and_does_not_publish(self):
        self.args.start_gate.unlink()
        result = self.run_fake()
        self.assertEqual((result["attempted_count"], result["error"]), (0, "readiness_or_gate_timeout"))
        self.assertLessEqual(self.clock.now, 30)

    def test_connect_returning_after_original_stage_deadline_cannot_publish(self):
        self.args.duration_seconds = 120
        self.mode = "slow_connect"
        result = self.run_fake()
        self.assertEqual((self.clock.now, result["attempted_count"], result["error"]),
                         (40, 0, "readiness_or_gate_timeout"))
        self.assertEqual(self.client.sent, [])
        self.assertFalse((self.args.output / "ready.json").exists())
        self.assertTrue(self.client.sock.closed)

    def test_blocking_connect_is_interrupted_by_stage_alarm(self):
        # Compress only the no-send tail to put the real stage alarm 50 ms away;
        # this invokes the actual POSIX timer/handler while connect is blocked.
        entered, returned = [], []
        def blocked_connect(client, *args, **kwargs):
            entered.append(True)
            time.sleep(1)
            returned.append(True)
            return 0
        with mock.patch.object(load, "RECEIPT_TIMEOUT", 29.95), \
             mock.patch.object(FakeClient, "connect", blocked_connect):
            result = load.run(self.args, self.mqtt, clock=time.monotonic)
        self.assertEqual((entered, returned, self.client.sent), ([True], [], []))
        self.assertEqual(result["error"], "readiness_or_gate_timeout")
        self.assertTrue(self.client.sock.closed)

    def test_fresh_report_cannot_renew_expired_readiness_stage(self):
        self.args.duration_seconds = 120
        original = FakeClient.subscribe
        def late_subscribe(client, topic, qos):
            self.clock.now = 40
            return original(client, topic, qos)
        with mock.patch.object(FakeClient, "subscribe", late_subscribe):
            result = self.run_fake()
        self.assertEqual((result["attempted_count"], result["error"]),
                         (0, "readiness_or_gate_timeout"))
        self.assertFalse((self.args.output / "ready.json").exists())

    def test_start_gate_read_cannot_bypass_expired_stage(self):
        self.args.duration_seconds = 120
        original = load.gate
        def late_gate(path, args, action):
            found = original(path, args, action)
            if action == "start": self.clock.now = 40
            return found
        with mock.patch.object(load, "gate", late_gate):
            result = self.run_fake()
        self.assertEqual((result["attempted_count"], result["error"]),
                         (0, "readiness_or_gate_timeout"))
        self.assertEqual(self.client.sent, [])

    def test_late_receipts_cannot_clear_the_expired_attempt(self):
        self.args.duration_seconds = 120
        self.mode = "late_receipts"
        result = self.run_fake()
        self.assertEqual((len(self.client.sent), result["error"]), (1, "receipt_timeout"))
        self.assertEqual((result["confirmed_through_sequence"], result["unknown_tail"]), (0, [1, 1]))
        self.assertEqual(result["broker_unacked"], [1])
        self.assertTrue(self.client.sock.closed)

    def test_queued_timely_receipts_are_conservatively_rejected_if_consumed_late(self):
        self.args.duration_seconds = 120
        self.mode = "queued_receipts_consumed_late"
        result = self.run_fake()
        self.assertEqual((len(self.client.sent), result["error"]), (1, "receipt_timeout"))
        self.assertEqual(result["unknown_tail"], [1, 1])

    def test_expiry_during_second_intent_persistence_does_not_expand_unknown_tail(self):
        self.args.max_unconfirmed = 3
        self.args.duration_seconds = 120
        self.mode = "no_progress"
        original = load.Evidence.record
        def delayed_second_intent(evidence, kind, **value):
            original(evidence, kind, **value)
            if kind == "publish_intent" and value["sequence"] == 2: self.clock.now = 16
        with mock.patch.object(load.Evidence, "record", delayed_second_intent):
            result = self.run_fake()
        self.assertEqual((len(self.client.sent), result["error"]), (1, "receipt_timeout"))
        self.assertEqual((result["attempted_count"], result["unknown_tail"]), (1, [1, 1]))
        self.assertTrue((self.args.output / "frames/2.bin").exists())
        self.assertTrue(self.client.sock.closed)

    def test_expiry_while_persisting_report_does_not_confirm_prefix(self):
        self.args.duration_seconds = 120
        original = load.Evidence.record
        def delayed_report(evidence, kind, **value):
            original(evidence, kind, **value)
            if kind == "reported_received" and self.client.sent: self.clock.now = 16
        with mock.patch.object(load.Evidence, "record", delayed_report):
            result = self.run_fake()
        self.assertEqual((len(self.client.sent), result["error"]), (1, "receipt_timeout"))
        self.assertEqual(result["unknown_tail"], [1, 1])

    def test_publish_exception_is_unknown_without_leaking_text(self):
        self.mode = "submit_raises"
        value = self.run_fake()
        self.assertEqual(value["unknown_tail"], [1, 1])
        self.assertEqual(len(self.client.sent), 1)
        self.assertNotIn("private simulated", (self.args.output / "events.jsonl").read_text())

    def test_ack_missing_is_not_full_success_despite_device_result(self):
        self.mode = "puback_missing"
        value = self.run_fake()
        self.assertFalse(value["all_attempts_reconciled"])
        self.assertEqual(value["broker_unacked"], [1, 2, 3])

    def test_default_window_stops_after_one_unconfirmed_event(self):
        self.mode = "no_progress"
        value = self.run_fake()
        self.assertEqual(value["unknown_tail"], [1, 1])
        self.assertEqual(value["attempted_count"], 1)
        self.assertEqual(json.loads((self.args.output / "inputs.json").read_bytes())["max_unconfirmed"], 1)

    def test_repeated_no_progress_does_not_extend_attempt_lifetime(self):
        self.args.max_unconfirmed = 3
        self.mode = "no_progress"
        value = self.run_fake()
        self.assertFalse(value["all_attempts_reconciled"])
        self.assertEqual(value["unknown_tail"], [1, 3])

    def test_input_symlink_and_existing_output_rejected_before_network(self):
        original = self.args.baseline_file
        original.rename(original.with_suffix(".saved"))
        original.symlink_to(original.with_suffix(".saved"))
        with self.assertRaises(OSError): load.run(self.args, self.mqtt, clock=self.clock)
        self.assertIsNone(self.client)

    def test_existing_output_never_reused(self):
        self.args.output.mkdir()
        with self.assertRaises(FileExistsError): load.run(self.args, self.mqtt, clock=self.clock)
        self.assertIsNone(self.client)


class FilesystemTests(unittest.TestCase):
    def test_local_lease_excludes_another_loader_but_not_a_global_claim(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder) / "lease"
            with load.local_lease(DEVICE, root):
                with self.assertRaises(BlockingIOError):
                    with load.local_lease(DEVICE, root): pass
            with load.local_lease(DEVICE, root): pass

    def test_evidence_limit_reserves_summary_space(self):
        with tempfile.TemporaryDirectory() as folder:
            evidence = load.Evidence(Path(folder) / "output")
            try:
                evidence.bytes = load.MAX_OUTPUT - 65536
                with self.assertRaises(ValueError): evidence.record("not_written")
                evidence.file("summary.json", b"{}\n", final=True)
            finally:
                evidence.close()


if __name__ == "__main__":
    unittest.main()
