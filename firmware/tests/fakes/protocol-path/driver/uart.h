#pragma once
#include "esp_err.h"
typedef int uart_port_t;
esp_err_t uart_driver_install(uart_port_t port, int rx_bytes, int tx_bytes,
                             int queue_size, void *queue, int flags);
esp_err_t uart_driver_delete(uart_port_t port);
