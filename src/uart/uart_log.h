#ifndef UART_LOG_H
#define UART_LOG_H

#include <stdbool.h>
#include <stdint.h>

/* Initialise UARTE0 on VCOM pins (P0.19 TX, P0.15 RX) at 115200 baud */
void uart_log_init(void);

/* Send a null-terminated string + \r\n over UART */
void uart_log_write(const char *str);

/* Poll RX state machine (non-blocking) and assemble a line command. */
void uart_log_poll_rx(void);

/* Return true when a full line command is available and copy it to out. */
bool uart_log_read_command(char *out, uint32_t out_len);

#endif /* UART_LOG_H */
