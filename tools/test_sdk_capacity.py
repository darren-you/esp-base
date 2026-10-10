"""用锁定受管 SDK 的实际源码验证分配路径和任务最终栈统计；不访问设备。"""
from pathlib import Path
import argparse
import os
import tempfile
import re
import subprocess

from check_sdk import check

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--idf-path', type=Path, required=True)
args = parser.parse_args()
check(args.idf_path)
root = Path(__file__).resolve().parent.parent
src = args.idf_path / 'components/freertos/FreeRTOS-Kernel'
tasks = (src / 'tasks.c').read_text()
header = (src / 'include/freertos/task.h').read_text()

def function(anchor):
    start = tasks.index(anchor)
    pos = tasks.index('{', start)
    level = 1
    pos += 1
    while level:
        if tasks.startswith('/*', pos):
            pos = tasks.index('*/', pos + 2) + 2
            continue
        if tasks[pos] == '{': level += 1
        elif tasks[pos] == '}': level -= 1
        pos += 1
    return tasks[start:pos]

struct = re.search(r'typedef struct \{\n    UBaseType_t created_instances;.*?\} TaskCapacityStats_t;', header, re.S).group()
create_begin = tasks.index('            pxNewTCB->uxTCBNumber = uxTaskNumber;')
create_end = tasks.index('        }\n        #endif /* configUSE_TRACE_FACILITY */', create_begin)
create_body = tasks[create_begin:create_end]
assert tasks.index('const UBaseType_t minimum =', tasks.index('    static void prvDeleteTCB( TCB_t * pxTCB )\n    {')) < tasks.index('portCLEAN_UP_TCB( pxTCB );')
prefix = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
typedef uint32_t UBaseType_t;
typedef int32_t BaseType_t;
typedef uint32_t configSTACK_DEPTH_TYPE;
#if TEST_STACK_WORD_BYTES == 1
typedef uint8_t StackType_t;
#else
typedef uint32_t StackType_t;
#endif
#define configMAX_TASK_NAME_LEN 16
#define configUSE_TRACE_FACILITY 1
#define portSTACK_GROWTH -1
#define pdTRUE 1
#define pdFALSE 0
#define tskSTACK_FILL_BYTE 0xa5
#define configUSE_NEWLIB_REENTRANT 0
#define configUSE_C_RUNTIME_TLS_SUPPORT 0
#define configSUPPORT_DYNAMIC_ALLOCATION 1
#define configSUPPORT_STATIC_ALLOCATION 1
#define portUSING_MPU_WRAPPERS 0
#define tskSTATIC_AND_DYNAMIC_ALLOCATION_POSSIBLE 1
#define tskDYNAMICALLY_ALLOCATED_STACK_AND_TCB 0
#define tskSTATICALLY_ALLOCATED_STACK_ONLY 1
#define tskSTATICALLY_ALLOCATED_STACK_AND_TCB 2
#define configASSERT assert
#define mtCOVERAGE_TEST_MARKER() ((void)0)
typedef struct { UBaseType_t xTaskNumber; } TaskStatus_t;
typedef struct {
    StackType_t *pxStack;
    UBaseType_t uxTCBNumber;
    char pcTaskName[configMAX_TASK_NAME_LEN];
    unsigned char ucStaticallyAllocated;
} TCB_t;
static int xKernelLock, critical_depth, suspend_depth;
static unsigned cleanup_calls, stack_free_calls, tcb_free_calls;
static UBaseType_t fake_live_count;
static UBaseType_t uxTaskNumber;
static void enter(int *lock) { (void)lock; ++critical_depth; }
static void leave(int *lock) { (void)lock; assert(critical_depth); --critical_depth; }
#define taskENTER_CRITICAL(lock) enter(lock)
#define taskEXIT_CRITICAL(lock) leave(lock)
static void vTaskSuspendAll(void) { ++suspend_depth; }
static int xTaskResumeAll(void) { assert(suspend_depth); --suspend_depth; return 0; }
static UBaseType_t uxTaskGetSystemState(TaskStatus_t *tasks, UBaseType_t limit, void *unused)
{
    (void)unused;
    assert(suspend_depth == 1);
    if (limit < fake_live_count) return 0;
    for (UBaseType_t i=0; i<fake_live_count; ++i) tasks[i].xTaskNumber=i+1;
    return fake_live_count;
}
'''
middle = r'''
static TaskCapacityStats_t xCapacityStats = {.completed_minimum=UINT32_MAX,.counters_valid=pdTRUE};
static void cleanup(TCB_t *tcb)
{
    assert(xCapacityStats.finalized_instances == cleanup_calls+1);
    ++cleanup_calls;
    memset(tcb->pxStack, 0, 128);
}
#define portCLEAN_UP_TCB(tcb) cleanup(tcb)
static void vPortFreeStack(void *stack) { (void)stack; ++stack_free_calls; }
static void vPortFree(void *tcb) { (void)tcb; ++tcb_free_calls; }
static void created(TCB_t *pxNewTCB)
{
    ++uxTaskNumber;
'''
tests = r'''
}
static _Alignas(8) unsigned char stacks[3][128];
int main(void)
{
    TCB_t instances[3] = {0};
    for (unsigned i=0; i<3; ++i) {
        memset(stacks[i],0xa5,128);
        stacks[i][120]=0;
        instances[i].pxStack=(StackType_t *)stacks[i];
        instances[i].ucStaticallyAllocated=(unsigned char)i;
        memcpy(instances[i].pcTaskName,"same_worker",12);
        created(&instances[i]);
        assert(instances[i].uxTCBNumber==i+1);
    }
    assert(xCapacityStats.created_instances==3);
    /* Early trace-delete could see 120 free bytes. The real stopped context uses another 24. */
    stacks[0][96]=0;
    prvDeleteTCB(&instances[0]);
    assert(xCapacityStats.completed_minimum*sizeof(StackType_t)==96);
    assert(xCapacityStats.worst_completed_instance==1);
    assert(stack_free_calls==1 && tcb_free_calls==1);
    stacks[1][32]=0;
    prvDeleteTCB(&instances[1]);
    assert(xCapacityStats.completed_minimum*sizeof(StackType_t)==32);
    assert(xCapacityStats.worst_completed_instance==2);
    assert(xCapacityStats.finalized_instances==2);
    assert(stack_free_calls==1 && tcb_free_calls==2);
    /* Static TCB+stack still needs final capture even though neither is freed. */
    stacks[2][64]=0;
    prvDeleteTCB(&instances[2]);
    assert(xCapacityStats.finalized_instances==3 && cleanup_calls==3);
    assert(stack_free_calls==1 && tcb_free_calls==2);
    assert(xCapacityStats.completed_minimum*sizeof(StackType_t)==32);
    assert(strcmp(xCapacityStats.worst_completed_name,"same_worker")==0);
    TaskCapacityStats_t stats;
    TaskStatus_t live[4];
    fake_live_count=0;
    assert(uxTaskGetCapacitySnapshot(live,4,&stats)==0);
    assert(stats.created_instances==3 && stats.finalized_instances==3 && stats.counters_valid);
    /* Removed-from-list but not finalized is a transient incomplete snapshot, not lost evidence. */
    stats=xCapacityStats;
    ++xCapacityStats.created_instances;
    fake_live_count=0;
    uxTaskGetCapacitySnapshot(live,4,&stats);
    assert(stats.created_instances-stats.finalized_instances != fake_live_count);
    assert(stats.counters_valid);
    fake_live_count=1;
    uxTaskGetCapacitySnapshot(live,4,&stats);
    assert(stats.created_instances-stats.finalized_instances==fake_live_count);
    assert(uxTaskGetCapacitySnapshot(live,0,&stats)==0);
    assert(critical_depth==0 && suspend_depth==0);
    xCapacityStats.created_instances=UINT32_MAX;
    created(&instances[0]);
    assert(!xCapacityStats.counters_valid && xCapacityStats.created_instances==UINT32_MAX);
    xCapacityStats.counters_valid=pdTRUE;
    uxTaskNumber=UINT32_MAX;
    xCapacityStats.created_instances=4;
    created(&instances[0]);
    assert(!xCapacityStats.counters_valid && xCapacityStats.created_instances==4);
    printf("task-final coverage passed; StackType_t bytes=%zu; SDK summary bytes=%zu\n", sizeof(StackType_t),sizeof(TaskCapacityStats_t));
    return 0;
}
'''
source = (prefix + struct + middle + create_body + '\n}\n' +
          function('    static configSTACK_DEPTH_TYPE prvTaskCheckFreeStackSpace( const uint8_t * pucStackByte )\n    {') + '\n' +
          function('    static void prvDeleteTCB( TCB_t * pxTCB )\n    {') + '\n' +
          function('    UBaseType_t uxTaskGetCapacitySnapshot(TaskStatus_t *tasks, UBaseType_t task_limit,') + '\n' + tests.removeprefix('\n}\n'))
workspace = tempfile.TemporaryDirectory(prefix='esp-base-sdk-capacity-')
work = Path(workspace.name)
out = work / 'task_capacity_extracted_test.c'
out.write_text(source); out.chmod(0o600)
for size in (1,4):
    binary=out.with_name('task_capacity_test_'+str(size))
    subprocess.run([os.environ.get('CC', 'cc'),'-std=c11','-Wall','-Wextra','-Werror','-O2','-g','-fsanitize=address,undefined',
                    '-DTEST_STACK_WORD_BYTES='+str(size),str(out),'-o',str(binary)],check=True)
    binary.chmod(0o700)
    subprocess.run([str(binary)],check=True)

allocator = work / 'allocator-capacity-test'
subprocess.run([os.environ.get('CC','cc'), '-std=c11','-Wall','-Wextra','-Werror','-O2','-g',
                '-fsanitize=address,undefined','-fno-sanitize=alignment',
                '-I',str(args.idf_path/'components/heap/tlsf/include'),
                str(args.idf_path/'components/heap/tlsf/tlsf.c'),
                str(root/'firmware/tests/allocator_capacity_test.c'),'-o',str(allocator)],check=True)
subprocess.run([str(allocator)],check=True)
workspace.cleanup()
