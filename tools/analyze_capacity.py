#!/usr/bin/env python3
"""只读解析正式容量帧：SDK 连续统计，截至 BEGIN；不授 R5／R6 资格。"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import stat

UINT32_MAX = (1 << 32) - 1
UINT64_MAX = (1 << 64) - 1
HEAP_GATE_BYTES = 16384
LARGEST_GATE_BYTES = 24576
STACK_GATE_BYTES = 1024
INTERNAL_8BIT = (1 << 11) | (1 << 2)
# 与正式 producer 的顺序一致。cap 是 SDK 原生位掩码，不能跨域替代。
DOMAIN_CAPS = (INTERNAL_8BIT, (1 << 11) | (1 << 1),
               (1 << 11) | (1 << 3), 1 << 12, (1 << 11) | (1 << 13))
MARKER = b"ESP_BASE_CAPACITY_"
UUID = re.compile(r"[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}")
HASH = re.compile(r"[0-9a-f]{64}")
FIELDS = {
    "BEGIN": "schema boot_id frame phase uptime_ms sdk_lock_sha256 task_limit".split(),
    "REGION": ("boot_id frame start end caps0 caps1 caps2 alias_start alias_end "
               "alias_inverted available_at_heap_init free_bytes min_free_bytes "
               "largest_request_bytes min_largest_request_bytes allocator_metadata_bytes").split(),
    "TASK": "boot_id frame instance name_hex minimum_stack_bytes state".split(),
    "DOMAIN": ("boot_id frame caps alignment_bytes minimum_free_lower_bound_bytes "
               "largest_request_lower_bound_bytes history").split(),
    "END": ("boot_id frame regions allocated_tasks created_instances finalized_instances "
            "completed_stack_min_bytes worst_completed_instance minimum_stack_bytes "
            "task_snapshot_complete counters_valid facts_valid workspace_bytes "
            "observation_cost_added_back").split(),
}
HEX_FIELDS = {"start", "end", "caps0", "caps1", "caps2", "alias_start", "alias_end", "caps"}
BOOL_FIELDS = {"alias_inverted", "available_at_heap_init", "task_snapshot_complete",
               "counters_valid", "facts_valid", "observation_cost_added_back"}
TEXT_FIELDS = {"boot_id", "phase", "sdk_lock_sha256", "name_hex", "history"}


def _parse(raw: bytes) -> tuple[str, dict] | None:
    if MARKER not in raw:
        return None  # 其它 UART 输出不属于容量帧，也不延长可证截止时刻。
    if not raw.startswith(MARKER) or not raw.endswith(b"\n") or len(raw) > 4096:
        raise ValueError("容量行混入前缀、过长或未完整结束")
    try:
        parts = raw.rstrip(b"\r\n").decode("ascii").split(" ")
    except UnicodeDecodeError as error:
        raise ValueError("容量行不是 ASCII") from error
    kind = parts[0].removeprefix("ESP_BASE_CAPACITY_")
    if kind not in FIELDS or len(parts) != len(FIELDS[kind]) + 1:
        raise ValueError("未知容量行或字段数不符")
    row = {}
    for expected, part in zip(FIELDS[kind], parts[1:]):
        key, separator, value = part.partition("=")
        if key != expected or not separator or not value:
            raise ValueError("容量字段缺失、重复或顺序错误")
        if key in TEXT_FIELDS:
            row[key] = value
        elif key in HEX_FIELDS:
            if re.fullmatch(r"[0-9a-f]{8}", value) is None:
                raise ValueError("能力域或地址不是规范八位十六进制")
            row[key] = int(value, 16)
        else:
            if re.fullmatch(r"0|[1-9][0-9]*", value) is None:
                raise ValueError("容量数值不是规范非负整数")
            number = int(value)
            if number > (UINT64_MAX if key == "uptime_ms" else UINT32_MAX):
                raise ValueError("容量数值超出正式目标的整数范围")
            if key in BOOL_FIELDS and number not in (0, 1):
                raise ValueError("容量布尔字段无效")
            row[key] = number
    return kind, row


def _finish(frame: dict) -> dict:
    begin, regions, tasks, domains, end = (frame[key] for key in
                                         ("begin", "regions", "tasks", "domains", "end"))
    if not regions or end["regions"] != len(regions) or end["allocated_tasks"] != len(tasks):
        raise ValueError("region／任务行与 END 计数不符")
    if tuple(row["caps"] for row in domains) != DOMAIN_CAPS:
        raise ValueError("能力域缺失、未知、重复或顺序错误")
    if len(tasks) > begin["task_limit"] or begin["task_limit"] != 32:
        raise ValueError("任务快照超出正式固定工作区")
    if any(end[key] != 1 for key in ("counters_valid", "facts_valid")):
        raise ValueError("连续统计已永久失效")
    created, finalized = end["created_instances"], end["finalized_instances"]
    if created == UINT32_MAX or finalized == UINT32_MAX or finalized > created:
        raise ValueError("任务计数饱和或 finalized 超出 created")
    expected = created - finalized
    complete = bool(tasks) and expected == len(tasks)
    if end["task_snapshot_complete"] != int(complete):
        raise ValueError("任务完整标记与 created／finalized／已报告实例矛盾")
    if not complete and (expected <= len(tasks) or (not tasks and expected <= begin["task_limit"])):
        raise ValueError("任务不足快照不符合清理间隙或官方列表容量边界")
    worst = end["worst_completed_instance"]
    if (finalized == 0 and (end["completed_stack_min_bytes"] != 0 or worst != 0)) or (finalized > 0 and worst == 0):
        raise ValueError("退出实例的最低栈与空集合标记不符")
    instances = [row["instance"] for row in tasks]
    if 0 in instances or len(set(instances)) != len(instances) or (finalized > 0 and worst in instances):
        raise ValueError("任务实例重复、为零或同时属于已退出与仍分配集合")
    for row in tasks:
        if re.fullmatch(r"(?:[0-9a-f]{2})+", row["name_hex"]) is None or row["state"] > 4:
            raise ValueError("任务名编码或 FreeRTOS 状态无效")
    if tasks and len({len(row["name_hex"]) for row in tasks}) != 1:
        raise ValueError("任务名工作区宽度不一致")
    stack_values = [row["minimum_stack_bytes"] for row in tasks]
    if finalized:
        stack_values.append(end["completed_stack_min_bytes"])
    reported_minimum = min(stack_values) if stack_values else 0
    if reported_minimum != end["minimum_stack_bytes"] or end["observation_cost_added_back"] != 0:
        raise ValueError("全实例最低栈不符或观察成本被加回")
    if end["workspace_bytes"] == 0:
        raise ValueError("正式观察工作区缺失")

    ordered = sorted(regions, key=lambda row: row["start"])
    for index, row in enumerate(ordered):
        if row["start"] >= row["end"] or (row["start"] | row["end"]) & 3:
            raise ValueError("实际 region 边界无效")
        if index and ordered[index - 1]["end"] > row["start"]:
            raise ValueError("物理 region 重复或重叠")
        if not row["caps0"] | row["caps1"] | row["caps2"]:
            raise ValueError("region 没有实际能力位")
        if not (row["min_largest_request_bytes"] <= row["largest_request_bytes"] <= row["free_bytes"]
                and row["min_largest_request_bytes"] <= row["min_free_bytes"] <= row["free_bytes"]):
            raise ValueError("region 当前值与连续历史最低值矛盾")
        if not 0 < row["allocator_metadata_bytes"] <= row["end"] - row["start"] - row["free_bytes"]:
            raise ValueError("region 大小、空闲与分配器元数据矛盾")
        alias_start, alias_end = row["alias_start"], row["alias_end"]
        if alias_start or alias_end:
            if (alias_start >= alias_end or (alias_start | alias_end) & 3
                    or alias_end - alias_start != row["end"] - row["start"]):
                raise ValueError("region alias 边界无效")
        elif row["alias_inverted"]:
            raise ValueError("缺少 alias 却声明地址反转")

    for domain in domains:
        if domain["alignment_bytes"] != 4 or domain["history"] != "region_minima_conservative_bound":
            raise ValueError("能力域更改了对齐或历史下界口径")
        matching = [row for row in regions if row["available_at_heap_init"] and
                    ((row["caps0"] | row["caps1"] | row["caps2"]) & domain["caps"]) == domain["caps"]]
        # 晚注册 region 的不存在前缀为零；各优先级和 alias 不重复计算。
        free_bound = sum(row["min_free_bytes"] for row in matching)
        largest_bound = max((row["min_largest_request_bytes"] for row in matching), default=0)
        if free_bound > UINT32_MAX or domain["minimum_free_lower_bound_bytes"] != free_bound or domain["largest_request_lower_bound_bytes"] != largest_bound:
            raise ValueError("能力域下界不等于实际 region 历史最低值的保守汇总")
    return frame


def _check_readout_history(previous: dict, current: dict) -> None:
    """所有帧的运输顺序、分配器与有效累计计数都连续核对。"""
    before, after = previous["begin"], current["begin"]
    if after["frame"] != before["frame"] + 1 or after["uptime_ms"] <= before["uptime_ms"] or before["phase"] == "before_reset":
        raise ValueError("帧重复、丢失、时间回退或同 boot 复位帧后继续统计")
    old_regions = {row["start"]: row for row in previous["regions"]}
    new_regions = {row["start"]: row for row in current["regions"]}
    if not old_regions.keys() <= new_regions.keys():
        raise ValueError("已登记 region 从后续帧消失")
    fixed = ("start", "end", "caps0", "caps1", "caps2", "alias_start", "alias_end",
             "alias_inverted", "available_at_heap_init", "allocator_metadata_bytes")
    for start, row in new_regions.items():
        old = old_regions.get(start)
        if old is None:
            if row["available_at_heap_init"]:
                raise ValueError("后出现的 region 被伪装成启动时已可用")
        elif any(old[key] != row[key] for key in fixed) or any(row[key] > old[key] for key in ("min_free_bytes", "min_largest_request_bytes")):
            raise ValueError("region 身份改变或连续历史最低值回升")
    old, new = previous["end"], current["end"]
    if (new["created_instances"] < old["created_instances"] or new["finalized_instances"] < old["finalized_instances"]
            or new["workspace_bytes"] != old["workspace_bytes"]):
        raise ValueError("任务历史计数或固定工作区改变")
    if old["finalized_instances"] and (new["completed_stack_min_bytes"] > old["completed_stack_min_bytes"] or
            (new["completed_stack_min_bytes"] == old["completed_stack_min_bytes"] and new["worst_completed_instance"] != old["worst_completed_instance"])):
        raise ValueError("退出实例历史最低栈或最差实例不连续")
    if new["finalized_instances"] == old["finalized_instances"] and any(
            new[key] != old[key] for key in ("completed_stack_min_bytes", "worst_completed_instance")):
        raise ValueError("没有新增最终记录却改变了退出实例摘要")


def _check_complete_task_history(previous: dict, current: dict, seen: dict[int, dict], retired: set[int]) -> None:
    """仅完整快照的集合差及全实例 HWM 构成生命周期锚点。"""
    old, new = previous["end"], current["end"]
    if new["minimum_stack_bytes"] > old["minimum_stack_bytes"]:
        raise ValueError("完整快照的全实例最低栈回升")
    previous_ids = {row["instance"] for row in previous["tasks"]}
    current_ids = {row["instance"] for row in current["tasks"]}
    if (len(current_ids - previous_ids) > new["created_instances"] - old["created_instances"] or
            len(previous_ids - current_ids) > new["finalized_instances"] - old["finalized_instances"]):
        raise ValueError("完整实例集合变化超出真实创建／最终记录计数")
    retired.update(previous_ids - current_ids)
    for row in current["tasks"]:
        old_task = seen.get(row["instance"])
        if row["instance"] in retired or (old_task and (old_task["name_hex"] != row["name_hex"] or row["minimum_stack_bytes"] > old_task["minimum_stack_bytes"])):
            raise ValueError("已退出实例重新出现或同实例名称／最低栈改变")


def _summary(frame: dict) -> dict:
    domain = next(row for row in frame["domains"] if row["caps"] == INTERNAL_8BIT)
    complete = bool(frame["end"]["task_snapshot_complete"])
    return {"frame": frame["begin"]["frame"], "phase": frame["begin"]["phase"],
            "begin_uptime_ms": frame["begin"]["uptime_ms"],
            "proved_until_begin_uptime_ms": frame["begin"]["uptime_ms"] if complete else None,
            "task_snapshot_complete": complete,
            "task_snapshot_issue": None if complete else ("task_list_capacity_not_complete" if not frame["tasks"]
                                                         else "created_minus_finalized_exceeds_reported_instances"),
            "regions": len(frame["regions"]), "allocated_tasks": len(frame["tasks"]),
            "created_instances": frame["end"]["created_instances"],
            "finalized_instances": frame["end"]["finalized_instances"],
            "minimum_free_lower_bound_bytes": domain["minimum_free_lower_bound_bytes"],
            "largest_request_lower_bound_bytes": domain["largest_request_lower_bound_bytes"],
            "minimum_stack_bytes": frame["end"]["minimum_stack_bytes"] if complete else None,
            "reported_minimum_stack_bytes": frame["end"]["minimum_stack_bytes"],
            "domains": [{key: row[key] for key in ("caps", "alignment_bytes",
                         "minimum_free_lower_bound_bytes", "largest_request_lower_bound_bytes")}
                        for row in frame["domains"]]}


def analyze(paths: list[Path], target: str, boot_id: str, sdk_lock_sha256: str) -> dict:
    """输入是同一 boot 的完整原始 UART 帧；不跨文件拼残帧。"""
    if target not in ("esp32", "esp32c3") or UUID.fullmatch(boot_id) is None or HASH.fullmatch(sdk_lock_sha256) is None or not paths:
        raise ValueError("须提供明确 target、规范 boot UUID、冻结 SDK lock 摘要及原日志")
    frames, inputs, seen, retired = [], [], {}, set()
    previous = previous_complete = None  # 72 小时日志不驻留全部原始 REGION／TASK 帧。
    name_width = None
    for path in paths:
        fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK | getattr(os, "O_NOFOLLOW", 0))
        with os.fdopen(fd, "rb") as stream:
            initial = os.fstat(stream.fileno())
            if not stat.S_ISREG(initial.st_mode):
                raise ValueError("容量日志必须是普通文件")
            digest, size, frame, stage = hashlib.sha256(), 0, None, None
            for line_number, raw in enumerate(stream, 1):
                digest.update(raw)
                size += len(raw)
                try:
                    parsed = _parse(raw)
                    if parsed is None:
                        continue
                    kind, row = parsed
                    if row["boot_id"] != boot_id or not 0 < row["frame"] < UINT32_MAX:
                        raise ValueError("boot 不符或帧编号为零／饱和")
                    if kind == "BEGIN":
                        if frame is not None or row["schema"] != 1 or row["sdk_lock_sha256"] != sdk_lock_sha256 or row["phase"] not in ("periodic", "before_reset"):
                            raise ValueError("BEGIN 嵌套、版本／冻结 SDK 不符或 phase 未知")
                        frame = {"begin": row, "regions": [], "tasks": [], "domains": []}
                        stage = "REGION"
                    else:
                        if frame is None or row["frame"] != frame["begin"]["frame"]:
                            raise ValueError("容量行缺 BEGIN 或不属于当前帧")
                        allowed = {"REGION": ("REGION", "TASK", "DOMAIN"), "TASK": ("TASK", "DOMAIN"),
                                   "DOMAIN": ("DOMAIN", "END")}[stage]
                        if kind not in allowed:
                            raise ValueError("REGION／TASK／DOMAIN／END 顺序错误")
                        stage = kind
                        if kind != "END":
                            frame[{"REGION": "regions", "TASK": "tasks", "DOMAIN": "domains"}[kind]].append(row)
                            continue
                        frame["end"] = row
                        _finish(frame)
                        if frame["tasks"]:
                            width = len(frame["tasks"][0]["name_hex"])
                            if name_width is not None and name_width != width:
                                raise ValueError("冻结任务名工作区宽度改变")
                            name_width = width
                        if previous is not None:
                            _check_readout_history(previous, frame)
                        if row["task_snapshot_complete"]:
                            if previous_complete is not None:
                                _check_complete_task_history(previous_complete, frame, seen, retired)
                            if any(task["instance"] in retired for task in frame["tasks"]):
                                raise ValueError("最终记录过的实例重新出现在完整快照")
                            seen.update({task["instance"]: task for task in frame["tasks"]})
                            previous_complete = frame
                        # 已最终记录的 worst ID 是明确事实，不从不足列表的缺席推断退休。
                        if row["finalized_instances"]:
                            retired.add(row["worst_completed_instance"])
                        frames.append(_summary(frame))
                        previous = frame
                        frame, stage = None, None
                except ValueError as error:
                    raise ValueError(f"日志 {len(inputs) + 1} 第 {line_number} 行：{error}") from error
            if frame is not None:
                raise ValueError("日志在完整 END 前结束；禁止跨文件拼接")
            final = os.fstat(stream.fileno())
            if (initial.st_size, initial.st_mtime_ns) != (final.st_size, final.st_mtime_ns) or size != initial.st_size:
                raise ValueError("分析期间原日志发生变化")
            inputs.append({"sha256": digest.hexdigest(), "size_bytes": size})
    if not frames:
        raise ValueError("没有完整正式容量帧")
    last_complete = next((frame for frame in reversed(frames) if frame["task_snapshot_complete"]), None)
    gates = {"heap_gate_passed": last_complete is not None and last_complete["minimum_free_lower_bound_bytes"] >= HEAP_GATE_BYTES,
             "largest_gate_passed": last_complete is not None and last_complete["largest_request_lower_bound_bytes"] >= LARGEST_GATE_BYTES,
             "stack_gate_passed": last_complete is not None and last_complete["minimum_stack_bytes"] >= STACK_GATE_BYTES}
    return {"schema_version": 1, "valid": True, "target": target, "boot_id": boot_id,
            "sdk_lock_sha256": sdk_lock_sha256, "frames": frames, "inputs": inputs,
            "proved_until_begin_uptime_ms": last_complete["begin_uptime_ms"] if last_complete else None,
            "latest_readout_begin_uptime_ms": frames[-1]["begin_uptime_ms"],
            "incomplete_task_readouts": sum(not frame["task_snapshot_complete"] for frame in frames),
            "task_history_closed_at_latest_readout": frames[-1]["task_snapshot_complete"],
            "history_start": "heap_initialization_complete_and_task_instance_creation",
            "metrics_source": "continuous_sdk_statistics_read_by_complete_frames",
            "current_region_values_are_sequential": True, "observation_cost_added_back": False,
            "numeric_gates": gates, "numeric_gates_passed": all(gates.values()),
            "next_maximum_legal_request_verified": False, "r5_qualified": False, "r6_qualified": False,
            "limitations": ["组合可证截止仅来自完整任务快照的 BEGIN；不足帧保留原件与原因，不插值或延长截止。END、打印、复位与 panic 后缀未获资格。",
                            "能力域是 sum(region min_free)／max(region min_largest) 保守下界；晚注册前缀贡献零，不是同一时刻快照。",
                            "连续块仅为 plain 4 B 对齐、non-EXEC 口径；下一最大合法申请的实际尺寸、caps 与额外对齐费用尚未证明。",
                            "栈采用官方填充模式 HWM，涵盖计数闭合的仍分配与已最终记录实例，不是逐指令 SP 峰值。",
                            "target 由调用方提供；固件身份、真实负载、故障场景与 R5／R6 生命周期仍须独立验收。"]}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target", required=True, choices=("esp32", "esp32c3"))
    parser.add_argument("--boot-id", required=True)
    parser.add_argument("--sdk-lock-sha256", required=True)
    parser.add_argument("--uart-log", required=True, action="append", type=Path)
    parser.add_argument("--json", action="store_true", help="只输出机器可读 JSON")
    args = parser.parse_args()
    try:
        result = analyze(args.uart_log, args.target, args.boot_id, args.sdk_lock_sha256)
    except (OSError, ValueError) as error:
        result = {"valid": False, "error": str(error), "r5_qualified": False, "r6_qualified": False}
        if args.json:
            print(json.dumps(result, ensure_ascii=False, indent=2))
        else:
            print(f"容量分析 summary\n  拒绝  {error}\n  R5／R6  未获资格")
        return 1
    if args.json:
        print(json.dumps(result, ensure_ascii=False, indent=2))
    else:
        last = next((frame for frame in reversed(result["frames"]) if frame["task_snapshot_complete"]), None)
        print("容量分析 summary")
        print(f"  target       {args.target}")
        print(f"  记录帧       {len(result['frames'])}")
        print(f"  任务不足帧   {result['incomplete_task_readouts']} / 最新闭合 {result['task_history_closed_at_latest_readout']}")
        cutoff = result["proved_until_begin_uptime_ms"]
        print(f"  可证截止     BEGIN uptime {cutoff} ms" if cutoff is not None else "  可证截止     尚无闭合快照")
        if last is not None:
            print(f"  内部空闲下界 {last['minimum_free_lower_bound_bytes']} B / {HEAP_GATE_BYTES} B")
            print(f"  连续申请下界 {last['largest_request_lower_bound_bytes']} B / {LARGEST_GATE_BYTES} B")
            print(f"  全实例栈HWM  {last['minimum_stack_bytes']} B / {STACK_GATE_BYTES} B")
        else:
            print("  数值门       尚无任务计数闭合帧可授组合证明")
        print("  后缀／下一申请／R5／R6  未获资格；观察成本不加回")
    return 0 if result["numeric_gates_passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
