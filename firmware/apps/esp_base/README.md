# 无业务基座应用

装配身份、配置读取、槽状态、复位事实和串口心跳；不配置 GPIO。

## 架构拓扑

```mermaid
flowchart LR
    main["main/esp_base_main.c"] --> components["本仓 components"]
    components --> sdk["ESP-IDF"]
```

在仓库根使用 `idf.py -C firmware build`。
