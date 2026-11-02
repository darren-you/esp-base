#!/usr/bin/env python3
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import analyze_flash_joint as parser

BOOT = "11111111-1111-4111-8111-111111111111"
OP = "22222222-2222-4222-8222-222222222222"
REQ = "33333333-3333-4333-8333-333333333333"
OTHER = "44444444-4444-4444-8444-444444444444"


def context(**changes):
    return dict(kind="ESP_BASE_FLASH_CONTEXT", boot_id=BOOT, context_id=1,
                operation_id=OP, request_id=REQ, origin="request", valid=1) | changes


def joint(**changes):
    return dict(kind="ESP_BASE_FLASH_JOINT", boot_id=BOOT, context_id=1,
        phase_id=3, phase="app_write_may_erase", snapshot_started_us=190,
        snapshot_finished_us=200, valid=1, begun=2, completed=2, sample_count=1,
        call_id=2, call_started_us=100, call_finished_us=180,
        sample_started_us=120, sample_finished_us=130, runtime_instance=1,
        complete_owner_count=3, complete_payload_bytes=12288,
        partial_declared_bytes=4096, partial_received_bytes=966,
        owned_request_bytes=17472, outbox_wire_bytes=120,
        owned_message_metadata_bytes=272, observation_storage_bytes=1600,
        snapshot_copy_bytes=96) | changes


def line(row):
    return (row["kind"] + " " + " ".join(f"{k}={row[k]}" for k in parser.FIELDS[row["kind"]]) + "\n").encode()


class JointParserTest(unittest.TestCase):
    def analyze(self, rows, request=REQ):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "raw.log"
            path.write_bytes(b"unrelated application line\n" + b"".join(map(line, rows)))
            return parser.analyze([path], BOOT, OP, request)

    def test_actual_tuple_and_scope(self):
        result = self.analyze([context(), joint()])
        self.assertEqual(result["phases"][0]["full_owner_tuple_indices"], [1])
        self.assertFalse(result["full_r5_qualified"])
        self.assertFalse(result["physical_flash_pulse_overlap_proven"])
        self.assertFalse(result["all_resource_atomic_snapshot_proven"])

    def test_no_sample_and_unfinished_are_missing(self):
        empty = joint(sample_count=0, **{k: 0 for k in parser.SAMPLE})
        result = self.analyze([context(), empty])
        self.assertEqual(result["phases"][0]["status"], "missing_joint_sample")
        result = self.analyze([context(), joint(completed=1, call_finished_us=0)])
        self.assertFalse(result["phases"][0]["completed_api_joint_sample_indices"])

    def test_partial_three_owner_is_not_full(self):
        result = self.analyze([context(), joint(complete_owner_count=2,
            complete_payload_bytes=8192, owned_request_bytes=13104)])
        self.assertTrue(result["phases"][0]["completed_api_joint_sample_indices"])
        self.assertFalse(result["phases"][0]["full_owner_tuple_indices"])

    def test_request_recovery_separate(self):
        recovery = context(origin="recovery", request_id="none")
        self.analyze([recovery, joint()], request=None)
        for rows, request in (([recovery,joint()],REQ),([context(),joint()],None),
                              ([context(origin="recovery"),joint()],None)):
            with self.assertRaises(ValueError): self.analyze(rows,request)

    def test_bad_geometry_or_boundary(self):
        for changes in (dict(partial_received_bytes=4096), dict(owned_request_bytes=17473),
                        dict(complete_payload_bytes=12289), dict(complete_owner_count=4),
                        dict(call_started_us=121), dict(call_finished_us=129),
                        dict(sample_finished_us=201), dict(runtime_instance=0),
                        dict(phase_id=4), dict(sample_count=0), dict(valid=2)):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                self.analyze([context(),joint(**changes)])

    def test_context_identity_and_call_reuse(self):
        for rows in ([joint()], [context(),context(),joint()],
                     [context(operation_id=OTHER),joint()], [context(),joint(boot_id=OTHER)],
                     [context(),joint(context_id=2)], [context(valid=0),joint()],
                     [context(),joint(),joint(phase_id=4,phase="source_or_recovery_image_verify")]):
            with self.subTest(rows=rows), self.assertRaises(ValueError): self.analyze(rows)

    def test_nonoverlap_history_cannot_recover_or_mix_maxima(self):
        later = dict(snapshot_started_us=210,snapshot_finished_us=220)
        for old,new in ((joint(valid=0),joint(**later)),
                        (joint(),joint(**later,begun=1,completed=1)),
                        (joint(),joint(**later,owned_request_bytes=13104,
                                       complete_owner_count=2,complete_payload_bytes=8192,outbox_wire_bytes=999))):
            with self.assertRaises(ValueError): self.analyze([context(),old,new])

    def test_source_verify_peak_is_not_candidate_verify(self):
        result=self.analyze([context(),joint(phase_id=4,phase="source_or_recovery_image_verify")])
        self.assertEqual(result["candidate_verification_phase_ids"],[5,15,16])
        self.assertNotIn(4,result["candidate_verification_phase_ids"])
        self.assertEqual([p["phase_id"] for p in result["phases"]],[4])
        self.assertTrue(result["source_verification_cannot_qualify_candidate"])

    def test_later_invalid_history_does_not_qualify_earlier_peak(self):
        result=self.analyze([context(),joint(),joint(valid=0,snapshot_started_us=210,snapshot_finished_us=220)])
        self.assertEqual(result["phases"][0]["status"],"uncertain")
        self.assertFalse(result["phases"][0]["completed_api_joint_sample_indices"])

    def test_format_and_raw_files_fail_closed(self):
        for raw in (line(joint()).replace(b"valid=1",b"valid=01"),line(joint())[:-1],
                    b"prefix "+line(joint()),line(joint()).replace(b"phase_id=3",b"phase_id=-3")):
            with self.assertRaises(ValueError): parser.parse(raw)
        with tempfile.TemporaryDirectory() as temporary:
            path=Path(temporary)/"raw"; path.write_bytes(line(context())+line(joint()))
            link=Path(temporary)/"link"; link.symlink_to(path)
            with self.assertRaises((ValueError,OSError)): parser.analyze([link],BOOT,OP,REQ)
            with self.assertRaises(ValueError): parser.analyze([path,path],BOOT,OP,REQ)
            path.write_bytes(path.read_bytes()+b"partial")
            with self.assertRaises(ValueError): parser.analyze([path],BOOT,OP,REQ)


if __name__ == "__main__": unittest.main()
