idf_component_register(SRCS "nvs_capacity_probe.c" INCLUDE_DIRS "."
    REQUIRES nvs_flash remote_config mbedtls)
if(NOT DEFINED PROBE_STAGE OR NOT PROBE_STAGE MATCHES "^[123]$")
    message(FATAL_ERROR "Pass -D PROBE_STAGE=1, 2 or 3")
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE PROBE_STAGE=${PROBE_STAGE})
