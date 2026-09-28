# ESP32-C3 六／八页 NVS 合成容量验证

## 2026-09-28：最近八条产品操作账本同分区复跑

按维护者的最近固定条数裁决，在已有最大 7,618 字节 Base v3 配置、186 字节 OTA V2 合成形态及 288 字节 Container ECS2 初态上，增加单个 **910 字节** `base_product/operations` 账本。它用 EPRD v1 八槽格式保存连续序号、最近八条不同 UUID、请求指纹、包 SHA-256 和结果；每次产品操作先写 `PREPARED` 再写 `SUCCEEDED`，每次均经 NVS commit 与逐字节读回。这个探针按真实编码布局测容量与页回收，未调用尚未开放的产品写命令，不证明物理操作的成功或历史幂等裁决。

独立仓外合成 Flash、构建目录和 `sdkconfig` 各用于 C3 六页与八页。固定 ESP-IDF `578cf89c343e388db43ba1f4ddcd602fedcb763c`、esp-lwIP `2758df4cd3666b3b2a5b53830148379326425c0d`、Espressif QEMU `9.2.2 (esp_develop_9.2.2_20260417)`；Component Manager 从空锁正式解析 `esp-container@f82e4b8f57eb6ae75309d5cfb7472feef2380912`，WAMR 保持 `c10736fffdf26d7c2ae234e05aa712df112eb6bf`。两档均完成三阶段：1–3 代与旧 CAS 拒绝、另一进程 4–100 代、再次新进程的 revision 100 四记录读回。全部 `PROBE_STEP` 的配置、OTA、Container 和产品账本读回为 `ok`，`PROBE_RESTART_MATCH=1 container_decoded=1`。

| `base_store` | 最终 `used/free/available/total` | 最终官方 NVS parser | 最高页序号 |
| --- | --- | --- | ---: |
| C3 六页 `0x138000/0x6000` | `297/459/333/756` | 五页 `CRC32: OK`、一页 Empty | 299 |
| C3 八页 `0x138000/0x8000` | `298/710/584/1008` | 七页 `CRC32: OK`、一页 Empty | 260 |

仓内探针源码 SHA-256 为 `66d74021fb4257515f3205637b23b72c1383d90b41a8852fbb92596c4cd76ab9`，runner 为 `bcaeffe63d1154244b4b7dac53de9739753b3ab5a2777981c016bc130aa222fe`，C3 官方生成锁为 `a2dfd06b32cdd3c9e915e56fd737132a85259be3f3ce7daf19096227969a5699`。六页阶段 1/2/3 脱敏日志 SHA-256 依次为 `83db0ef1572bb27d45a28b161c92ebab70ceeabc07a513ab8d260816059e923b`、`6d9a821355a47d0a94dd36204e081072fbf3c369bb54a60f3f50bad1a764a818`、`1ab4da0a3ef06193c9bc4e7577d9b3a446fe5698dc8605145c848e0928d721d0`；八页依次为 `f8dc3282a9c36c813600aa51328ed3862085eae51bfd8927719b075d8bd97239`、`3ec4e18bd143fea527f2ca71be54c6122bf7b9ed6b3030f7ef0061f294555ab3`、`78d106f62f7b8085c3602e94c7a31289f7140fb0c148d92139d9dc584c680945`。最终 NVS 提取摘要分别为 `d5204379b1f5fe5c5415fb9c32a9ca63223e46494cff17e3f5c6699e412ea7a0`、`1e6c401bc1201ed8e0bb92b0bce340f0603bd3598d795ee7f06f3a39deaa8530`；原始串口日志、Flash 与解析输出留在仓外。

这仅证明固定 SDK 的 QEMU 正常写入与新进程读回可承载上述 100 代。产品账本的首次建账安全性、写入中断电、真实 Flash 磨损、两板迁移和完整五能力仍待分别验证；八条上限暂不作为实板磨损验收结论。

## 2026-09-27：当前 OTA V2 与 Container ECS2 的六／八页复跑

