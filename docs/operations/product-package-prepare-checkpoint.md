# 产品包内部准备事务检查点

2026-09-28，以 Base `32ad84ab9c462b6c7adaa37f8e1ece2e9eaa9009` 为基线，在 Base 内接入产品包内部准备阶段。该阶段由上层已持久登记操作后持有 Base 的 app／otadata 长事务 claim，双次观察当前签名固件集合，核对当前 ECS2 序号与旧包摘要，再调用固定 Container 源码持久保留未引用槽、写入、整包 Flash 回读 SHA-256、签名／Wasm／授权验证，并独立读回 `PREPARED`。旧确认 guest 在准备期间继续运行；此阶段不停止或激活 guest。

确定性失败在同一操作下将 `WRITING` 或 `PREPARED` 持久转为 `ABORTED` 并再次读回；保留写入但读回不明、固件集合变化或清理未证实时返回不确定，调用者必须保留事务 claim 并从持久状态恢复。验证工作区由准备调用临时分配并释放，避免叠加控制任务栈。旧包槽和另一可启动固件引用由 Container 保留集保护。

## 验证

- C3／ESP32 各自运行 Base 全套 host ASan／UBSan 回归；真实签名 guest 生命周期测试使用精确 Container `d370899b88883d8c23c60884dda9e2dae8bc295d` 与 WAMR `c10736fffdf26d7c2ae234e05aa712df112eb6bf`，两目标均通过。新增用例核对错误旧摘要在 NVS 写入前拒绝、篡改下载字节后持久放弃、合法签名包准备成功且未覆盖旧槽、旧 guest 仍接收事件，以及保留写入读回失败时不擦包槽并保留不确定状态。测试 Flash、NVS 和固件集合均为宿主替身。临时构建输入和 C3 固件构建日志位于 `mac-work-1:/private/tmp/esp-base-package-prepare-20260928/`。
- 固定 ESP-IDF `578cf89c343e388db43ba1f4ddcd602fedcb763c`、原测试 RSA-3072 键与上一轮三份 `0x77000` 槽候选几何下，正式 C3 USB 控制台完整构建通过。`espsecure verify-signature --version 2` 验证签名块 0 成功；`esp_base.bin` 为 `0x121000` B，SHA-256 `54cf6afe0fecf74cae42fc920dcc8eef923d7b7eb07329f8dea87359ae1c2259`。每个 `0x130000` app 槽余 `0xf000` B，分区二进制 SHA-256 `b60a99a70dafed4a0b5089dd1ede3d2c5f310f88d72a40831b82c8fe403c0842`。候选几何仍未写回正式分区表。

公开 `product.install`／`product.upgrade` 命令、受限 HTTPS 来源、产品试运行和持久健康确认尚未接通。业务事件虽已有独立 MQTT Topic 的软件入口，本检查点没有 Broker 真消息或实体板；不能据此验收 P6-03／P6-04、刷写设备或宣布迁移完成。
