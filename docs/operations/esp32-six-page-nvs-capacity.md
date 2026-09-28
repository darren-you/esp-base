# ESP32 六页 NVS 合成容量验证

## 2026-09-28：最近八条产品操作账本同分区复跑

在上一轮最大 7,618 字节 Base v3 配置、186 字节 OTA V2 合成形态与 288 字节 Container ECS2 初态的同分区压力上，加入单个 **910 字节** `base_product/operations` 账本。它按当前八条槽位格式保存连续序号、请求指纹和结果；每代先提交 `PREPARED` 再提交 `SUCCEEDED`，两次均逐字节读回。探针只按真实编码布局验证 NVS 容量和页回收，未调用尚未开放的产品写命令。

固定 ESP-IDF `578cf89c343e388db43ba1f4ddcd602fedcb763c`、esp-lwIP `2758df4cd3666b3b2a5b53830148379326425c0d`、Espressif Xtensa QEMU `9.2.2 (esp_develop_9.2.2_20260417)`；从空锁经 Component Manager 正式解析 `esp-container@f82e4b8f57eb6ae75309d5cfb7472feef2380912` 与 WAMR `c10736fffdf26d7c2ae234e05aa712df112eb6bf`。全 `0xff` 的独立 4 MiB 合成 Flash 在 ESP32 六页 `base_store@0x3fa000/0x6000` 完成阶段 1 的 revision 1–3 和旧 CAS 拒绝、新进程阶段 2 的 4–100，以及再次新进程阶段 3 的 revision 100 四类记录读回。100 条 `PROBE_STEP` 均显示 `ota=ok container=ok product=ok`，最终 `PROBE_RESTART_MATCH=1 container_decoded=1`，`used/free/available/total=297/459/333/756`。固定 SDK 官方 `nvs_tool.py -i -d none` 返回 0，五页 `CRC32: OK`、一页 Empty，最高页序号 299。合成 `product_pkgs`、`at_old_raw` 和 `frp_scratch` 仍逐字节为 `0xff`。

仓内探针源码、runner 与 ESP32 官方生成锁 SHA-256 分别为 `66d74021fb4257515f3205637b23b72c1383d90b41a8852fbb92596c4cd76ab9`、`bcaeffe63d1154244b4b7dac53de9739753b3ab5a2777981c016bc130aa222fe`、`b887ad04fcd677577df9f4ec189144aa2da7b96e4e3b7999b91e1926949951a4`。三阶段脱敏日志 SHA-256 依次为 `83db0ef1572bb27d45a28b161c92ebab70ceeabc07a513ab8d260816059e923b`、`6d9a821355a47d0a94dd36204e081072fbf3c369bb54a60f3f50bad1a764a818`、`1ab4da0a3ef06193c9bc4e7577d9b3a446fe5698dc8605145c848e0928d721d0`；最终 NVS 提取摘要为 `d5204379b1f5fe5c5415fb9c32a9ca63223e46494cff17e3f5c6699e412ea7a0`。原始串口日志、合成 Flash、parser 输出和构建目录留在仓外。

本轮只支持固定 SDK/QEMU 的正常写入、页回收和新进程读回结论。八条上限仍须结合真实 Flash 磨损、写入中断电与正式产品写入链冻结；旧 AT 迁移、签名双槽、网络并发和实板五能力未因此验收。

## 2026-09-27：此前三记录容量验证

2026-09-27，以 Base `2af15411c27d585361f516b03a38ec6a70d7d776` 建立独立工作树，扩展[现有容量探针](../../firmware/tests/nvs-capacity-probe/README.md)，在 `mac-work-1` 的仓外 4 MiB 合成 Flash 上运行。固定 ESP-IDF 为 `578cf89c343e388db43ba1f4ddcd602fedcb763c`，esp-lwIP 为 `2758df4cd3666b3b2a5b53830148379326425c0d`；Espressif Xtensa QEMU 为 `9.2.2 (esp_develop_9.2.2_20260417)`，`-machine help` 明确包含 `esp32`。目标专用依赖锁解析公开 `esp-container@bf52b17a26e51d35a261bf852ac0c9cde76adefc` 与 `wasm-micro-runtime@26c235e53e29acd8b43abe7f3b524577bd4d1ae5`。本测试没有接触板卡、私有 Flash 或真实配置。

合成分区表经固定 SDK 生成和回读：`product_pkgs@0x260000/0x186000`、`at_old_raw@0x3e6000/0x4000`、`frp_scratch@0x3ea000/0x10000`、`base_store@0x3fa000/0x6000` 与 ESP32 条件几何的尾部一致；测试专用 `factory@0x10000/0x100000` 取代产品双 OTA app。生成表 SHA-256 为 `f7c2b9acde45620cac0a81e6177567ad39f476b696a48722df8e5a3d4181d2fa`。生成 `sdkconfig` 核对为 `esp32`、4 MiB、自定义表、UART0 控制台，NVS 加密未启用。正式 ESP32 CSV、签名启动链和设备均未修改。

