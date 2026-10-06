"""仓外生成拒绝与真实C编译执行；FreeRTOS/heap是显式宿主替身，不产生MCU测量。"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest import mock

import capacity_observation
import prepare_capacity_observer as observer


class CapacityObserverPreparationTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="capacity_observer_host_")
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.source = self.directory / "source"
        self.source.mkdir(mode=0o700)
        self.frozen = self.directory / "frozen"
        self.frozen.mkdir(mode=0o700)
        self.members = []
        for name in (observer.PROTOCOL, observer.CMAKE, observer.FIRMWARE_CMAKE, "sdk-lock.json",
                     "firmware/dependencies.lock", "firmware/dependencies.lock.esp32",
                     "firmware/apps/mqtt_integration/main/mqtt_lab_main.c"):
            raw = (observer.ROOT / name).read_bytes()
            output = self.source / name
            output.parent.mkdir(parents=True, exist_ok=True)
            output.write_bytes(raw)
            self.members.append({"path": name, "sha256": observer.digest(raw), "size_bytes": len(raw)})
        self.baseline = self.directory / "baseline.json"
        self.baseline.write_text(json.dumps({"source_inputs": {"source_root": str(observer.ROOT), "files": self.members}}))
        self.args = argparse.Namespace(source_root=self.source, target="esp32c3",
            frozen_source_root=[self.frozen], baseline_receipt=self.baseline)
        self.original = {name: (observer.ROOT / name).read_bytes()
                         for name in (observer.PROTOCOL, observer.CMAKE, observer.FIRMWARE_CMAKE)}

    def assert_no_changes(self):
        for member in self.members:
            self.assertEqual(observer.digest((self.source / member["path"]).read_bytes()), member["sha256"])
        self.assertFalse((self.source / observer.RECEIPT).exists())

    def test_both_targets_prepare_separate_lab_inputs_and_original_is_unchanged(self):
        for target in observer.TARGETS:
            with self.subTest(target=target):
                source = self.directory / target
                shutil.copytree(self.source, source)
                self.args.source_root = source
                self.args.target = target
                report = observer.prepare(self.args)
                self.assertTrue(report["lab_only"])
                self.assertFalse(report["production_release_allowed"])
                self.assertFalse(report["frozen_signed_image_qualification"])
                self.assertFalse(report["hardware_write_authorized"])
                self.assertFalse(report["full_peak_or_native_lifecycle_qualification"])
                self.assertFalse(report["observation_cost_added_back"])
                self.assertTrue(report["software_only"])
                self.assertFalse(report["candidate_qualified"])
                self.assertFalse(report["production_release_gate_implemented"])
                self.assertIn('set(PROJECT_VER "0.2.0-capacity-lab")', (source / observer.FIRMWARE_CMAKE).read_text())
                self.assertEqual(report["worker_exit_coverage"], ["base_ota_before_done", "all_freertos_normal_cleanup"])
                self.assertEqual(report["unobserved_worker_exits"], ["reset", "panic", "deletion_not_yet_cleaned_up"])
                self.assertEqual(report["exit_fact_limit"], 64)
                self.assertEqual(report["exit_flush_limit_per_control_pass"], 64)
                self.assertTrue(report["exit_capture_from_bss"])
                self.assertTrue(report["exit_overflow_invalidates_parser"])
                self.assertEqual(report["extra_tlsp"], 0)
                self.assertFalse(report["heap_hooks_enabled"])
                self.assertFalse(report["global_largest_block_history_measured"])
                self.assertEqual(json.loads((source / observer.RECEIPT).read_text()), report)
                for name, fact in report["generated_files"].items():
                    self.assertEqual(observer.digest((source / name).read_bytes()), fact["sha256"])
                protocol = (source / observer.PROTOCOL).read_text()
                self.assertLess(protocol.index("    esp_base_capacity_observer_ota_exit();"),
                                protocol.index("    atomic_store_explicit(&s_ota_done, true, memory_order_release);"))
                self.assertEqual((source / observer.DEFAULTS).read_text().splitlines()[-2:],
                                 ["CONFIG_FREERTOS_USE_TRACE_FACILITY=y", "CONFIG_FREERTOS_TASK_PRE_DELETION_HOOK=y"])
                with self.assertRaises(observer.PreparationError):
                    observer.prepare(self.args)
        for name, raw in self.original.items():
            self.assertEqual((observer.ROOT / name).read_bytes(), raw)

    def test_reject_canonical_frozen_git_and_overlapping_roots(self):
        for path in (observer.ROOT, self.frozen, self.directory):
            with self.subTest(path=path):
                self.args.source_root = path
                with self.assertRaises(observer.PreparationError):
                    observer.prepare(self.args)
        self.args.source_root = self.source
        for git_path in (self.source / ".git", self.source / "firmware/.git"):
            git_path.write_text("gitdir: frozen")
            with self.assertRaises(observer.PreparationError):
                observer.prepare(self.args)
            git_path.unlink()
        self.args.frozen_source_root = []
        with self.assertRaises(observer.PreparationError):
            observer.prepare(self.args)
        self.assert_no_changes()

    def test_reject_symlinks_and_hardlinks_before_mutation(self):
        member = self.source / observer.PROTOCOL
        raw = member.read_bytes()
        member.unlink()
        member.symlink_to(observer.ROOT / observer.PROTOCOL)
        with self.assertRaises((observer.PreparationError, OSError)):
            observer.prepare(self.args)
        member.unlink()
        extra = self.directory / "linked_protocol.c"
        extra.write_bytes(raw)
        os.link(extra, member)
        with self.assertRaises(observer.PreparationError):
            observer.prepare(self.args)
        member.unlink()
        member.write_bytes(raw)
        member.unlink()
        os.mkfifo(member)
        with self.assertRaises(observer.PreparationError):
            observer.prepare(self.args)
        member.unlink()
        member.write_bytes(raw)
        self.assert_no_changes()

    def test_malformed_duplicate_and_traversal_baseline_members_rejected(self):
        original = self.baseline.read_text()
        for members in ([None], [{"path": 1}], [dict(self.members[0], size_bytes=True)],
                        [dict(self.members[0], path="../escape")], [self.members[0], self.members[0]]):
            with self.subTest(members=members):
                self.baseline.write_text(json.dumps({"source_inputs": {"source_root": str(observer.ROOT), "files": members}}))
                with self.assertRaises(observer.PreparationError):
                    observer.prepare(self.args)
                self.assert_no_changes()
        self.baseline.write_text(original[:-1] + ',"source_inputs":{}}')
        with self.assertRaises(observer.PreparationError):
            observer.prepare(self.args)
        self.assert_no_changes()

    def test_reject_stale_anchor_lab_wrong_target_and_bad_baseline(self):
        for addition in (observer.CONTROL_ANCHOR.encode(), b"\nESP_BASE_LAB_ONLY already_lab\n"):
            member = self.source / observer.PROTOCOL
            member.write_bytes(self.original[observer.PROTOCOL] + addition)
            with self.assertRaises(observer.PreparationError):
                observer.prepare(self.args)
            member.write_bytes(self.original[observer.PROTOCOL])
        self.args.target = "esp8266"
        with self.assertRaises(observer.PreparationError):
            observer.prepare(self.args)
        self.args.target = "esp32"
        (self.source / "firmware/sdkconfig").write_text('CONFIG_IDF_TARGET="esp32c3"\n')
        with self.assertRaises(observer.PreparationError):
            observer.prepare(self.args)
        (self.source / "firmware/sdkconfig").unlink()
        self.args.target = "esp32c3"
        (self.source / "firmware/sdkconfig").write_text('CONFIG_IDF_TARGET="esp32c3"\nCONFIG_FREERTOS_TASK_PRE_DELETION_HOOK=y\n')
        with self.assertRaises(observer.PreparationError):
            observer.prepare(self.args)
        (self.source / "firmware/sdkconfig").unlink()
        self.baseline.write_text("{}")
        with self.assertRaises(observer.PreparationError):
            observer.prepare(self.args)
        self.assert_no_changes()

    def test_failed_output_does_not_publish_a_build_receipt(self):
        real_write = observer.write_file
        def fail_new_source(path, raw, **options):
            if str(path).endswith(observer.OBSERVER_C):
                raise OSError("explicit host output fault")
            return real_write(path, raw, **options)
        with mock.patch.object(observer, "write_file", side_effect=fail_new_source):
            with self.assertRaises(OSError):
                observer.prepare(self.args)
        self.assertFalse((self.source / observer.RECEIPT).exists())
        with self.assertRaises(observer.PreparationError):
            observer.prepare(self.args)

    def compile_observer(self, target):
        self.args.target = target
        source = self.directory / target
        shutil.copytree(self.source, source)
        self.args.source_root = source
        observer.prepare(self.args)
        include = self.directory / (target + "_fake_sdk")
        (include / "freertos").mkdir(parents=True)
        (include / "sdkconfig.h").write_text(f"#define CONFIG_FREERTOS_USE_TRACE_FACILITY 1\n#define CONFIG_FREERTOS_UNICORE 1\n#define CONFIG_FREERTOS_SMP 0\n#define CONFIG_FREERTOS_TASK_PRE_DELETION_HOOK 1\n#define CONFIG_IDF_TARGET_{target.upper()} 1\n")
        (include / "esp_heap_caps.h").write_text("""#include <stddef.h>
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
size_t heap_caps_get_free_size(unsigned);
size_t heap_caps_get_minimum_free_size(unsigned);
size_t heap_caps_get_largest_free_block(unsigned);
""")
        (include / "freertos/FreeRTOS.h").write_text("""#pragma once
