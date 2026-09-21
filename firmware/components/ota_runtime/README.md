# ota_runtime

读取当前运行槽与 pending 状态，尚未实现下载与验签

## 架构拓扑

```mermaid
flowchart LR
    app["apps/esp_base"] --> component["ota_runtime"]
    component --> sdk["ESP-IDF"]
```


