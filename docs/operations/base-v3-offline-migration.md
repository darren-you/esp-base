# ESP Base v1/v2→v3 配置离线预检与迁移边界

## 当前事实

P1-04 保存的 C3 实板基线仍运行 v1 配置，已观测 revision 5；`base_store/base_config/committed` 为可选的 112 字节 `EBCF` v1 blob。v2 只在此前软件候选中实现，并未完成实板部署。普通新固件只读 `EBCF` v3，遇到 v1 或 v2 会停止启动并保留 NVS，不会自动转换、擦除或生成凭据。分区仍为 4 MiB：默认 `nvs` 位于 `0x9000/0x6000`，`otadata` 位于 `0xf000/0x2000`，`ota_0`、`ota_1` 分别位于 `0x20000/0x1e0000`、`0x200000/0x1e0000`，`base_store` 位于 `0x3e0000/0x20000`。P1-04 是已保存的基线，未来物理写入前仍须重新确认同一设备的实时身份和状态。

v3 候选只改同一个 `base_config/committed` 键，保留 revision 与原字段：v1 输入转为 40 字节 header 加原 Wi-Fi 字节，MQTT/FRP 未配置；v2 输入在严格校验后保留原 Wi-Fi、MQTT 凭据和管理 HMAC key 字节，FRP 未配置。可选 `base_ota/operation` 收据保持原 blob。候选不生成或更换任何 Token、key、密码、证书，也不替换默认 `nvs/base_identity/device_uuid`。

## 只读预检与候选

设备工具须在本轮重新确认唯一 ESP32-C3、4 MiB、设备 UUID 和传输端点，获取两次独立读取的完整 Flash 恢复件，保存在仓外私有目录。输入须为当前用户拥有、权限 0600 或更严的两个不同普通文件，且每份恰为 `0x400000` 字节。在固定 SDK 的 Python 环境运行：

```bash
python3 tools/preflight_v3_migration.py \
  --backup-a <仓外第一份完整Flash备份> \
  --backup-b <仓外第二份完整Flash备份> \
  --idf-path "$IDF_PATH" \
  --device-id <本轮独立核对的设备UUID>
```

可加 `--output-base-store <仓外未存在的目标文件>`，生成权限 0600 的独立 `0x20000` 字节 v3 候选分区镜像。脚本使用固定 SDK 官方 NVS generator 生成候选，再以官方 NVS parser 逐键读回，验证精确键集、类型与字节；输出完整 Flash 和候选 SHA-256，不输出身份值、Wi-Fi、MQTT 凭据或 OTA 收据内容。脚本不打开串口，不执行 Flash/NVS 写入。

预检先逐字节比较两份 Flash，核对本仓固定分区表、otadata 选择器和双槽头/全槽摘要；`base_store` 仅允许同键配置与可选 OTA 收据。默认 `nvs` 允许身份，以及实板已见的 SDK `nvs.net80211/ap.sndchan`、`phy/cal_mac`、`phy/cal_data`、`phy/cal_version` 和无活动键的 `misc` namespace；各记录须满足精确类型，PHY 三项须同时存在，候选不会重建或改写默认 `nvs`。非规范 v1/v2、未知/重复键、无效 NVS 页/CRC、加密 NVS、身份不符、未决或非 VALID 选槽均阻断。镜像头与全槽摘要仍不证明固件可启动或签名有效。候选使用官方生成器新建 NVS 分区，会改变页历史与空闲布局；证明范围是白名单内活动记录及目标配置值，不是其他 Flash 字节无差异。

测试入口：

```bash
IDF_PATH=<固定SDK路径> python3 tools/test_preflight_v3_migration.py
```

## C3 现物只读结果与阻断

2026-09-26 以 P1-04 两份私有 4 MiB 完整 Flash 恢复件和独立保存的 status 身份，在固定 SDK 上运行现行预检。双份逐字节比较、固定分区表和 otadata 检查已通过；默认 `nvs` 身份与记录的 status 匹配，SDK 记录按上述白名单校验。`base_store` 第 0 个 4 KiB 页可被官方 NVS parser 验为 Active、条目/CRC 有效；只在**内存中**以空白页代替其后各页进行独立诊断时，能解出唯一 `base_config/committed` 的 EBCF v1、112 字节、revision 5，未见 `base_ota/operation`。这不是完整分区通过。

