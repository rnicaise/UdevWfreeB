/*
 * main_responder.c - Module B (Responder) SS-TWR
 */

#include "deca_probe_interface.h"
#include <deca_device_api.h>
#include <deca_spi.h>
#include <port.h>
#include <shared_defines.h>
#include <shared_functions.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <nrf.h>
#include <nrf_delay.h>

#include "../common/ranging.h"
#include "../common/uwb_profiles.h"
#include "../accel/accel.h"
#include "../board/pyro_buzzer.h"
#include "../uart/uart_log.h"
#include "../common/radio_quality.h"

#define POLL_MSG_PROFILE_IDX      16
#define POLL_MSG_SWITCH_TOKEN_IDX 17
#define POLL_MSG_ACQ_PERIOD_IDX   18
#define POLL_MSG_ACQ_TOKEN_IDX    19
#define POLL_MSG_TEST_PROFILE_IDX 20
#define POLL_MSG_RANGING_MODE_IDX 21
#define POLL_MSG_FIRE_IDX         22
#define POLL_MSG_GYRO_X_IDX       23
#define POLL_MSG_GYRO_Y_IDX       25
#define POLL_MSG_GYRO_Z_IDX       27
#define POLL_MSG_TX_POWER_LEVEL_IDX 29

#define POLL_MSG_FIRE_COUNTDOWN   1u
#define POLL_MSG_FIRE_IMMEDIATE   2u
#define BOOTLOADER_DFU_START      0xB1u

#define RESP_MSG_CTRL_OPT_IDX         11
#define RESP_MSG_CTRL_TOKEN_IDX       12
#define RESP_MSG_CTRL_FLAGS_IDX       13
#define RESP_MSG_CTRL_ACQ_MS_IDX      14
#define RESP_MSG_CTRL_ACQ_TOKEN_IDX   15
#define RESP_MSG_CTRL_TEST_PROFILE_IDX 16
#define RESP_MSG_SS_POLL_RX_TS_IDX    17
#define RESP_MSG_SS_RESP_TX_TS_IDX    21
#define RESP_MSG_ACCEL_X_IDX          25
#define RESP_MSG_ACCEL_Y_IDX          27
#define RESP_MSG_ACCEL_Z_IDX          29
#define RESP_MSG_GYRO_X_IDX           31
#define RESP_MSG_GYRO_Y_IDX           33
#define RESP_MSG_GYRO_Z_IDX           35
#define RESP_MSG_LOAD_MV_IDX          37
#define RESP_MSG_LOAD_CONNECTED_IDX   39

#define LOAD_SENSE_THRESHOLD_MV       1000u

#define RESP_FLAG_SWITCH_PENDING 0x01u
#define RESP_FLAG_ACQ_PENDING    0x02u

static uint8_t rx_poll_msg[] = {
    0x41, 0x88,
    0,
    0xCA, 0xDE,
    'W', 'A',
    'V', 'E',
    FUNC_CODE_POLL,
    0, 0,
    0, 0,
    0, 0,
    0, 0,
    0, 0,
    0, 0,
    0, 0,
    0, 0,
    0, 0,
    0
};

static uint8_t tx_resp_msg[RESP_MSG_LOAD_CONNECTED_IDX + 1u] = {
    0x41, 0x88,
    0,
    0xCA, 0xDE,
    'V', 'E',
    'W', 'A',
    FUNC_CODE_RESPONSE,
    0x02,
    0, 0, 0, 0, 0, 0,
    0, 0, 0, 0,
    0, 0, 0, 0,
    0, 0,
    0, 0,
    0, 0,
    0, 0,
    0
};

static uint8_t rx_final_msg[] = {
    0x41, 0x88,
    0,
    0xCA, 0xDE,
    'W', 'A',
    'V', 'E',
    FUNC_CODE_FINAL,
    0, 0, 0, 0,
    0, 0, 0, 0,
    0, 0, 0, 0,
    0, 0
};

static uint8_t frame_seq_nb = 0;
static uint8_t rx_buffer[RX_BUF_LEN];
static uint32_t status_reg = 0;

static uint64_t poll_rx_ts;
static uint64_t resp_tx_ts;
static uint64_t final_rx_ts;

static double tof;
static double distance;
static uint32_t ranging_count = 0;

static int16_t accel_rx[3];
static int16_t gyro_rx[3];

static accel_data_t accel_local;
static gyro_data_t gyro_local;
static bool accel_ok = false;

static uint8_t current_profile_opt = UWB_PROFILE_OPT_6M8_STABLE;
static uint8_t pending_profile_opt = UWB_PROFILE_OPT_6M8_STABLE;
static uint8_t pending_switch_token = 0;
static bool switch_pending = false;
static bool switch_after_final = false;

static uint8_t current_acq_period_ms = RNG_DELAY_MS;
static uint8_t pending_acq_period_ms = RNG_DELAY_MS;
static uint8_t pending_acq_token = 0;
static bool period_pending = false;
static bool period_after_final = false;

