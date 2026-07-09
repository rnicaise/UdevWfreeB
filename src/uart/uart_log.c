#include "uart_log.h"

#include "nrf.h"
#include "nrf_gpio.h"
#include <string.h>

#define UART_TX_TIMEOUT_CYCLES 20000u
#define UART_TX_CHUNK_MAX 192u
#define UART_RX_CMD_MAX 96u
#define UART_RX_CMD_QUEUE_LEN 4u
#define UART_RX_DMA_BUF_LEN 64u
#define UART_RX_STOP_TIMEOUT_CYCLES 2000u

#define UART_TX_PIN NRF_GPIO_PIN_MAP(0, 19)
#define UART_RX_PIN NRF_GPIO_PIN_MAP(0, 15)

static uint32_t rx_line_len;
static uint8_t cmd_queue_head;
static uint8_t cmd_queue_tail;
static uint8_t cmd_queue_count;

#ifndef UWB_UART_RX_DISABLED
static uint8_t rx_buf[UART_RX_DMA_BUF_LEN];
static char rx_line[UART_RX_CMD_MAX];
static char cmd_queue[UART_RX_CMD_QUEUE_LEN][UART_RX_CMD_MAX];

static void uart_enqueue_cmd(const char *line);

static void uart_process_rx_byte(uint8_t b)
{
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
}

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

static void uart_start_rx_buffer(void)
{
    NRF_UARTE0->RXD.PTR = (uint32_t)rx_buf;
    NRF_UARTE0->RXD.MAXCNT = UART_RX_DMA_BUF_LEN;
    NRF_UARTE0->EVENTS_ENDRX = 0;
    NRF_UARTE0->EVENTS_RXTO = 0;
    NRF_UARTE0->TASKS_STARTRX = 1;
}
#endif

static bool tx_busy = false;

#ifndef UWB_UART_TX_DISABLED
static void uart_wait_tx_done(void)
{
    uint32_t guard = 0;

    if (!tx_busy)
    {
        return;
    }

    while ((NRF_UARTE0->EVENTS_ENDTX == 0) && (guard < UART_TX_TIMEOUT_CYCLES))
    {
        guard++;
    }

    if (NRF_UARTE0->EVENTS_ENDTX == 0)
    {
        /* Fail-safe: never block the ranging loop if UART stalls. */
        NRF_UARTE0->TASKS_STOPTX = 1;
    }

    NRF_UARTE0->EVENTS_ENDTX = 0;
    tx_busy = false;
}

/* Pipelined TX: start the DMA transfer and return immediately.
 * The wait happens at the START of the next transfer, so UART
 * transmission overlaps the UWB ranging exchange instead of
 * blocking the hot loop (~1.7 ms saved per CSV line at 460800). */
static void uart_write_bytes(const uint8_t *data, uint32_t len)
{
    if (len == 0)
    {
        return;
    }

    uart_wait_tx_done();

    NRF_UARTE0->TXD.PTR = (uint32_t)data;
    NRF_UARTE0->TXD.MAXCNT = len;
    NRF_UARTE0->EVENTS_ENDTX = 0;
    NRF_UARTE0->TASKS_STARTTX = 1;
    tx_busy = true;
}
#endif

