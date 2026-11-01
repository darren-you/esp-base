#!/usr/bin/env python3
"""单设备有界业务负载；只发布 event，不提交 OTA 或操作串口。"""

from __future__ import annotations

import argparse
import base64
import contextlib
import fcntl
import hashlib
import importlib.metadata
import json
import os
from pathlib import Path
import queue
import secrets
import signal
import socket
import ssl
import stat
import tempfile
import time

import business_event as wire
import business_event_publish as single

DATA_BYTES = 3923
MAX_EVENTS = 4096
MAX_UNCONFIRMED = 128
MAX_INFLIGHT = 4
MAX_OUTPUT = 32 * 1024 * 1024
RECEIPT_TIMEOUT = 15.0
INT32_MAX = (1 << 31) - 1


class Rejected(ValueError):
    """Only fixed local reason codes, never broker or credential text."""


def require(condition, code):
    if not condition:
        raise Rejected(code)


def integer(value, low, high):
    return type(value) is int and low <= value <= high


def decode(raw):
    return json.loads(raw.decode("utf8"), object_pairs_hook=single.unique_object)


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


def event_bytes(run_id, sequence):
    seed = b"esp-base-business-load-v1\n" + run_id.encode("ascii") + sequence.to_bytes(8, "big")
    return b"\x01" + hashlib.shake_256(seed).digest(DATA_BYTES)


def validate_report(payload, device, boot):
    row = single.reported_message(payload, device, boot)
    require(integer(row["uptime_ms"], 0, wire.MAX_SEQUENCE)
            and integer(row["revision"], 0, (1 << 32) - 1)
            and row["wifi_state"] == "connected" and row["time_ready"] is True,
            "reported_not_ready")
    accepted, completed = row["last_accepted_event_sequence"], row["last_completed_event_sequence"]
    require((accepted == 0 and completed is None) or accepted == completed,
            "reported_prefix_not_closed")
    return row


def baseline(raw, device, boot, count):
    value = decode(raw)
    require(type(value) is dict and set(value) == {"business_status", "reported"}, "baseline_fields")
    status = value["business_status"]
    require(type(status) is dict and set(status) == {
        "protocol_version", "device_id", "boot_id", "request_id", "state", "error_code", "result"},
        "business_status_fields")
    wire.canonical_uuid(status["request_id"])
    require(type(status["protocol_version"]) is int and status["protocol_version"] == 1
            and status["device_id"] == device and status["boot_id"] == boot
            and status["state"] == "succeeded" and status["error_code"] is None, "business_status_identity")
    result = status["result"]
    require(type(result) is dict and set(result) == {"byte_count", "state", "window_deadline_uptime_ms"}
            and integer(result["byte_count"], 0, INT32_MAX)
            and result["state"] in {"idle", "active"}
            and integer(result["window_deadline_uptime_ms"], 0, wire.MAX_SEQUENCE), "business_status_result")
    row = validate_report(json.dumps(value["reported"]).encode(), device, boot)
    start = row["last_accepted_event_sequence"]
    require(start + count <= wire.MAX_SEQUENCE and result["byte_count"] + DATA_BYTES * count <= INT32_MAX,
            "counter_would_overflow")
    # Only admit a baseline whose last successful count-returning event agrees
    # with independent business.status. Sequence zero has never added bytes.
    require((start == 0 and result["byte_count"] == 0) or
            (start > 0 and row["last_event_outcome"] == "succeeded"
             and row["last_business_result"] == result["byte_count"]), "baseline_count_mismatch")
    return row, result["byte_count"]