真实 `base_store` 后续 31 个 4 KiB 页均被官方解析器判为 Invalid，没有可确认的有效 NVS 页结构，也无全 `0xff` 或全 `0x00` 页；32 页摘要各不相同。其中四页与同一完整 Flash 的 `ota_1` 已占用镜像页逐字节相同，余 27 页仍非空且来源未定，不能当作可丢弃的空闲区。第 0 页的 126 个条目中有 95 个 Empty、8 个 Erased、3 个 Written，说明可以设计原位单键转换探针，但不能预先断言提交不会触发页擦除。摘要重合只支持存在重复字节的判断，不能确认全部无效页的来源或授权丢弃。完整分区预检明确返回“`base_store` 含无法安全审计的 NVS 页状态”，**未生成 v3 候选**。`tools/test_preflight_v3_migration.py` 的官方生成器假件 12/12 通过只证明脚本的已覆盖分支，不替代此真实阻断；本页不公开私有 Flash 摘要、UUID 或配置值。

旧 v1 源码在 `c3d22c5` 对 `base_store@0x3e0000/0x20000` 只使用官方 `nvs_flash_init_partition` 和 `base_config/committed` blob 的 `nvs_get_blob`/`nvs_set_blob`，没有自有 journal 或原始块格式。固定 SDK 的 `nvs_page.cpp`、`nvs_pagemanager.cpp` 会把无有效序号的异常页放入可用页列表，已有 Active 页仍可供读取；使用这类页时才可能擦除它们。这解释了现物仍能从第 0 页读出 revision 5 的可能路径，但不证明全部异常字节可删除，也不证明旧实板二进制与当前 SDK 内部路径完全一致。

同键正常路径的仓外 QEMU 探针现已证明**在本次固定输入和 SDK 下**可以仅修改第 0 页，见下节。后续 Base 的 `config.set` 与 OTA 收据仍会向同一 `0x20000` NVS 分区写入；固定 SDK `PageManager::activatePage()` 在未来切换到列为可用的异常页时会擦除该页。因此一次转换成功不足以建立长期保留后 31 页的合同，也不能将正式预检改为放行。继续迁移前仍须独立查明异常页来源，决定其可保留/可处置边界，并使后续 NVS 写入、双槽签名启动与失败恢复共同满足这个边界；当前完整分区预检保持阻断。

## 固定 SDK QEMU 同键保页探针

2026-09-26 使用 P1-04 已保存且逐字节相同的 C3 完整 Flash 备份，**只在仓外**复制 `base_store@0x3e0000/0x20000` 到 0700 目录内的 0600 文件。公开输入固定为 ESP-IDF `578cf89c343e388db43ba1f4ddcd602fedcb763c`、esp-lwIP `2758df4cd3666b3b2a5b53830148379326425c0d`、QEMU `9.2.2 (esp_develop_9.2.2_20260417)`、[独立探针源码](../../firmware/tests/nvs-same-key-probe/README.md) `nvs_same_key_probe.c` SHA-256 `136e833db669e8b5932f0405421d4799106422cb284be1b63e287a77c0107e64` 和当前 C3 分区 CSV SHA-256 `2cd5b1d78b697b7d3c0c715654eacddddad1756b5ee9a6287b3c5ca09d5c5b43`。探针与普通 Base 生成配置中的 NVS 加密、旧重复键、BDL 栈及擦除验证选项相同，均未启用；探针和 Base 生成的二进制分区表逐字节相同。未复制默认身份 NVS，也未把私有完整镜像、UUID、Wi-Fi 字段或私有摘要写入仓库/输出。

三个独立模式以固定 SDK 构建，用官方 `bootloader`、当前分区表、初始 `otadata`、探针 app 与原始 `base_store` 组合成仓外 4 MiB QEMU Flash 副本；只运行 `qemu-system-riscv32 -nographic -machine esp32c3 -drive file=<仓外副本>,if=mtd,format=raw`，不接真实设备。`PROBE_INIT_ONLY=ON` 的 `nvs_flash_init_partition` 返回 `ESP_OK`，初始化后 32 页逐字节均未变化。默认提交模式依次 `nvs_get_blob` 读 v1、同键 `nvs_set_blob` 写 v3、`nvs_commit`、重新打开并读回，全部返回 `ESP_OK` 且候选与读回相等；**只有第 0 页改变，后 31 页逐字节保持原值**。用 `PROBE_VERIFY_ONLY=ON` 的新镜像/新 QEMU 进程读取提交后的副本，初始化及读取仍为 `ESP_OK`、版本为 v3，重启过程 32 页无额外变化。

