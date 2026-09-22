#!/usr/bin/env python3
"""分析 MQTT 实验串口资源快照；只输出计数、任务栈和堆趋势，不输出设备身份。"""
import argparse
import json
from pathlib import Path
import re
import statistics
import sys


RESOURCE = re.compile(r"EBASE_MQTT_RESOURCE phase=(\w+) cycle=(\d+) heap=(\d+) min_heap=(\d+) largest=(\d+) tasks=(\d+) sockets=(\d+) socket_errors=(\d+)")
TASK = re.compile(r"EBASE_MQTT_TASK phase=(\w+) cycle=(\d+) name=(.+) stack_bytes=(\d+)")
TIMER_START = re.compile(r"EBASE_MQTT_TIMERS_BEGIN phase=(\w+) cycle=(\d+)")
TIMER_END = re.compile(r"EBASE_MQTT_TIMERS_END phase=(\w+) cycle=(\d+) error=(\d+)")
TIMER_ROW = re.compile(r"(\S.*?)\s+\d+\s+\d+\s+\d+\s+\d+\s+\d+\s+\d+\s*")


def parse(path):
    snapshots = {}
    current = None
    timer_key = None
    with Path(path).open(encoding="utf-8", errors="replace") as stream:
        for raw in stream:
            line = raw.rstrip("\r\n")
            match = RESOURCE.fullmatch(line)
            if match:
                if current is not None:
                    raise ValueError("资源快照未结束或发生交错")
                phase, cycle, *values = match.groups()
                key = (phase, int(cycle))
                if key in snapshots:
                    raise ValueError("同一 phase/cycle 出现重复资源快照")
                current = key
                snapshots[key] = dict(zip(("heap", "min_heap", "largest", "tasks", "sockets", "socket_errors"), map(int, values)))
                snapshots[key].update(task_stacks={}, timers=None, timer_names={})
                continue
            match = TASK.fullmatch(line)
            if match:
                phase, cycle, name, stack = match.groups()
                if current != (phase, int(cycle)) or timer_key is not None or name in snapshots[current]["task_stacks"]:
                    raise ValueError("任务行与当前资源快照不一致")
                snapshots[current]["task_stacks"][name] = int(stack)
                continue
            match = TIMER_START.fullmatch(line)
            if match:
                phase, cycle = match.groups()
                if current != (phase, int(cycle)) or timer_key is not None:
                    raise ValueError("计时器快照起点不一致")
                timer_key = current
                snapshots[current]["timers"] = 0
                continue
            match = TIMER_END.fullmatch(line)
            if match:
                phase, cycle, error = match.groups()
                if current != (phase, int(cycle)) or timer_key != current or int(error) != 0:
                    raise ValueError("计时器快照失败或终点不一致")
                current = timer_key = None
                continue
            if line.startswith(("EBASE_MQTT_RESOURCE", "EBASE_MQTT_TASK", "EBASE_MQTT_TIMERS_")):
                raise ValueError("资源日志格式无效")
            if timer_key is not None:
                row = TIMER_ROW.fullmatch(line)
                if row:
                    snapshots[timer_key]["timers"] += 1
                    names = snapshots[timer_key]["timer_names"]
                    names[row[1]] = names.get(row[1], 0) + 1
                elif line.strip() and line != "Timer stats:" and not line.startswith("Name "):
                    raise ValueError("计时器表包含无法验证的行")
    if current is not None or timer_key is not None:
        raise ValueError("资源日志被截断")
    return snapshots