每代在同一 `base_store` 写入并读回三份记录：最大 **7,618 字节** Base v3 规范配置走真实 `esp_base_remote_config_commit_verified` 和 revision CAS；当前 **186 字节** OTA V2 合成形态直接写 `base_ota/operation`，以 SHA 前四字节承载测试 revision，未调用正式收据解码或 OTA 注册策略；**288 字节** Container ECS2 初态由锁定组件的 `econtainer_slots_initialize` 正式编码，此后只变 sequence 并重算 CRC，经真实 IDF provider 写 `base_pkg/slots`，每代由 `econtainer_slots_load` 正式解码。OTA 形态和 ECS2 压力序列只验证容量，不代表 100 次合法 OTA 或包状态迁移。

| 阶段 | QEMU 行为 | 结果 |
| --- | --- | --- |
| 1 | 全 `0xff` Flash 提交 revision 1–3，再以旧 revision 请求 CAS | 3 次写读成功；旧 CAS 返回预期冲突 |
| 2 | 新进程先读回 revision 3，再提交 4–100 | 重启读回一致；97 次写读成功 |
| 3 | 再次新进程读取同一 Flash | `PROBE_RESTART_MATCH=1 container_decoded=1`，配置、OTA 形态、ECS2 均为 revision 100 |

100 代配置 SHA-256 全部不同，100 条 `PROBE_STEP` 的 OTA／Container 回读均为 `ok`。`nvs_get_stats` 全程 `used=264–265`、`free=491–492`、`available=365–366`、`total=756`，最终为 `265/491/365/756`。固定 SDK 官方 `nvs_tool.py -i -d none` 对最终六页返回 0：五页 `CRC32: OK`、一页 Empty，最高页序号 209。最终合成 Flash 中的 `product_pkgs`、`at_old_raw` 与 `frp_scratch` 仍逐字节全为 `0xff`。页序号与 Empty 页说明合成正常路径经历换页和回收；这不证明实际掉电或 Flash 磨损寿命。修改后的共用代码另在 C3 六页 QEMU 完成阶段 1 回归；C3 已公开的完整三阶段结果仍以其[原记录](c3-eight-page-nvs-capacity.md)为准。

可复核的仓内输入 SHA-256：

| 输入 | SHA-256 |
| --- | --- |
| `CMakeLists.txt`／`main/CMakeLists.txt` | `b495d38962d6bee75dbb7d41f4753eb52fc0985719ea348d2f9dcbe8a8482daf`／`5fe5880d2adf968935a7411aacacaa67e5f4850458bc63e62559c1e05d23d921` |
| `main/nvs_capacity_probe.c`／`run-qemu.py` | `6c4542797302ec271f978dde0d5238b955f207fd934b6337cbe1b21f26d2cb2f`／`7fe7bb455ed77234dd0c09128317838a20b9b6e62f9239bb72c91c5bcd3d67ca` |
| `partitions-esp32-6page.csv` | `1ef4db1deb767e123b182d9fddc94a3a81cc961473879b9ef61bb91836d1e79c` |
| `sdkconfig.defaults.esp32`／`.esp32.6page` | `233e8130d554bba042e51e22c7cbce4fe335e565429fdb5a3d05062729be0cff`／`0ac81ba9c8270576f2220a70b9d4e13b9dc2063866ceee74e30e5d091a7bdbfb` |
| `dependencies.lock.esp32` | `6fe5b2b854f685facca51ff9ee441e15cd4388fa2dbfa56ea72c7eed738b141a` |

仓外原始串口日志、合成 Flash、提取 NVS、构建目录位于 `mac-work-1:/private/tmp/esp32-six-page-nvs-run-20260927/`；三阶段 `PROBE_` 日志 SHA-256 依次为 `3938635533099e68308646e88b093bc6c7b4024e8f61c949d0e34d5e0d33672c`、`a41f38eba13ec328181941740a8c8cdc3e17bff1a60108bac71224020c5eb08d`、`900e4722012908d8e1220861c18716f88f34db515831e6668b09c618f460d643`；parser 日志为 `37742f91c9cf23b50f71f1100baedf963928b3904219393aa143d152fea80b70`，最终 24 KiB NVS 为 `9cab3bacbfddd62b80ece6204d447187abcc4f1833682738dcb54d4e2712ff74`。它们没有加入仓库，复现命令见探针 README。

本结果只支持固定 SDK、Espressif QEMU、合成正常写入与重启下的 ESP32 六页容量判断。未验证写入中掉电、旧 AT 归档或迁移、真实旧 `base_store`、签名双槽与 bootloader、OTA 回滚、FRP scratch 并发、实际 package 状态、真实五能力内存峰值或实板运行。正式 P6-03 布局与迁移仍未验收。
