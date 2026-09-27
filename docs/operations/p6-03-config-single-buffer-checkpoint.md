# P6-03：Base 配置提交单缓冲检查点

2026-09-28。Base 主固件在 `esp-frp@b462c1497438cfeb514f022bc2d2d6c026599a41` 及其 C3／ESP32 官方依赖锁不变的前提下，收缩 v3 配置 NVS 工作区。原 `s_load_bytes`、`s_commit_bytes` 各为 `EBASE_CONFIG_MAX_BYTES=7618` 字节，常驻 `.bss`；启动读取、控制任务配置指纹、提交写入与提交后读回均由同一个 Base 控制 owner 串行执行。写入前编码字节交给 `nvs_set_blob`，写入及 commit 成功后由同一缓冲读取并解码持久值，写入字节无需继续占用第二块缓冲。

提交后的检查仍要求完整读回：v3 解码器校验头部每个字节、保留位、精确载荷长度和各字段值，合法配置在 wire 上只有一个规范表示。读回模型逐字段与原候选比较，并要求 revision 精确为 `expected_revision+1`；比较不依赖 C struct padding。缺失、无效或不同的读回仍返回 `ESP_BASE_CONFIG_UNCERTAIN`，不改调用方已提交配置，也不自动重试或擦除 NVS。工作模型和共享字节缓冲在返回前擦除。配置 schema、单键、最大 7,618 字节容量、控制协议与密钥来源均未改变。

宿主 ASan/UBSan 配置存储定向回归通过，覆盖完整 MQTT／FRP 配置成功提交，以及 revision、Wi-Fi、MQTT 主机名、FRP 主机名被改写后的不确定结果。隔离 `mac-work-1:/private/tmp/esp-base-frp-status-consumer-20260928/` 的 Base 双目标 host 全套分别 **20/20／19/19** 通过。固定 ESP-IDF `578cf89c`／lwIP `2758df4` 的双目标普通完整构建及官方 app 分区尺寸检查通过；C3 未签名 app `0xdf120` B、SHA-256 `e0a27c7e778d016cf1582ac927541be8485ca3e4b0e575cf298a6b21b1e6e59a`，ESP32 显式离线探针未签名 app `0xd2ce0` B、SHA-256 `21cb50f062be22ccb5e6bca9cd80307237ab3a14b8c9dd9fb0c34a7a38854e49`。两目标 ELF 均由原先 `s_load_bytes` 与 `s_commit_bytes` 各 `0x1dc2` B 改为唯一 `s_config_bytes` `0x1dc2` B，故各减少 **7,618 B** 常驻 `.bss`；没有改任何依赖锁或正式分区。

这只证明静态内存释放和宿主故障语义。尚未在同一签名 guest／FRPS／MQTT／OTA 输入下重测启动历史最低堆，不把 7,618 B 机械加到旧 QEMU 低水；实体 NVS 掉电和两块板的 48 KiB 全组合容量门仍未验收。
