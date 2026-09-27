# P6-03 当前锁 C3 签名 guest 与 FRPS 工作流容量检查点

2026-09-28。维护者已确认继续使用现有两块 4 MiB 板，普通内部 8BIT 堆历史最低空闲门仍为 **49,152 B**。本轮在 `mac-work-1:/private/tmp/esp-base-c3-current-frps-20260928/` 和 `mac-work-1:/private/tmp/esp-base-c3-iram-off-frps-20260928/` 的仓外副本比较 C3 两项 Wi-Fi IRAM 配置。输入为本 Base 分支 `0850cd19b762fa7a1fdb65c075bcd0631757e8e6`、FRP `8f056273b3b93ea3273b4637038ddd0c6aea82a8` 与同一份官方 Component Manager 生成锁（SHA-256 `e5b0c66f402ee518931be19e110fed90df8cbe9049b7d60e64f510fcf8845b54`）。固定 ESP-IDF `578cf89c`、lwIP `2758df4`、真实签名 ABI 2 counter 包、OpenETH、官方 FRPS v0.71.0、测试 CA／IP SAN、测试时钟和额外 4 KiB 探针任务相同；两份 `source/firmware` 逐文件比较无差异。最终生成的 `sdkconfig` 仅在 `ESP_WIFI_IRAM_OPT`／`ESP_WIFI_RX_IRAM_OPT` 及其 IDF 同义镜像行不同，没有改正式产品配置。

## 签名和运行结果

完整 FRPS 诊断因加入网络探针后，RSA v2 签名 app 为 **`0x121000` B**，比保持三份 `0x82000` 包槽的既有 C3 `0x118000` app 候选槽大 **`0x9000` B**。因此仅在仓外使用双 `0x130000` app、三份 `0x78000` 包槽的 4 MiB 诊断几何；该几何缩小产品允许的包上限，**不作为正式 Flash 方案**。关 IRAM 两项后的签名 app SHA-256 为 `72cd4d5b07348cf59d32cdaeda6617aa15d317ec0af2a9920a82b63ba51bad02`，官方 RSA v2 验签、官方分区解码和 app 尺寸门通过，分区表 SHA-256 为 `c0972228af7357d62cb9a8cbb1a23fc0aff07d9d4a57cf1f775148ef85ed22aa`。签名 app 摘要重新绑定 ECS2 sequence 6，合成 Flash SHA-256 为 `6035be29d4cc49c6b547144e2c64ac501d59c82bacc13c0efa856caf4b42ef9c`。测试键和私钥只在仓外，实体设备未写入。

默认 IRAM 配置的同锁镜像也通过官方签名和诊断尺寸门，SHA-256 为 `8680f65621fdefce7080ba611dcaf92b17fdcdbfc65ae153c5a2c21578c8aeec`。QEMU 取得 OpenETH DHCP，客户端进入 `AUTHENTICATING` 且 `tls_error=0`、`verify=0`；120 秒观察内没有注册完成或代理端口，故不能归因于单一申请或宣称默认配置完成 FRPS 工作流。该次 GDB 只记录阶段，未记录堆值。

关 IRAM 两项的首次运行中，客户端依次到 `REGISTERING`、`READY`，`ready_sessions=1`、`pongs=1`、`verify=0`；官方 FRPS 工作代理完成一条 **300001 B 双向逐字节回显**，`work.completed=1`、`work.failed=0`，销毁返回成功。GDB 在 `before_create` 测得普通内部 8BIT `free/largest/min=78,436/45,056/76,580 B`；READY 为 `24,512/13,312/15,436 B`；工作流首次采样为 `12,332/8,192/6,460 B`，结束后历史最低仍为 **6,460 B**。原始 GDB SHA-256 `3371ebe0ee044d288508801450ca98fe932ca00c13d06780813c6a437878c6ee`，宿主结果为 `attempt-1/network-run-result.json`。

