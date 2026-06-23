#ifndef UART_LOG_H
#define UART_LOG_H

#include <stdbool.h>
#include <stdint.h>

/* Initialize UARTE0 on VCOM pins (P0.19 TX, P0.15 RX) at 460800 baud */
void uart_log_init(void);

/* Send a null-terminated string + \r\n over UART */
void uart_log_write(const char *str);

/* Wait for the current UART TX DMA transfer to complete. */
void uart_log_flush(void);

/* Poll RX state machine (non-blocking) and assemble a line command. */
void uart_log_poll_rx(void);

/* Return true when a full line command is available and copy it to out. */
bool uart_log_read_command(char *out, uint32_t out_len);

#endif /* UART_LOG_H */
