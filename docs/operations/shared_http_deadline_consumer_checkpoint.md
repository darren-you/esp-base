# 产品包 HTTPS 共享期限消费检查点

2026-10-02，仓外 Base 候选以公开 `e302e217f617e30749f286992c2b9115d91f6207` 完整导出为基础，精确叠加其至公开 `84d565d6ba612e56ed0acfd9874c385854481bc7` 的停止／启动改动，再验证产品包 HTTPS 适配。新增行为不依赖独立生命周期结果冒充最终组合。

`product_package_source.c` 使用公开 OTA 的 `eota_http_transport.h` 创建唯一 transport，并由来源 owner 保有共享期限对象。固定 SDK HTTP client 只借用 transport；失败路径和末段读取成功均先 cleanup HTTP，再 destroy transport，最后释放来源 owner。末段读取结束后离线包摘要／Flash 验签不再占用 TLS。工厂负责默认 CA bundle、强制验证、证书日期与 URL 原主机名，Base 不传入第二套 CA 或跳过验证配置。

原产品策略保持：最多 1024 字节 URL、HTTPS、HTTP 200、精确且一致的 Content-Length、非 chunked、不重定向、连续 offset；连接 5 秒、单次操作 1 秒、无进展 30 秒、总传输 300 秒。网络进展只能在 OTA 的私有函数中刷新；迟到字节不能延长旧期限，持续进展不能延长总期限。包签名、授权、槽、Flash、操作意图和持久结果继续由 Base／Container 的原合同负责。

## 前序软件验证与实验装配

- C3 与 ESP32 全套 ASan／UBSan host 入口通过，包括公开停止／启动、原 ID 查询及产品包读取器。新增假件覆盖 owner 分配、transport 创建、HTTP 初始化、打开、慢响应、EAGAIN、错误响应、迟到／未完整正文，以及三层释放顺序。真实期限源码参与适配回归；假件不执行真实 TLS 或 SDK HTTP 解析。
- OTA 的同一 transport 另通过固定 SDK 真实 mbedTLS 8 项回环；CA／主机名／SNI 与慢握手／慢滴流验证属于机制证据。
- 固定 SDK 为公开 IDF `578cf89c343e388db43ba1f4ddcd602fedcb763c` 与独立 lwIP `2758df4cd3666b3b2a5b53830148379326425c0d`。Base C3、ESP32 普通完整构建均通过，ESP32 普通构建明确为不可刷写的 unsigned offline probe。
- 最终完整产品签名构建与官方验签均通过：C3 为 RSA v2，镜像 `0x121000`／原槽 `0x130000`，余量 `0xf000`；ESP32 为 ECDSA v1，镜像 `0x10fff4`／原槽 `0x120000`，余量 `0x1000c`。签名和产品授权复用先前已有仓外软件测试 fixture，不生成新秘密，不代表设备首次启动信任。
- 候选仅通过 Component Manager 的 `override_path` 装配 OTA；同时保留 `git` 时实际 source 仍是 GitSource，因此实验依赖块改为只有 override_path，并从 `project_description.json` 核对实际 provider 目录／编译输入。实验 manifest 与两个生成锁不属于正式改动；正式消费必须在 OTA provider 保存后用其完整公开 SHA 和官方组件管理器重新生成双目标锁并复验。

## 正式 Git 消费复验

后续以公开 Base `ee530d81ecc3743827779818caa7beea075e6f32` 为完整输入，保留其新 MQTT 任务栈诊断文档及公开停止／启动源码，精确叠加上述读取器。正式组件清单与官方生成的 C3／ESP32 普通锁均改为 OTA `bf11916ab904be4ee9bcdfae213c85336363e96a`，原生来源为 `git`，没有 `override_path`；两个 NVS 探针锁保持原字节。

此正式 Git 组合重新通过两目标完整 host、四份普通／签名 SDK 构建、C3 RSA v2 与 ESP32 ECDSA v1 官方验签。签名镜像及槽余量仍分别为 `0x121000/0x130000`、`0x10fff4/0x120000`。公开 OTA 同时通过 7 项 host 与 8 项固定 SDK 真实 TLS 回环。MQTT／FRP／OTA／Container／WAMR 的官方组件打包字节和执行位逐份对照公开 Git；每份实际编译输入核对 OTA 4 单元、lwIP 子仓 80 单元和父 SDK wrapper 11 单元。独立 lwIP 的 771 个 tracked 文件一致且工作树干净；SDK 父仓保留既有原生 gitlink 与独立 lwIP 双锁差异，不能声称父仓完全干净。

最终 65 项冷证据及 328 文件源导出逐份核验，归档 SHA-256 为 `867bf90d2bb203fa19270c9836f881c5a3533784c7af2af1fdbc99153cbb86ec`。主任务独立核对全部摘要与主树变更前后字节；证据冻结于 Tool 的私有 `c3-validation-20261002/shared-http-official-git-software`。本文正式消费段为复验后的文档回写，不把前序 override 实验当作正式原生锁结果。

## 仍开放的边界

本记录证明最终软件组合与既有产品策略的适配。没有设备／生产访问、分区迁移、秘密生成或 SDK 修改。默认设备 CA bundle、实际 DNS／HTTP／TLS、Flash 延迟、双方硬件、完整五能力峰值及掉电恢复仍未验收；不以软件回环或槽尺寸认定实板可交付。共享接口没有新增外部取消能力，原产品 owner 的业务取消、停止与原生回收继续使用既有合同。
