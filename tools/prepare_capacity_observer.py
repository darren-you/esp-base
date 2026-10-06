#!/usr/bin/env python3
"""仅向仓外原生源码副本加入一次性容量观察；不连接设备或修改冻结候选。"""
from __future__ import annotations

import argparse
import difflib
import hashlib
import json
import os
from pathlib import Path
import re
import stat

ROOT = Path(__file__).resolve().parents[1]
PROTOCOL = "firmware/components/device_protocol/esp_base_protocol.c"
CMAKE = "firmware/components/device_protocol/CMakeLists.txt"
FIRMWARE_CMAKE = "firmware/CMakeLists.txt"
OBSERVER_C = "firmware/components/device_protocol/esp_base_capacity_observer.c"
OBSERVER_H = "firmware/components/device_protocol/esp_base_capacity_observer.h"
DEFAULTS = "capacity_observer_lab.defaults"
PATCH = "capacity_observer_patch.diff"
RECEIPT = "capacity_observer_receipt.json"
TARGETS = ("esp32c3", "esp32")
INCLUDE_ANCHOR = '#include "control_state.h"\n'
CONTROL_ANCHOR = '        esp_base_control_state_note_progress(&s_control_state);\n'
EXIT_ANCHOR = '    atomic_store_explicit(&s_ota_result, result, memory_order_relaxed);\n'
CMAKE_ANCHOR = '    SRCS "esp_base_protocol.c"'
VERSION_ANCHOR = '    set(PROJECT_VER "0.2.0")'
MARKER = "ESP_BASE_CAPACITY_OBSERVER LAB_ONLY"

HEADER = '''// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>
void esp_base_capacity_observer_poll(uint64_t now_ms);
void esp_base_capacity_observer_ota_exit(void);
'''

