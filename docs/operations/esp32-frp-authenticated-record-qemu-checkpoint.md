# ESP32 签名 guest 与 FRP 64 KiB 认证记录 QEMU 检查点

2026-09-27，P6-03 独立软件切片。从公开 Base `9befb0f3fc3ab26551fcd59653d4108b62864166` 出发，仅将 FRP 精确锁更新到修复完整记录头分段问题的 `9a0839a603ed1f6bbce0d1b3c65a6bb43e501cf3`，在仓外隔离源码归档加入测试任务。**ESP32 正式 CSV、默认运行策略和实体设备均未修改；本记录不是 FRPS 会话或 P6-03 资源验收。**

## 固定输入

原始工作目录为 `mac-work-1:/private/tmp/esp-base-p603-auth-record-qemu-20260927/`（下文记为 `$RUN`），包含仓外测试键、签名包、合成 Flash、完整原始 UART 和私有构建日志；这些字节不入 Git。Base `9befb0f` 的 Git 归档 gzip SHA-256 为 `0ec57ad4ef568c6ac2893d71a32941a5c2480cd9467767777a6027449749cd56`。固定 ESP-IDF 为 `578cf89c343e388db43ba1f4ddcd602fedcb763c`、lwIP 为 `2758df4cd3666b3b2a5b53830148379326425c0d`；Container `6ef74faabb675bce0180570f5bdf0232af11106a`、WAMR `c10736fffdf26d7c2ae234e05aa712df112eb6bf`、OTA `f4fb0b4f3fa7b384edf540bac626314418156d22` 保持 Base 原锁。官方 Component Manager 重新生成 C3、ESP32 两份目标锁，SHA-256 分别为 `a9a7c8c737adb31008535bc8e9b56b76a530fce7139009f0ffd4b51a8a6fb36b`、`477d558a10d50b456f174477a14ce740bac2bd2313a94048fff4710388ebd23f`；受管 `aead_flash.c` 与 FRP 新提交的文件 SHA-256 同为 `609e41e6a65c11142c957fa04823baba740fce18a6059045daef149c452ee573`。

仓外 ESP32 条件性 CSV SHA-256 `0bd97f4bf6c597328e862f8359eaf6c2b64d107b8bd5f095133ba6e7ff8e23e1`：双 `0x120000` app、三 `0x82000` 包槽、旧 AT 只读区 `0x3e6000/0x4000`、`frp_scratch@0x3ea000/0x10000`、六页 `base_store@0x3fa000/0x6000`。仓外 `sdkconfig` SHA-256 `ef26067e8b54486b047b9a70a1a38e0124088a979e250bf53c16921a91ba45dc`，仅测试配置启用 scratch 和 ECDSA v1 测试签名；签名分区表 SHA-256 `49f7ee4e5b70b3bd12cb68e3e10a8ab6d0ac180c24cfa4951b8326c28e2fd602`。C3 普通完整构建 `0xdefb0` B，ESP32 仓外测试签名 app `0x10fff4` B、SHA-256 `1e5dede83833850fa4f7c15d2aeda6c213add084cabd6758fc996094ecc81ee2`，双 app 槽各余 `0x1000c`。官方 `espsecure verify-signature --version 1` 分别验证 app 与分区表有效。Base 双目标 host ASan/UBSan 回归分别 **20/20**、**19/19**；FRP 新提交的 Mbed TLS／官方 FRPS 宿主回归 **22/22**。

仓外 seed 以本轮签名 app 完整摘要重新生成 ECS2 sequence 6、`CONFIRMED` 的真实 ABI 2 counter 包绑定；签名包 10,240 B，SHA-256 `9a95b5e8fa5619f0559eb673865ce287e058a1646c9f4f0b4e5964feb4508f8e`。固定 SDK 的 NVS V2 generator 将其放入六页 `base_store`。初始 4 MiB 合成 Flash SHA-256 为 `5fea5c2ce91e9f5581a5b4b2f6cffa8c70a7b7fc17a3c797a2403bcc88b481ee`；这仍是仓外预置，不是设备公开安装 API。

## 测试任务与边界缺陷

[源码准备工具](../../tools/prepare_qemu_frp_authenticated_probe.py)只改不含 `.git` 的独立归档；本轮注入后的 `esp_base_main.c` SHA-256 `c0fb2d5a649ad404774966995b3139e3834f84d39bcca66e1b6f68cce4bad40f`。任务在 Base 输出 `READY container=running`、启动 storage claim 已释放后运行，直接使用 Base 已绑定并 boot recover 的正式 `efrp_idf_flash_store_callbacks` 和 FRP 会话所用的正式 `efrp_aead_flash_reader_*`。它将公开固定 key、nonce 与 AAD 生成的 **65,568 B 真实 AES-256-GCM 记录**分块送入 reader：16 B 完整头单独一次、64 KiB 全零密文按 1 KiB 分块、最后 16 B tag `38a5816a35ba760a993b63c4bb7b4d76`。解密明文为固定 AES counter keystream，SHA-256 `8019538e2bae43c56c77ee8dd149240aadbae258810b7f0c05ad46d1be9213dd`。这一明文不是合法 FRPS 控制消息，因此不把 reader 调用称为 session 通过。