static uint8_t last_initiator_acq_period_ms = RNG_DELAY_MS;
static uint8_t last_initiator_profile_opt = UWB_PROFILE_OPT_6M8_STABLE;
static uint8_t current_test_profile = UWB_TEST_PROFILE_DEFAULT;
static uint8_t last_initiator_test_profile = UWB_TEST_PROFILE_DEFAULT;
static uint8_t current_tx_power_level = UWB_TX_POWER_LEVEL_DEFAULT;
static uint32_t responder_accel_sample_count = 0;
static int16_t load_saadc_sample = 0;
static uint16_t load_p0_28_mv = 0;
static uint8_t load_connected = 0;

static const uwb_runtime_profile_t *active_profile = NULL;

#if defined(UWB_ACCEL_TARGET_GENA)
#define PYRO_TRIGGER_PIN NRF_GPIO_PIN_MAP(1, 9)
#define PYRO_LED_PIN_0 NRF_GPIO_PIN_MAP(0, 14)
#define PYRO_LED_PIN_1 NRF_GPIO_PIN_MAP(0, 22)
#define PYRO_LED_PIN_2 NRF_GPIO_PIN_MAP(0, 5)
#define PYRO_LED_PIN_3 NRF_GPIO_PIN_MAP(0, 4)
#define PYRO_COUNTDOWN_TICKS (32768u * 10u)
#define PYRO_FIRE_TICKS      (32768u * 2u)
#define RTC2_COUNTER_MASK    0x00FFFFFFu

typedef enum {
    PYRO_STATE_IDLE = 0,
    PYRO_STATE_COUNTDOWN,
    PYRO_STATE_FIRE
} pyro_state_t;

static pyro_state_t pyro_state = PYRO_STATE_IDLE;
static uint32_t pyro_state_start_tick = 0u;
static uint32_t pyro_next_fx_tick = 0u;
static bool pyro_leds_on = false;
static bool pyro_fire_requested = false;
#endif

static char output_buf[320];
static char cmd_buf[96];

extern dwt_config_t config_options;
extern dwt_txconfig_t txconfig_options;
extern dwt_txconfig_t txconfig_options_ch9;

extern void test_run_info(unsigned char *data);

static void handle_app_command(const char *cmd);

static void load_sense_init(void)
{
    NRF_SAADC->ENABLE = SAADC_ENABLE_ENABLE_Enabled;
    NRF_SAADC->RESOLUTION = SAADC_RESOLUTION_VAL_10bit;
    NRF_SAADC->OVERSAMPLE = SAADC_OVERSAMPLE_OVERSAMPLE_Bypass;
    NRF_SAADC->CH[0].PSELP = SAADC_CH_PSELP_PSELP_AnalogInput4;
    NRF_SAADC->CH[0].PSELN = SAADC_CH_PSELN_PSELN_NC;
    NRF_SAADC->CH[0].CONFIG = (SAADC_CH_CONFIG_RESP_Bypass << SAADC_CH_CONFIG_RESP_Pos)
                            | (SAADC_CH_CONFIG_RESN_Bypass << SAADC_CH_CONFIG_RESN_Pos)
                            | (SAADC_CH_CONFIG_GAIN_Gain1_6 << SAADC_CH_CONFIG_GAIN_Pos)
                            | (SAADC_CH_CONFIG_REFSEL_Internal << SAADC_CH_CONFIG_REFSEL_Pos)
                            | (SAADC_CH_CONFIG_TACQ_10us << SAADC_CH_CONFIG_TACQ_Pos)
                            | (SAADC_CH_CONFIG_MODE_SE << SAADC_CH_CONFIG_MODE_Pos)
                            | (SAADC_CH_CONFIG_BURST_Disabled << SAADC_CH_CONFIG_BURST_Pos);
}

static bool load_sense_sample(uint16_t *mv)
{
    uint32_t guard;
    int32_t raw;

    NRF_SAADC->RESULT.PTR = (uint32_t)&load_saadc_sample;
    NRF_SAADC->RESULT.MAXCNT = 1u;
    NRF_SAADC->EVENTS_STARTED = 0u;
    NRF_SAADC->EVENTS_END = 0u;
    NRF_SAADC->TASKS_START = 1u;

    guard = 10000u;
    while ((NRF_SAADC->EVENTS_STARTED == 0u) && (guard-- > 0u))
    {
    }
    if (NRF_SAADC->EVENTS_STARTED == 0u)
    {
        return false;
    }

    NRF_SAADC->TASKS_SAMPLE = 1u;
    guard = 10000u;
    while ((NRF_SAADC->EVENTS_END == 0u) && (guard-- > 0u))
    {
    }
    NRF_SAADC->TASKS_STOP = 1u;
    if (NRF_SAADC->EVENTS_END == 0u)
    {
        return false;
    }

    raw = load_saadc_sample;
    if (raw < 0)
    {
        raw = 0;
    }
    if (raw > 1023)
    {
        raw = 1023;
    }

    *mv = (uint16_t)(((uint32_t)raw * 3600u + 511u) / 1023u);
    return true;
}

static void load_sense_update(void)
{
    uint16_t mv;

    if (load_sense_sample(&mv))
    {
        load_p0_28_mv = mv;
        load_connected = (mv >= LOAD_SENSE_THRESHOLD_MV) ? 1u : 0u;
    }
}

