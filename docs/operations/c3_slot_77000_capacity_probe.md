# C3 三份 `0x77000` 包槽容量探针

2026-09-28，在 `mac-work-1` 用固定 ESP-IDF `578cf89c343e388db43ba1f4ddcd602fedcb763c`、Base `29db878d8dbaa4772e9b1d7fc3d97f0de9454f6d` 的源码归档和原有仓外测试 RSA-3072 键，验证一组隔离的 C3 4 MiB 分区候选。Base `57b19fc` 只增加前一轮检查点文档；本探针没有写回正式分区表、设备或生产凭据。输入和完整构建日志位于 `mac-work-1:/private/tmp/esp-base-57b19fc-c3-slot77000-probe-20260928/`。

| 分区 | 起点 | 长度 | 说明 |
| --- | ---: | ---: | --- |
| `ota_0` | `0x20000` | `0x130000` | 签名 app A |
| `ota_1` | `0x150000` | `0x130000` | 签名 app C／回退候选 |
| `product_pkgs` | `0x280000` | `0x165000` | 三份各 `0x77000`，起点为 `0x280000`、`0x2f7000`、`0x36e000` |
| `frp_scratch` | `0x3e5000` | `0x10000` | FRP 独立密文 scratch |
| `base_store` | `0x3f5000` | `0xb000` | 11 页 NVS，止于 `0x400000` |

前置系统 NVS、otadata、phy、coredump 仍占 `0x9000..0x20000`。仓外同时修改候选 CSV、C3 OTA policy 的 app 地址／长度，以及 `sdkconfig` 中 FRP、Container 包区／三槽、Base NVS 的精确配置；ESP32 目标分支没有改。ESP-IDF `gen_esp32part.py` 从 CSV 生成二进制并反向解析，分区无重叠且止于 4 MiB。

初次复制的 `sdkconfig` 继承了上一轮尺寸实验关闭的六条额外 TLS 曲线、P-256 NIST 优化及 SDK 错误名查表，所得 `0x111000` 签名 app **不用于本结论**。恢复这些选项到原默认值后重新完整链接、RSA v2 签名及官方 `check_sizes.py` 检查，得到 `esp_base.bin` **`0x121000` B**、SHA-256 `62b783c7ef9cba396881b64e76ab02110aecfa06af57a927c9a974820c9d4cbf`；每个 `0x130000` app 槽余 **`0xf000` B／60 KiB**。官方 `espsecure verify-signature --version 2` 使用测试键验证签名块 0 成功。生成的分区表 SHA-256 为 `b60a99a70dafed4a0b5089dd1ede3d2c5f310f88d72a40831b82c8fe403c0842`。完整结果在 `build-baseline.log`；裁剪配置初次构建另留 `build.log`，不可混用。

Container 通用打包器允许 512 KiB Wasm，现有最大规范签名样包实际为 `0x82000` B，因此这组 `0x77000` 物理槽会拒绝那类包。当前 Base C3 固件配置的 `max_wasm_bytes` 是 131,072 B；Container 设备验包器按此值加 4,096 B manifest 与 8,192 B 额外开销限制包长，当前上界为 `0x23000` B，落在候选槽内。包槽缩小是否成为正式产品上限仍由维护者裁决，不能因当前固件上限较低就宣称通用签名格式的最大包仍受支持。

本探针只证明分区解析、当前签名源码的静态尺寸及签名校验。公开安装／升级调用路径还未完成，新增代码可能再次吃掉 60 KiB；没有用这组精确镜像验证 QEMU guest、真实 HTTPS/MQTT/FRPS 并存、11 页 NVS 的持久负载、分区迁移、物理 Flash 时延或实体板。P6-03、P6-04 和 P7 继续开放，且未获得任何刷板授权。