固定 SDK 官方 NVS parser 只在**隔离第 0 页并在内存中用空白页替换尾部**时，确认转换前后都仅有一个活动 `base_config/committed`，v3 blob 与独立 `convert_v1_wifi_only` 逐字节相等，revision 原样保留。这只验证第 0 页记录。对原始或模拟提交后的**完整 `base_store` 字节**，官方 parser 的完整分区审计仍因后 31 页无效而拒绝；正式预检对原始完整 Flash 继续阻断，没有生成完整分区 v3 候选。QEMU 正常路径不能证明真实 Flash 写入、断电中断、未来 page switch 不擦尾页、旧/新 bootloader 双槽启动或恢复。未刷板、未变更 eFuse。

另有[全合成八页 NVS 容量验证](c3-eight-page-nvs-capacity.md)完成 100 代最大 v3 配置与两份旁侧 blob 的换页、回收及重启读回。该实验没有读取上述私有备份，也不改变旧 `base_store` 后 31 页的来源不明事实或当前完整分区预检阻断。

## 首次启动与一次性写入边界

本仓交付离线只读预检、候选生成能力与 QEMU 探针，不提供设备写入或选槽命令。首次 v3-only 启动前，必须保全两份可恢复的完整 Flash 基线、确认身份与真实 otadata，先使两个可能启动的应用槽都具备读取 v3 的能力，完成同键配置转换和精确读回，再确认启动槽及可回退槽安全。不能让 v1/v2-only 镜像成为 v3 NVS 的自动回滚目标。

对当前 C3，离线执行顺序目前停在 P1-04 双份原始 Flash 的完整分区预检阻断；QEMU 第 0 页保留探针不会自动生成可写板候选。待异常页与未来 NVS 写入的保存合同闭合后，才可准备与当前 C3 分区精确一致、能解码 v3 的**两个**应用槽镜像和匹配的签名启动链，逐镜像核对芯片、项目、签名方案、大小及实际签名。只有这些离线产物与完整 Flash 恢复件均可读回，才进入同板独占的物理维护窗口：重读设备事实、做新的双份完整 Flash 基线、按已审核的完整写入方案安装两个新应用及 v3 `base_store`，保留默认 `nvs/base_identity/device_uuid`，明确设置并读回安全启动槽与回退槽。首次启动后由真实 status 核对同一 UUID、revision 5、Wi-Fi 配置与启动槽；再按 Bridge v3 的物理 `config.set` 输入需要新增的 MQTT/FRP 凭据，并以设备提交回执核对新 revision。v1 只有 Wi-Fi，离线转换不会凭空产生 MQTT/FRP 材料。

当前**可执行且已实现的只有离线预检/候选生成器与 QEMU 探针**；当前 C3 完整分区不会通过候选生成前置。P5-05 签名运行基线、两槽 v3 镜像、一次性受控写入/选槽程序、写后完整 Flash 与身份/配置读回，以及 Tool Bridge 对目标串口的枚举仍缺；不能把上面的顺序当作今天可直接执行的刷写命令。`esp32` 旧 ESP-AT 分区与本 C3 过程无关。首次启动失败或写后事实不确定时，停止再次写入与自动重试，保全失败现场，然后按 P1-04 的同板完整 Flash 恢复件恢复并逐字节读回，重启核对原 UUID、revision 5、Wi-Fi 与 Bridge 状态；整个窗口不得改变 eFuse 或用清空 NVS 冒充迁移/恢复。实际恢复仍须在获得设备授权和明确独占窗口后单独验收。

若采用一次性维护镜像原位写入，维护镜像需先安全启动并确认 VALID，再使用固定 SDK NVS API 枚举白名单记录、验证原 v1/v2 blob、生成 v3 同键值、`nvs_set_blob`/`nvs_commit` 并重新打开逐字节读回。开始写入后的异常都视为状态未知，只读取真实持久状态，不自动重试、擦除分区或复位。还须读回默认身份与 OTA 收据，核对未授权修改的其他分区。该维护实现和真实双槽迁移仍未完成；没有精确设备授权与恢复基线时，不得执行物理写入。