def summarize(snapshots, cycles):
    expected = {("before_create", 0)} | {("destroyed", n) for n in range(1, cycles + 1)} | {("requested", n) for n in range(cycles + 1)}
    if set(snapshots) != expected:
        raise ValueError("资源快照集合缺失或多出，无法覆盖指定轮次")
    baseline = snapshots[("before_create", 0)]
    online_baseline = snapshots[("requested", 0)]
    baseline_names = set(baseline["task_stacks"])
    if not baseline_names or not baseline["timers"]:
        raise ValueError("基线缺少任务或计时器事实")
    issues = []
    minimum_stacks = {}
    for (phase, cycle), sample in snapshots.items():
        names = set(sample["task_stacks"])
        required_names = baseline_names | {"mqtt_task"} if phase == "requested" else baseline_names
        if names != required_names or sample["tasks"] != len(names):
            issues.append({"phase": phase, "cycle": cycle, "error": "task_set_changed"})
        expected_sockets = baseline["sockets"] + (1 if phase == "requested" else 0)
        if sample["sockets"] != expected_sockets or sample["socket_errors"]:
            issues.append({"phase": phase, "cycle": cycle, "error": "socket_count_or_scan_error"})
        # 初次上线与重复重建是不同阶段。SDK 可延迟建立 PHY 等对象；
        # 单独报告首次变化，后续轮次逐名称与首次在线的数量比较，不设置容差。
        if not sample["timers"] or (phase != "before_create" and any(
            count > online_baseline["timer_names"].get(name, 0)
            for name, count in sample["timer_names"].items()
        )):
            issues.append({"phase": phase, "cycle": cycle, "error": "esp_timer_count_increased_or_missing"})
        for name, count in sample["task_stacks"].items():
            minimum_stacks[name] = min(minimum_stacks.get(name, count), count)
            if count < 1024:
                issues.append({"phase": phase, "cycle": cycle, "error": "task_stack_below_1024_bytes", "task": name})
    phases = {}
    for phase in ("destroyed", "requested"):
        rows = [v for (p, n), v in sorted(snapshots.items()) if p == phase]
        if not rows:
            continue
        heaps = [v["heap"] for v in rows]
        phases[phase] = {"samples": len(rows), "heap_min_bytes": min(heaps), "heap_max_bytes": max(heaps),
                         "first_ten_heap_median_bytes": statistics.median(heaps[:10]),
                         "last_ten_heap_median_bytes": statistics.median(heaps[-10:]),
                         "largest_free_blocks_bytes": sorted({v["largest"] for v in rows}),
                         "task_counts": sorted({v["tasks"] for v in rows}),
                         "socket_counts": sorted({v["sockets"] for v in rows}),
                         "esp_timer_counts": sorted({v["timers"] for v in rows})}
    initial_timer_changes = {name: online_baseline["timer_names"].get(name, 0) - baseline["timer_names"].get(name, 0)
                             for name in baseline["timer_names"].keys() | online_baseline["timer_names"].keys()
                             if online_baseline["timer_names"].get(name, 0) != baseline["timer_names"].get(name, 0)}
    return {"scope": "mqtt-lab-resource-samples", "cycles": cycles,
            "count_and_stack_checks_passed": not issues, "issues": issues,
            "minimum_task_stacks_bytes": minimum_stacks, "phases": phases,
            "initial_online_timer_names": online_baseline["timer_names"],
            "initial_timer_changes_requiring_attribution": initial_timer_changes,
            "minimum_free_heap_bytes": min(v["min_heap"] for v in snapshots.values()),
            "heap_trend_requires_review": True,
            "limits": ["instrumented laboratory image", "socket scans are not atomic",
                       "esp_timer counts exclude FreeRTOS software timers and internal lwIP timeouts",
                       "not a 72-hour soak or a proof of absence of every memory leak"]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial-log", required=True)
    parser.add_argument("--cycles", type=int, default=100)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    if not 0 <= args.cycles <= 100:
        parser.error("轮次必须在 0–100 之间")
    result = summarize(parse(args.serial_log), args.cycles)
    if args.json:
        print(json.dumps(result, ensure_ascii=False))
    else:
        print("MQTT 资源观测\n  计数与栈  %s\n  重建轮次  %d\n  堆趋势    需结合首尾中位数与原始证据评估" %
              ("通过" if result["count_and_stack_checks_passed"] else "未通过", args.cycles))
    return 0 if result["count_and_stack_checks_passed"] else 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ValueError, OSError) as error:
        print("MQTT 资源观测失败\n  原因  " + str(error), file=sys.stderr)
        raise SystemExit(1)