#if defined(UWB_ACCEL_TARGET_GENA)
static void pyro_leds_set(bool on)
{
    pyro_leds_on = on;
    if (on)
    {
        nrf_gpio_pin_clear(PYRO_LED_PIN_0);
        nrf_gpio_pin_clear(PYRO_LED_PIN_1);
        nrf_gpio_pin_clear(PYRO_LED_PIN_2);
        nrf_gpio_pin_clear(PYRO_LED_PIN_3);
    }
    else
    {
        nrf_gpio_pin_set(PYRO_LED_PIN_0);
        nrf_gpio_pin_set(PYRO_LED_PIN_1);
        nrf_gpio_pin_set(PYRO_LED_PIN_2);
        nrf_gpio_pin_set(PYRO_LED_PIN_3);
    }
}

static void pyro_leds_init(void)
{
    nrf_gpio_cfg_output(PYRO_LED_PIN_0);
    nrf_gpio_cfg_output(PYRO_LED_PIN_1);
    nrf_gpio_cfg_output(PYRO_LED_PIN_2);
    nrf_gpio_cfg_output(PYRO_LED_PIN_3);
    pyro_leds_set(false);
}

static void pyro_buzzer_tone(uint16_t freq_hz, uint16_t duration_ms)
{
    uint32_t half_period_us;
    uint32_t toggles;

    if (duration_ms == 0u)
    {
        return;
    }

    if (freq_hz == 0u)
    {
        nrf_gpio_pin_clear(PYRO_BUZZER_PIN);
        nrf_delay_ms(duration_ms);
        return;
    }

    half_period_us = 500000u / (uint32_t)freq_hz;
    if (half_period_us == 0u)
    {
        half_period_us = 1u;
    }

    toggles = ((uint32_t)duration_ms * 1000u) / half_period_us;
    while (toggles-- > 0u)
    {
        nrf_gpio_pin_toggle(PYRO_BUZZER_PIN);
        nrf_delay_us((uint16_t)half_period_us);
    }
    nrf_gpio_pin_clear(PYRO_BUZZER_PIN);
}

/* Short video-game style jingle played once when a trigger starts the
 * countdown: rising C-major arpeggio C6-E6-G6-C7 (~300 ms total). */
static void pyro_buzzer_trigger_jingle(void)
{
    static const uint16_t notes[] = { 1047u, 1319u, 1568u, 2093u };

    for (uint32_t i = 0u; i < (sizeof(notes) / sizeof(notes[0])); i++)
    {
        pyro_buzzer_tone(notes[i], 60u);
        nrf_delay_ms(15);
    }
}

static void pyro_trigger_init(void)
{
    nrf_gpio_cfg_output(PYRO_BUZZER_PIN);
    nrf_gpio_pin_clear(PYRO_BUZZER_PIN);
    nrf_gpio_cfg_output(PYRO_TRIGGER_PIN);
    nrf_gpio_pin_clear(PYRO_TRIGGER_PIN);
    pyro_leds_init();

    pyro_state = PYRO_STATE_IDLE;
    pyro_state_start_tick = NRF_RTC2->COUNTER;
    pyro_next_fx_tick = pyro_state_start_tick;
    pyro_fire_requested = false;
    uart_log_write("PYRO,GPIO_MODE");
}

static void pyro_request_fire(const char *ack)
{
    if (pyro_state != PYRO_STATE_IDLE || pyro_fire_requested)
    {
        uart_log_write("ERR,PYRO_BUSY");
        return;
    }

    pyro_fire_requested = true;
    uart_log_write(ack);
}

static void pyro_fire_now(const char *ack)
{
    if (pyro_state != PYRO_STATE_IDLE || pyro_fire_requested)
    {
        uart_log_write("ERR,PYRO_BUSY");
        return;
    }

    pyro_fire_requested = false;
    pyro_state = PYRO_STATE_FIRE;
    pyro_state_start_tick = NRF_RTC2->COUNTER;
    pyro_leds_set(true);
    nrf_gpio_pin_set(PYRO_TRIGGER_PIN);
    pyro_buzzer_tone(1568u, 80u);
    uart_log_write(ack);
    uart_log_write("PYRO,FIRE_NOW");
}