class Ledger:
    """Accepted/completed count events; byte_count counts data excluding opcode."""

    def __init__(self, run_id, device, boot, before, count, limit):
        self.run_id, self.device, self.boot = run_id, device, boot
        self.before, self.count, self.limit = before, count, limit
        self.start = self.confirmed = before["last_accepted_event_sequence"]
        self.uptime = before["uptime_ms"]
        self.entries, self.mids = {}, {}
        self.fresh = False
        self.stopped = self.error = None

    @property
    def last(self):
        return self.start + len(self.entries)

    def stop(self, reason, *, unknown=False):
        if self.stopped is None:
            self.stopped = reason
        if unknown and self.error is None:
            self.error = reason

    def can_send(self):
        return (self.fresh and self.stopped is None and len(self.entries) < self.limit
                and self.last - self.confirmed < MAX_UNCONFIRMED
                and sum(not e["broker_acked"] for e in self.entries.values()) < MAX_INFLIGHT)

    def attempt(self, now):
        require(self.can_send(), "publication_not_admitted")
        sequence = self.last + 1
        self.entries[sequence] = {"sent_at": now, "mid": None, "broker_acked": False}
        return sequence

    def submitted(self, sequence, mid):
        require(integer(mid, 1, 65535) and mid not in self.mids, "publication_mid_conflict")
        self.entries[sequence]["mid"] = mid
        self.mids[mid] = sequence

    def ack(self, mid):
        require(mid in self.mids, "unexpected_puback")
        self.entries[self.mids[mid]]["broker_acked"] = True

    def reported(self, payload):
        require(self.error is None, "ledger_already_unknown")
        row = validate_report(payload, self.device, self.boot)
        require(row["revision"] == self.before["revision"] and row["uptime_ms"] >= self.uptime,
                "reported_revision_or_clock_changed")
        sequence = row["last_accepted_event_sequence"]
        require(self.confirmed <= sequence <= self.last, "reported_sequence_outside_prefix")
        if sequence == self.start:
            for key in ("last_completed_event_sequence", "last_completed_event_sha256",
                        "last_event_outcome", "last_business_result"):
                require(row[key] == self.before[key], "baseline_changed")
        else:
            require(row["last_event_outcome"] == "succeeded"
                    and row["last_business_result"] == self.count + DATA_BYTES * (sequence - self.start)
                    and row["last_completed_event_sha256"] == sha(event_bytes(self.run_id, sequence)),
                    "reported_count_or_digest_mismatch")
        self.fresh = True
        self.uptime, self.confirmed = row["uptime_ms"], sequence
        return row

    def expired(self, now):
        return any(now - e["sent_at"] >= RECEIPT_TIMEOUT
                   for seq, e in self.entries.items() if seq > self.confirmed or not e["broker_acked"])

    def settled(self):
        return self.last == self.confirmed and all(e["broker_acked"] for e in self.entries.values())

    def summary(self):
        return {"baseline_sequence": self.start, "attempted_count": len(self.entries),
                "confirmed_through_sequence": self.confirmed,
                "unknown_tail": None if self.last == self.confirmed else [self.confirmed + 1, self.last],
                "broker_acked_only": [s for s, e in self.entries.items() if s > self.confirmed and e["broker_acked"]],
                "broker_unacked": [s for s, e in self.entries.items() if not e["broker_acked"]],
                "stop_reason": self.stopped, "error": self.error,
                "all_attempts_reconciled": self.settled() and self.error is None,
                "requested_count_completed": len(self.entries) == self.limit and self.settled() and self.error is None,
                "per_event_receipts": False, "exclusive_writer_independently_proven": False,
                "device_connection_puback_proven": False, "r5_qualified": False}


