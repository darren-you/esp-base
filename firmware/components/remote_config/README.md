# remote_config

读取 NVS 配置代次，尚未实现配置写事务

## 架构拓扑

```mermaid
flowchart LR
    app["apps/esp_base"] --> component["remote_config"]
    component --> sdk["ESP-IDF"]
```


