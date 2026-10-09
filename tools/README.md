# ESP Base 宿主工具

运行在 macOS／Linux 宿主，不进入 MCU 固件。设备操作必须使用本轮精确 UUID、boot、端点与唯一租约；以下占位不指向真实设备。软件与实板进度见[唯一计划](../docs/operations/ota-allocation-diagnostic-checkpoint.md)。

## 设备控制与升级

`device_control.py` 复用独占串口、严格 JSON、精确设备／boot 和原 ID 查询。命令为 status、firmware.status、business.status／pause／resume、restart、config.set、ota.start／ota.result。旧 product.* 与联合包选项在打开串口前拒绝，没有 wrapper。

```bash
python3 tools/device_control.py --port /dev/cu.usbmodemEXAMPLE status
python3 tools/device_control.py --port /dev/cu.usbmodemEXAMPLE firmware.status
python3 tools/device_control.py --port /dev/cu.usbmodemEXAMPLE business.status
python3 tools/device_control.py --port /dev/cu.usbmodemEXAMPLE --device-id <本轮UUID> business.pause
python3 tools/device_control.py --port /dev/cu.usbmodemEXAMPLE --device-id <本轮UUID> --operation-id <固定原UUID> --image-file /absolute/private/signed.bin --image-url https://example.invalid/signed.bin --target esp32c3/esp_base ota.start
python3 tools/device_control.py --port /dev/cu.usbmodemEXAMPLE --operation-id <固定原UUID> ota.result
```

有线 OTA 是下发 URL 后由设备 HTTPS 拉取完整 signed bin；软件扫描本地文件得到摘要／长度，提交前核对同设备、当前 boot 与 OTA 可用状态。当前 USB CLI 提交后返回 running／unknown；持久结果须另按原 operation_id 查询，当前镜像身份须独立 `firmware.status` 核对，不能把这些分开的调用当成已自动完成新 boot 成功对账。写只提交一次，超时／unknown不重发、不另建操作。ESP Tool Mac App 的当前职责是本机有线管理，使用这些设备命令；其内置 FRP／frpc 与远程 Bridge 正按最新计划硬切删除，新本机调用链另行验收，不再作为正式使用路线。官方有线刷写／恢复由对应宿主能力执行，不能与 app OTA 混同。

`frp_ota.py` 经明确设备 FRP 地址执行认证控制、固件 PUT 和原 ID 查询，固件正文不走设备直连 HTTPS。持久成功还须来自不同于提交时的新 boot，随后独立 `firmware.status` 必须在同一新 boot 精确匹配完整摘要、尺寸、target 与真实 OTA 槽；原 ID 查询和身份核对共用原终态绝对期限。迟到、错身份或未知结果保留原 ID，不重发。HTTP／HMAC／期限和命令示例见[FRP 软件检查点](../docs/operations/frp_ota_software_checkpoint.md)。独立 FRP 不要求 Mac／USB 在线；宿主 loopback 测试不证明已经经过正式 FRPS或实板。

## 原生 MQTT 业务事件

`business_event.py` 从私有普通事件文件生成域隔离 HMAC帧；key 文件须0600、非符号链接，不回显key。`business_event_publish.py` 通过严格 TLS／QoS1／非 retained 仅发布一次，先核对本 boot reported 的下一连续序号，随后按 boot、序号、原始事件摘要和实际 business result 对账。输入只绑定原生业务身份。

完整 event frame 最多4096 B，业务原始字节最多3924 B；输入语义与13字段reported见[设备协议](../docs/design/device-protocol.md)。Broker PUBACK不是设备执行成功；丢失结果不自动重发业务或换序号。

## 构建与回归

```bash
python3 -m unittest discover -s tools -p 'test_business_event*.py' -v
python3 -m unittest discover -s tools -p 'test_device_control.py' -v
python3 -m unittest discover -s tools -p 'test_frp_ota.py' -v
python3 -m unittest discover -s tools -p 'test_*partition_table.py' -v
```

分区测试使用 IDF_PATH 指向锁定SDK，调用官方生成器核对两目标4MiB几何、签名对齐、双app、scratch／NVS和ESP32只读AT区。实板未连接时不写串口或Flash。

