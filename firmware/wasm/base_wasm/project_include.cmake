# WAMR（WASM 宿主的解释器）自己的 Kconfig 没有覆盖的构建开关。ESP-IDF 在处理各组件的 CMakeLists.txt 之前，先在
# 工程作用域里包含构建闭包内每个组件的 project_include.cmake，所以这里设的变量 WAMR 的组件读得到。
# AOT、WASI、线程和内置 libc 在 sdkconfig.defaults 里关闭。
set(WAMR_BUILD_INSTRUCTION_METERING 1) # 每次调用有指令预算：应用死循环时能停下来
set(WAMR_BUILD_BULK_MEMORY 0)          # 应用的目标 wasm32v1-none 不产生这些指令
set(WAMR_BUILD_SIMD 0)
