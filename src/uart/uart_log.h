#ifndef UART_LOG_H
#define UART_LOG_H

#include <stdint.h>

/* Initialise UARTE0 on VCOM pins (P0.19 TX, P0.15 RX) at 115200 baud */
void uart_log_init(void);

/* Send a null-terminated string + \r\n over UART */
void uart_log_write(const char *str);

#endif /* UART_LOG_H */