## SDK 源码准备

本仓 `sdk-lock.json` 锁定公开 ESP-IDF 受控来源 的 OTA 擦除失败和 HTTP 初始化低内存清理修正，以及公开 esp-lwip 的零窗口修正。首次准备独立 SDK 时，将 `ESP_BASE_IDF` 指向仓外的新路径：

```bash
ESP_BASE_IDF=/private/path/esp-base-idf
git clone --no-checkout --branch master \
  https://github.com/darren-you/reference-esp-idf.git "$ESP_BASE_IDF"
git -C "$ESP_BASE_IDF" checkout --detach fb53f8a76df5ea913715658f5ac602e91a094e72
git -C "$ESP_BASE_IDF" submodule update --init --recursive --checkout --no-recommend-shallow
git -C "$ESP_BASE_IDF/components/lwip/lwip" fetch \
  https://github.com/darren-you/esp-lwip.git 2758df4cd3666b3b2a5b53830148379326425c0d
git -C "$ESP_BASE_IDF/components/lwip/lwip" checkout --detach FETCH_HEAD
bash "$ESP_BASE_IDF/install.sh" esp32c3 esp32
source "$ESP_BASE_IDF/export.sh"
python3 tools/check_sdk.py --path "$IDF_PATH"
```

取源显式忽略上游浅克隆建议，保留根与所有递归依赖的完整历史。构建同时核对两个精确提交、SDK 索引与工作树、所有其他子模块及最终解析的 lwIP 组件路径；SDK 工作树只允许这一个锁定 lwIP gitlink 差异。来源 gitdir 与 common-dir 必须一致，对象目录及对象不能通过符号链接借用仓外存储；linked worktree 被拒绝，正常 absorbed submodule 的 `.git` 定位文件保留。每个递归来源显式检查工作树，不受 `submodule.*.ignore` 配置影响。Git remote 使用 HTTPS 或 SSH 不改变提交身份。C3 使用 `firmware/dependencies.lock`，ESP32 使用 `firmware/dependencies.lock.esp32`；二者分别固定 target，引用同一组精确组件提交，不能共用生成的 sdkconfig/build 目录。以上准备和检查不访问串口或写设备；实验应用仍须提供仓外输入，并按固件 README 使用独立 build 与 sdkconfig。

## 一次性有线迁入与历史输入

`preflight_v3_migration.py` 仍只做旧C3 v1/v2配置的只读预检，不是当前原生布局迁入。`archive_esp32_at.py` 核对两份完整备份，归档旧AT原始NVS／at_customize而不解码或打印凭据。新布局的一次性离线准备工具只在审计原终态、归档回读与完整输入验证后生成候选，不触达设备；未决／损坏保持阻断，不能清空NVS。最终精确命令与软件边界见[原生软件检查点](../docs/operations/native_software_checkpoint.md)。