SOURCE = r'''// SPDX-License-Identifier: Apache-2.0
/* 一次性实验副本；不取得冻结生产镜像的容量资格。 */
#include "esp_base_capacity_observer.h"
#include "sdkconfig.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#if !CONFIG_FREERTOS_USE_TRACE_FACILITY || !CONFIG_FREERTOS_UNICORE || CONFIG_FREERTOS_SMP || !CONFIG_FREERTOS_TASK_PRE_DELETION_HOOK
#error "Capacity observer requires the explicit non-SMP single-core trace and task cleanup lab build"
#endif
@@TARGET_GUARD@@
_Static_assert(sizeof(StackType_t) == 1, "Fixed IDF stack high-water units must be bytes");
enum { CAPACITY_TASK_LIMIT = 32, CAPACITY_EXIT_LIMIT = 64, CAPACITY_PERIOD_MS = 5000 };
typedef struct {
    char name[configMAX_TASK_NAME_LEN];
    UBaseType_t number, minimum_stack_bytes, priority;
    eTaskState state;
} capacity_task_fact_t;
static TaskStatus_t s_capacity_tasks[CAPACITY_TASK_LIMIT];
static capacity_task_fact_t s_capacity_facts[CAPACITY_TASK_LIMIT];
static uint64_t s_capacity_next_ms;
static bool s_capacity_started;
/* BSS立即可收集app_main之前的正常清理；首poll不能清空早期事实。 */
static capacity_task_fact_t s_capacity_exits[CAPACITY_EXIT_LIMIT];
static portMUX_TYPE s_capacity_exit_mux = portMUX_INITIALIZER_UNLOCKED;
static unsigned s_capacity_exit_head, s_capacity_exit_count;
static uint32_t s_capacity_exit_dropped;

static void copy_fact(capacity_task_fact_t *out, const TaskStatus_t *task)
{
    memset(out, 0, sizeof *out);
    if (task->pcTaskName != NULL) {
        strncpy(out->name, task->pcTaskName, sizeof out->name - 1);
        for (size_t i = 0; out->name[i]; ++i)
            if ((unsigned char)out->name[i] < 0x20 || (unsigned char)out->name[i] > 0x7e)
                out->name[i] = '?';
    }
    if (!out->name[0]) memcpy(out->name, "unknown", 8);
    out->number = task->xTaskNumber;
    out->minimum_stack_bytes = task->usStackHighWaterMark;
    out->priority = task->uxCurrentPriority;
    out->state = task->eCurrentState;
}

void vTaskPreDeletionHook(void *tcb)
{
    /* 锁定IDF non-SMP cleanup已释放kernel锁，目标不再运行且栈尚未释放。
     * eDeleted避免查询已移除的状态链表；仅复制事实，无heap/log/TLSP。 */
    TaskStatus_t task = {0};
    capacity_task_fact_t fact;
    vTaskGetInfo((TaskHandle_t)tcb, &task, pdTRUE, eDeleted);
    copy_fact(&fact, &task);
    portENTER_CRITICAL(&s_capacity_exit_mux);
    if (s_capacity_exit_count < CAPACITY_EXIT_LIMIT) {
        const unsigned tail = (s_capacity_exit_head + s_capacity_exit_count) % CAPACITY_EXIT_LIMIT;
        s_capacity_exits[tail] = fact;
        ++s_capacity_exit_count;
    } else if (s_capacity_exit_dropped < UINT32_MAX) {
        ++s_capacity_exit_dropped;
    }
    portEXIT_CRITICAL(&s_capacity_exit_mux);
}

static void flush_exits(void)
{
    flockfile(stdout);
    /* 有限control pass：持续退出也不能让本次flush无限延长。 */
    for (unsigned i = 0; i < CAPACITY_EXIT_LIMIT; ++i) {
        capacity_task_fact_t fact;
        bool present;
        portENTER_CRITICAL(&s_capacity_exit_mux);
        present = s_capacity_exit_count != 0;
        if (present) {
            fact = s_capacity_exits[s_capacity_exit_head];
            s_capacity_exit_head = (s_capacity_exit_head + 1) % CAPACITY_EXIT_LIMIT;
            --s_capacity_exit_count;
        }
        portEXIT_CRITICAL(&s_capacity_exit_mux);
        if (!present) break;
        printf("ESP_BASE_LAB_TASK_EXIT phase=task_predelete task=%s task_number=%u minimum_stack_bytes=%u\n",
               fact.name, (unsigned)fact.number, (unsigned)fact.minimum_stack_bytes);
    }
    portENTER_CRITICAL(&s_capacity_exit_mux);
    const uint32_t dropped = s_capacity_exit_dropped;
    portEXIT_CRITICAL(&s_capacity_exit_mux);
    /* sticky且饱和；原parser对未知ESP_BASE_LAB_行使整轮失格。 */
    if (dropped != 0)
        printf("ESP_BASE_LAB_TASK_EXIT_OVERFLOW dropped=%" PRIu32 " limit=64\n", dropped);
    fflush(stdout);
    funlockfile(stdout);
}

void esp_base_capacity_observer_poll(uint64_t now_ms)
{
    if (!s_capacity_started) {
        puts("ESP_BASE_CAPACITY_OBSERVER LAB_ONLY target=@@TARGET@@ period_ms=5000"
             " task_limit=32 exit_limit=64 other_capability_domains=unmeasured"
             " worker_exit_coverage=ota_before_done_and_normal_task_cleanup"
             " uncaptured_exits=reset_panic_before_cleanup non_task_stacks=unmeasured"
             " largest_history=unmeasured full_peak_qualification=0"
             " frozen_signed_image_qualification=0 observation_cost_added_back=0");
    }
    flush_exits();
    if (s_capacity_started && now_ms < s_capacity_next_ms) return;
    s_capacity_started = true;
    s_capacity_next_ms = now_ms <= UINT64_MAX - CAPACITY_PERIOD_MS ?
        now_ms + CAPACITY_PERIOD_MS : UINT64_MAX;
    const unsigned caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const size_t free_bytes = heap_caps_get_free_size(caps);
    const size_t min_bytes = heap_caps_get_minimum_free_size(caps);
    const size_t largest_bytes = heap_caps_get_largest_free_block(caps);
    const UBaseType_t control_stack_bytes = uxTaskGetStackHighWaterMark(NULL);
    /* 固定单核：名字/编号/最低栈必须在恢复调度前复制；日志不借用TCB指针。 */
    vTaskSuspendAll();
    const UBaseType_t expected = uxTaskGetNumberOfTasks();
    UBaseType_t captured = expected <= CAPACITY_TASK_LIMIT ?
        uxTaskGetSystemState(s_capacity_tasks, CAPACITY_TASK_LIMIT, NULL) : 0;
    if (captured > CAPACITY_TASK_LIMIT) captured = 0;
    for (UBaseType_t i = 0; i < captured; ++i) copy_fact(&s_capacity_facts[i], &s_capacity_tasks[i]);
    (void)xTaskResumeAll();
    flockfile(stdout);
    printf("ESP_BASE_LAB_MEMORY uptime_ms=%" PRIu64 " free_bytes=%zu min_bytes=%zu"
           " largest_bytes=%zu control_stack_min_bytes=%u\n", now_ms, free_bytes,
           min_bytes, largest_bytes, (unsigned)control_stack_bytes);
    printf("ESP_BASE_LAB_TASKS uptime_ms=%" PRIu64 " expected=%u captured=%u workspace_bytes=%zu\n",
           now_ms, (unsigned)expected, (unsigned)captured,
           sizeof s_capacity_tasks + sizeof s_capacity_facts + sizeof s_capacity_next_ms + sizeof s_capacity_started +
           sizeof s_capacity_exits + sizeof s_capacity_exit_mux + sizeof s_capacity_exit_head +
           sizeof s_capacity_exit_count + sizeof s_capacity_exit_dropped);
    for (UBaseType_t i = 0; i < captured; ++i) {
        const capacity_task_fact_t *fact = &s_capacity_facts[i];
        printf("ESP_BASE_LAB_TASK uptime_ms=%" PRIu64 " task=%s task_number=%u"
               " minimum_stack_bytes=%u priority=%u state=%u\n", now_ms, fact->name,
               (unsigned)fact->number, (unsigned)fact->minimum_stack_bytes,
               (unsigned)fact->priority, (unsigned)fact->state);
    }
    fflush(stdout);
    funlockfile(stdout);
}

void esp_base_capacity_observer_ota_exit(void)
{
    TaskStatus_t task = {0};
    capacity_task_fact_t fact;
    vTaskSuspendAll();
    vTaskGetInfo(NULL, &task, pdTRUE, eInvalid);
    copy_fact(&fact, &task);
    (void)xTaskResumeAll();
    flockfile(stdout);
    printf("ESP_BASE_LAB_TASK_EXIT phase=ota_worker_exit task=%s task_number=%u minimum_stack_bytes=%u\n",
           fact.name, (unsigned)fact.number, (unsigned)fact.minimum_stack_bytes);
    fflush(stdout);
    funlockfile(stdout);
}
'''


