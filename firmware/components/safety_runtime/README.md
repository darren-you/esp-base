# safety_runtime

读取复位原因并初始化看门狗

## 架构拓扑

```mermaid
flowchart LR
    app["apps/esp_base"] --> component["safety_runtime"]
    component --> sdk["ESP-IDF"]
```