原生候选／公钥／分区输入、旧C3双备份和现役AT双备份的读取先以非阻塞方式打开，再核对普通文件及原权限／尺寸规则；无写入方的FIFO会明确拒绝，不会停在打开阶段。完整受影响回归35项及原失败见[输入拒绝补审](../docs/operations/native_software_checkpoint.md#一次性迁入输入拒绝补审)；本工具仍只准备离线输入，不能代替本轮实体身份、正式信任、恢复基线和唯一租约。

旧动态包生命周期、专属NVS容量探针与旧QEMU组合入口已删除。当前离线准备只消费明确SDK、真实旧布局终态与签名固件，工具与本轮设备写入资格分别核对。

## 资源与独立 MQTT 实验

`capacity_observation.py` 从实验UART读取顺序周期堆／完整任务快照／退出记录，保持16384／24576／1024 B门；必须匹配真实观察器的完整启动声明、明确 target、唯一启动及非递减 uptime。缺失、错目标、重复启动、uptime 回退、损坏或不完整数据不通过，跨文件也不合并不同启动轮。原日志只读并保存摘要。它不是瞬时峰值或完整native生命周期资格，观察器成本不加回。

`prepare_capacity_observer.py` 仅在仓外、0700、无 Git 或链接的独立源码副本加入观察器。必须提供本轮软件收据和所有冻结源根；副本的原生产输入逐项与 canonical／收据核对，重复实验、错误 target、旧 anchor 和不稳定输入拒绝。实验使用既有控制 pass 每5秒枚举最多32个真实任务；锁定 SDK 的官方 task pre-deletion hook 在正常清理前复制任务名、编号与最低栈，启动早期、FRP、MQTT、SDK任务都按实际 RTOS 清理记录。64条退出缓存不新增任务／队列／堆申请，每次 control pass 最多输出64条；溢出标记使本轮解析失格，另保留OTA worker完成前记录。

构建须显式启用 `ESP_BASE_CAPACITY_OBSERVER_LAB=ON`，加载生成的 `capacity_observer_lab.defaults` 中 trace／pre-deletion hook，使用锁定单核非SMP的独立 sdkconfig／build。版本和收据标记 `0.2.0-capacity-lab`／`LAB_ONLY`，没有生产发布资格；正式发布门尚未接入，不能仅靠标记声称平台已经拒绝该制品。异常重启／panic／尚未清理任务、日志丢失、非任务栈、其他内存能力域及瞬时峰值仍缺资格，不能把实验读数移给已冻结生产镜像。5秒采样的 largest 不能证明全程最低连续块；锁定 SDK 的 heap hooks 不提供安全、完整的全域重建依据，本工具不在 allocator／ISR 中遍历 heap。

```bash
python3 tools/prepare_capacity_observer.py \
  --source-root /private/path/esp32c3/base --target esp32c3 \
  --baseline-receipt /absolute/private/native_software_20261006/receipt.json \
  --frozen-source-root /absolute/private/frozen-build-root \
  --frozen-source-root /absolute/private/native_software_20261006
python3 tools/capacity_observation.py --target esp32c3 --uart-log /absolute/private/uart.log --json
```

两个目标分别复制、生成和构建。只在该私有副本中移除第三方 `.git` 元数据；不能改 canonical、SDK 或冻结归档。解析器不跨日志拼接残缺任务帧，也不合并多个 boot 获得长稳资格。

`mqtt_lab_check.py`／`mqtt_resource_report.py` 继续用于独立MQTT实验的严格TLS、往返、计数／栈和回收。实验输入必须位于仓外，不混入普通Base，独立实验不替代本轮FRP／MQTT／原生业务／OTA同存验收。

## 官方 FRPS 宿主互操作

[frps_ota_scenario.py](frps_ota_scenario.py) 是 macOS／Linux 主机消费者，与 `frp_ota.py` 同目录导入；[frps-ota-interop](../firmware/tests/frps-ota-interop/) 是 C／Go 主机联调夹具的职责目录，保留各语言文件名；构建器只读取该最终路径。构建器收据分别记录夹具清单和该主机场景的精确路径、摘要。

[run_frps_ota_interop_test.sh](../firmware/tests/run_frps_ota_interop_test.sh) 在本机回环启动 catalog 精确版本的官方 FRPS，链接冻结 efrp／eota 与 Base 实际 listener、handler、owner 和 V4 收据。双目标完整 signed 输入、成功／写入失败／NVS不确定六场景、真实流预算及替身范围见[FRP 软件检查点](../docs/operations/frp_ota_software_checkpoint.md#官方-frps-与原生-ota-宿主联调检查点)。它不安装生产 FRPS，不访问设备；POSIX／RAM Flash／SDK替身不能授予公网、实板或bootloader资格。

## 百次与连续72小时有限驱动

`native_lifecycle_run.py --help` 列出完整参数。`--mode cycles-100` 固定100次完整周期，`--mode soak-72h` 固定本轮连续259200秒，默认每小时OTA、每5秒业务。运行前由既有流程完成实体UUID／芯片／4MiB／安全状态、本轮恢复基线、唯一租约、正式信任和R5验收；本工具不管理备份／租约，也不读取一份摘要就授予R5通过。

宿主MQTT依赖复用[mqtt-lab-requirements.txt](mqtt-lab-requirements.txt)的固定Paho 2.1.0，安装到仓外私有环境；实际运行和验证须使用这个环境的Python：

```bash
python3 -m venv /absolute/private/native-run-venv
chmod 700 /absolute/private/native-run-venv
/absolute/private/native-run-venv/bin/python -m pip install -r tools/mqtt-lab-requirements.txt
/absolute/private/native-run-venv/bin/python tools/native_lifecycle_run.py --help
```

两份 `--image-a`／`--image-c` 必须是同一已评审原生实现、同目标、不同完整signed摘要且均被外部R5覆盖的冻结镜像，分别显式提供 `--image-*-sha256`、`--image-*-size-bytes`、`--image-*-target`。同镜像OTA由实际收据拒绝，所以不能以一份bin运行百次，也不能把FRPS测试的旧架构A当成本轮已验pair。启动的 `--device-id`、`--initial-boot-id` 和 `--endpoint` 精确绑定本轮设备；当前运行身份不在pair、提交前boot改变或本地文件变化都会停止写入。

`--r5-evidence-file` 为0600原件，必须给出预先核对的 `--r5-evidence-sha256`；`--capacity-log-file` 为0600活动原始日志，绑定本轮起始前缀、inode及结束追加范围，不跨轮拼接。MQTT使用既有 `--credentials-file`／`--business-management-key-file`／`--ca-file` 与严格TLS、QoS1、非retained；FRP key只从 `--frp-management-key-env` 指定的现有环境变量读取，不打印凭据。输出 `--output-directory` 必须是新目录，0700／0600 journal逐条fsync，保存单次run、原operation、精确身份、时刻与中断原因。

每次周期包含最大3924 B认证原生事件、后台上传期间业务、一次FRP固件PUT、新boot原ID成功与完整镜像对账、再次业务及暂停／恢复。PUBACK不是业务成功；必须收到本boot／序号／摘要对应的实际reported结果。上传回调只唤醒有限业务线程，记录业务待决窗口与host上传progress区间的相交，不能据此证明业务执行时刻或设备满合法峰值。丢失结果、unknown、错误身份或非预期重启均保留原ID并停止，不重发写、不换ID。区分保留ID、命令已尝试与设备实际确认准入；尝试边界不证明字节已发出。已尝试操作异常后尽力一次有限原ID只读查询，保留实际响应和独立身份裁决，不覆盖主异常或改判成功。

72小时普通步骤超过10秒宿主进展空档即中断，OTA有界等待单独计时；每秒检查等待并对照墙钟／单调钟增量，任一回退或差值超过固定1秒容限中断，系统睡眠不能绕过Mac单调钟不推进的事实。并发观察在同一现有锁内采样和更新进展／OTA窗口，避免线程调度把正常递增时钟误判为回退；日志与异常处理在锁外。最终R5／容量原件摘要与已绑定前缀核对后，再执行同一宿主观察；正常摘要终点直接使用该次已核验采样，异常保留最近实际记录的原始采样，`observation_end_scope=last_recorded_host_clock_sample`。采样前失败不会伪称取得新时刻，真实回退的负时长保持，已有主中断原因不被收尾异常覆盖。后续摘要／Journal写入不计入观察时段，也不宣称检测了这段时间。OTA窗口仍只是有界等待，允许的等待时间不证明设备瞬时连续状态。两种模式始终输出 `qualified=false`／`r6_passed=false`，只记录是否完成本轮有限观察；全任务回收、连续块峰值、公网脱离Mac、断电和Flash寿命须由外部实板原件共同裁决，短轮次不能相加成为72小时。依赖／凭据／CA等Journal创建前的预检错误只返回CLI失败；Journal建立后的中断才保存run摘要。

SDK 的 Actions 退出来源以原 `esp-space/esp-idf@578cf89c343e388db43ba1f4ddcd602fedcb763c` 为业务基线，只追加源码退出与实际嵌套来源绑定；受控来源的 `workspace-source.json` 保留精确上游追溯。lwIP 锁需在源码退出 PR 合入 `darren-you/esp-lwip` canonical `master` 后再选择精确版本，当前不使用未合并任务 head；源码变更不代表固件、Broker、实板或发布已完成。