void uart_log_init(void)
{
    tx_busy = false;
    rx_line_len = 0;
    cmd_queue_head = 0;
    cmd_queue_tail = 0;
    cmd_queue_count = 0;

#if defined(UWB_UART_RX_DISABLED) && defined(UWB_UART_TX_DISABLED)
    return;
#else
    NRF_UARTE0->ENABLE = 0;

#ifndef UWB_UART_TX_DISABLED
    nrf_gpio_cfg_output(UART_TX_PIN);
#endif
#ifndef UWB_UART_RX_DISABLED
    nrf_gpio_cfg_input(UART_RX_PIN, NRF_GPIO_PIN_NOPULL);
#endif

#ifndef UWB_UART_TX_DISABLED
    NRF_UARTE0->PSEL.TXD = UART_TX_PIN;
#else
    NRF_UARTE0->PSEL.TXD = 0xFFFFFFFF;
#endif
#ifndef UWB_UART_RX_DISABLED
    NRF_UARTE0->PSEL.RXD = UART_RX_PIN;
#else
    NRF_UARTE0->PSEL.RXD = 0xFFFFFFFF;
#endif
    NRF_UARTE0->PSEL.CTS = 0xFFFFFFFF;
    NRF_UARTE0->PSEL.RTS = 0xFFFFFFFF;

    NRF_UARTE0->CONFIG = 0;
#if defined(UWB_UART_BAUD_1M)
    /* Qorvo advice: faster offload so UART never bounds the ranging rate. */
    NRF_UARTE0->BAUDRATE = UARTE_BAUDRATE_BAUDRATE_Baud1M;
#else
    NRF_UARTE0->BAUDRATE = UARTE_BAUDRATE_BAUDRATE_Baud460800;
#endif

    NRF_UARTE0->ENABLE = UARTE_ENABLE_ENABLE_Enabled;

#ifndef UWB_UART_RX_DISABLED
    uart_start_rx_buffer();
#endif
#endif
}

void uart_log_write(const char *str)
{
#ifdef UWB_UART_TX_DISABLED
    (void)str;
    return;
#else
    /* Ping-pong buffers: one can be filled while the other is being
     * read by EasyDMA (TX is pipelined, see uart_write_bytes). */
    static uint8_t tx_bufs[2][UART_TX_CHUNK_MAX + 2];
    static uint8_t tx_buf_idx = 0;
    uint32_t len = 0;
    uint32_t i = 0;

    if (str == 0)
    {
        return;
    }

    while (str[len] != '\0')
    {
        len++;
    }

    if (len == 0)
    {
        uint8_t *buf = tx_bufs[tx_buf_idx];
        buf[0] = '\r';
        buf[1] = '\n';
        uart_write_bytes(buf, 2);
        tx_buf_idx ^= 1u;
        return;
    }

    /*
     * UARTE EasyDMA can only read from RAM.
     * Copy text into a RAM buffer before starting TX.
     * CRLF is appended to the final chunk so a full line goes out
     * in a single transfer (avoids unterminated lines on the host).
     */
    while (i < len)
    {
        uint8_t *buf = tx_bufs[tx_buf_idx];
        uint32_t chunk = len - i;
        uint32_t j;

        if (chunk > UART_TX_CHUNK_MAX)
        {
            chunk = UART_TX_CHUNK_MAX;
        }

        for (j = 0; j < chunk; j++)
        {
            buf[j] = (uint8_t)str[i + j];
        }

        i += chunk;

        if (i >= len)
        {
            buf[chunk++] = '\r';
            buf[chunk++] = '\n';
        }

        uart_write_bytes(buf, chunk);
        tx_buf_idx ^= 1u;
    }
#endif
}

void uart_log_flush(void)
{
#ifndef UWB_UART_TX_DISABLED
    uart_wait_tx_done();
#endif
}

void uart_log_poll_rx(void)
{
#ifdef UWB_UART_RX_DISABLED
    return;
#else
    uint32_t guard = 0;
    uint32_t amount;
    uint32_t idx;

    NRF_UARTE0->TASKS_STOPRX = 1;
    while ((NRF_UARTE0->EVENTS_RXTO == 0) && (NRF_UARTE0->EVENTS_ENDRX == 0) &&
           (guard < UART_RX_STOP_TIMEOUT_CYCLES))
    {
        guard++;
    }

    amount = NRF_UARTE0->RXD.AMOUNT;
    if (amount > UART_RX_DMA_BUF_LEN)
    {
        amount = UART_RX_DMA_BUF_LEN;
    }

    NRF_UARTE0->EVENTS_ENDRX = 0;
    NRF_UARTE0->EVENTS_RXTO = 0;

    for (idx = 0; idx < amount; idx++)
    {
        uart_process_rx_byte(rx_buf[idx]);
    }

    uart_start_rx_buffer();
#endif
}

bool uart_log_read_command(char *out, uint32_t out_len)
{
#ifdef UWB_UART_RX_DISABLED
    (void)out;
    (void)out_len;
    return false;
#else
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
#endif
}