旧 FRP `1660ac2` 的首次真实 QEMU 输入恰在 16 B 头处结束：provider `begin` 的擦除与短 owner operation 返回成功，但 reader 随后错误地调用零长度 Flash write，得到 `EFRP_STORAGE_ERROR`；该失败原始 UART 私有保留。旧锁将头与首批密文合并输入时可完成满长认证，仅作为**独立探索性收据**保存在 `$RUN/esp32/old-1660-combined/`。FRP `9a0839a` 在正文输入耗尽时退出本次 feed；整头与分段头的旧版红／新版绿宿主回归、OpenSSL 10/10、Mbed TLS／官方 FRPS 22/22 均在 FRP 源仓完成。本节的最终 QEMU 使用新锁且恢复 16 B 单独送入方式。

第一次尝试运行新锁 QEMU 时，仓外 runner 读到之前保留的旧 UART `PASS` marker 而提前退出；该次不计入任何运行结果。清除旧输出后重新启动独立 QEMU 进程，以下仅使用新 UART SHA-256 `7461ecf80ad08d13081ae9138b90b595077b26b40078f27571c09bf525c081e7` 和本轮 MTD 读回。

## 新锁 QEMU 运行与独立读回

Espressif Xtensa QEMU `9.2.2 (esp_develop_9.2.2_20260417)` 从上述合成 Flash 启动，日志顺序为 `ESP_BASE_CONTAINER_RUNNING sequence=6 trial=0`、`ESP_BASE_READY ... container=running`、`QEMU_FRP_BEGIN guest=running owner=idle`、`QEMU_FRP_PASS`。完整 tag 到达前没有明文；之后认证一条 64 KiB 记录，16 个 4 KiB 窗口逐一复验并计算上述明文 SHA-256。reader 记录 **17 次**全长复验、读取 **1,114,112 B**，坏 tag 的第二条满长记录被拒绝且零明文交付。结束时窗口清零、reader close 成功、provider 为 `IDLE` 且无 lease，Base storage owner active token 为零；探针期间 owner 发放 **2,434** 个短 claim。此处没有创建正式 FRP client、TLS、Yamux、`efrp_session_step` 或官方 FRPS 连接。

同一 guest 与读写任务并行时，内部 byte-accessible heap 的探针启动／阶段最低／认证后／任务结束前空闲分别为 **34,128／34,128／38,476／38,476 B**；boot 至该点最低为 **33,164 B**，最大连续块最低 **26,624 B**，8 KiB 探针任务的栈高水余量 **4,700 B**。任务结束前的读数不证明任务自删之后的全系统堆回收；它已经证明 reader、Flash lease 和 storage owner 的清理。计划 §13.3 的初始门为最低 free heap ≥48 KiB、largest ≥24 KiB、任务栈余 ≥1 KiB，因此本轮**堆门不通过**，不能据最大连续块与栈读数放行。真实 FRP session、TLS、MQTT、OTA 同板会增加负载，本探针不能推定其容量。

主机直接逐字节比较三份 4 MiB MTD：首启前、记录完成后、同片第二次冷启在 `ESP_BASE_BOOT` 前置恢复后。全片 SHA-256 依次为 `5fea5c2ce91e9f5581a5b4b2f6cffa8c70a7b7fc17a3c797a2403bcc88b481ee`、`d347758b7ea6715224f944a7b57e2ce77f0cd6933e49932e0fc6086de87f9eda`、`cd6997fcea6ee475f63d185425f1f8f3b456c758e38c41c9d6c236e9f5e37d37`。第一次完成后 scratch **65,536 B 全零密文**；第二次独立启动的 Base boot recover 后 **65,536 B 全 `0xff`**，第二次 UART SHA-256 `2a0f80daaea25a83233003713f39843cd54cf35da3ac937f19854c0112e73d84`。双 app 槽、整个包区、旧 AT 只读区和六页 `base_store` 在三份镜像间逐字节不变；第一次运行只另改系统 NVS 101 B 和 otadata 12 B。固定 SDK NVS parser 对三份六页区域的完整性检查均通过。

本结果是候选几何中**签名 Base＋运行 guest＋正式 FRP reader/provider＋真实 storage owner**的同进程设备软件证据。它没有正式 session/FRPS、Wi-Fi 与 SNTP、MQTT、OTA 长持 owner 竞争、实板 SPI Flash 时序、掉电或旧 AT 迁移；正式 ESP32 CSV 仍没有 scratch。P6-03 的完整组合资源与实体板验收继续开放。
