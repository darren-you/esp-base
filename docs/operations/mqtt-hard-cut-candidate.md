# MQTT 公开组件硬切软件候选

2026-09-23 在独立公开仓检出 `esp-base@10cb8514e8f7a3a55b8ec4622cce4f98a0f90eea` 上制作本软件候选，未刷入设备。目标组件为 `esp-mqtt@36c23dcdc44dd0c3df863b2ae635f8bc929ed860`，使用 ESP-IDF `fff9895c82d744c7237be8847347bdd1b07c6643` 与 esp-lwip `2758df4cd3666b3b2a5b53830148379326425c0d`。

## 源码与依赖范围

- 删除 Base 的 `firmware/components/mqtt_runtime`、其官方 Registry 清单和两份通用 MQTT host 测试；通用运行层及其回归由公开 `esp-mqtt` 唯一拥有。
- `mqtt_integration` 实验应用直接调用 `emqtt_`，Base 继续从原持久 UUID 装配 ClientID，以及原 `esp-base-lab/<UUID>/in|out|extra|status` Topic、LWT、ACL 与实验输入语义。私有实验 header 在下次构建前须将配置类型改为 `emqtt_config_t`；这不要求变更有效凭据内容。
- `device_protocol/idf_component.yml` 统一声明 Git 完整 SHA，使普通与实验应用共用一个 `firmware/dependencies.lock`。普通应用目前没有 MQTT 客户端调用、网络命令或结果 ACK；声明依赖不等于功能接入。

## 本机验证

| 检查 | 结果 |
| --- | --- |
| Base host ASan/UBSan | `bash firmware/tests/run_host_tests.sh` 全部通过；MQTT 通用测试已归公开仓 |
| 普通 ESP32-C3 编译 | `esp_base.bin` 786336 字节，SHA-256 `16c2e2ed19fca7d2617b430ee2941fca7de40231d76a18db2f3f193680219104` |
| 实验 ESP32-C3 编译 | 只用公开 Git 依赖及仓外无效地址/CA 占位输入，`esp_base_mqtt_lab.bin` 889712 字节，SHA-256 `66ced7026e8fef88cf0fc6006eedfbdb4d282a50aeaeb25a1eb90a8afbf9f98a` |
| 锁文件 | 普通构建与实验构建后均为 SHA-256 `24bfed7d0997577ea2c75dca973768d3ac84ccd5b782c03f2e511b58a4289fc0`；唯一 `mqtt` 来源为公开 Git 完整 SHA，无 `espressif/mqtt` Registry 项 |
| 组件与最终符号 | 两种构建都只登记 `managed_components/mqtt` 一份 MQTT 组件；普通 ELF 无 `emqtt_create` / `esp_mqtt_client_init`，实验 ELF 各有一份，均无旧 `esp_base_mqtt_` 符号 |
| 普通应用明文门禁 | 独立 sdkconfig 设置 `CONFIG_EMQTT_PLAINTEXT_LAB=y` 时 CMake 明确拒绝 |

以上仅证明源码切换、锁文件与编译链接。旧 Base 适配层在 2026-09-22 的实板矩阵继续保留为历史证据，不能转记到本候选。`esp-mqtt` 独立 Broker/C3 与 100 次资源验收、Base 的完整消息→权限/幂等→执行→结果 ACK、MQTT/FRP/Wi-Fi/OTA 组合资源，以及实板安全迁移尚未闭合。P3-08 不据此标记验收；本轮没有 Flash、eFuse、NVS 或生产凭据写入。

## SDK fork 升级复验

同日从公开 `esp-base@a0eabdfdafab97a68c6dcbfb63e305801023922b` 的独立 clone 将构建源锁到公开 `darren-you/esp-idf@855937cf9dcee13ee9c423fb0319238cdc8d53fd`、`esp-lwip@2758df4cd3666b3b2a5b53830148379326425c0d`，MQTT 精确依赖升级到 `esp-mqtt@9cac455b0184420353ff0283df3f100abaac3e6b`。旧 `dependencies.lock` 仍指向 `36c23dcdc44dd0c3df863b2ae635f8bc929ed860` 时，Component Manager 会保留旧源码，且其 SDK 守卫正确拒绝新 IDF；重新从 manifest 解析后得到本次提交的单一锁文件。

| 本机检查 | 结果 |
| --- | --- |
| SDK 核对 | 完整独立 fork checkout 的两个提交、唯一 lwIP gitlink 差异和其他子模块均通过；旧 IDF checkout 被精确版本检查拒绝；CMake 还核对实际 lwIP 组件路径 |
| 默认 C3 | `esp_base.bin` 789232 字节，SHA-256 `88b133e93b088c68ee403bb408f85767459f6f79d9c9c2b992972385d5ce51a1` |
| 隔离实验 C3 | 使用仓外无效域名/CA 占位输入，`esp_base_mqtt_lab.bin` 892624 字节，SHA-256 `a39205f96850f9832772570c531e8ce0880d4fca95e298d74305fa425601a47a` |
| 测试键签名 C3 | 仓外临时 RSA-3072 密钥构建 `esp_base.bin` 1052672 字节，SHA-256 `900a7a62ea39a172bc6d238cfd01b2e8b7aa737eb369b8131c2062e271b80aa1`；`espsecure` 验证 RSA 签名块 0 有效 |
| MQTT 锁 | 三次构建后同一 `firmware/dependencies.lock` SHA-256 `92a779278fad02773e30a3b27aa0fd39f368b71031a96afad284c840cf0dd932`；唯一 `mqtt` 源为公开 Git 完整 SHA |
| Base host | `bash firmware/tests/run_host_tests.sh` 全部 ASan/UBSan 通过，覆盖配置、OTA 故障裁决、时间和 Wi-Fi 初始化故障 |

SDK 检查负例使用独立临时 worktree：修改非 lwIP 的 IDF `README.md`、修改 lwIP 已跟踪文件、暂存 IDF 文件、使 micro-ecc 子模块 HEAD 漂移以及让其他子模块未初始化，均被拒绝。负例未修改本仓锁、正式 SDK checkout 或真实设备。

本轮仅本机源码与构建复验；测试签名密钥留在仓外临时目录，未连接 Broker/真实板卡，也未验证 SDK fork 在真实 Flash I/O 擦除错误下的行为。普通 Base 的 MQTT 客户端、设备命令与结果 ACK 仍未接入，P3-08 继续未验收。
