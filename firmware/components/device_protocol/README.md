# device_protocol

每 5 秒串口输出启动周期及设备心跳

## 架构拓扑

```mermaid
flowchart LR
    app["apps/esp_base"] --> component["device_protocol"]
    component --> sdk["ESP-IDF"]
```


