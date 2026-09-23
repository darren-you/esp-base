idf_component_register(SRCS "esp_base_container_binding.c" INCLUDE_DIRS "include"
    REQUIRES ota_operation esp_container)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