class Evidence:
    def __init__(self, path):
        path.mkdir(mode=0o700)  # Never overwrite a previous run.
        self.path, self.bytes = path, 0
        (path / "frames").mkdir(mode=0o700)
        self.fd = os.open(path / "events.jsonl", os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        self.sync_directory(path)
        self.sync_directory(path.parent)

    @staticmethod
    def sync_directory(path):
        fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(fd)
        finally:
            os.close(fd)

    def budget(self, size, final=False):
        require(self.bytes + size <= MAX_OUTPUT - (0 if final else 65536), "evidence_limit")
        self.bytes += size

    def file(self, name, raw, *, final=False):
        self.budget(len(raw), final)
        path = self.path / name
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
        with os.fdopen(fd, "wb") as stream:
            stream.write(raw)
            stream.flush()
            os.fsync(stream.fileno())
        self.sync_directory(path.parent)

    def record(self, kind, **value):
        raw = (json.dumps({"kind": kind, "wall_time_ns": time.time_ns(),
                           "monotonic_ns": time.monotonic_ns(), **value}, separators=(",", ":")) + "\n").encode()
        self.budget(len(raw))
        with os.fdopen(os.dup(self.fd), "ab") as stream:
            stream.write(raw)
            stream.flush()
            os.fsync(stream.fileno())

    def close(self):
        os.close(self.fd)


@contextlib.contextmanager
def local_lease(device, directory=None):
    directory = directory or Path(tempfile.gettempdir()) / f"esp-base-business-load-{os.getuid()}"
    try:
        directory.mkdir(mode=0o700)
    except FileExistsError:
        pass
    facts = directory.lstat()
    require(stat.S_ISDIR(facts.st_mode) and facts.st_uid == os.getuid()
            and stat.S_IMODE(facts.st_mode) == 0o700, "lease_directory_rejected")
    fd = os.open(directory / (device + ".lock"), os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW, 0o600)
    try:
        facts = os.fstat(fd)
        require(stat.S_ISREG(facts.st_mode) and facts.st_uid == os.getuid()
                and facts.st_nlink == 1 and stat.S_IMODE(facts.st_mode) == 0o600, "lease_file_rejected")
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        yield
    finally:
        os.close(fd)  # Leave the inode in place: unlinking a flock file races waiters.


def identity(args):
    return {key: getattr(args, key) for key in ("run_id", "operation_id", "device_id", "boot_id")}


def gate(path, args, action):
    try:
        raw = single.private_file(path, 2048)
    except FileNotFoundError:
        return False
    value = decode(raw)
    require(type(value) is dict and type(value.get("schema_version")) is int
            and value == {"schema_version": 1, **identity(args), "action": action}, "gate_mismatch")
    return True


def close_client(client, abort):
    if abort:
        # No loop thread exists. Close the transport before another loop could
        # flush queued packets; do not reconnect or submit DISCONNECT behind them.
        sock = client.socket()
        if sock is not None:
            with contextlib.suppress(OSError):
                sock.shutdown(socket.SHUT_RDWR)
            with contextlib.suppress(OSError):
                sock.close()
    else:
        client.disconnect()


def run(args, mqtt, *, clock=time.monotonic):
    before_raw = single.private_file(args.baseline_file, 8192)
    before, count = baseline(before_raw, args.device_id, args.boot_id, args.events)
    # Consume the same exact file checks as the single-event publisher.
    key_hex = single.private_file(args.management_key_file, 65).removesuffix(b"\n")
    require(len(key_hex) == 64 and all(c in b"0123456789abcdef" for c in key_hex), "management_key_format")
    key = bytes.fromhex(key_hex.decode("ascii"))
    require(any(key), "empty_management_key")
    account = single.credentials(args.credentials_file)
    require(account["username"] == "esp_base_control_" + args.device_id, "controller_identity")
    ca = single.private_file(args.ca_file, 4096)
    ledger = Ledger(args.run_id, args.device_id, args.boot_id, before, count, args.events)
    with local_lease(args.device_id):
        evidence = Evidence(args.output)
        client = None
        interrupted = False
        previous_signals = {}
        alarm_armed = False
        try:
            for sig in (signal.SIGINT, signal.SIGTERM):
                previous_signals[sig] = signal.getsignal(sig)
                def interrupt(_signum, _frame):
                    nonlocal interrupted
                    interrupted = True
                signal.signal(sig, interrupt)
            evidence.file("baseline.json", before_raw)
            source = {name: sha(Path(__file__).with_name(name).read_bytes()) for name in
                      ("business_event_load.py", "business_event.py", "business_event_publish.py", "mqtt-lab-requirements.txt")}
            evidence.file("inputs.json", (json.dumps({"schema_version": 1, **identity(args),
                "baseline_sha256": sha(before_raw), "ca_sha256": sha(ca), "source_sha256": source,
                "broker_host": args.host, "broker_port": args.port,
                "events": args.events, "rate": args.rate, "duration_seconds": args.duration_seconds,
                "frame_bytes": 4096, "data_bytes": DATA_BYTES, "max_unconfirmed": MAX_UNCONFIRMED,
                "mqtt_inflight": MAX_INFLIGHT, "mqtt_queue": 8, "observer_queue": 256,
                "output_limit_bytes": MAX_OUTPUT, "receipt_timeout_seconds": RECEIPT_TIMEOUT,
                "paho_version": "2.1.0", "exclusive_business_writer_required": True}) + "\n").encode())
            events = queue.Queue(maxsize=256)
            callback_error = None
            def post(item):
                nonlocal callback_error
                try:
                    events.put_nowait(item)
                except queue.Full:
                    callback_error = "observer_queue_overflow"
            def message(_client, _userdata, value):
                nonlocal callback_error
                if value.topic != f"esp-base/{args.device_id}/reported" or len(value.payload) > 1024 or value.qos != 1:
                    callback_error = "reported_packet_rejected"
                elif not value.retain:
                    post(("reported", bytes(value.payload)))
            client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,
                client_id="base-load-" + secrets.token_hex(8), protocol=mqtt.MQTTv311,
                clean_session=True, reconnect_on_failure=False)
            client.max_inflight_messages_set(MAX_INFLIGHT)
            client.max_queued_messages_set(8)
            client.connect_timeout = 5.0
            client.username_pw_set(account["username"], account["password"])
            # Consume the verified bytes, not a second pathname read after hashing.
            client.tls_set_context(ssl.create_default_context(cadata=ca.decode("ascii")))
            client.on_connect = lambda c, u, f, reason, p: post(("connect", not reason.is_failure))
            client.on_subscribe = lambda c, u, mid, reasons, p: post(("subscribe", mid, [r.value for r in reasons]))
            client.on_publish = lambda c, u, mid, reason, p: post(("puback", mid, not reason.is_failure))
            client.on_disconnect = lambda *unused: post(("disconnect",))
            client.on_message = message
            started = clock()
            deadline = started + args.duration_seconds
            stage_deadline = min(started + 30, deadline - RECEIPT_TIMEOUT)
            subscribed, subscription, ready, active = False, None, False, False

            def check_deadlines():
                now = clock()
                require(active or now < stage_deadline, "readiness_or_gate_timeout")
                require(now < deadline, "runtime_deadline")
                require(not ledger.expired(now), "receipt_timeout")
                return now

            def arm_deadline():
                # Bound blocking connect/DNS and gate persistence by the current
                # stage, then by the overall window after the gate is accepted.
                check_deadlines()
                signal.setitimer(signal.ITIMER_REAL,
                                 max(0.000001, (deadline if active else stage_deadline) - clock()))

            # Paho's socket timeout does not bound a blocking system DNS call.
            # This standalone Mac/Linux tool also bounds its whole network phase.
            require(signal.getitimer(signal.ITIMER_REAL) == (0.0, 0.0), "existing_alarm_rejected")
            previous_signals[signal.SIGALRM] = signal.getsignal(signal.SIGALRM)
            def deadline_signal(_signum, _frame):
                raise Rejected("runtime_deadline" if active else "readiness_or_gate_timeout")
            signal.signal(signal.SIGALRM, deadline_signal)
            arm_deadline()
            alarm_armed = True
            require(client.connect(args.host, args.port, keepalive=30) == 0, "connect_submit_failed")
            check_deadlines()
            peer = client.socket()
            require(peer is not None and peer.version() in {"TLSv1.2", "TLSv1.3"}, "tls_peer_missing")
            certificate = peer.getpeercert(binary_form=True)
            require(type(certificate) is bytes and bool(certificate), "tls_certificate_missing")
            evidence.record("tls_peer", hostname=args.host, tls_version=peer.version(),
                            cipher=peer.cipher(), certificate_sha256=sha(certificate))
            next_send = started
            while clock() < deadline:
                if interrupted:
                    ledger.stop("interrupted", unknown=True)
                if callback_error:
                    ledger.stop(callback_error, unknown=True)
                if ledger.error:
                    break
                check_deadlines()
                code = client.loop(timeout=0.05)
                if callback_error:
                    ledger.stop(callback_error, unknown=True)
                    break
                check_deadlines()
                while not events.empty():
                    item = events.get_nowait()
                    # Conservatively require timely consumption, not merely a
                    # callback queued before expiry. A late receipt cannot erase
                    # an unknown tail or renew the original readiness deadline.
                    check_deadlines()
                    if item[0] == "connect":
                        require(item[1] and subscription is None, "connect_failed_or_repeated")
                        result, subscription = client.subscribe(f"esp-base/{args.device_id}/reported", qos=1)
                        require(result == 0, "subscribe_submit_failed")
                    elif item[0] == "subscribe":
                        require(item[1] == subscription and item[2] == [1], "suback_rejected")
                        subscribed = True
                    elif item[0] == "disconnect":
                        ledger.stop("disconnected", unknown=True)
                        break
                    elif item[0] == "puback":
                        require(item[2], "puback_rejected")
                        ledger.ack(item[1])
                        evidence.record("publisher_puback", mid=item[1])
                    elif item[0] == "reported":
                        require(subscribed, "reported_before_suback")
                        evidence.record("reported_received", payload_sha256=sha(item[1]),
                                        payload_base64=base64.b64encode(item[1]).decode("ascii"))
                        check_deadlines()
                        row = ledger.reported(item[1])
                        evidence.record("device_reported", value=row)
                if callback_error:
                    ledger.stop(callback_error, unknown=True)
                if code != 0:
                    ledger.stop("network_loop_failed", unknown=True)
                if interrupted:
                    ledger.stop("interrupted", unknown=True)
                if ledger.error:
                    break
                now = check_deadlines()
                if not ready and subscribed and ledger.fresh:
                    evidence.file("ready.json", (json.dumps({"schema_version": 1, **identity(args),
                        "ready": True, "baseline_sequence": ledger.start, "baseline_count": count,
                        "wall_time_ns": time.time_ns(), "monotonic_ns": time.monotonic_ns()}) + "\n").encode())
                    now = check_deadlines()
                    ready = True
                    stage_deadline = min(now + 30, deadline - RECEIPT_TIMEOUT)
                    arm_deadline()
                if gate(args.stop_gate, args, "stop"):
                    ledger.stop("stop_gate")
                if ready and not active and ledger.stopped is None and gate(args.start_gate, args, "start"):
                    check_deadlines()
                    active = True
                    arm_deadline()
                    evidence.record("start_gate", **identity(args))
                now = check_deadlines()
                if now >= deadline - RECEIPT_TIMEOUT:
                    ledger.stop("duration_limit")
                if ledger.error:
                    break
                if active and ledger.can_send() and now >= next_send:
                    sequence = ledger.last + 1
                    event = event_bytes(args.run_id, sequence)
                    frame = wire.event_frame(key, args.device_id, args.boot_id, sequence, event)
                    require(len(frame) == 4096, "frame_size")
                    evidence.file(f"frames/{sequence}.bin", frame)
                    evidence.record("publish_intent", sequence=sequence, frame_sha256=sha(frame), event_sha256=sha(event))
                    # Signals, a stop gate or the deadline can arrive during fsync.
                    if interrupted:
                        ledger.stop("interrupted", unknown=True)
                        break
                    if gate(args.stop_gate, args, "stop"):
                        ledger.stop("stop_gate")
                        continue
                    now = check_deadlines()
                    if now >= deadline - RECEIPT_TIMEOUT:
                        ledger.stop("duration_limit")
                        continue
                    ledger.attempt(now)  # Any following error leaves an explicitly unknown attempt.
                    info = client.publish(f"esp-base/{args.device_id}/event", frame, qos=1, retain=False)
                    require(info.rc == 0, "publish_submit_failed")
                    ledger.submitted(sequence, info.mid)
                    evidence.record("publish_submitted", sequence=sequence, mid=info.mid)
                    next_send = clock() + 1 / args.rate  # Never catch up with an unbounded burst.
                    if len(ledger.entries) == args.events:
                        ledger.stop("event_limit")
                if ledger.stopped and ledger.settled():
                    break
            if not ledger.stopped:
                ledger.stop("duration_limit")
            if not ledger.settled():
                ledger.stop("unresolved_at_stop", unknown=True)
        except (OSError, ValueError, RuntimeError, UnicodeError, KeyError, TypeError) as error:
            # Input values, broker text and credentials never become exception output.
            ledger.stop(str(error) if isinstance(error, Rejected) else "run_failed", unknown=True)
            with contextlib.suppress(OSError, ValueError):
                evidence.record("failure", error_type=type(error).__name__)
        finally:
            if alarm_armed:
                signal.setitimer(signal.ITIMER_REAL, 0)
            if client is not None:
                with contextlib.suppress(OSError, ValueError):
                    close_client(client, bool(ledger.error) or not ledger.settled())
            for sig, previous in previous_signals.items():
                signal.signal(sig, previous)
            summary = {"schema_version": 1, **identity(args), **ledger.summary(),
                       "uart_opened": False, "ota_submitted": False, "automatic_reconnect": False}
            try:
                evidence.file("summary.json", (json.dumps(summary, indent=2) + "\n").encode(), final=True)
            finally:
                evidence.close()
        return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("host", "device-id", "boot-id", "run-id", "operation-id"):
        parser.add_argument("--" + name, required=True)
    parser.add_argument("--port", type=int, default=8883)
    for name in ("ca-file", "credentials-file", "management-key-file", "baseline-file", "start-gate", "stop-gate", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--events", type=int, default=100)
    parser.add_argument("--rate", type=int, default=5)
    parser.add_argument("--duration-seconds", type=int, default=120)
    args = parser.parse_args()
    try:
        for key in ("device_id", "boot_id", "run_id", "operation_id"):
            wire.canonical_uuid(getattr(args, key))
        require(args.host and len(args.host) <= 253 and all(ord(c) >= 33 for c in args.host)
                and 1 <= args.port <= 65535 and 1 <= args.events <= MAX_EVENTS
                and 1 <= args.rate <= 20 and 30 <= args.duration_seconds <= 360, "limits")
        for name in ("ca_file", "credentials_file", "management_key_file", "baseline_file", "start_gate", "stop_gate", "output"):
            require(getattr(args, name).is_absolute(), "absolute_path_required")
        require(args.start_gate != args.stop_gate, "gates_must_differ")
        require(importlib.metadata.version("paho-mqtt") == "2.1.0", "paho_version")
        import paho.mqtt.client as mqtt
        result = run(args, mqtt)
        print(json.dumps(result, separators=(",", ":")))
        return 0 if result["all_attempts_reconciled"] and result["attempted_count"] else 2
    except (OSError, ValueError, RuntimeError, UnicodeError, KeyError, TypeError, ImportError, importlib.metadata.PackageNotFoundError) as error:
        print(json.dumps({"load_failed": True, "error_type": type(error).__name__}))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