class PreparationError(Exception):
    pass


def require(condition, message):
    if not condition:
        raise PreparationError(message)


def digest(raw):
    return hashlib.sha256(raw).hexdigest()


def read_file(path, maximum=2 * 1024 * 1024):
    descriptor = os.open(path, os.O_RDONLY | os.O_NONBLOCK | getattr(os, "O_NOFOLLOW", 0))
    try:
        before = os.fstat(descriptor)
        require(stat.S_ISREG(before.st_mode) and before.st_size <= maximum and before.st_nlink == 1,
                "输入必须是有限普通文件且不能与其他源硬链接")
        with os.fdopen(descriptor, "rb", closefd=False) as stream:
            raw = stream.read(maximum + 1)
        after = os.fstat(descriptor)
        require((before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns, before.st_ctime_ns) ==
                (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns, after.st_ctime_ns) and
                len(raw) == before.st_size, "读取期间源输入变化")
        return raw
    finally:
        os.close(descriptor)


def safe_source(path, frozen_roots):
    require(not path.is_symlink(), "源码根不能是符号链接")
    root = path.resolve(strict=True)
    require(root.is_dir(), "源码根不是目录")
    require(root.stat().st_uid == os.getuid() and stat.S_IMODE(root.stat().st_mode) & 0o077 == 0,
            "实验源码根必须为当前用户独占0700目录")
    require(not any((parent / ".git").exists() for parent in (root, *root.parents)), "只允许仓外无.git源码副本")
    excluded = [ROOT.resolve(), *(p.resolve(strict=True) for p in frozen_roots)]
    require(all(not (root.is_relative_to(p) or p.is_relative_to(root)) for p in excluded),
            "不能修改canonical或已冻结源码根及其相交路径")
    for directory, names, files in os.walk(root, followlinks=False):
        require(".git" not in names and ".git" not in files, "源码副本包含嵌套.git")
        require(not any(Path(directory, name).is_symlink() for name in names), "源码副本目录不能借用符号链接")
    return root