从 Base `f2d8b3623d60ca18b332c0a5f9913d50f406634d` 建立独立工作树，用[公开容量探针](../../firmware/tests/nvs-capacity-probe/README.md)在另一台 Mac 的独立 checkout 中构建、运行。该 checkout 不依靠邻仓源码：ESP-IDF Component Manager 通过公开 Git URL 解析 `esp-container@bf52b17a26e51d35a261bf852ac0c9cde76adefc`、`wasm-micro-runtime@26c235e53e29acd8b43abe7f3b524577bd4d1ae5`，生成的 `dependencies.lock` 与探针仓内副本摘要相同。固定 ESP-IDF `578cf89c343e388db43ba1f4ddcd602fedcb763c`、esp-lwIP `2758df4cd3666b3b2a5b53830148379326425c0d`、Espressif QEMU `9.2.2 (esp_develop_9.2.2_20260417)`；六、八页分别使用独立构建目录、`sdkconfig` 与初始全 `0xff` 的 4 MiB 合成 Flash。

两种几何的 `base_store` 均从 `0x138000` 开始，大小分别为 `0x6000`／`0x8000`。测试分区表另含供真实 Container IDF provider 校验几何的合成 `product_pkgs@0x260000/0x186000`；本轮未写生产分区表、未操作设备。每代在同一 NVS 写入并读回最大 **7,618 字节** Base v3 规范配置、当前 **186 字节** OTA V2 合成收据形态，以及 **288 字节** ECS2 无包绑定元数据。配置走产品 `base_config/committed` CAS；OTA 字节遵循当前产品 V2 布局并写入 `base_ota/operation`，前四个 SHA-256 字节用于承载测试 revision，不调用正式收据解码或 OTA 注册策略，**不声称它是完整有效的 OTA 事务**；Container 的初态由锁定组件的 `econtainer_slots_initialize` 正式编码，真实 IDF provider 使用产品 `base_pkg/slots` 键提交，每代改变 sequence 和 CRC 后由 `econtainer_slots_load` 正式解码。此压力序列不代表 100 次合法 Container 产品状态迁移。

| 页数 | 阶段 1 | 阶段 2 | 阶段 3 | 最终 `used/free/available/total` | 官方 parser |
| ---: | --- | --- | --- | --- | --- |
| 六页 | 1–3，旧 CAS 拒绝 | 新进程读回 3；提交 4–100 | 再次新进程读回 100，三份记录一致 | `265/491/365/756` | 五页 `CRC32: OK`、一页 Empty |
| 八页 | 1–3，旧 CAS 拒绝 | 新进程读回 3；提交 4–100 | 再次新进程读回 100，三份记录一致 | `265/743/617/1008` | 七页 `CRC32: OK`、一页 Empty |

两种几何各有 100 个互不相同的配置 SHA-256；全部 100 次 `PROBE_STEP` 的 OTA／Container 读回为 `ok`，阶段 3 均输出 `PROBE_RESTART_MATCH=1 container_decoded=1`，官方固定 SDK `nvs_tool.py -i -d none` 返回 0。六页的页序号达到 209，八页达到 208，且最终各留一页 Empty，说明正常路径触发了换页与回收。六页 `used/free/available` 全程范围分别为 `264–265`／`491–492`／`365–366`；八页为 `264–265`／`743–744`／`617–618`。这些是合成数据在固定 SDK、QEMU 下的容量事实，**不直接裁决正式分区大小**。

可复核输入摘要（SHA-256，均为当前仓内文件）：

| 输入 | SHA-256 |
| --- | --- |
| `main/nvs_capacity_probe.c` | `78426e645a6343ec534364cee57b9a55e7d7d2b4f1b02bb563740bdbdaca21a8` |
| `run-qemu.py` | `88db0f768a8067aa1309f61c50faa828e3f4b877b9cb81cb37880fdb924ed45c` |
| `partitions-6page.csv`／`partitions.csv` | `868f329970c813258da1bcdb99806afdeae295c60d09409ce9cecca1f92fae5a`／`8eaf68ef494591d3c90a8dbb96da649e03d3bac0b7182bc50b17ec5831148b18` |
| `main/idf_component.yml`／`dependencies.lock` | `dde6450973d78d5874efa18fa8e00d310e066776ddfd97060fd71951913b5e81`／`3048acad2ca380e4ef2cc6455235c5c8775cc63ae620be432abded380817ac00` |
| `CMakeLists.txt`／`sdkconfig.defaults` | `8acfc41f2fffc57761af99c0be8fff5ab661eea2bb6cb16e61d930a8fb8dfcdd`／`e7924e9c558ad211600afd0edfc878da553442bb66c05c092aaa3aed393f6b77` |
| `sdkconfig.defaults.6page`／`sdkconfig.defaults.8page` | `c36017cca0a5a8348de50784c6de99ff6c2dfb9a0a161473160d813c1976eee9`／`52f40abb07b265c34ee11b2a43eb454b970d73f9a11526724c816ce33fc4a589` |

