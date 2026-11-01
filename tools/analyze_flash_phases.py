#!/usr/bin/env python3
"""只读关联正式 Flash 聚合与 OTA 外层阶段；不授 R5／R6 或断电资格。"""
from __future__ import annotations

import argparse
import hashlib
import heapq
import json
import os
from pathlib import Path
import re
import stat

UUID = re.compile(r"[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}")
U32 = (1 << 32) - 1
I64 = (1 << 63) - 1
MAX_BYTES = 256 * 1024 * 1024
MAX_RECORDS = 100000
MAX_LINE = 16384
KINDS = {
    "ESP_BASE_FLASH_IO": ("boot_id snapshot_started_us snapshot_finished_us consumer "
        "completed_claim_count acquire_failed_count release_failed_count max_wait_us "
        "max_held_upper_bound_us counters_valid aggregate_storage_bytes snapshot_copy_bytes").split(),
    "ESP_BASE_FLASH_IO_OUTPUT": "boot_id started_us finished_us scope".split(),
    "ESP_BASE_OTA_PHASE": ("boot_id operation_id request_id phase boundary snapshot_started_us "
        "snapshot_finished_us result stage_uncertain ota_completed_claim_count "
        "config_completed_claim_count ota_counters_valid config_counters_valid").split(),
}
TEXT = {"boot_id", "operation_id", "request_id", "consumer", "scope", "phase", "boundary", "result"}
BOOL = {"counters_valid", "stage_uncertain", "ota_counters_valid", "config_counters_valid"}
TIMES = {"snapshot_started_us", "snapshot_finished_us", "started_us", "finished_us"}
OUTER = ("retire_inactive", "prepare", "select", "retire_cleanup")
TERMINAL_STAGES = (*OUTER[:3], "receipt_load", "begin_mqtt_stream", "mqtt_selection_boundary")
RESULTS = {"ok", "unknown", "ota_signing_unavailable", "invalid_request", "ota_slot_unavailable",
           "ota_image_too_large", "ota_wrong_target", "ota_download_failed", "ota_hash_mismatch",
           "ota_signature_invalid", "ota_boot_state_unknown", "resource_failure", "ota_image_invalid"}
COUNTERS = ("completed_claim_count", "acquire_failed_count", "release_failed_count",
            "max_wait_us", "max_held_upper_bound_us")


def require(condition, message):
    if not condition:
        raise ValueError(message)


def parse(raw):
    if b"ESP_BASE_FLASH_" not in raw and b"ESP_BASE_OTA_PHASE" not in raw:
        return None
    require(raw.endswith(b"\n") and len(raw) <= MAX_LINE, "观察行截断或过长")
    try:
        parts = raw.removesuffix(b"\n").removesuffix(b"\r").decode("ascii").split(" ")
    except UnicodeDecodeError as error:
        raise ValueError("观察行不是 ASCII") from error
    kind = parts[0]
    require(kind in KINDS and len(parts) == len(KINDS[kind]) + 1, "观察行前缀、类型或字段数不符")
    row = {}
    for field, part in zip(KINDS[kind], parts[1:]):
        key, separator, value = part.partition("=")
        require(key == field and separator and value, "观察字段缺失、重复或顺序错误")
        if field in TEXT:
            row[field] = value
        else:
            require(re.fullmatch(r"0|[1-9][0-9]*", value) is not None, "观察数值不是规范非负整数")
            number = int(value)
            require(number <= (I64 if field in TIMES else U32), "观察数值超出 producer 整数范围")
            require(field not in BOOL or number in (0, 1), "观察布尔值错误")
            row[field] = number
    require(UUID.fullmatch(row["boot_id"]) is not None, "boot 不是规范 UUID v4")
    if kind == "ESP_BASE_FLASH_IO_OUTPUT":
        require(row["scope"] == "snapshot_and_two_rows_excluding_this_line", "输出成本范围未知")
        require(row["started_us"] <= row["finished_us"], "输出时钟倒退")
    else:
        require(row["snapshot_started_us"] <= row["snapshot_finished_us"], "快照时钟倒退")
    if kind == "ESP_BASE_FLASH_IO":
        require(row["consumer"] in ("ota", "config"), "未知 Flash consumer")
        require(row["snapshot_copy_bytes"] == 48 and row["aggregate_storage_bytes"] >= 48
                and row["aggregate_storage_bytes"] % 4 == 0, "Flash 聚合大小不符")
        require(not row["release_failed_count"] or not row["counters_valid"], "失败 release 却报告有效历史")
        require(row["completed_claim_count"] or not row["max_held_upper_bound_us"], "无完成 claim 却报告占用最大值")
    if kind == "ESP_BASE_OTA_PHASE":
        require(all(UUID.fullmatch(row[k]) for k in ("operation_id", "request_id")), "操作身份不是规范 UUID v4")
        require(row["boundary"] in ("begin", "end", "terminal"), "未知阶段边界")
        require(row["phase"] in (TERMINAL_STAGES if row["boundary"] == "terminal" else OUTER), "未知外层阶段")
        require(row["result"] == "pending" if row["boundary"] == "begin" else row["result"] in RESULTS,
                "阶段结果不符")
        require(row["boundary"] == "terminal" or row["stage_uncertain"] == 0, "非 terminal 阶段不确定标记不符")
        require(row["boundary"] != "terminal" or row["result"] != "ok" or
                (row["phase"] == "select" and row["stage_uncertain"] == 0), "成功 terminal 与真实阶段不符")
    return kind, row