static void pyro_trigger_process(void)
{
    uint32_t now = NRF_RTC2->COUNTER;

    if ((pyro_state == PYRO_STATE_IDLE) && pyro_fire_requested)
    {
        pyro_fire_requested = false;
        pyro_state = PYRO_STATE_COUNTDOWN;
        pyro_state_start_tick = now;
        pyro_next_fx_tick = now;
        pyro_leds_set(false);
        uart_log_write("PYRO,COUNTDOWN_START");
        pyro_buzzer_trigger_jingle();
    }

    if (pyro_state == PYRO_STATE_COUNTDOWN)
    {
        uint32_t elapsed = (now - pyro_state_start_tick) & RTC2_COUNTER_MASK;

        if (elapsed >= PYRO_COUNTDOWN_TICKS)
        {
            nrf_gpio_pin_set(PYRO_TRIGGER_PIN);
            pyro_state = PYRO_STATE_FIRE;
            pyro_state_start_tick = now;
            pyro_leds_set(true);
            pyro_buzzer_tone(1568u, 80u);
            uart_log_write("PYRO,FIRE");
            return;
        }

        if (((now - pyro_next_fx_tick) & RTC2_COUNTER_MASK) < 0x00800000u)
        {
            uint32_t remaining = PYRO_COUNTDOWN_TICKS - elapsed;
            uint32_t interval_ticks;
            uint16_t freq_hz = (uint16_t)(520u + ((elapsed * 1200u) / PYRO_COUNTDOWN_TICKS));

            if (remaining > (32768u * 7u))
            {
                interval_ticks = 16384u;
            }
            else if (remaining > (32768u * 4u))
            {
                interval_ticks = 8192u;
            }
            else if (remaining > (32768u * 2u))
            {
                interval_ticks = 4096u;
            }
            else
            {
                interval_ticks = 2730u;
            }

            pyro_leds_set(!pyro_leds_on);
            pyro_buzzer_tone(freq_hz, pyro_leds_on ? 40u : 25u);
            pyro_next_fx_tick = now + interval_ticks;
        }
        return;
    }

    if (pyro_state == PYRO_STATE_FIRE)
    {
        if (((now - pyro_state_start_tick) & RTC2_COUNTER_MASK) >= PYRO_FIRE_TICKS)
        {
            nrf_gpio_pin_clear(PYRO_TRIGGER_PIN);
            pyro_leds_set(false);
            pyro_state = PYRO_STATE_IDLE;
            uart_log_write("PYRO,DONE");
        }
    }
}
#endif

static bool is_supported_profile_opt(uint8_t opt)
{
    return uwb_profile_find(opt) != NULL;
}

static bool is_supported_acq_period(uint8_t period_ms)
{
    return (period_ms >= 1u) && (period_ms <= 200u);
}

static bool is_supported_test_profile(uint8_t profile)
{
    return profile == UWB_TEST_PROFILE_TURBO_DISTANCE_ONLY;
}

static uint8_t test_profile_accel_decimation(uint8_t profile)
{
    (void)profile;
    return 1u;
}

static int apply_profile_option(uint8_t opt)
{
    const uwb_runtime_profile_t *profile = uwb_profile_find(opt);

    if (profile == NULL)
    {
        return DWT_ERROR;
    }

    config_options = profile->config;

    if (dwt_configure(&config_options))
    {
        return DWT_ERROR;
    }
    dwt_configciadiag((uint8_t)DW_CIA_DIAG_LOG_OFF);

    {
        dwt_txconfig_t tx_config = (config_options.chan == 9) ? txconfig_options_ch9 : txconfig_options;
        tx_config.power = uwb_tx_power_value_for_level((uint8_t)config_options.chan, current_tx_power_level);
        dwt_configuretxrf(&tx_config);
    }

    dwt_setrxantennadelay(RX_ANT_DLY);
    dwt_settxantennadelay(TX_ANT_DLY);
    dwt_setlnapamode(DWT_LNA_ENABLE | DWT_PA_ENABLE);

    active_profile = profile;

    return DWT_SUCCESS;
}

static void process_app_commands(void)
{
    uart_log_poll_rx();
    while (uart_log_read_command(cmd_buf, sizeof(cmd_buf)))
    {
        handle_app_command(cmd_buf);
    }
}

static void handle_app_command(const char *cmd)
{
    if (strcmp(cmd, "BOOT,DFU") == 0)
    {
        uart_log_write("ACK,BOOT_DFU");
        uart_log_flush();
        NRF_POWER->GPREGRET = (uint32_t)BOOTLOADER_DFU_START;
        NVIC_SystemReset();
        return;
    }

    if ((strcmp(cmd, "CFG,GET_ROLE") == 0) || (strcmp(cmd, "INFO?") == 0))
    {
        uart_log_write("ROLE,RESPONDER");
        return;
    }

    if ((strcmp(cmd, "PYRO,FIRE") == 0) || (strcmp(cmd, "FIRE") == 0))
    {
#if defined(UWB_ACCEL_TARGET_GENA)
        pyro_request_fire("ACK,PYRO_ARMED");
#else
        uart_log_write("ERR,PYRO_UNAVAILABLE");
#endif
        return;
    }

    if ((strcmp(cmd, "PYRO,FIRE_NOW") == 0) || (strcmp(cmd, "FIRE_NOW") == 0))
    {
#if defined(UWB_ACCEL_TARGET_GENA)
        pyro_fire_now("ACK,PYRO_FIRE_NOW");
#else
        uart_log_write("ERR,PYRO_UNAVAILABLE");
#endif
        return;
    }

    uart_log_write("ERR,READ_ONLY_SS_TWR");
}

