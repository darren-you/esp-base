# P6-03：FRP 阶段复用新锁的双目标签名 QEMU 检查点

2026-09-27。以公开 Base `1fe24302f5ec94dfc30180eb86369dbdc62774b8` 和唯一 FRP `0af12209ee731617e635684309c026ae6b49c5ae` 为输入，在 `mac-work-1:/private/tmp/esp-base-1fe2430-frp-0af1220-qemu-20260927/` 构建测试键签名产品镜像。此次从 Base 精确提交重新归档源码，未复用较早的签名 app、ECS2 种子、Flash 或 QEMU PASS；正式分区、设备和密钥未改。

## 源码与镜像输入

- 固定 ESP-IDF `578cf89c343e388db43ba1f4ddcd602fedcb763c`、lwIP `2758df4cd3666b3b2a5b53830148379326425c0d`。官方 Component Manager 为两个目标分别重新求解，所得 `dependencies.lock` 与 Base 提交的 C3、ESP32 锁逐字节一致，SHA-256 分别为 `d6fbd852c038d3dd3d62fa8afdf4c88520dcf007df300555d77d362051200b6d`、`6cbdf62f7230d90f0bd1925460ddb852eaeca64b04bbef77f7c9362790992b69`。
- Container、FRP、OTA、MQTT、WAMR 与 cJSON 六项均为本轮新下载的受管组件；固定 Component Manager 的内容校验逐目录重算，与各自目标锁匹配。五个 Git 组件还按锁定完整提交与 Component Manager 文件过滤规则逐文件比对：Container 25、FRP 179、OTA 11、MQTT 428、含子模块 WAMR 1856 个文件均一致，仅排除动态 `.git` 元数据。cJSON 按官方注册组件内容摘要核对。未使用只读取 `.component_hash` 文件的旧审计方式。
- C3 只在仓外采用 scratch 候选 CSV 和对应 OTA policy；ESP32 只在仓外采用含 `frp_scratch@0x3ea000/0x10000`、六页 `base_store@0x3fa000/0x6000` 的候选 CSV。真实分区表解码与各自 CSV 全条目一致，ESP32 `product_pkgs` 上限按 CSV 解析为 `0x186000`。官方 `espsecure` 验证 C3 RSA v2、ESP32 ECDSA v1 app 与 ESP32 签名分区表，官方容量门均通过。两份签名 app 都是本次新构建产物，与前一个[只做静态容量的 Base 检查点](frp-session-phase-union-base-dependency-checkpoint.md)中的 app 摘要不同，不能混用 ECS2 或 Flash。

| 目标 | 本轮签名 app 长度／SHA-256 | 双 `0x120000` 槽单槽剩余 | 全新种子 Flash SHA-256 |
| --- | --- | ---: | --- |
| C3 | `0x111000`／`a45073423839100aaa7d717d78870c85b3937403df6b23ce9ec3d396c3ba8e64` | 61,440 B | `de3e9cff6c9032450c8948c2dc7b0bd032077c20000fe3c4c5dba26f9ba40a74` |
| ESP32 | `0x10fff4`／`52259ea74ea7b3b1610a2c1f9f3080a88de5b264f694f0b918e57c6343b740ef` | 65,548 B | `e8b7a7859a23f29b77d8cec6123e50b62cf55f8e4d3e81ac6bafa903d089f504` |

两目标各自用本轮签名 app 的**完整** SHA-256 重新生成真实签名 ABI 2 counter 包的 ECS2 sequence 6 `CONFIRMED` 种子，再独立装配 4 MiB Flash。scratch 起始为本轮独有非空输入，用以检查正式 boot recover。两个 QEMU 串行运行，端口、进程和 Flash 不共享。

## 无网络产品启动与读回

固定 SDK QEMU 中，C3 只对模拟器缺失的 ADC2 校准入口做一次 GDB 跳过；ESP32 不需要该跳过。两目标都断在正式 Container 产品 `RUNNING`、`open.sequence=6`，随后到达 Base `READY`；ESP32 UART 也记录两个真实状态标记。ESP32 第一次 runner 将 Xtensa GDB 指向不存在的工具目录，QEMU 因 `-S` 一直暂停，尚未启动 guest；保留失败日志后，只修正仓外 GDB 路径并使用全新运行 Flash 重试。表中 ESP32 数据只来自第二次有效运行。

| 目标 | 截至 `READY` 的内部 8BIT 堆历史最低空闲 | `READY` 堆空闲／当刻最大连续块 | 产品线程到 `RUNNING` 的最低未用栈 |
| --- | ---: | ---: | ---: |
| C3 | 54,104 B | 61,544／45,056 B | 5,140 B |
| ESP32 | 52,596 B | 56,380／43,008 B | 5,020 B |

堆最低空闲来自固定 SDK `heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)`，因此涵盖该次启动至 `READY` 的历史；最大连续块只在 `READY` 瞬时采集，不能称为全程最低。产品线程栈只在 `RUNNING` 采集，不代表后续 event、stop 或联网负载的最坏深度。

| 目标 | 原始 GDB／UART SHA-256 | QEMU 后 Flash SHA-256 | 逐区读回 |
| --- | --- | --- | --- |
| C3 | GDB `80f876abfce95370ae56c59085d629a647c470d5e0e3ba4b059260bdec307c29` | `cc1f1135c9e3b9df96b57717588dcb004e23006f7f26f7d4da362d9d96e0ac8a` | app、包区、`base_store` 与本轮种子逐字节相同；scratch 全 `0xff` |
| ESP32 | GDB `8a058e44e742e119c79b5a97148d1c281c7b265b6b69e328a008c9988b7beb57`；UART `4f3834c157ae78d1d0b0244fd7d07fd6ec53c429eec9175f52771a0902df6fff` | `66338441a6282b9c217e087a88f608021d63f97d9535a4fe51aa53ff1628fb59` | app、包区、`base_store` 与本轮种子逐字节相同；scratch 全 `0xff` |

固定 IDF 的 NVS parser 重新读取两份运行后 Flash，`base_pkg` 和 `slots` 数据／索引存在，页与条目元数据 CRC 通过；两份产品 NVS 内容逐字节等于各自本轮 ECS2 种子。上述签名启动切片满足 48 KiB 内部堆历史最低空闲、`READY` 24 KiB 最大连续块和 1 KiB 产品线程栈余的初始观察门。

仓外机器收据保留精确输入和原始文件：`strict-component-audit.json` SHA-256 `66e71c53f922c4b338de91e247c94eaebc1fe0d64146d403870315435fd565e1`、`git-source-file-audit.json` `a734168d4b46163bd0da5740ed244f9b90725f088111d3838ce0b3d26eb07bb0`、`signed-build-verification.json` `e90772ad298b1992ffcfac1fadb95157b64169c25026e0e20d1614a665ee55e3`、`new-flash-manifest.json` `0d382b0aa74668036d30d97c0ea9adb1f6041595ac5e6fdb60e47c2a22c1ed6e`、`new-flash-readback.json` `a75db167bd6fd3891d0ebdd6eb74e825f9431966a996db9e07e2af698934c11b`。这些文件和上表原始 GDB、UART、Flash 位于同一仓外目录。

## 验收边界

这两份原样产品镜像没有 FRPS、Broker、HTTPS 网络连接；未测试 guest event／stop、满长 FRP Flash 记录、MQTT／OTA 同存、实体 Flash 时延、掉电或旧持久数据迁移。正式 Base 的本次启动可信时间门仍未由无网络 QEMU 满足。静态会话对象缩小 872 B 与本轮堆读数不能机械归因相加；P6-03/P7-02 保持进行中。