def comparable_history(observations):
    """并发打印不是时间顺序；只比较可证明先后、互不重叠的快照。"""
    pending, maxima, invalid, overlaps = [], {}, False, 0
    for number, item in enumerate(sorted(observations, key=lambda r: (r["start"], r["end"]))):
        while pending and pending[0][0] < item["start"]:
            _, _, previous = heapq.heappop(pending)
            for key, value in previous["values"].items():
                maxima[key] = max(maxima.get(key, 0), value)
            invalid |= previous["valid"] == 0
        require(not invalid or item["valid"] == 0, "Flash 失效历史恢复为有效")
        for key, value in item["values"].items():
            require(value >= maxima.get(key, 0), "可确定先后的 Flash 累计值回退")
        overlaps += bool(pending)
        heapq.heappush(pending, (item["end"], number, item))
    return overlaps


def analyze(paths, boot_id, operation_id, request_id):
    require(all(UUID.fullmatch(x) for x in (boot_id, operation_id, request_id)), "须提供三个规范 UUID v4")
    require(0 < len(paths) <= 16, "须提供 1—16 个原始日志")
    records, inputs, identities, total_bytes = [], [], set(), 0
    for path in paths:
        fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK | getattr(os, "O_NOFOLLOW", 0))
        with os.fdopen(fd, "rb") as stream:
            before = os.fstat(stream.fileno())
            require(stat.S_ISREG(before.st_mode), "UART 日志必须是非链接普通文件")
            identity = (before.st_dev, before.st_ino)
            require(identity not in identities, "同一原始文件被重复输入")
            identities.add(identity)
            digest, size, line_number = hashlib.sha256(), 0, 0
            while raw := stream.readline(MAX_LINE + 1):
                line_number += 1
                size += len(raw)
                total_bytes += len(raw)
                require(len(raw) <= MAX_LINE and raw.endswith(b"\n"), "日志存在过长或截断行；不裁剪或拼接")
                require(total_bytes <= MAX_BYTES, "日志超过 256 MiB 总上限")
                digest.update(raw)
                try:
                    parsed = parse(raw)
                    if parsed is None:
                        continue
                    kind, row = parsed
                    require(row["boot_id"] == boot_id, "跨 boot 观察拒绝；不得拼接资格")
                    if kind == "ESP_BASE_OTA_PHASE":
                        require(row["operation_id"] == operation_id and row["request_id"] == request_id,
                                "跨 operation/request 观察拒绝")
                    require(len(records) < MAX_RECORDS, "观察记录超过十万条上限")
                    records.append({"kind": kind, "input_index": len(inputs), "line": line_number, **row})
                except ValueError as error:
                    raise ValueError(f"输入 {len(inputs)} 第 {line_number} 行：{error}") from error
            after = os.fstat(stream.fileno())
            signature = lambda s: (s.st_dev, s.st_ino, s.st_size, s.st_mtime_ns, s.st_ctime_ns)
            named = os.stat(path, follow_symlinks=False)
            require(signature(before) == signature(after) == signature(named) and size == before.st_size,
                    "日志在读取期间发生变化")
            inputs.append({"path": str(path), "bytes": size, "sha256": digest.hexdigest()})
    require(records, "没有 Flash/phase 观察记录")

    groups, phases, terminal, last_phase, histories = {}, {}, None, None, {"ota": [], "config": []}
    cost, phase_rank = None, -1
    for index, row in enumerate(records):
        kind = row["kind"]
        if kind == "ESP_BASE_FLASH_IO_OUTPUT":
            group = groups.setdefault(row["started_us"], {"rows": {}, "output": None})
            require(group["output"] is None, "重复 Flash 输出边界")
            group["output"] = index
            continue
        start, end = row["snapshot_started_us"], row["snapshot_finished_us"]
        if kind == "ESP_BASE_FLASH_IO":
            group = groups.setdefault(start, {"rows": {}, "output": None})
            require(row["consumer"] not in group["rows"], "重复或无法区分的 Flash consumer 快照")
            group["rows"][row["consumer"]] = index
            cost = row["aggregate_storage_bytes"] if cost is None else cost
            require(cost == row["aggregate_storage_bytes"], "同 boot 聚合成本变化")
            histories[row["consumer"]].append({"start": start, "end": end,
                "valid": row["counters_valid"], "values": {k: row[k] for k in COUNTERS}})
            continue
        require(terminal is None, "terminal 后仍出现同操作阶段")
        if last_phase is not None:
            require(start >= last_phase["snapshot_finished_us"], "同一 OTA worker 的阶段时钟倒退")
            for consumer in histories:
                require(row[consumer + "_completed_claim_count"] >= last_phase[consumer + "_completed_claim_count"],
                        "同一 OTA worker 的完成计数回退")
                require(row[consumer + "_counters_valid"] <= last_phase[consumer + "_counters_valid"],
                        "同一 OTA worker 的计数有效性恢复")
        last_phase = row
        for consumer in histories:
            histories[consumer].append({"start": start, "end": end,
                "valid": row[consumer + "_counters_valid"],
                "values": {"completed_claim_count": row[consumer + "_completed_claim_count"]}})
        if row["boundary"] == "terminal":
            terminal = index
        else:
            rank = OUTER.index(row["phase"])
            require(rank >= phase_rank, "外层阶段顺序回退")
            phase_rank = rank
            phase = phases.setdefault(row["phase"], {})
            require(row["boundary"] not in phase, "重复阶段边界")
            require(row["boundary"] != "begin" or "end" not in phase, "阶段 end 后出现 begin")
            phase[row["boundary"]] = index

    overlaps = {consumer: comparable_history(items) for consumer, items in histories.items()}
    snapshots = []
    for start, group in sorted(groups.items()):
        rows = [records[i] for i in group["rows"].values()]
        require(len({row["snapshot_finished_us"] for row in rows}) <= 1, "同一 Flash 快照边界不一致")
        if group["output"] is not None and rows:
            require(records[group["output"]]["finished_us"] >= rows[0]["snapshot_finished_us"], "输出结束早于其快照")
        missing = [name for name in ("ota", "config") if name not in group["rows"]]
        if group["output"] is None:
            missing.append("output")
        invalid = any(not row["counters_valid"] for row in rows)
        snapshots.append({"started_us": start, "finished_us": rows[0]["snapshot_finished_us"] if rows else None,
            **group, "status": "missing" if missing else "uncertain" if invalid else "observed",
            "missing": missing, "counters_valid": bool(rows) and not invalid})

    phase_results = []
    for name in OUTER:
        boundaries = phases.get(name, {})
        missing = [boundary for boundary in ("begin", "end") if boundary not in boundaries]
        result = {"phase": name, "boundaries": boundaries, "missing": missing,
                  "status": "missing", "published_completed_claim_delta": None,
                  "inside_snapshot_indices": [], "boundary_overlapping_snapshot_indices": []}
        if not missing:
            begin, end = (records[boundaries[key]] for key in ("begin", "end"))
            valid = all(row[c + "_counters_valid"] for row in (begin, end) for c in histories)
            result.update(status="observed" if valid else "uncertain", result=end["result"],
                boundary_snapshot_separation_us={
                    "minimum": end["snapshot_started_us"] - begin["snapshot_finished_us"],
                    "maximum": end["snapshot_finished_us"] - begin["snapshot_started_us"]})
            if valid:
                result["published_completed_claim_delta"] = {c: end[c + "_completed_claim_count"] -
                    begin[c + "_completed_claim_count"] for c in histories}
                require(all(v >= 0 for v in result["published_completed_claim_delta"].values()), "阶段完成计数回退")
            for index, snapshot in enumerate(snapshots):
                if snapshot["finished_us"] is None:
                    continue
                if begin["snapshot_finished_us"] <= snapshot["started_us"] and snapshot["finished_us"] <= end["snapshot_started_us"]:
                    result["inside_snapshot_indices"].append(index)
                elif snapshot["started_us"] <= end["snapshot_finished_us"] and snapshot["finished_us"] >= begin["snapshot_started_us"]:
                    result["boundary_overlapping_snapshot_indices"].append(index)
        phase_results.append(result)
    terminal_status = "missing"
    if terminal is not None:
        row = records[terminal]
        terminal_status = "uncertain" if row["stage_uncertain"] or row["result"] == "unknown" or not all(
            row[c + "_counters_valid"] for c in histories) else "observed"
    return {"valid": True, "schema_version": 1, "scope": "outer_phases_and_published_flash_observations",
        "boot_id": boot_id, "operation_id": operation_id, "request_id": request_id,
        "inputs": inputs, "records": records, "snapshots": snapshots, "phases": phase_results,
        "terminal_record_index": terminal, "terminal_observation": terminal_status,
        "overlapping_snapshot_order_counts": overlaps,
        "limits": ["输入身份不替代完整候选与实机身份绑定", "打印顺序不等于跨任务快照顺序",
            "阶段分离数值是边界快照间隔，不是内部擦写或验签耗时", "完成 claim 差值只反映聚合已发布观察",
            "聚合最大值可能来自本操作之前，缺失阶段不自动视为未执行", "不证明 MQTT/业务峰值持续重叠、功耗或断电阶段"],
        "internal_erase_verify_timing_proven": False, "unfinished_holder_bound_proven": False,
        "whole_interval_worst_case_proven": False, "r5_qualified": False, "r6_qualified": False}


def main():
    parser = argparse.ArgumentParser(description=__doc__, epilog=
        "跨 boot/op/request 日志拒绝；不自动裁剪、拼接残行或授完整观察。exit 0 仅表示解析成功；exit 2 表示输入被拒绝。")
    for name in ("boot-id", "operation-id", "request-id"):
        parser.add_argument("--" + name, required=True)
    parser.add_argument("--uart-log", type=Path, action="append", required=True, help="已封存的原始普通文件，可重复")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    try:
        result = analyze(args.uart_log, args.boot_id, args.operation_id, args.request_id)
    except (OSError, ValueError) as error:
        result = {"valid": False, "error": str(error), "r5_qualified": False, "r6_qualified": False}
    if args.json:
        print(json.dumps(result, ensure_ascii=False, indent=2))
    elif result["valid"]:
        print("Flash/phase 原始观察解析成功；exit 0 不代表验收通过")
        for phase in result["phases"]:
            print(f"  {phase['phase']}: {phase['status']}")
        print(f"  terminal: {result['terminal_observation']}；R5/R6 未获资格")
    else:
        print("Flash/phase 输入拒绝：" + result["error"])
    return 0 if result["valid"] else 2


if __name__ == "__main__":
    raise SystemExit(main())
