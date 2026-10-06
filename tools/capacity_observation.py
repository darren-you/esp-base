#!/usr/bin/env python3
"""读取实验 UART 的周期资源观察；不将采样结果冒充完整峰值验收。"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re

HEAP_GATE_BYTES = 16384
SUPPORTED_TARGETS = ("esp32c3", "esp32")
LARGEST_GATE_BYTES = 24576
STACK_GATE_BYTES = 1024
OBSERVER = re.compile(
    rb"ESP_BASE_CAPACITY_OBSERVER LAB_ONLY target=(esp32c3|esp32) period_ms=5000"
    rb" task_limit=32 exit_limit=64 other_capability_domains=unmeasured"
    rb" worker_exit_coverage=ota_before_done_and_normal_task_cleanup"
    rb" uncaptured_exits=reset_panic_before_cleanup non_task_stacks=unmeasured"
    rb" largest_history=unmeasured full_peak_qualification=0"
    rb" frozen_signed_image_qualification=0 observation_cost_added_back=0")
MEMORY = re.compile(rb"ESP_BASE_LAB_MEMORY uptime_ms=(\d+) free_bytes=(\d+) min_bytes=(\d+) largest_bytes=(\d+) control_stack_min_bytes=(\d+)")
TASKS = re.compile(rb"ESP_BASE_LAB_TASKS uptime_ms=(\d+) expected=(\d+) captured=(\d+) workspace_bytes=(\d+)")
TASK = re.compile(rb"ESP_BASE_LAB_TASK uptime_ms=(\d+) task=([^\r\n]+?) task_number=(\d+) minimum_stack_bytes=(\d+) priority=(\d+) state=(\d+)")
EXIT = re.compile(rb"ESP_BASE_LAB_TASK_EXIT phase=(\S+) task=([^\r\n]+?) task_number=(\d+) minimum_stack_bytes=(\d+)")


def observe(paths: list[Path], target: str) -> dict:
    if target not in SUPPORTED_TARGETS:
        raise ValueError("必须明确选择 esp32c3 或 esp32")
    memory = []
    task_margins = []
    malformed = 0
    incomplete = 0
    complete = 0
    expected_tasks = None
    captured_tasks = {}
    frame_uptime = None
    observer_starts = 0
    observer_bound = False
    last_uptime = None
    uptime_regressions = 0
    inputs = []

    def finish_frame():
        nonlocal complete, incomplete, expected_tasks, captured_tasks
        if expected_tasks is not None:
            if expected_tasks > 0 and len(captured_tasks) == expected_tasks:
                complete += 1
                task_margins.extend(captured_tasks.values())
            else:
                incomplete += 1
        expected_tasks, captured_tasks = None, {}

    def accept_uptime(uptime):
        nonlocal last_uptime, malformed, uptime_regressions
        if last_uptime is not None and uptime < last_uptime:
            finish_frame()
            malformed += 1
            uptime_regressions += 1
            return False
        last_uptime = uptime
        return True

    for path in paths:
        digest = hashlib.sha256()
        with path.open("rb") as stream:
            for raw in stream:
                digest.update(raw)
                # 原串口其它输出不属于这个观察格式，原样留在输入文件。
                prefixes = [position for marker in (b"ESP_BASE_CAPACITY_OBSERVER", b"ESP_BASE_LAB_")
                            if (position := raw.find(marker)) >= 0]
                if not prefixes:
                    continue
                line = raw[min(prefixes):].rstrip(b"\r\n")
                if line.startswith(b"ESP_BASE_CAPACITY_OBSERVER"):
                    finish_frame()
                    observer_starts += 1
                    match = OBSERVER.fullmatch(line)
                    observer_bound = (observer_starts == 1 and match is not None and
                                      match.group(1) == target.encode("ascii"))
                    if not observer_bound:
                        malformed += 1
                    continue
                if not observer_bound:
                    malformed += 1
                    continue
                match = MEMORY.fullmatch(line)
                if match:
                    values = tuple(map(int, match.groups()))
                    if accept_uptime(values[0]):
                        memory.append(values)
                    continue
                match = TASKS.fullmatch(line)
                if match:
                    finish_frame()
                    frame_uptime, expected, captured, _ = map(int, match.groups())
                    if accept_uptime(frame_uptime):
                        expected_tasks = expected if expected == captured and expected <= 32 else 0
                    continue
                match = TASK.fullmatch(line)
                if match:
                    uptime, _, number, margin, _, _ = match.groups()
                    number, margin = int(number), int(margin)
                    if not accept_uptime(int(uptime)):
                        continue
                    if expected_tasks is None or int(uptime) != frame_uptime or number in captured_tasks:
                        malformed += 1
                    else:
                        captured_tasks[number] = margin
                    continue
                match = EXIT.fullmatch(line)
                if match:
                    task_margins.append(int(match.group(4)))
                    continue
                finish_frame()
                malformed += 1
        inputs.append({"sha256": digest.hexdigest(), "size_bytes": path.stat().st_size})
        # 不把不同日志／启动轮中的残缺任务帧拼成完整快照。
        finish_frame()
    finish_frame()
    history = min((r[2] for r in memory), default=None)
    largest = min((r[3] for r in memory), default=None)
    control = min((r[4] for r in memory), default=None)
    tasks = min(task_margins, default=None)
    valid = malformed == 0 and observer_bound and observer_starts == 1
    return {
        "target": target,
        "scope": "periodic_uart_observations_and_observed_worker_exits",
        "heap_gate_bytes": HEAP_GATE_BYTES,
        "largest_gate_bytes": LARGEST_GATE_BYTES,
        "stack_gate_bytes": STACK_GATE_BYTES,
        "memory_samples": len(memory),
        "complete_task_snapshots": complete,
        "incomplete_task_snapshots": incomplete,
        "malformed_resource_lines": malformed,
        "observer_start_declarations": observer_starts,
        "uptime_regressions": uptime_regressions,
        "observed_history_heap_min_bytes": history,
        "observed_largest_block_min_bytes": largest,
        "observed_control_stack_min_bytes": control,
        "observed_task_stack_min_bytes": tasks,
        "observed_heap_gate_passed": valid and history is not None and history >= HEAP_GATE_BYTES,
        "observed_largest_gate_passed": valid and largest is not None and largest >= LARGEST_GATE_BYTES,
        "observed_stack_gate_passed": valid and complete > 0 and incomplete == 0 and control is not None and tasks is not None and min(control, tasks) >= STACK_GATE_BYTES,
        "full_peak_or_native_lifecycle_qualification": False,
        "metrics_are_sequential_samples": True,
        "observation_cost_added_back": False,
        "inputs": inputs,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target", required=True, choices=SUPPORTED_TARGETS)
    parser.add_argument("--uart-log", required=True, action="append", type=Path)
    parser.add_argument("--json", action="store_true", help="输出纯 JSON 供机器消费")
    args = parser.parse_args()
    result = observe(args.uart_log, args.target)
    if args.json:
        print(json.dumps(result, ensure_ascii=False, indent=2))
    else:
        print("资源观察 summary")
        print(f"  target      {result['target']}")
        print(f"  samples     {result['memory_samples']}")
        for label, value, gate, passed in (
            ("history", result['observed_history_heap_min_bytes'], HEAP_GATE_BYTES,
             result['observed_heap_gate_passed']),
            ("largest", result['observed_largest_block_min_bytes'], LARGEST_GATE_BYTES,
             result['observed_largest_gate_passed']),
            ("stack", result['observed_task_stack_min_bytes'], STACK_GATE_BYTES,
             result['observed_stack_gate_passed']),
        ):
            reading = "缺记录" if value is None else f"{value} B"
            state = "通过" if passed else "未通过"
            print(f"  {label:<11} {reading} / 门槛 {gate} B / {state}")
        print(f"  snapshots   完整 {result['complete_task_snapshots']} / 不完整 {result['incomplete_task_snapshots']}")
        print(f"  malformed   {result['malformed_resource_lines']}")
        print("  scope       周期采样及已观察到的 worker 退出")
        print("  full_peak   完整峰值与 native 生命周期待测")
    return 0 if all(result[k] for k in ("observed_heap_gate_passed", "observed_largest_gate_passed", "observed_stack_gate_passed")) else 1


if __name__ == "__main__":
    raise SystemExit(main())
