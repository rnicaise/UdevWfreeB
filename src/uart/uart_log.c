#include "uart_log.h"

#include "nrf.h"
#include "nrf_gpio.h"
#include <string.h>

#ifdef USE_USB_CDC
#include "app_usbd.h"
#include "app_usbd_cdc_acm.h"
#include "app_usbd_serial_num.h"
#include "nrf_drv_clock.h"
#endif

#define UART_TX_TIMEOUT_CYCLES 20000u
#define UART_TX_CHUNK_MAX 96u
#define UART_RX_CMD_MAX 96u
#define UART_RX_CMD_QUEUE_LEN 4u

#define UART_TX_PIN NRF_GPIO_PIN_MAP(0, 19)
#define UART_RX_PIN NRF_GPIO_PIN_MAP(0, 15)

#ifndef USE_USB_CDC
static uint8_t rx_byte;
#endif
static char rx_line[UART_RX_CMD_MAX];
static char cmd_queue[UART_RX_CMD_QUEUE_LEN][UART_RX_CMD_MAX];
static uint32_t rx_line_len;
static uint8_t cmd_queue_head;
static uint8_t cmd_queue_tail;
static uint8_t cmd_queue_count;

static void uart_enqueue_cmd(const char *line);

#ifdef USE_USB_CDC
#define USB_CDC_READ_SIZE 1u

static bool usb_ready;
static uint8_t usb_rx_buf[USB_CDC_READ_SIZE];

static void cdc_acm_user_ev_handler(app_usbd_class_inst_t const *p_inst,
                                    app_usbd_cdc_acm_user_event_t event);
static void usbd_user_ev_handler(app_usbd_event_type_t event);

APP_USBD_CDC_ACM_GLOBAL_DEF(m_app_cdc_acm,
                            cdc_acm_user_ev_handler,
                            0,
                            1,
                            NRF_DRV_USBD_EPIN2,
                            NRF_DRV_USBD_EPIN1,
                            NRF_DRV_USBD_EPOUT1,
                            APP_USBD_CDC_COMM_PROTOCOL_AT_V250);

static void usb_cdc_process_byte(uint8_t b)
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

static void cdc_acm_user_ev_handler(app_usbd_class_inst_t const *p_inst,
                                    app_usbd_cdc_acm_user_event_t event)
{
    app_usbd_cdc_acm_t const *p_cdc = app_usbd_cdc_acm_class_get(p_inst);
    (void)p_cdc;

    switch (event)
    {
        case APP_USBD_CDC_ACM_USER_EVT_PORT_OPEN:
            usb_ready = true;
            (void)app_usbd_cdc_acm_read(&m_app_cdc_acm, usb_rx_buf, USB_CDC_READ_SIZE);
            break;

        case APP_USBD_CDC_ACM_USER_EVT_PORT_CLOSE:
            usb_ready = false;
            break;

        case APP_USBD_CDC_ACM_USER_EVT_RX_DONE:
            usb_cdc_process_byte(usb_rx_buf[0]);
            (void)app_usbd_cdc_acm_read(&m_app_cdc_acm, usb_rx_buf, USB_CDC_READ_SIZE);
            break;

        default:
            break;
    }
}

static void usbd_user_ev_handler(app_usbd_event_type_t event)
{
    switch (event)
    {
        case APP_USBD_EVT_POWER_DETECTED:
            if (!nrf_drv_usbd_is_enabled())
            {
                app_usbd_enable();
            }
            break;

        case APP_USBD_EVT_POWER_READY:
            app_usbd_start();
            break;

        case APP_USBD_EVT_POWER_REMOVED:
            usb_ready = false;
            app_usbd_stop();
            break;

        case APP_USBD_EVT_STOPPED:
            usb_ready = false;
            app_usbd_disable();
            break;

        default:
            break;
    }
}

static void usb_cdc_write_bytes(const uint8_t *data, uint32_t len)
{
    uint32_t sent = 0;

    if ((!usb_ready) || (data == 0u) || (len == 0u))
    {
        return;
    }

    while (sent < len)
    {
        uint32_t chunk = len - sent;
        ret_code_t ret;

        if (chunk > NRF_DRV_USBD_EPSIZE)
        {
            chunk = NRF_DRV_USBD_EPSIZE;
        }

        ret = app_usbd_cdc_acm_write(&m_app_cdc_acm, (void const *)&data[sent], chunk);
        if (ret != NRF_SUCCESS)
        {
            break;
        }
        sent += chunk;
    }
}
#endif

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

#ifndef USE_USB_CDC
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
#endif

void uart_log_init(void)
{
#ifdef USE_USB_CDC
    static const app_usbd_config_t usbd_config = {
        .ev_state_proc = usbd_user_ev_handler,
    };
    app_usbd_class_inst_t const *class_cdc;

    rx_line_len = 0;
    cmd_queue_head = 0;
    cmd_queue_tail = 0;
    cmd_queue_count = 0;
    usb_ready = false;

    if (!nrf_drv_clock_init_check())
    {
        (void)nrf_drv_clock_init();
    }
    nrf_drv_clock_lfclk_request(NULL);

    (void)app_usbd_serial_num_generate();
    (void)app_usbd_init(&usbd_config);
    class_cdc = app_usbd_cdc_acm_class_inst_get(&m_app_cdc_acm);
    (void)app_usbd_class_append(class_cdc);
    (void)app_usbd_power_events_enable();
#else
    NRF_UARTE0->ENABLE = 0;

    nrf_gpio_cfg_output(UART_TX_PIN);
    nrf_gpio_cfg_input(UART_RX_PIN, NRF_GPIO_PIN_NOPULL);

    NRF_UARTE0->PSEL.TXD = UART_TX_PIN;
    NRF_UARTE0->PSEL.RXD = UART_RX_PIN;
    NRF_UARTE0->PSEL.CTS = 0xFFFFFFFF;
    NRF_UARTE0->PSEL.RTS = 0xFFFFFFFF;

    NRF_UARTE0->CONFIG = 0;
    NRF_UARTE0->BAUDRATE = UARTE_BAUDRATE_BAUDRATE_Baud460800;

    NRF_UARTE0->ENABLE = UARTE_ENABLE_ENABLE_Enabled;

    rx_line_len = 0;
    cmd_queue_head = 0;
    cmd_queue_tail = 0;
    cmd_queue_count = 0;
    uart_start_rx_byte();
#endif
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

#ifdef USE_USB_CDC
        usb_cdc_write_bytes(tx_buf, chunk);
#else
        uart_write_bytes(tx_buf, chunk);
#endif
        i += chunk;
    }

    tx_buf[0] = '\r';
    tx_buf[1] = '\n';
#ifdef USE_USB_CDC
    usb_cdc_write_bytes(tx_buf, 2);
#else
    uart_write_bytes(tx_buf, 2);
#endif
}

void uart_log_poll_rx(void)
{
#ifdef USE_USB_CDC
    /* USB events are handled by the USBD interrupt path in this config. */
#else
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
#endif
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
