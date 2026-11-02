#!/usr/bin/env python3
"""只读核对实际 SDK 调用 envelope 内的 MQTT 共同样本；不授完整 R5。"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import stat

from analyze_flash_phases import UUID, require, MAX_BYTES, MAX_RECORDS, MAX_LINE, U32, I64

PHASES = ("inactive_erase", "ota_begin", "app_write_may_erase", "source_or_recovery_image_verify",
          "ota_end_verify", "boot_select_or_restore_api", "boot_confirm_api", "boot_invalidate_api",
          "intent_set_blob", "intent_commit", "failure_set_blob", "failure_commit",
          "success_set_blob", "success_commit", "prepare_image_verify", "select_image_verify")
FIELDS = {
    "ESP_BASE_FLASH_CONTEXT": "boot_id context_id operation_id request_id origin valid".split(),
    "ESP_BASE_FLASH_JOINT": ("boot_id context_id phase_id phase snapshot_started_us snapshot_finished_us valid "
        "begun completed sample_count call_id call_started_us call_finished_us sample_started_us sample_finished_us "
        "runtime_instance complete_owner_count complete_payload_bytes partial_declared_bytes partial_received_bytes "
        "owned_request_bytes outbox_wire_bytes owned_message_metadata_bytes observation_storage_bytes snapshot_copy_bytes").split(),
}
TEXT = {"boot_id", "operation_id", "request_id", "origin", "phase"}
SAMPLE = ("call_id call_started_us call_finished_us sample_started_us sample_finished_us runtime_instance "
          "complete_owner_count complete_payload_bytes partial_declared_bytes partial_received_bytes "
          "owned_request_bytes outbox_wire_bytes owned_message_metadata_bytes").split()


def parse(raw):
    if b"ESP_BASE_FLASH_CONTEXT" not in raw and b"ESP_BASE_FLASH_JOINT" not in raw:
        return None
    require(raw.endswith(b"\n") and len(raw) <= MAX_LINE, "共同观察行截断或过长")
    try:
        parts = raw.removesuffix(b"\n").removesuffix(b"\r").decode("ascii").split(" ")
    except UnicodeDecodeError as error:
        raise ValueError("共同观察行不是 ASCII") from error
    kind = parts[0]
    require(kind in FIELDS and len(parts) == len(FIELDS[kind]) + 1, "共同观察前缀、字段数错误")
    row = {"kind": kind}
    for field, part in zip(FIELDS[kind], parts[1:]):
        key, separator, value = part.partition("=")
        require(key == field and separator and value, "共同观察字段顺序、重复或缺失")
        if field in TEXT:
            row[field] = value
        else:
            require(re.fullmatch(r"0|[1-9][0-9]*", value), "共同观察整数不规范")
            row[field] = int(value)
            require(row[field] <= (I64 if field.endswith("_us") else U32), "共同观察整数越界")
    require(UUID.fullmatch(row["boot_id"]), "boot 不是 UUID v4")
    require(row["valid"] in (0, 1), "有效位错误")
    require(row["context_id"] > 0, "上下文未绑定")
    if kind == "ESP_BASE_FLASH_CONTEXT":
        require(UUID.fullmatch(row["operation_id"]), "操作不是 UUID v4")
        require(row["origin"] in ("request", "recovery"), "未知上下文来源")
        require(row["request_id"] == "none" if row["origin"] == "recovery" else
                UUID.fullmatch(row["request_id"]), "重启不能伪造原 request；请求上下文必须有原 ID")
        return row
    require(1 <= row["phase_id"] <= len(PHASES) and PHASES[row["phase_id"] - 1] == row["phase"], "阶段 ID/名称不符")
    require(row["snapshot_started_us"] <= row["snapshot_finished_us"], "快照时钟倒退")
    require(0 < row["begun"] and row["completed"] <= row["begun"], "调用计数不符")
    require(row["observation_storage_bytes"] >= row["snapshot_copy_bytes"] > 0 and
            row["snapshot_copy_bytes"] % 4 == 0, "观测存储大小不符")
    if not row["sample_count"]:
        require(all(row[k] == 0 for k in SAMPLE), "无样本却携带共同量")
        return row
    require(row["call_id"] > 0 and row["runtime_instance"] > 0, "样本缺真实调用或运行实例")
    require(row["call_started_us"] <= row["sample_started_us"] <= row["sample_finished_us"] <=
            row["snapshot_finished_us"], "样本不在已知调用及快照时间内")
    if row["call_finished_us"]:
        require(row["sample_finished_us"] <= row["call_finished_us"] <= row["snapshot_finished_us"] and
                row["completed"] > 0, "已结束调用时间或计数不符")
    require(row["complete_owner_count"] <= 3 and row["complete_payload_bytes"] <=
            row["complete_owner_count"] * 4096, "完成 owner 超过合法容量")
    require(row["complete_payload_bytes"] >= row["complete_owner_count"], "完成 owner 的 payload 不可能为空")
    require(0 < row["partial_received_bytes"] < row["partial_declared_bytes"] <= 4096, "并非真实未完成 DATA")
    metadata = row["owned_message_metadata_bytes"]
    require(0 < metadata <= 1024 and metadata % 4 == 0, "metadata 几何不符")
    require(row["owned_request_bytes"] == row["complete_payload_bytes"] + row["partial_declared_bytes"] +
            (row["complete_owner_count"] + 1) * metadata, "共同 owner 字节不能由独立峰相加")
    return row


def analyze(paths, boot_id, operation_id, request_id=None):
    require(UUID.fullmatch(boot_id) and UUID.fullmatch(operation_id), "须提供 boot 与 operation UUID v4")
    require(request_id is None or UUID.fullmatch(request_id), "request 必须为 UUID v4；重启恢复省略")
    require(0 < len(paths) <= 16, "须提供 1—16 个原件")
    records, inputs, identities, total = [], [], set(), 0
    for path in paths:
        fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK | getattr(os, "O_NOFOLLOW", 0))
        with os.fdopen(fd, "rb") as stream:
            before = os.fstat(stream.fileno())
            require(stat.S_ISREG(before.st_mode), "只接受非链接普通原件")
            identity = (before.st_dev, before.st_ino)
            require(identity not in identities, "同一原件重复输入")
            identities.add(identity)
            digest, size, number = hashlib.sha256(), 0, 0
            while raw := stream.readline(MAX_LINE + 1):
                number += 1; size += len(raw); total += len(raw)
                require(len(raw) <= MAX_LINE and raw.endswith(b"\n"), "原件含过长或截断行；不得裁剪拼接")
                require(total <= MAX_BYTES, "超过原件总大小上限")
                digest.update(raw)
                try:
                    row = parse(raw)
                    if row is None: continue
                    require(row["boot_id"] == boot_id, "跨 boot 样本拒绝")
                    require(len(records) < MAX_RECORDS, "共同观察记录过多")
                    records.append({"input_index": len(inputs), "line": number, **row})
                except ValueError as error:
                    raise ValueError(f"输入{len(inputs)}第{number}行：{error}") from error
            after = os.fstat(stream.fileno())
            signature = lambda s: (s.st_dev, s.st_ino, s.st_size, s.st_mtime_ns, s.st_ctime_ns)
            require(signature(before) == signature(after) == signature(os.stat(path, follow_symlinks=False)) and
                    size == before.st_size, "读取期间原件变化")
            inputs.append({"path": str(path), "bytes": size, "sha256": digest.hexdigest()})
    require(records, "没有共同观察记录")
    contexts, phases = {}, {}
    for index, row in enumerate(records):
        if row["kind"] != "ESP_BASE_FLASH_CONTEXT": continue
        require(row["context_id"] not in contexts, "上下文 ID 重用或重复映射")
        require(row["operation_id"] == operation_id and row["request_id"] == (request_id or "none") and
                row["origin"] == ("request" if request_id else "recovery"), "跨原操作/请求或来源上下文")
        contexts[row["context_id"]] = index
    require(contexts, "缺上下文映射")
    for index, row in enumerate(records):
        if row["kind"] != "ESP_BASE_FLASH_JOINT": continue
        require(row["context_id"] in contexts, "共同样本找不到原操作映射")
        require(records[contexts[row["context_id"]]]["valid"] >= row["valid"], "失效上下文恢复有效")
        phases.setdefault((row["context_id"], row["phase_id"]), []).append(index)
    results = []
    invalid_contexts = {row["context_id"] for row in records if not row["valid"]}
    call_ids = {}
    for (context_id, phase_id), indices in sorted(phases.items()):
        ordered = sorted(indices, key=lambda i: (records[i]["snapshot_started_us"], records[i]["snapshot_finished_us"]))
        previous = None
        for index in ordered:
            row = records[index]
            if previous and previous["snapshot_finished_us"] < row["snapshot_started_us"]:
                require(all(row[k] >= previous[k] for k in ("begun", "completed", "sample_count")) and
                        row["valid"] <= previous["valid"], "累计计数回退或观察恢复有效")
                if previous["sample_count"]:
                    require((row["owned_request_bytes"], row["outbox_wire_bytes"]) >=
                            (previous["owned_request_bytes"], previous["outbox_wire_bytes"]), "实际共同最优元组回退")
            previous = row
            if row["sample_count"]:
                identity = (context_id, phase_id, row["call_started_us"])
                require(row["call_id"] not in call_ids or call_ids[row["call_id"]] == identity, "call ID 复用到不同实际调用")
                call_ids[row["call_id"]] = identity
        observed = [i for i in ordered if context_id not in invalid_contexts and records[i]["valid"] and records[i]["sample_count"] and records[i]["call_finished_us"]]
        full = [i for i in observed if records[i]["complete_owner_count"] == 3 and
                records[i]["complete_payload_bytes"] == 12288 and records[i]["partial_declared_bytes"] == 4096]
        results.append({"context_id": context_id, "phase_id": phase_id, "phase": PHASES[phase_id-1],
            "record_indices": ordered, "completed_api_joint_sample_indices": observed,
            "full_owner_tuple_indices": full,
            "status": "uncertain" if context_id in invalid_contexts else "observed" if observed else "missing_joint_sample"})
    return {"schema_version": 1, "valid": True, "scope": "actual_sdk_call_envelope_and_fresh_mqtt_tuple",
        "boot_id": boot_id, "operation_id": operation_id, "request_id": request_id,
        "inputs": inputs, "records": records, "contexts": contexts, "phases": results,
        "invalid_context_ids": sorted(invalid_contexts),
        "candidate_verification_phase_ids": [5, 15, 16],
        "source_verification_cannot_qualify_candidate": True,
        "selector_target_or_restore_not_disambiguated": True,
        "structurally_excluded_joint_phases": ["boot_confirm_api", "success_set_blob", "success_commit"] if request_id is None else [],
        "limits": ["同存证明只针对同一真实 DATA 样本与稳定调用身份，不表示整个阶段持续满载",
            "API envelope 不等于内部 Flash 脉冲；app_write 可能擦除，end/select 可能同时验签",
            "缺样本不能证明该阶段不发生，也不能授予互斥阶段同时执行",
            "heap/stack 需独立完整容量帧历史下界，不是同一时刻的原子资源快照",
            "未证明业务帧授权/确认、outbox 最大合法负载、持久成功或跨 boot 收据"],
        "full_r5_qualified": False, "physical_flash_pulse_overlap_proven": False,
        "all_resource_atomic_snapshot_proven": False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--boot-id", required=True)
    parser.add_argument("--operation-id", required=True)
    parser.add_argument("--request-id", help="请求 boot 必填；恢复 boot 省略，原收据未持久化 request_id")
    parser.add_argument("--uart-log", action="append", type=Path, required=True)
    args = parser.parse_args()
    try:
        result = analyze(args.uart_log, args.boot_id, args.operation_id, args.request_id)
    except (OSError, ValueError) as error:
        print(json.dumps({"valid": False, "error": str(error)}, ensure_ascii=False)); return 2
    print(json.dumps(result, ensure_ascii=False, indent=2)); return 0


if __name__ == "__main__":
    raise SystemExit(main())