#include <pthread.h>
typedef unsigned char StackType_t;
typedef unsigned UBaseType_t;
typedef int BaseType_t;
typedef void *TaskHandle_t;
typedef pthread_mutex_t portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER
void host_port_enter(portMUX_TYPE *);
void host_port_exit(portMUX_TYPE *);
#define portENTER_CRITICAL(mux) host_port_enter(mux)
#define portEXIT_CRITICAL(mux) host_port_exit(mux)
#define configMAX_TASK_NAME_LEN 16
#define pdTRUE 1
""")
        (include / "freertos/task.h").write_text("""#include "FreeRTOS.h"
typedef enum { eRunning, eReady, eBlocked, eSuspended, eDeleted, eInvalid } eTaskState;
typedef struct { const char *pcTaskName; unsigned xTaskNumber, usStackHighWaterMark, uxCurrentPriority; eTaskState eCurrentState; } TaskStatus_t;
void vTaskSuspendAll(void);
BaseType_t xTaskResumeAll(void);
UBaseType_t uxTaskGetNumberOfTasks(void);
UBaseType_t uxTaskGetSystemState(TaskStatus_t *, UBaseType_t, unsigned *);
UBaseType_t uxTaskGetStackHighWaterMark(void *);
void vTaskGetInfo(void *, TaskStatus_t *, BaseType_t, eTaskState);
""")
        harness = self.directory / (target + "_host.c")
        harness.write_text(r'''#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "freertos/task.h"
#include "esp_base_capacity_observer.h"
static int suspended;
static unsigned expected = 2, enumeration_calls;
static char task_name[16];
static _Thread_local int critical_depth;
static atomic_uint deletion_calls;
static unsigned replenish_budget, replenish_number = 1000;
static int replenish;
void vTaskPreDeletionHook(void *);
static void capture_deleted(const char *, unsigned, unsigned);
void host_port_enter(portMUX_TYPE *mux) {
    assert(critical_depth == 0); assert(pthread_mutex_lock(mux) == 0); ++critical_depth;
}
void host_port_exit(portMUX_TYPE *mux) {
    assert(critical_depth == 1); --critical_depth; assert(pthread_mutex_unlock(mux) == 0);
    /* 模拟刚离开短临界区就被正常cleanup任务抢占并补入事实。 */
    if (replenish && replenish_budget != 0) {
        replenish = 0; --replenish_budget;
        capture_deleted("refill_short", replenish_number++, 1400);
        replenish = 1;
    }
}
typedef struct { char name[16]; unsigned number, margin; } deleted_task_t;
static void capture_deleted(const char *name, unsigned number, unsigned margin) {
    deleted_task_t task = {.number = number, .margin = margin};
    strcpy(task.name, name); vTaskPreDeletionHook(&task);
    memset(task.name, 'x', sizeof task.name); /* TCB复用/释放后不能再借用名字。 */
}
size_t heap_caps_get_free_size(unsigned caps) { assert(caps == 3); return 32768; }
size_t heap_caps_get_minimum_free_size(unsigned caps) { assert(caps == 3); return 16384; }
size_t heap_caps_get_largest_free_block(unsigned caps) { assert(caps == 3); return 24576; }
void vTaskSuspendAll(void) { assert(!suspended); suspended = 1; }
BaseType_t xTaskResumeAll(void) { assert(suspended); suspended = 0; strcpy(task_name, "changed"); return 0; }
UBaseType_t uxTaskGetNumberOfTasks(void) { assert(suspended); return expected; }
UBaseType_t uxTaskGetSystemState(TaskStatus_t *tasks, UBaseType_t capacity, unsigned *runtime) {
    assert(suspended && capacity == 32 && !runtime); ++enumeration_calls;
    strcpy(task_name, "base_control");
    tasks[0] = (TaskStatus_t){task_name, 1, 2048, 5, eRunning};
    tasks[1] = (TaskStatus_t){"IDLE", 2, 1536, 0, eReady}; return 2;
}
UBaseType_t uxTaskGetStackHighWaterMark(void *task) { assert(!task); return 2048; }
void vTaskGetInfo(void *task, TaskStatus_t *info, BaseType_t stack, eTaskState state) {
    assert(critical_depth == 0 && stack == pdTRUE);
    if (task != NULL) {
        assert(state == eDeleted); const deleted_task_t *deleted = task;
        *info = (TaskStatus_t){deleted->name, deleted->number, deleted->margin, 4, eDeleted};
        atomic_fetch_add(&deletion_calls, 1);
    } else {
        assert(suspended && state == eInvalid);
        strcpy(task_name, "base_ota");
        *info = (TaskStatus_t){task_name, 3, 1300, 4, eRunning};
    }
}
/* 可复用host barrier；仅验证缓冲并发，不模仿MCU调度或生成实板值。 */
static pthread_mutex_t barrier_mux = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t barrier_condition = PTHREAD_COND_INITIALIZER;
static unsigned barrier_waiters, barrier_generation;
static void barrier(void) {
    assert(pthread_mutex_lock(&barrier_mux) == 0);
    const unsigned generation = barrier_generation;
    if (++barrier_waiters == 3) {
        barrier_waiters = 0; ++barrier_generation;
        assert(pthread_cond_broadcast(&barrier_condition) == 0);
    } else {
        while (generation == barrier_generation)
            assert(pthread_cond_wait(&barrier_condition, &barrier_mux) == 0);
    }
    assert(pthread_mutex_unlock(&barrier_mux) == 0);
}
static void *producer(void *argument) {
    const unsigned index = *(const unsigned *)argument;
    for (unsigned round = 0; round < 80; ++round) {
        barrier();
        capture_deleted(index == 0 ? "frp_short" : "mqtt_short", 10000 + index * 1000 + round, 1400);
        barrier(); barrier();
    }
    return NULL;
}
int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "bound") == 0) {
        for (unsigned i = 0; i < 64; ++i) capture_deleted("startup_short", 100 + i, 1400);
        replenish = 1; replenish_budget = 64;
        esp_base_capacity_observer_poll(1000);
        puts("FIRST_FLUSH_END"); replenish = 0;
        assert(replenish_budget == 0);
        esp_base_capacity_observer_poll(1001);
        assert(atomic_load(&deletion_calls) == 128 && enumeration_calls == 1); return 0;
    }
    if (argc == 2 && strcmp(argv[1], "concurrency") == 0) {
        pthread_t threads[2]; const unsigned indexes[2] = {0, 1};
        for (unsigned i = 0; i < 2; ++i)
            assert(pthread_create(&threads[i], NULL, producer, (void *)&indexes[i]) == 0);
        for (unsigned round = 0; round < 80; ++round) {
            barrier(); esp_base_capacity_observer_poll(1000 + round);
            barrier(); esp_base_capacity_observer_poll(1000 + round); barrier();
        }
        for (unsigned i = 0; i < 2; ++i) assert(pthread_join(threads[i], NULL) == 0);
        assert(atomic_load(&deletion_calls) == 160 && enumeration_calls == 1); return 0;
    }
    if (argc == 2 && (strcmp(argv[1], "reuse") == 0 || strcmp(argv[1], "overflow") == 0)) {
        const int overflow = strcmp(argv[1], "overflow") == 0;
        for (unsigned i = 0; i < (overflow ? 65U : 64U); ++i) capture_deleted("startup_short", 100 + i, 1400);
        puts("BEFORE_CONTROL"); esp_base_capacity_observer_poll(1000);
        for (unsigned i = 0; i < 64; ++i) capture_deleted("sdk_short", 200 + i, 1500);
        esp_base_capacity_observer_poll(1001);
        esp_base_capacity_observer_poll(1002);
        assert(enumeration_calls == 1); return 0;
    }
    capture_deleted("startup_short", 11, 1400); puts("BEFORE_CONTROL");
    esp_base_capacity_observer_poll(1000);
    capture_deleted("frp_short", 12, 1500);
    esp_base_capacity_observer_poll(3000); assert(enumeration_calls == 1);
    esp_base_capacity_observer_poll(6000); assert(enumeration_calls == 2);
    expected = 33; esp_base_capacity_observer_poll(11000); assert(enumeration_calls == 2);
    esp_base_capacity_observer_ota_exit();
    assert(!suspended); return 0;
}
''')
        output = self.directory / (target + "_host")
        command = [os.environ.get("CC", "cc"), "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra", "-Werror", "-pthread",
                   "-fsanitize=address,undefined", "-I", str(include), "-I", str((source / observer.OBSERVER_H).parent),
                   str(source / observer.OBSERVER_C), str(harness), "-o", str(output)]
        compiled = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(compiled.returncode, 0, compiled.stderr)
        result = subprocess.run([str(output)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        return result.stdout, include, command

    def test_real_c_execution_cadence_copied_names_bounds_and_parser_schema(self):
        for target in observer.TARGETS:
            with self.subTest(target=target):
                stdout, include, command = self.compile_observer(target)
                self.assertEqual(stdout.count(observer.MARKER), 1)
                self.assertNotIn("task=changed", stdout)
                self.assertIn("task=base_ota task_number=3 minimum_stack_bytes=1300", stdout)
                self.assertLess(stdout.index("BEFORE_CONTROL"), stdout.index("phase=task_predelete"))
                self.assertEqual(stdout.count("phase=task_predelete"), 2)
                self.assertIn("task=startup_short task_number=11", stdout)
                self.assertIn("task=frp_short task_number=12", stdout)
                log = self.directory / (target + "_host_fixture.log")
                log.write_text(stdout)
                report = capacity_observation.observe([log], target)
                self.assertEqual(report["memory_samples"], 3)
                self.assertEqual(report["complete_task_snapshots"], 2)
                self.assertEqual(report["incomplete_task_snapshots"], 1)
                self.assertEqual(report["malformed_resource_lines"], 0)
                self.assertFalse(report["observed_stack_gate_passed"])
                self.assertFalse(report["full_peak_or_native_lifecycle_qualification"])
                for wrong in ("#define CONFIG_FREERTOS_USE_TRACE_FACILITY 0\n#define CONFIG_FREERTOS_UNICORE 1\n",
                              "#define CONFIG_FREERTOS_USE_TRACE_FACILITY 1\n#define CONFIG_FREERTOS_UNICORE 0\n",
                              "#define CONFIG_FREERTOS_USE_TRACE_FACILITY 1\n#define CONFIG_FREERTOS_UNICORE 1\n",
                              f"#define CONFIG_FREERTOS_USE_TRACE_FACILITY 1\n#define CONFIG_FREERTOS_UNICORE 1\n#define CONFIG_FREERTOS_TASK_PRE_DELETION_HOOK 0\n#define CONFIG_IDF_TARGET_{target.upper()} 1\n",
                              f"#define CONFIG_FREERTOS_USE_TRACE_FACILITY 1\n#define CONFIG_FREERTOS_UNICORE 1\n#define CONFIG_FREERTOS_TASK_PRE_DELETION_HOOK 1\n#define CONFIG_FREERTOS_SMP 1\n#define CONFIG_IDF_TARGET_{target.upper()} 1\n"):
                    (include / "sdkconfig.h").write_text(wrong)
                    self.assertNotEqual(subprocess.run(command, capture_output=True).returncode, 0)

    def test_predelete_bss_capture_ring_reuse_overflow_and_concurrent_flush(self):
        for target in observer.TARGETS:
            with self.subTest(target=target):
                _, _, command = self.compile_observer(target)
                for mode in ("reuse", "overflow", "concurrency", "bound"):
                    with self.subTest(mode=mode):
                        result = subprocess.run([command[-1], mode], capture_output=True, text=True, timeout=30)
                        self.assertEqual(result.returncode, 0, result.stderr)
                        self.assertNotIn("task=xxxxxxxx", result.stdout)
                        exits = [line for line in result.stdout.splitlines() if " phase=task_predelete " in line]
                        self.assertEqual(len(exits), 160 if mode == "concurrency" else 128)
                        self.assertEqual(len({line.split(" task_number=")[1].split()[0] for line in exits}), len(exits))
                        if mode in ("reuse", "overflow"):
                            self.assertLess(result.stdout.index("BEFORE_CONTROL"), result.stdout.index("phase=task_predelete"))
                            self.assertIn("task=sdk_short task_number=263", result.stdout)
                        if mode == "bound":
                            before, after = result.stdout.split("FIRST_FLUSH_END")
                            self.assertEqual(before.count(" phase=task_predelete "), 64)
                            self.assertEqual(after.count(" phase=task_predelete "), 64)
                        log = self.directory / (target + "_" + mode + ".log")
                        log.write_text(result.stdout)
                        report = capacity_observation.observe([log], target)
                        self.assertEqual(report["memory_samples"], 1)
                        self.assertEqual(report["malformed_resource_lines"], 3 if mode == "overflow" else 0)
                        self.assertEqual(report["observed_stack_gate_passed"], mode != "overflow")
                        self.assertFalse(report["full_peak_or_native_lifecycle_qualification"])
                        if mode == "overflow":
                            self.assertEqual(result.stdout.count("ESP_BASE_LAB_TASK_EXIT_OVERFLOW dropped=1 limit=64"), 3)


if __name__ == "__main__":
    unittest.main()