第二次从同一初始合成 Flash 独立启动，GDB 在正式 `esp_base_container_product_boot` 返回处读得 `ESP_BASE_CONTAINER_RUNNING=3`，随后到达 Base `READY`；因此本次 FRPS 工作流确实与签名 guest 存活同机运行。再次完成严格 TLS、注册、Pong 和同样的 **300001 B** 双向回显，`work.completed=1`、`work.failed=0`；历史最低为 **7,072 B**，比不变的 49,152 B 门低 **42,080 B**。本轮原始 GDB SHA-256 `cd52a22730b61863ab4f55d659f675978745e5c3cf5fb8fcb815c62679bd9a48`，种子／运行后整片 Flash SHA-256 分别为 `6035be29d4cc49c6b547144e2c64ac501d59c82bacc13c0efa856caf4b42ef9c`／`7af93a699a9aaeff777f640c55d184edac180a4b05cc94cdc092b86796e7ee0c`。`validated-readback.json` 独立逐区核对 bootloader、分区表、双 app、产品包区和 `base_store` 均逐字节未变；系统 NVS 改 101 B、otadata 改 12 B，FRP scratch 改 65,285 B，其具体状态尚未单独审计。

## 固定 SDK TLS 动态缓冲单变量

继续从上段关闭两项 Wi-Fi IRAM 的输入复制到 `mac-work-1:/private/tmp/esp-base-c3-dynamic-tls-frps-20260928/`，逐文件比较 `source/firmware` 无差异；生成配置只增加 `CONFIG_MBEDTLS_DYNAMIC_BUFFER=y` 和该开关引出的默认注释行，仍保留 **16,384 B** 最大 TLS 入站片段及 **4,096 B** 出站片段。固定 SDK 的 Kconfig 声明其收发缓冲按需分配，并在发送完成或上层读完后释放。本实验没有改 FRP 协议、最大合法片段或 TLS 校验策略。诊断几何、测试 CA／Token、官方 FRPS、签名 guest、工作流和额外任务与上段相同。

新 RSA v2 签名 app 仍为 `0x121000` B，SHA-256 `44b509ef07d6e92b0edf83bcc88cfc37f78e44f8c1ef56bc6135c7d5ced326a0`；官方验签、分区解码与尺寸门通过。新签名摘要绑定 ECS2 后的合成 Flash SHA-256 为 `11a043324b683c9ccbc7c68fbdf8c0fa3392a4a25cd067a42d61ba235394dd8f`。GDB 再次读取正式产品启动返回 `RUNNING=3`、Base `READY`，官方 FRPS 严格 TLS 的 `verify=0`，注册后 `ready_sessions=1`、Pong 和 **300001 B** 双向逐字节回显，`work.completed=1`、`work.failed=0`，销毁成功。READY 时普通内部 8BIT `free/largest/min=46,456/30,720/35,472 B`；工作流历史最低 **23,876 B**，相对上一组已确认 guest 的 **7,072 B** 增加 **16,804 B**，但仍低于 49,152 B 门 **25,276 B**。原始 GDB SHA-256 `41dbc66eec7c00c331c1b6661ad1361e871c15f001ebf8b053d5b0eadd5b80ff`，`validated-readback.json` SHA-256 `ccc7916a29ceb2ed8cb29348b7a979d1d7933c08b85b014d136fd7e827f0eb85`；读回确认双 app、包区和 `base_store` 均未改变。动态缓冲仅为仓外单次会话试验，尚未验证断线重连、错误退出后的完整资源回收或实板性能，因此没有写入产品配置。

这些 C3 QEMU 每次都只在未模拟的 ADC2 校准入口由 GDB 跳过一次；产品 USB Serial/JTAG 日志不出现在 QEMU UART0，所以状态采用 GDB 在真实任务调用点和正式函数返回处读取，签名 app 字节未修改。各组输入、构建、签名、原始 GDB、QEMU 命令、合成 Flash 和逐区读回均保留在上述仓外目录。此次测的是 OpenETH／测试时钟／直连 FRP 公共 API，尚未经过正式 Base Wi-Fi、SNTP、HMAC、MQTT 与 OTA owner，也未送入会话内完整 64 KiB AEAD 记录、双活动工作流、实板无线或掉电恢复。两项配置试验改善了该诊断的协议可达性与堆水位，**资源门仍失败**；P6-03、P7-02 不验收，正式 4 MiB 几何和产品配置均不改变。
