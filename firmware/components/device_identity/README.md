# device_identity

读取芯片、Flash、MAC，并在 NVS 保存或验证 UUID v4

## 架构拓扑

```mermaid
flowchart LR
    app["apps/esp_base"] --> component["device_identity"]
    component --> sdk["ESP-IDF"]
```


