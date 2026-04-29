#include "uart_log.h"

#include "nrf.h"
#include "nrf_gpio.h"
#include <string.h>

#define UART_TX_TIMEOUT_CYCLES 20000u
#define UART_TX_CHUNK_MAX 96u
#define UART_RX_CMD_MAX 96u
#define UART_RX_CMD_QUEUE_LEN 4u

#define UART_TX_PIN NRF_GPIO_PIN_MAP(0, 19)
#define UART_RX_PIN NRF_GPIO_PIN_MAP(0, 15)

static uint8_t rx_byte;
static char rx_line[UART_RX_CMD_MAX];
static char cmd_queue[UART_RX_CMD_QUEUE_LEN][UART_RX_CMD_MAX];
static uint32_t rx_line_len;
static uint8_t cmd_queue_head;
static uint8_t cmd_queue_tail;
static uint8_t cmd_queue_count;

static void uart_enqueue_cmd(const char *line)
{
    if (line == 0)
    {
        return;
    }

    /* If queue is full, drop the oldest so the newest commands still apply. */
    if (cmd_queue_count >= UART_RX_CMD_QUEUE_LEN)
    {
        cmd_queue_head = (uint8_t)((cmd_queue_head + 1u) % UART_RX_CMD_QUEUE_LEN);
        cmd_queue_count--;
    }

    memcpy(cmd_queue[cmd_queue_tail], line, UART_RX_CMD_MAX);
    cmd_queue_tail = (uint8_t)((cmd_queue_tail + 1u) % UART_RX_CMD_QUEUE_LEN);
    cmd_queue_count++;
}

static void uart_start_rx_byte(void)
{
    NRF_UARTE0->RXD.PTR = (uint32_t)&rx_byte;
    NRF_UARTE0->RXD.MAXCNT = 1;
    NRF_UARTE0->EVENTS_ENDRX = 0;
    NRF_UARTE0->TASKS_STARTRX = 1;
}

static void uart_write_bytes(const uint8_t *data, uint32_t len)
{
    uint32_t sent = 0;
    uint32_t guard = 0;

    if (len == 0)
    {
        return;
    }

    NRF_UARTE0->TXD.PTR = (uint32_t)data;
    NRF_UARTE0->TXD.MAXCNT = len;
    NRF_UARTE0->EVENTS_ENDTX = 0;
    NRF_UARTE0->TASKS_STARTTX = 1;

    while ((NRF_UARTE0->EVENTS_ENDTX == 0) && (guard < UART_TX_TIMEOUT_CYCLES))
    {
        guard++;
    }

    if (NRF_UARTE0->EVENTS_ENDTX == 0)
    {
        /* Fail-safe: never block the ranging loop if UART stalls. */
        NRF_UARTE0->TASKS_STOPTX = 1;
        return;
    }

    sent = NRF_UARTE0->TXD.AMOUNT;
    (void)sent;

    NRF_UARTE0->TASKS_STOPTX = 1;
    NRF_UARTE0->EVENTS_TXSTOPPED = 0;
    guard = 0;
    while ((NRF_UARTE0->EVENTS_TXSTOPPED == 0) && (guard < UART_TX_TIMEOUT_CYCLES))
    {
        guard++;
    }
}

void uart_log_init(void)
{
    NRF_UARTE0->ENABLE = 0;

    nrf_gpio_cfg_output(UART_TX_PIN);
    nrf_gpio_cfg_input(UART_RX_PIN, NRF_GPIO_PIN_NOPULL);

    NRF_UARTE0->PSEL.TXD = UART_TX_PIN;
    NRF_UARTE0->PSEL.RXD = UART_RX_PIN;
    NRF_UARTE0->PSEL.CTS = 0xFFFFFFFF;
    NRF_UARTE0->PSEL.RTS = 0xFFFFFFFF;

    NRF_UARTE0->CONFIG = 0;
    NRF_UARTE0->BAUDRATE = UARTE_BAUDRATE_BAUDRATE_Baud115200;

    NRF_UARTE0->ENABLE = UARTE_ENABLE_ENABLE_Enabled;

    rx_line_len = 0;
    cmd_queue_head = 0;
    cmd_queue_tail = 0;
    cmd_queue_count = 0;
    uart_start_rx_byte();
}

void uart_log_write(const char *str)
{
    uint32_t len = 0;
    static uint8_t tx_buf[UART_TX_CHUNK_MAX + 2];
    uint32_t i = 0;

    if (str == 0)
    {
        return;
    }

    while (str[len] != '\0')
    {
        len++;
    }

    /*
     * UARTE EasyDMA can only read from RAM.
     * Copy text into a RAM buffer before starting TX.
     */
    while (i < len)
    {
        uint32_t chunk = len - i;
        uint32_t j;

        if (chunk > UART_TX_CHUNK_MAX)
        {
            chunk = UART_TX_CHUNK_MAX;
        }

        for (j = 0; j < chunk; j++)
        {
            tx_buf[j] = (uint8_t)str[i + j];
        }

        uart_write_bytes(tx_buf, chunk);
        i += chunk;
    }

    tx_buf[0] = '\r';
    tx_buf[1] = '\n';
    uart_write_bytes(tx_buf, 2);
}

void uart_log_poll_rx(void)
{
    uint8_t b;

    if (NRF_UARTE0->EVENTS_ENDRX == 0)
    {
        return;
    }

    NRF_UARTE0->EVENTS_ENDRX = 0;
    b = rx_byte;

    if ((b == '\r') || (b == '\n'))
    {
        if (rx_line_len > 0)
        {
            rx_line[rx_line_len] = '\0';
            uart_enqueue_cmd(rx_line);
        }
        rx_line_len = 0;
    }
    else
    {
        if (rx_line_len < (UART_RX_CMD_MAX - 1u))
        {
            rx_line[rx_line_len++] = (char)b;
        }
        else
        {
            rx_line_len = 0;
        }
    }

    uart_start_rx_byte();
}

bool uart_log_read_command(char *out, uint32_t out_len)
{
    uint32_t n;
    const char *ready_line;

    if ((out == 0) || (out_len == 0) || (cmd_queue_count == 0u))
    {
        return false;
    }

    ready_line = cmd_queue[cmd_queue_head];
    n = (uint32_t)strlen(ready_line);
    if (n >= out_len)
    {
        n = out_len - 1u;
    }

    memcpy(out, ready_line, n);
    out[n] = '\0';
    cmd_queue_head = (uint8_t)((cmd_queue_head + 1u) % UART_RX_CMD_QUEUE_LEN);
    cmd_queue_count--;
    return true;
}