int ss_twr_responder_custom(void)
{
    test_run_info((unsigned char *)"UWB RANGING RESP v1.0");
    uart_log_init();
    uart_log_write("UWB RANGING RESP v1.0");
    uart_log_write("ROLE,RESPONDER");

    port_set_dw_ic_spi_fastrate();

    reset_DWIC();
    Sleep(2);

    if (dwt_probe((struct dwt_probe_s *)&dw3000_probe_interf) == DWT_ERROR)
    {
        test_run_info((unsigned char *)"PROBE FAILED");
        while (1) { };
    }

    while (!dwt_checkidlerc()) { };

    if (dwt_initialise(DWT_READ_OTP_ALL) == DWT_ERROR)
    {
        test_run_info((unsigned char *)"INIT FAILED");
        while (1) { };
    }

    active_profile = uwb_profile_find(current_profile_opt);
    if (active_profile != NULL)
    {
        config_options = active_profile->config;
    }

    if (dwt_configure(&config_options))
    {
        test_run_info((unsigned char *)"CONFIG FAILED");
        while (1) { };
    }
    dwt_configciadiag((uint8_t)DW_CIA_DIAG_LOG_OFF);

    {
        dwt_txconfig_t tx_config = (config_options.chan == 9) ? txconfig_options_ch9 : txconfig_options;
        tx_config.power = uwb_tx_power_value_for_level((uint8_t)config_options.chan, current_tx_power_level);
        dwt_configuretxrf(&tx_config);
    }

    dwt_setrxantennadelay(RX_ANT_DLY);
    dwt_settxantennadelay(TX_ANT_DLY);

    dwt_setlnapamode(DWT_LNA_ENABLE | DWT_PA_ENABLE);
    dwt_setleds(DWT_LEDS_ENABLE | DWT_LEDS_INIT_BLINK);

    accel_ok = accel_init();
    if (accel_ok) {
        test_run_info((unsigned char *)accel_backend_name());
        if (accel_backend_is_bmi323())
        {
            pyro_buzzer_beep_three_times();
        }
    } else {
        test_run_info((unsigned char *)"ACCEL FAIL");
    }

    load_sense_init();
    load_sense_update();

    NRF_RTC2->PRESCALER = 0;
    NRF_RTC2->TASKS_START = 1;
#if defined(UWB_ACCEL_TARGET_GENA)
    pyro_trigger_init();
#endif

    test_run_info((unsigned char *)"# ms,sample,dist,iax,iay,iaz,rax,ray,raz,resp_acq_ms,init_acq_ms,resp_profile_opt,init_profile_opt,igx,igy,igz,rgx,rgy,rgz,rload_mv,rload_connected");
    uart_log_write("# ms,sample,dist,iax,iay,iaz,rax,ray,raz,resp_acq_ms,init_acq_ms,resp_profile_opt,init_profile_opt,igx,igy,igz,rgx,rgy,rgz,rload_mv,rload_connected");

    while (1)
    {
        process_app_commands();
#if defined(UWB_ACCEL_TARGET_GENA)
        pyro_trigger_process();
#endif

        dwt_setpreambledetecttimeout(0);
        dwt_setrxtimeout(0);
        dwt_rxenable(DWT_START_RX_IMMEDIATE);

        do
        {
            process_app_commands();
#if defined(UWB_ACCEL_TARGET_GENA)
            pyro_trigger_process();
#endif
            status_reg = dwt_readsysstatuslo();
        } while ((status_reg & (DWT_INT_RXFCG_BIT_MASK | SYS_STATUS_ALL_RX_TO | SYS_STATUS_ALL_RX_ERR)) == 0u);

        if (status_reg & DWT_INT_RXFCG_BIT_MASK)
        {
            uint16_t frame_len;

            dwt_writesysstatuslo(DWT_INT_RXFCG_BIT_MASK);

            frame_len = dwt_getframelength(0);
            if (frame_len <= RX_BUF_LEN)
            {
                dwt_readrxdata(rx_buffer, frame_len, 0);
            }

            rx_buffer[ALL_MSG_SN_IDX] = 0;
            if (memcmp(rx_buffer, rx_poll_msg, ALL_MSG_COMMON_LEN) == 0)
            {
                uint32_t resp_tx_time;
                uint16_t response_delay_uus;
                int ret;

                accel_rx[0] = (int16_t)(rx_buffer[POLL_MSG_ACCEL_X_IDX] |
                              (rx_buffer[POLL_MSG_ACCEL_X_IDX + 1] << 8));
                accel_rx[1] = (int16_t)(rx_buffer[POLL_MSG_ACCEL_Y_IDX] |
                              (rx_buffer[POLL_MSG_ACCEL_Y_IDX + 1] << 8));
                accel_rx[2] = (int16_t)(rx_buffer[POLL_MSG_ACCEL_Z_IDX] |
                              (rx_buffer[POLL_MSG_ACCEL_Z_IDX + 1] << 8));

                gyro_rx[0] = 0;
                gyro_rx[1] = 0;
                gyro_rx[2] = 0;
                if (frame_len > POLL_MSG_GYRO_Z_IDX + 1)
                {
                    gyro_rx[0] = (int16_t)(rx_buffer[POLL_MSG_GYRO_X_IDX] |
                                 (rx_buffer[POLL_MSG_GYRO_X_IDX + 1] << 8));
                    gyro_rx[1] = (int16_t)(rx_buffer[POLL_MSG_GYRO_Y_IDX] |
                                 (rx_buffer[POLL_MSG_GYRO_Y_IDX + 1] << 8));
                    gyro_rx[2] = (int16_t)(rx_buffer[POLL_MSG_GYRO_Z_IDX] |
                                 (rx_buffer[POLL_MSG_GYRO_Z_IDX + 1] << 8));
                }

                if (frame_len > POLL_MSG_ACQ_PERIOD_IDX)
                {
                    last_initiator_acq_period_ms = rx_buffer[POLL_MSG_ACQ_PERIOD_IDX];
                }
                if (frame_len > POLL_MSG_PROFILE_IDX)
                {
                    last_initiator_profile_opt = rx_buffer[POLL_MSG_PROFILE_IDX];
                }
                if (frame_len > POLL_MSG_TEST_PROFILE_IDX)
                {
                    last_initiator_test_profile = rx_buffer[POLL_MSG_TEST_PROFILE_IDX];
                }
                if (frame_len > POLL_MSG_TX_POWER_LEVEL_IDX)
                {
                    uint8_t requested_tx_power_level = rx_buffer[POLL_MSG_TX_POWER_LEVEL_IDX];
                    if (uwb_tx_power_level_is_supported(requested_tx_power_level) && (requested_tx_power_level != current_tx_power_level))
                    {
                        dwt_txconfig_t tx_config;
                        current_tx_power_level = requested_tx_power_level;
                        tx_config = (config_options.chan == 9) ? txconfig_options_ch9 : txconfig_options;
                        tx_config.power = uwb_tx_power_value_for_level((uint8_t)config_options.chan, current_tx_power_level);
                        dwt_configuretxrf(&tx_config);
                    }
                }
#if defined(UWB_ACCEL_TARGET_GENA)
                if ((frame_len > POLL_MSG_FIRE_IDX) && (rx_buffer[POLL_MSG_FIRE_IDX] != 0u))
                {
                    if ((pyro_state == PYRO_STATE_IDLE) && !pyro_fire_requested)
                    {
                        if (rx_buffer[POLL_MSG_FIRE_IDX] == POLL_MSG_FIRE_IMMEDIATE)
                        {
                            pyro_fire_now("ACK,PYRO_REMOTE_FIRE_NOW");
                        }
                        else if (rx_buffer[POLL_MSG_FIRE_IDX] == POLL_MSG_FIRE_COUNTDOWN)
                        {
                            pyro_request_fire("ACK,PYRO_REMOTE_ARMED");
                        }
                    }
                }
#endif
                if (is_supported_acq_period(last_initiator_acq_period_ms))
                {
                    current_acq_period_ms = last_initiator_acq_period_ms;
                }
                if (is_supported_test_profile(last_initiator_test_profile))
                {
                    current_test_profile = last_initiator_test_profile;
                }
                period_pending = false;
                period_after_final = false;

                if (frame_len > POLL_MSG_SWITCH_TOKEN_IDX)
                {
                    uint8_t initiator_opt = rx_buffer[POLL_MSG_PROFILE_IDX];
                    uint8_t req_token = rx_buffer[POLL_MSG_SWITCH_TOKEN_IDX];

                    if (switch_pending && (req_token == pending_switch_token) && ((initiator_opt == current_profile_opt) || (initiator_opt == pending_profile_opt)))
                    {
                        switch_after_final = true;
                    }
                    else if ((req_token != 0u) && is_supported_profile_opt(initiator_opt) && (initiator_opt != current_profile_opt))
                    {
                        pending_profile_opt = initiator_opt;
                        pending_switch_token = req_token;
                        switch_pending = true;
                        switch_after_final = true;
                    }
                }

                if (period_pending && (frame_len > POLL_MSG_ACQ_TOKEN_IDX))
                {
                    uint8_t req_token = rx_buffer[POLL_MSG_ACQ_TOKEN_IDX];

                    /* The initiator advertises its current period in POLL and sends the pending token
                     * when it is ready to switch. Match on token to complete the coordinated change. */
                    if (req_token == pending_acq_token)
                    {
                        period_after_final = true;
                    }
                }

                poll_rx_ts = ranging_get_rx_timestamp_u64();

                response_delay_uus = active_profile->responder_ss_poll_rx_to_resp_tx_dly_uus;
                resp_tx_time = (poll_rx_ts + (response_delay_uus * UUS_TO_DWT_TIME)) >> 8;
                dwt_setdelayedtrxtime(resp_tx_time);
                resp_tx_ts = (((uint64_t)(resp_tx_time & 0xFFFFFFFEUL)) << 8) + TX_ANT_DLY;

                dwt_setrxaftertxdelay(active_profile->responder_resp_tx_to_final_rx_dly_uus);
                dwt_setrxtimeout(active_profile->responder_final_rx_timeout_uus);
                dwt_setpreambledetecttimeout(active_profile->pre_timeout_symbols);

                tx_resp_msg[ALL_MSG_SN_IDX] = frame_seq_nb;
                tx_resp_msg[RESP_MSG_CTRL_OPT_IDX] = switch_pending ? pending_profile_opt : 0u;
                tx_resp_msg[RESP_MSG_CTRL_TOKEN_IDX] = switch_pending ? pending_switch_token : 0u;
                tx_resp_msg[RESP_MSG_CTRL_FLAGS_IDX] = (switch_pending ? RESP_FLAG_SWITCH_PENDING : 0u)
                                                      | (period_pending ? RESP_FLAG_ACQ_PENDING : 0u);
                tx_resp_msg[RESP_MSG_CTRL_ACQ_MS_IDX] = period_pending ? pending_acq_period_ms : current_acq_period_ms;
                tx_resp_msg[RESP_MSG_CTRL_ACQ_TOKEN_IDX] = period_pending ? pending_acq_token : 0u;
                tx_resp_msg[RESP_MSG_CTRL_TEST_PROFILE_IDX] = current_test_profile;
                ranging_msg_set_ts(&tx_resp_msg[RESP_MSG_SS_POLL_RX_TS_IDX], poll_rx_ts);
                ranging_msg_set_ts(&tx_resp_msg[RESP_MSG_SS_RESP_TX_TS_IDX], resp_tx_ts);
                tx_resp_msg[RESP_MSG_ACCEL_X_IDX] = (uint8_t)(accel_local.x & 0xFF);
                tx_resp_msg[RESP_MSG_ACCEL_X_IDX + 1] = (uint8_t)((accel_local.x >> 8) & 0xFF);
                tx_resp_msg[RESP_MSG_ACCEL_Y_IDX] = (uint8_t)(accel_local.y & 0xFF);
                tx_resp_msg[RESP_MSG_ACCEL_Y_IDX + 1] = (uint8_t)((accel_local.y >> 8) & 0xFF);
                tx_resp_msg[RESP_MSG_ACCEL_Z_IDX] = (uint8_t)(accel_local.z & 0xFF);
                tx_resp_msg[RESP_MSG_ACCEL_Z_IDX + 1] = (uint8_t)((accel_local.z >> 8) & 0xFF);
                tx_resp_msg[RESP_MSG_GYRO_X_IDX] = (uint8_t)(gyro_local.x & 0xFF);
                tx_resp_msg[RESP_MSG_GYRO_X_IDX + 1] = (uint8_t)((gyro_local.x >> 8) & 0xFF);
                tx_resp_msg[RESP_MSG_GYRO_Y_IDX] = (uint8_t)(gyro_local.y & 0xFF);
                tx_resp_msg[RESP_MSG_GYRO_Y_IDX + 1] = (uint8_t)((gyro_local.y >> 8) & 0xFF);
                tx_resp_msg[RESP_MSG_GYRO_Z_IDX] = (uint8_t)(gyro_local.z & 0xFF);
                tx_resp_msg[RESP_MSG_GYRO_Z_IDX + 1] = (uint8_t)((gyro_local.z >> 8) & 0xFF);
                tx_resp_msg[RESP_MSG_LOAD_MV_IDX] = (uint8_t)(load_p0_28_mv & 0xFF);
                tx_resp_msg[RESP_MSG_LOAD_MV_IDX + 1] = (uint8_t)((load_p0_28_mv >> 8) & 0xFF);
                tx_resp_msg[RESP_MSG_LOAD_CONNECTED_IDX] = load_connected;
                dwt_writetxdata(sizeof(tx_resp_msg), tx_resp_msg, 0);
                dwt_writetxfctrl(sizeof(tx_resp_msg) + FCS_LEN, 0, 1);

                ret = dwt_starttx(DWT_START_TX_DELAYED);

                if (ret == DWT_ERROR)
                {
                    continue;
                }

                waitforsysstatus(NULL, NULL, DWT_INT_TXFRS_BIT_MASK, 0);
                dwt_writesysstatuslo(DWT_INT_TXFRS_BIT_MASK);
                frame_seq_nb++;
                load_sense_update();

                if (accel_ok && !accel_read(&accel_local))
                {
                    accel_ok = false;
                    gyro_local.x = 0;
                    gyro_local.y = 0;
                    gyro_local.z = 0;
                }
                else if (accel_ok && !accel_read_gyro(&gyro_local))
                {
                    gyro_local.x = 0;
                    gyro_local.y = 0;
                    gyro_local.z = 0;
                }

                if (switch_after_final)
                {
                    if (apply_profile_option(pending_profile_opt) == DWT_SUCCESS)
                    {
                        current_profile_opt = pending_profile_opt;
                        switch_pending = false;
                        switch_after_final = false;
                        uart_log_write("ACK,UWB_CHANNEL_SWITCHED");
                    }
                }
                continue;

                waitforsysstatus(&status_reg, NULL,
                    (DWT_INT_RXFCG_BIT_MASK | SYS_STATUS_ALL_RX_TO | SYS_STATUS_ALL_RX_ERR), 0);

                frame_seq_nb++;

                if (status_reg & DWT_INT_RXFCG_BIT_MASK)
                {
                    dwt_writesysstatuslo(DWT_INT_RXFCG_BIT_MASK | DWT_INT_TXFRS_BIT_MASK);

                    frame_len = dwt_getframelength(0);
                    if (frame_len <= RX_BUF_LEN)
                    {
                        dwt_readrxdata(rx_buffer, frame_len, 0);
                    }

                    rx_buffer[ALL_MSG_SN_IDX] = 0;
                    if (memcmp(rx_buffer, rx_final_msg, ALL_MSG_COMMON_LEN) == 0)
                    {
                        uint32_t poll_tx_ts, resp_rx_ts, final_tx_ts;
                        uint32_t poll_rx_ts_32, resp_tx_ts_32, final_rx_ts_32;
                        double Ra, Rb, Da, Db;
                        int64_t tof_dtu;

                        resp_tx_ts = ranging_get_tx_timestamp_u64();
                        final_rx_ts = ranging_get_rx_timestamp_u64();

                        ranging_msg_get_ts(&rx_buffer[FINAL_MSG_POLL_TX_TS_IDX], &poll_tx_ts);
                        ranging_msg_get_ts(&rx_buffer[FINAL_MSG_RESP_RX_TS_IDX], &resp_rx_ts);
                        ranging_msg_get_ts(&rx_buffer[FINAL_MSG_FINAL_TX_TS_IDX], &final_tx_ts);

                        poll_rx_ts_32  = (uint32_t)poll_rx_ts;
                        resp_tx_ts_32  = (uint32_t)resp_tx_ts;
                        final_rx_ts_32 = (uint32_t)final_rx_ts;

                        Ra = (double)(resp_rx_ts - poll_tx_ts);
                        Rb = (double)(final_rx_ts_32 - resp_tx_ts_32);
                        Da = (double)(final_tx_ts - resp_rx_ts);
                        Db = (double)(resp_tx_ts_32 - poll_rx_ts_32);

                        tof_dtu = (int64_t)((Ra * Rb - Da * Db) / (Ra + Rb + Da + Db));

                        tof = tof_dtu * DWT_TIME_UNITS;
                        distance = tof * SPEED_OF_LIGHT;
                        ranging_count++;

                        if (accel_ok) {
                            uint8_t accel_decimation = test_profile_accel_decimation(current_test_profile);
                            if (accel_decimation > 0u)
                            {
                                responder_accel_sample_count++;
                                if ((accel_decimation == 1u) || ((responder_accel_sample_count % accel_decimation) == 0u))
                                {
                                    if (!accel_read(&accel_local))
                                    {
                                        accel_ok = false;
                                        gyro_local.x = 0;
                                        gyro_local.y = 0;
                                        gyro_local.z = 0;
                                    }
                                    else if (!accel_read_gyro(&gyro_local))
                                    {
                                        gyro_local.x = 0;
                                        gyro_local.y = 0;
                                        gyro_local.z = 0;
                                    }
                                }
                            }
                        }

                        {
                            uint32_t ms = (uint32_t)(((uint64_t)NRF_RTC2->COUNTER * 1000u) / 32768u);
                            radio_quality_t radio_quality;
                            radio_quality_read(&radio_quality);

                            snprintf(output_buf, sizeof(output_buf),
                            "%lu,%lu,%.2f,%.1f,%.1f,%.2f,%u,%u,%.2f,%u,%d,%d,%d,%d,%d,%d,%d,%u,%u,%u,%u,%d,%d,%d,%d,%d,%d,%u,%u",
                            (unsigned long)ms,
                            (unsigned long)ranging_count,
                            distance,
                            (double)radio_quality.rx_power_dbm,
                            (double)radio_quality.fp_power_dbm,
                            (double)radio_quality.clock_offset_ppm,
                            (unsigned int)radio_quality.score_10,
                            (unsigned int)radio_quality.nlos_score_10,
                            (double)radio_quality.peak_to_fp_samples,
                            (unsigned int)radio_quality.fp_conf_level,
                            (int)radio_quality.sts_quality,
                            (int)accel_rx[0],
                            (int)accel_rx[1],
                            (int)accel_rx[2],
                            (int)accel_local.x,
                            (int)accel_local.y,
                            (int)accel_local.z,
                            (unsigned int)current_acq_period_ms,
                            (unsigned int)last_initiator_acq_period_ms,
                            (unsigned int)current_profile_opt,
                            (unsigned int)last_initiator_profile_opt,
                            (int)gyro_rx[0],
                            (int)gyro_rx[1],
                            (int)gyro_rx[2],
                            (int)gyro_local.x,
                            (int)gyro_local.y,
                            (int)gyro_local.z,
                            (unsigned int)load_p0_28_mv,
                            (unsigned int)load_connected);
                        }
                        uart_log_write(output_buf);

                        if (switch_after_final)
                        {
                            if (apply_profile_option(pending_profile_opt) == DWT_SUCCESS)
                            {
                                current_profile_opt = pending_profile_opt;
                                switch_pending = false;
                                switch_after_final = false;
                                uart_log_write("ACK,UWB_DATARATE_SWITCHED");
                            }
                        }

                        if (period_after_final)
                        {
                            current_acq_period_ms = pending_acq_period_ms;
                            period_pending = false;
                            period_after_final = false;
                            uart_log_write("ACK,ACQ_PERIOD_SWITCHED");
                        }
                    }
                }
                else
                {
                    switch_after_final = false;
                    period_after_final = false;
                    dwt_writesysstatuslo(SYS_STATUS_ALL_RX_TO | SYS_STATUS_ALL_RX_ERR);
                }
            }
        }
        else
        {
            switch_after_final = false;
            period_after_final = false;
            dwt_writesysstatuslo(SYS_STATUS_ALL_RX_TO | SYS_STATUS_ALL_RX_ERR);
        }
    }
}