仓外原始日志和最终提取 NVS 的摘要足以与独立复跑比较，二进制及原始串口日志不入仓：

| 页数 | 构建阶段 1／2／3 | `PROBE_` 日志阶段 1／2／3 | parser 日志／最终 NVS |
| ---: | --- | --- | --- |
| 六页 | `62bfb9c14202f69b3a0384929e0ef2661bdc1708fbb8222928b773dbe3c9d195`／`3711985a3536ab2690b98fdb61f0c992e47fea7973a6b235890b1ed6d8557b22`／`6489b31fe55f7368372128e0c34d68b14ed750571306b3fa46e7016377cbc979` | `3938635533099e68308646e88b093bc6c7b4024e8f61c949d0e34d5e0d33672c`／`a41f38eba13ec328181941740a8c8cdc3e17bff1a60108bac71224020c5eb08d`／`900e4722012908d8e1220861c18716f88f34db515831e6668b09c618f460d643` | `37742f91c9cf23b50f71f1100baedf963928b3904219393aa143d152fea80b70`／`9cab3bacbfddd62b80ece6204d447187abcc4f1833682738dcb54d4e2712ff74` |
| 八页 | `fb0e5deefc9377311f078df5589ef1cd041346340ad5ec7b8b7d123259b52ebc`／`05c457257b0558fdacd3b3bfbe43632e00c7a6d75a6dc4675d7b641a8a015a69`／`fd98e0d646c52ea7a7512da64200414ee7c25faaaae91e079d384f3ef0aa6b99` | `4d2a074ccce25eee9780fe24b7ff803ead0250d854e95017af41897805f0de7d`／`d5a68a33000bac9d775e597e1ed05543e02fdc9d80ddb191dee301d98daabe93`／`a7df97d14e684cadd908c8334b62dcd044f7b0f39842377690216a3b41a09364` | `0fd253fd3f79bf1cedfac11e6ca86807b58b829f24b4d525d421c6baa43104d2`／`df4bb8c8c0130f0fa4397c3800ff443aa23431d2d9a97be48981fe832d95748d` |

复现构建、运行与官方 parser 命令见探针 README。此次没有验证写入中断电、真实 Flash 磨损、OTA 回滚、真实 package 事务、旧完整 `base_store` 的迁移和异常旧页。现有正式 preflight 对异常旧页继续阻断；完整旧分区收缩、App／Container 包槽及 FRP scratch 的正式几何仍需分别以精确合同裁决。以下 2026-09-26 的 118 字节／占位键实验保留为历史，不可替代当前产品记录结果。

## 2026-09-26：历史八页占用实验

2026-09-26，在 Base 独立工作树中，使用[合成容量探针](../../firmware/tests/nvs-capacity-probe/README.md)验证连续八个 4 KiB 页能否承载最大 Base v3 配置和两份旁侧记录。固定 ESP-IDF 为 `578cf89c343e388db43ba1f4ddcd602fedcb763c`，实际 lwIP checkout 为 `2758df4cd3666b3b2a5b53830148379326425c0d`，QEMU 为 Espressif `9.2.2 (esp_develop_9.2.2_20260417)`。SDK fork 当前 gitlink 指向另一 lwIP 提交，构建会发出 submodule out of date 提示；本探针明确按 Base `sdk-lock.json` 固定的 checkout 构建。生成 `sdkconfig` 核对为 `esp32c3`、4 MiB Flash、自定义合成分区表、NVS 加密关闭。

