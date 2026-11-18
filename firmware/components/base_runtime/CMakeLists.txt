# 业务层只依赖这个组件：公开目录里只有 base_runtime.h。system/ 是留给系统层的接口，不在公开目录里，
# 系统组件在自己的 CMakeLists.txt 里显式加入；业务组件 #include 不到系统的配置、存储和协议头文件。
idf_component_register(SRCS "base_runtime.c" "base_guard.c" INCLUDE_DIRS "include" PRIV_INCLUDE_DIRS "system"
    PRIV_REQUIRES base_protocol base_store esp_timer)