def write_file(path, raw, *, new):
    flags = os.O_WRONLY | os.O_NONBLOCK | getattr(os, "O_NOFOLLOW", 0)
    if new:
        flags |= os.O_CREAT | os.O_EXCL
    descriptor = os.open(path, flags, 0o600)
    try:
        info = os.fstat(descriptor)
        require(stat.S_ISREG(info.st_mode) and info.st_nlink == 1, "输出必须为独立普通文件")
        if not new:
            os.ftruncate(descriptor, 0)
        with os.fdopen(descriptor, "wb", closefd=False) as stream:
            stream.write(raw)
            stream.flush()
            os.fsync(descriptor)
    finally:
        os.close(descriptor)
    require(read_file(path) == raw, "实验输出落盘读回不符；该副本不具备构建资格")


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, "baseline JSON字段重复")
        result[key] = value
    return result


def prepare(args):
    require(args.target in TARGETS, "target必须为esp32c3或esp32")
    require(bool(args.frozen_source_root), "必须明确列出本轮已冻结源码根")
    root = safe_source(args.source_root, args.frozen_source_root)
    baseline_bytes = read_file(args.baseline_receipt)
    try:
        baseline = json.loads(baseline_bytes, object_pairs_hook=unique_object)
        require(isinstance(baseline, dict) and isinstance(baseline.get("source_inputs"), dict), "baseline缺source_inputs")
        inputs = baseline["source_inputs"]
        require(isinstance(inputs.get("source_root"), str), "baseline源根字段无效")
        require(Path(inputs["source_root"]).resolve() == ROOT.resolve(), "baseline不是当前canonical源码收据")
        members = inputs["files"]
        require(isinstance(members, list) and bool(members), "baseline缺实际生产输入")
    except (ValueError, KeyError, TypeError) as error:
        raise PreparationError("baseline收据结构无效") from error
    before = {}
    for member in members:
        require(isinstance(member, dict) and set(member) == {"path", "sha256", "size_bytes"} and
                isinstance(member["path"], str) and bool(member["path"]) and
                isinstance(member["sha256"], str) and re.fullmatch("[0-9a-f]{64}", member["sha256"]) and
                type(member["size_bytes"]) is int and 0 <= member["size_bytes"] <= 2 * 1024 * 1024,
                "baseline成员结构、摘要或尺寸无效")
        relative = Path(member["path"])
        require(not relative.is_absolute() and relative.parts and ".." not in relative.parts and
                str(relative) == member["path"] and str(relative) not in before,
                "baseline路径越界或重复")
        require((root / relative).resolve(strict=True).is_relative_to(root), "源码成员借用外部路径")
        raw = read_file(root / relative)
        require(digest(raw) == member["sha256"] and len(raw) == member["size_bytes"] and
                raw == read_file(ROOT / relative), "独立副本与本轮冻结生产输入不一致")
        # baseline归档也包含未选中的mqtt_integration；只对原生活动装配拒绝lab标记。
        if str(relative) in (PROTOCOL, CMAKE, FIRMWARE_CMAKE, "firmware/apps/esp_base/main/esp_base_main.c"):
            require(b"ESP_BASE_LAB_ONLY" not in raw and b"ESP_BASE_LAB_MEMORY" not in raw and
                    b"esp_base_capacity_observer" not in raw, "原生活动源码已含实验观察输入")
        before[str(relative)] = raw
    require(PROTOCOL in before and CMAKE in before and FIRMWARE_CMAKE in before and "sdk-lock.json" in before,
            "baseline缺必要输入")
    selected_lock = "firmware/dependencies.lock" + (".esp32" if args.target == "esp32" else "")
    require(selected_lock in before and re.findall(rb"(?m)^target: (\w+)$", before[selected_lock]) == [args.target.encode()],
            "target与精确依赖锁不符")
    sdkconfig = root / "firmware/sdkconfig"
    if sdkconfig.exists():
        config = read_file(sdkconfig)
        require(re.findall(rb'(?m)^CONFIG_IDF_TARGET="([^"]+)"$', config) == [args.target.encode()],
                "已有sdkconfig属于另一target")
        require(b"CONFIG_FREERTOS_USE_TRACE_FACILITY=y" not in config and
                b"CONFIG_FREERTOS_TASK_PRE_DELETION_HOOK=y" not in config and
                b"CONFIG_EMQTT_PLAINTEXT_LAB=y" not in config, "已有sdkconfig已属于实验构建")
    for name in (OBSERVER_C, OBSERVER_H, DEFAULTS, PATCH, RECEIPT):
        require(not (root / name).exists() and not (root / name).is_symlink(), "副本已有实验产物，禁止覆盖")
    protocol = before[PROTOCOL].decode()
    cmake = before[CMAKE].decode()
    firmware_cmake = before[FIRMWARE_CMAKE].decode()
    require(all(protocol.count(anchor) == 1 for anchor in (INCLUDE_ANCHOR, CONTROL_ANCHOR, EXIT_ANCHOR)) and
            cmake.count(CMAKE_ANCHOR) == 1 and firmware_cmake.count(VERSION_ANCHOR) == 1,
            "原生源码插入anchor不唯一或不匹配")
    firmware_cmake = firmware_cmake.replace(VERSION_ANCHOR, '    set(PROJECT_VER "0.2.0-capacity-lab")')
    protocol = protocol.replace(INCLUDE_ANCHOR, INCLUDE_ANCHOR + '#include "esp_base_capacity_observer.h"\n')
    protocol = protocol.replace(CONTROL_ANCHOR, '        esp_base_capacity_observer_poll(uptime_ms());\n' + CONTROL_ANCHOR)
    # 记录必须先于done release，避免更高优先级control观察done后立即重启。
    protocol = protocol.replace(EXIT_ANCHOR, '    esp_base_capacity_observer_ota_exit();\n' + EXIT_ANCHOR)
    guard = ('if(NOT CMAKE_BUILD_EARLY_EXPANSION)\n'
             '    if(NOT ESP_BASE_CAPACITY_OBSERVER_LAB OR NOT CONFIG_FREERTOS_USE_TRACE_FACILITY OR NOT CONFIG_FREERTOS_TASK_PRE_DELETION_HOOK OR CONFIG_FREERTOS_SMP)\n'
             '        message(FATAL_ERROR "LAB_ONLY capacity observer requires explicit lab flag, non-SMP trace and task cleanup defaults; never release")\n'
             '    endif()\n'
             f'    if(NOT IDF_TARGET STREQUAL "{args.target}")\n'
             '        message(FATAL_ERROR "Capacity observer target differs from prepared receipt")\n'
             '    endif()\n'
             'endif()\n')
    cmake = guard + cmake.replace(CMAKE_ANCHOR, '    SRCS "esp_base_capacity_observer.c" "esp_base_protocol.c"')
    target_guard = f'#if !defined(CONFIG_IDF_TARGET_{args.target.upper()}) || !CONFIG_IDF_TARGET_{args.target.upper()}\n#error "Prepared capacity observer target mismatch"\n#endif'
    outputs = {PROTOCOL: protocol.encode(), CMAKE: cmake.encode(), FIRMWARE_CMAKE: firmware_cmake.encode(),
               OBSERVER_C: SOURCE.replace("@@TARGET_GUARD@@", target_guard).replace("@@TARGET@@", args.target).encode(),
               OBSERVER_H: HEADER.encode(), DEFAULTS: b"# LAB_ONLY; only for the independent observer build.\nCONFIG_FREERTOS_USE_TRACE_FACILITY=y\nCONFIG_FREERTOS_TASK_PRE_DELETION_HOOK=y\n"}
    patch = "".join("".join(difflib.unified_diff(before.get(name, b"").decode().splitlines(True),
                     raw.decode().splitlines(True), fromfile="a/" + name, tofile="b/" + name))
                    for name, raw in outputs.items()).encode()
    outputs[PATCH] = patch
    report = {"schema_version": 1, "observer_revision": 2, "artifact_kind": "lab_only_capacity_observer_source", "lab_only": True,
              "software_only": True, "candidate_qualified": False, "project_version": "0.2.0-capacity-lab",
              "target": args.target, "source_root": str(root), "baseline_receipt_sha256": digest(baseline_bytes),
              "baseline_production_inputs": members, "lab_source_patch_sha256": digest(patch),
              "generated_files": {name: {"sha256": digest(raw), "size_bytes": len(raw)} for name, raw in outputs.items()},
              "required_cmake_option": "ESP_BASE_CAPACITY_OBSERVER_LAB=ON", "lab_defaults": DEFAULTS,
              "sample_period_ms": 5000, "task_limit": 32, "extra_tasks": 0, "extra_queues": 0,
              "exit_fact_limit": 64, "exit_flush_limit_per_control_pass": 64,
              "exit_capture_from_bss": True, "exit_overflow_invalidates_parser": True, "extra_tlsp": 0,
              "sample_capabilities": "MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT", "stack_unit": "bytes",
              "worker_exit_coverage": ["base_ota_before_done", "all_freertos_normal_cleanup"],
              "unobserved_worker_exits": ["reset", "panic", "deletion_not_yet_cleaned_up"],
              "unobserved_stack_domains": ["bootloader", "ROM", "non_RTOS_startup", "ISR"],
              "heap_hooks_enabled": False, "global_largest_block_history_measured": False,
              "workspace_cost": "reported by the actual compiled sizeof expression; also inspect ELF/map",
              "observation_cost_added_back": False, "metrics_are_sequential_samples": True,
              "other_capability_domains_measured": False, "full_peak_or_native_lifecycle_qualification": False,
              "frozen_signed_image_qualification": False, "production_release_allowed": False,
              "production_release_gate_implemented": False,
              "hardware_write_authorized": False, "physical_device_access": False, "build_verified": False,
              "qualification_limits": ["instrumented laboratory candidate only", "control-pass sampling can miss transient allocations",
                  "trace changes TCB and observer reserves workspace", "reset, panic and pending cleanup may omit exit facts",
                  "exit overflow or output loss disqualifies the observation", "non-task stacks remain unobserved",
                  "printf may add stack depth after the reported sample", "no physical fault, cycle or 72-hour qualification"]}
    # 所有拒绝条件先完成；写前再次核对被覆盖的源文件，receipt只在全部读回后发布。
    require(all(read_file(root / name) == before[name] for name in (PROTOCOL, CMAKE, FIRMWARE_CMAKE)), "写前源码变化")
    for name, raw in outputs.items():
        write_file(root / name, raw, new=name not in (PROTOCOL, CMAKE, FIRMWARE_CMAKE))
    write_file(root / RECEIPT, (json.dumps(report, ensure_ascii=False, sort_keys=True, indent=2) + "\n").encode(), new=True)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", required=True, type=Path)
    parser.add_argument("--target", required=True, choices=TARGETS)
    parser.add_argument("--baseline-receipt", required=True, type=Path)
    parser.add_argument("--frozen-source-root", required=True, action="append", type=Path)
    args = parser.parse_args()
    try:
        report = prepare(args)
    except (PreparationError, OSError, UnicodeError, KeyError, TypeError) as error:
        print("容量观察准备阻断\n  原因  " + str(error))
        return 1
    print("容量观察实验副本准备完成")
    print("  target      " + report["target"])
    print("  receipt     " + str(args.source_root / RECEIPT))
    print("  scope       LAB_ONLY／周期采样、OTA退出前与正常任务清理，不授生产镜像或实板资格")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