本次运行的公开输入收据：`main/nvs_capacity_probe.c` SHA-256 `a63eb4c10ec38f0cff0d4d1b7e979e4fbab76c9dda587d9c770628ed458f2621`；`run-qemu.py` SHA-256 `dd05de09517f8a6aa2a259471250e0ee1ded51b150f24a52d0cd3aaf1e3d78c6`；合成 `partitions.csv` SHA-256 `106a690443b1a99ed5aec04eefd7875020a0bebdd62200a3f9c2dc133bc8f1dc`。远端执行副本与仓内源码摘要一致。三阶段构建与 QEMU 命令见探针 README；官方解析命令为 `nvs_tool.py -i -d none <仓外提取的合成八页 NVS>`。

## 输入与结果

- 仅用人工构造的有效 Base v3 配置。每代规范编码长度均为上限 **7,618 字节**，管理 key 与 revision 改变；阶段 1、2 合计 revision 1–100 的 100 个读回 SHA-256 全部不同。
- 同一 `base_store@0x138000/0x8000` 内，真实 `base_config/committed` CAS 每代成功；真实 `base_ota/operation` 键每代写入并读回 118 字节的合成收据形态值；合成 `base_container/state` 键每代写入并读回 288 字节占用值。Container 产品键尚未冻结，未验证 metadata 解析；OTA 未运行注册策略或下载。
- 阶段 1 的三代 CAS 成功，旧 revision 请求返回预期冲突码。阶段 2 是新 QEMU 进程，在任何新写入前读回 revision 3 和两份旁侧 blob，随后完成 revision 4–100。阶段 3 又用同一合成 Flash 的新进程读回 revision 100、配置摘要和两份旁侧 blob，`PROBE_RESTART_MATCH=1`。没有 NVS API 失败或读回不一致。

| revision | used | free | available | total | namespaces |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 262 | 746 | 620 | 1008 | 3 |
| 3 | 263 | 745 | 619 | 1008 | 3 |
| 10 | 263 | 745 | 619 | 1008 | 3 |
| 50 | 263 | 745 | 619 | 1008 | 3 |
| 100 | 262 | 746 | 620 | 1008 | 3 |

`nvs_get_stats` 的 `available` 在 revision 15、29、43、57、71、85、99 从 619 回升 620。更直接的页证据：阶段 1 后页序号从 0 开始，页状态为六个 Full、一个 Active、一个 Empty；第 100 代后有一个 Active、一个 Empty、六个 Full，最高页序号达到 **207**。固定 SDK 官方 `nvs_tool.py -i -d none` 对最终八页逐页报告 CRC32 OK 或 Empty，返回码 0。统计值周期回升与页序号、状态共同证明 QEMU 中发生换页及回收；不把 `available` 的单独波动当作 Flash 擦除证明。

另从阶段 1 的同一合成 Flash 副本重跑阶段 2，在 revision 37 的配置 CAS、两份 blob 提交与读回、`nvs_get_stats` 返回且完整日志行输出后，强制结束 QEMU。再启动阶段 3，读回 revision 37 的三份值且相互匹配。此为**已返回写入后的进程终止**，不代表写入中断电、真实 Flash 的磨损或供电故障恢复。

逐代摘要与统计、阶段重启和受控终止证据保存在[合成日志目录](../../firmware/tests/nvs-capacity-probe/evidence/README.md)。日志不含配置原文、私有备份、设备 UUID 或真实凭据；完整合成 Flash、原始 QEMU 串口输出与构建产物留在仓外。

## 边界与后续

本轮证明固定 SDK 在 QEMU 正常路径下，八页可完成 100 代最大 Base 配置 CAS 与两份指定长度 blob 的共同写入、回收和重启读取。没有测试七页或更小容量，因此不声称八页是数学最小值。合成 factory app 与分区表只用于容量实验，**产品 CSV 未修改**；尚未核对新布局对双槽应用、bootloader、签名镜像、旧分区保留和 OTA 回滚的全链路约束。Container 仍需在真实产品键、metadata 编码及调用入口冻结后重跑。实板写入及写入中断电未授权也未验证，现有完整旧 `base_store` 的正式 preflight 仍阻断。
