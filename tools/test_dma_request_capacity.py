"""比较锁定 SDK 的实际内部 DMA 归一化、目标布局与 TLSF；不访问设备。"""
import argparse
import ctypes
import importlib.util
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

from check_sdk import check


def function(source, declaration):
    match = re.search(re.escape(declaration) + r'[^;{]*\{', source)
    if match is None:
        raise ValueError('实际 SDK 函数体未找到: ' + declaration)
    start = match.start()
    brace = source.index('{', start)
    level, position = 1, brace + 1
    while level:
        if source.startswith('/*', position):
            position = source.index('*/', position + 2) + 2
            continue
        if source[position] == '{': level += 1
        elif source[position] == '}': level -= 1
        position += 1
    return source[start:position]


def compiler_args(entry):
    arguments = shlex.split(entry['command'])
    clean, index = [], 0
    while index < len(arguments):
        if arguments[index] in ('-c', '-o'):
            index += 2
        else:
            clean.append(arguments[index])
            index += 1
    return clean


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--idf-path', type=Path, required=True)
    parser.add_argument('--compile-commands', type=Path, action='append', required=True)
    parser.add_argument('--json', action='store_true')
    args = parser.parse_args()
    check(args.idf_path)
    root = Path(__file__).resolve().parent.parent
    spec = importlib.util.spec_from_file_location('capacity_parser', root / 'tools/analyze_capacity.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    total, targets, build_results = 0, set(), []
    with tempfile.TemporaryDirectory(prefix='esp-base-dma-request-') as directory:
        work = Path(directory)
        for sequence, commands_path in enumerate(args.compile_commands):
            commands = json.loads(commands_path.read_text())
            def entry(relative):
                source = str(args.idf_path / relative)
                rows = [row for row in commands if row['file'] == source]
                if len(rows) != 1:
                    raise ValueError('冻结编译入口缺失或非唯一: ' + relative)
                return rows[0]
            hardware = entry('components/esp_hw_support/heap_align_hw.c')
            flags = subprocess.check_output(compiler_args(hardware) + ['-E', '-dM', hardware['file']],
                                            cwd=hardware['directory'], text=True)
            target = next((name for name in ('esp32c3', 'esp32')
                           if '#define CONFIG_IDF_TARGET_' + name.upper() + ' 1\n' in flags), None)
            if not target:
                raise ValueError('只核对真实 ESP32-C3／ESP32 编译输入')
            if ('#define CONFIG_HEAP_POISONING_DISABLED 1\n' not in flags or
                    re.search(r'^#define CONFIG_HEAP_TASK_TRACKING [1-9]', flags, re.M)):
                raise ValueError('只核对当前无 owner／poison 的锁定 recipe')
            targets.add(target)
            cache = entry('components/hal/' + ('esp32/cache_hal_esp32.c' if target == 'esp32' else 'cache_hal.c'))
            tlsf = entry('components/heap/tlsf/tlsf.c')
            for key in ('CONFIG_SPIRAM', 'SOC_CACHE_INTERNAL_MEM_VIA_L1CACHE',
                        'SOC_PSRAM_DMA_CAPABLE', 'SOC_DMA_CAN_ACCESS_FLASH'):
                if re.search(r'^#define ' + key + r' [1-9]', flags, re.M):
                    raise ValueError('本轮已核对的内部 DMA 构建选择发生变化: ' + key)
            hardware_source = subprocess.check_output(compiler_args(hardware) + ['-E', '-P', hardware['file']],
                                                      cwd=hardware['directory'], text=True)
            cache_source = subprocess.check_output(compiler_args(cache) + ['-E', '-P', cache['file']],
                                                   cwd=cache['directory'], text=True)
            adjust = function(hardware_source, 'void esp_heap_adjust_alignment_to_hw(')
            # 调用方 size_t 是目标 uint32_t；host allocator 自身仍按宿主 ABI 编译。
            adjust = re.sub(r'\bsize_t\b', 'uint32_t', adjust)
            cache_body = function(cache_source, 'uint32_t cache_hal_get_cache_line_size(')
            probe = work / f'target-layout-{sequence}.c'
            probe.write_text('#include "' + tlsf['file'] + '"\n'
                             'const unsigned dma_target_layout[] = {sizeof(void *), sizeof(size_t), '
                             'sizeof(block_header_t), block_size_min, ALIGN_SIZE};\n')
            assembly = work / f'target-layout-{sequence}.s'
            subprocess.run(compiler_args(tlsf) + ['-S', str(probe), '-o', str(assembly)],
                           cwd=tlsf['directory'], check=True, capture_output=True, text=True)
            data = assembly.read_text().split('dma_target_layout:', 1)[1].split('.size', 1)[0]
            layout = [int(value) for value in re.findall(r'\.(?:word|long)\s+(\d+)', data)[:5]]
            if layout != [4, 4, 16, 12, 4]:
                raise ValueError('实际目标 TLSF layout 漂移: ' + str(layout))
            test = work / f'dma-{sequence}.c'
            test.write_text(r'''
#include "tlsf.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
typedef int cache_type_t;
typedef int esp_err_t;
static unsigned cache_calls;
static __attribute__((unused)) uint32_t cache_ll_get_line_size(uint32_t level, int type, unsigned id)
{ (void)type; (void)id; assert(level != 0); return 32; }
void __assert_func(const char *f, int n, const char *func, const char *expr)
{ (void)f; (void)n; (void)func; (void)expr; abort(); }
''' + cache_body + r'''
static esp_err_t esp_cache_get_alignment(uint32_t caps, uint32_t *alignment)
{ assert(!(caps & (1U<<10))); ++cache_calls; *alignment=cache_hal_get_cache_line_size(0, 1); return 0; }
''' + adjust + r'''
bool tlsf_check_hook(void *p, size_t size, bool free_block)
{ (void)p; (void)size; (void)free_block; return true; }
void block_absorb_post_hook(void *p, size_t size, bool free_block)
{ (void)p; (void)size; (void)free_block; }
static _Alignas(64) unsigned char pool[65536];
int main(void) {
    tlsf_t tlsf=tlsf_create_with_pool(pool, sizeof(pool), 0); assert(tlsf);
    void *slots[24]={0}; unsigned seed=17;
    const uint32_t masks[]={8,2056,2060}, aligns[]={1,2,4}, sizes[]={1,12,24,100,104,116,120,232,1599,1600,4097,26001};
    for(unsigned step=0;step<240;++step) {
        seed=seed*1664525U+1013904223U; unsigned slot=seed%24;
        if(slots[slot]) {tlsf_free(tlsf,slots[slot]);slots[slot]=NULL;}
        else slots[slot]=tlsf_malloc(tlsf,32+seed%2048);
        for(unsigned c=0;c<3;++c) for(unsigned a=0;a<3;++a) for(unsigned s=0;s<12;++s) {
            uint32_t raw=sizes[s], size=raw, alignment=aligns[a], caps=masks[c];
            esp_heap_adjust_alignment_to_hw(&alignment,&size,&caps);
            assert(caps==masks[c] && alignment==aligns[a]);
            assert(size==((raw+alignment-1)&~(alignment-1)));
            tlsf_capacity_stats_t before; tlsf_get_capacity_stats(tlsf,&before);
            void *p=tlsf_malloc(tlsf,size);
            /* 小于24B的host minimum block不作为32bit MCU allocator反例。目标12B已由真实交叉编译证明。 */
            if(raw>=24 && before.minimum_largest_allocatable_bytes>=((raw+3)&~3U)) assert(p);
            printf("%u %u %u %u %zu %u\n",raw,caps,alignment,size,
                   before.minimum_largest_allocatable_bytes,p!=NULL);
            tlsf_free(tlsf,p);
        }
    }
    assert(cache_calls==240*3*3*12); assert(tlsf_check(tlsf)==0);
    for(unsigned i=0;i<24;++i)tlsf_free(tlsf,slots[i]);
    tlsf_destroy(tlsf);
    return 0;
}
''')
            binary = work / f'dma-{sequence}'
            subprocess.run([os.environ.get('CC','cc'), '-std=c11','-O2','-g','-Wall','-Wextra','-Werror',
                            '-Wno-unused-parameter',
                            '-fsanitize=address,undefined','-fno-sanitize=alignment',
                            '-I',str(args.idf_path/'components/heap/tlsf/include'),
                            str(args.idf_path/'components/heap/tlsf/tlsf.c'),str(test),'-o',str(binary)],
                           check=True,text=True)
            output = subprocess.check_output([str(binary)], text=True)
            for line in output.splitlines():
                raw,caps,alignment,normalized,bound,succeeded = map(int,line.split())
                evidence={'target':target,'allocator_recipe':module.REQUEST_RECIPE,
                          'requests':[{'id':'dma','size_bytes':raw,'caps':caps,'alignment_bytes':alignment}]}
                frame={'begin':{'uptime_ms':0},'regions':[{'start':1,'available_at_heap_init':True,
                       'caps0':caps,'caps1':0,'caps2':0,'min_largest_request_bytes':bound,'min_free_bytes':bound}]}
                result=module._provided_request_fit(evidence,{},frame)['requests'][0]
                assert result['search_size_bytes']==max(12,(normalized+3)&~3)
                if raw>=24 and result['status']=='fit': assert succeeded
                total += 1
            build_results.append({'target':target,'target_layout':layout,'dma_cases':len(output.splitlines()),
                                  'host_allocator_pointer_bytes':ctypes.sizeof(ctypes.c_void_p),
                                  'scope':'实际目标归一化＋32bit布局；host TLSF成功充分条件比较限request>=24B'})
    if targets != {'esp32c3','esp32'}:
        raise ValueError('须同时提供两目标冻结 compile_commands')
    result={'passed_dma_cases':total,'build_results':build_results,'device_access':False,'r5_qualified':False}
    if args.json:
        print(json.dumps(result,ensure_ascii=False))
    else:
        print('DMA request capacity\n  result        pass\n  builds        '+str(len(build_results))+
              '\n  targets       esp32c3 / esp32\n  DMA cases     '+str(total)+
              '\n  scope         target normalization/layout + host TLSF sufficient fit (request >= 24 B)'+
              '\n  qualification software only; R5 remains unverified')


if __name__ == '__main__':
    try:
        main()
    except subprocess.CalledProcessError as error:
        raise SystemExit(error.stderr or str(error)) from error
