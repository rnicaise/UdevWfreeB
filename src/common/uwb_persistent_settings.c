#include "uwb_persistent_settings.h"

#include <string.h>

#include "nrf.h"
#include "nrf_error.h"
#include "nrf_sdh.h"
#include "nrf_sdh_soc.h"
#include "nrf_soc.h"

#ifndef UWB_SETTINGS_FLASH_ADDR
#error "UWB_SETTINGS_FLASH_ADDR must be defined for persistent settings builds"
#endif

#define UWB_SETTINGS_MAGIC     0x55574253u
#define UWB_SETTINGS_VERSION   2u
#define UWB_SETTINGS_PAGE_SIZE 0x1000u
#define UWB_SETTINGS_WORDS     10u
#define UWB_SETTINGS_SOC_PRIO  1u

typedef struct
{
    uint32_t magic;
    uint32_t version;
    uint32_t arm_mode;
    uint32_t seq;
    char     name[UWB_SETTINGS_NAME_MAX + 1u]; /* 16 bytes = 4 words */
    uint32_t crc;
    uint32_t reserved;
} uwb_settings_record_t;

/* Legacy v1 record layout (no name field) for migration. */
typedef struct
{
    uint32_t magic;
    uint32_t version;
    uint32_t arm_mode;
    uint32_t seq;
    uint32_t crc;
    uint32_t reserved;
} uwb_settings_record_v1_t;

typedef enum
{
    SETTINGS_WRITE_IDLE = 0,
    SETTINGS_WRITE_ERASING,
    SETTINGS_WRITE_WRITING
} settings_write_state_t;

static uwb_persistent_settings_t m_settings;
static uwb_settings_record_t m_pending_record;
static settings_write_state_t m_write_state = SETTINGS_WRITE_IDLE;
static bool m_write_success = false;
static bool m_write_error = false;

static uint32_t settings_crc(const uwb_settings_record_t *record)
{
    uint32_t crc = record->magic ^ record->version ^ record->arm_mode ^ record->seq ^ 0xA5A55A5Au;
    const uint32_t *name_words = (const uint32_t *)record->name;
    for (uint32_t i = 0u; i < (sizeof(record->name) / 4u); i++)
    {
        crc ^= name_words[i];
    }
    return crc;
}

static uint32_t settings_crc_v1(const uwb_settings_record_v1_t *record)
{
    return record->magic ^ record->version ^ record->arm_mode ^ record->seq ^ 0xA5A55A5Au;
}

static bool arm_mode_is_valid(uint32_t arm_mode)
{
    return (arm_mode == UWB_SETTINGS_ARM_NONE) ||
           (arm_mode == UWB_SETTINGS_ARM_DISTANCE_2M) ||
           (arm_mode == UWB_SETTINGS_ARM_TILT_50);
}

bool uwb_persistent_settings_name_is_valid(const char *name)
{
    size_t len;

    if (name == NULL)
    {
        return false;
    }

    len = strlen(name);
    if ((len == 0u) || (len > UWB_SETTINGS_NAME_MAX))
    {
        return false;
    }

    for (size_t i = 0u; i < len; i++)
    {
        char c = name[i];
        bool ok = ((c >= 'A') && (c <= 'Z')) ||
                  ((c >= 'a') && (c <= 'z')) ||
                  ((c >= '0') && (c <= '9')) ||
                  (c == '-') || (c == '_');
        if (!ok)
        {
            return false;
        }
    }
    return true;
}

static bool record_is_valid(const uwb_settings_record_t *record)
{
    return (record->magic == UWB_SETTINGS_MAGIC) &&
           (record->version == UWB_SETTINGS_VERSION) &&
           arm_mode_is_valid(record->arm_mode) &&
           (record->name[UWB_SETTINGS_NAME_MAX] == '\0') &&
           uwb_persistent_settings_name_is_valid(record->name) &&
           (record->crc == settings_crc(record));
}

static bool record_v1_is_valid(const uwb_settings_record_v1_t *record)
{
    return (record->magic == UWB_SETTINGS_MAGIC) &&
           (record->version == 1u) &&
           arm_mode_is_valid(record->arm_mode) &&
           (record->crc == settings_crc_v1(record));
}

static const uwb_settings_record_t *flash_record(void)
{
    return (const uwb_settings_record_t *)(uintptr_t)UWB_SETTINGS_FLASH_ADDR;
}

static void settings_nvmc_wait_ready(void)
{
    while (NRF_NVMC->READY == NVMC_READY_READY_Busy)
    {
    }
}

static void settings_nvmc_write_record(const uwb_settings_record_t *record)
{
    volatile uint32_t *dst = (volatile uint32_t *)(uintptr_t)UWB_SETTINGS_FLASH_ADDR;
    const uint32_t *src = (const uint32_t *)record;

    settings_nvmc_wait_ready();
    NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Een;
    settings_nvmc_wait_ready();
    NRF_NVMC->ERASEPAGE = UWB_SETTINGS_FLASH_ADDR;
    settings_nvmc_wait_ready();

    NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Wen;
    settings_nvmc_wait_ready();
    for (uint32_t i = 0u; i < UWB_SETTINGS_WORDS; i++)
    {
        dst[i] = src[i];
        settings_nvmc_wait_ready();
    }

    NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Ren;
    settings_nvmc_wait_ready();
}

static void settings_load_defaults(void)
{
    m_settings.arm_mode = UWB_SETTINGS_ARM_NONE;
    m_settings.seq = 0u;
    strncpy(m_settings.name, UWB_SETTINGS_NAME_DEFAULT, sizeof(m_settings.name) - 1u);
    m_settings.name[sizeof(m_settings.name) - 1u] = '\0';
}

static void settings_apply_record(const uwb_settings_record_t *record)
{
    m_settings.arm_mode = record->arm_mode;
    m_settings.seq = record->seq;
    memcpy(m_settings.name, record->name, sizeof(m_settings.name));
    m_settings.name[sizeof(m_settings.name) - 1u] = '\0';
}

static void settings_mark_error(void)
{
    m_write_state = SETTINGS_WRITE_IDLE;
    m_write_error = true;
}

static void settings_soc_evt_handler(uint32_t evt_id, void *p_context)
{
    uint32_t err_code;
    (void)p_context;

    if (evt_id == NRF_EVT_FLASH_OPERATION_ERROR)
    {
        settings_mark_error();
        return;
    }

    if (evt_id != NRF_EVT_FLASH_OPERATION_SUCCESS)
    {
        return;
    }

    if (m_write_state == SETTINGS_WRITE_ERASING)
    {
        m_write_state = SETTINGS_WRITE_WRITING;
        err_code = sd_flash_write((uint32_t *)(uintptr_t)UWB_SETTINGS_FLASH_ADDR,
                                  (const uint32_t *)&m_pending_record,
                                  UWB_SETTINGS_WORDS);
        if (err_code != NRF_SUCCESS)
        {
            settings_mark_error();
        }
        return;
    }

    if (m_write_state == SETTINGS_WRITE_WRITING)
    {
        settings_apply_record(&m_pending_record);
        m_write_state = SETTINGS_WRITE_IDLE;
        m_write_success = true;
    }
}

NRF_SDH_SOC_OBSERVER(m_settings_soc_observer, UWB_SETTINGS_SOC_PRIO, settings_soc_evt_handler, NULL);

void uwb_persistent_settings_init(void)
{
    const uwb_settings_record_t *record = flash_record();
    settings_load_defaults();
    m_write_state = SETTINGS_WRITE_IDLE;
    m_write_success = false;
    m_write_error = false;

    if (record_is_valid(record))
    {
        settings_apply_record(record);
    }
    else
    {
        /* Migrate a valid legacy v1 record: keep arm/seq, default name. */
        const uwb_settings_record_v1_t *record_v1 =
            (const uwb_settings_record_v1_t *)(uintptr_t)UWB_SETTINGS_FLASH_ADDR;
        if (record_v1_is_valid(record_v1))
        {
            m_settings.arm_mode = record_v1->arm_mode;
            m_settings.seq = record_v1->seq;
        }
    }
}

uwb_persistent_settings_t uwb_persistent_settings_get(void)
{
    return m_settings;
}

static bool settings_start_write(uint32_t arm_mode, const char *name)
{
    uint32_t page_number;
    uint32_t err_code;

    if (!arm_mode_is_valid(arm_mode) ||
        !uwb_persistent_settings_name_is_valid(name) ||
        (m_write_state != SETTINGS_WRITE_IDLE))
    {
        return false;
    }

    memset(&m_pending_record, 0xFF, sizeof(m_pending_record));
    m_pending_record.magic = UWB_SETTINGS_MAGIC;
    m_pending_record.version = UWB_SETTINGS_VERSION;
    m_pending_record.arm_mode = arm_mode;
    m_pending_record.seq = m_settings.seq + 1u;
    memset(m_pending_record.name, 0, sizeof(m_pending_record.name));
    strncpy(m_pending_record.name, name, sizeof(m_pending_record.name) - 1u);
    m_pending_record.crc = settings_crc(&m_pending_record);
    m_write_success = false;
    m_write_error = false;
    m_write_state = SETTINGS_WRITE_ERASING;

    if (!nrf_sdh_is_enabled())
    {
        settings_nvmc_write_record(&m_pending_record);
        settings_apply_record(&m_pending_record);
        m_write_state = SETTINGS_WRITE_IDLE;
        m_write_success = true;
        return true;
    }

    page_number = UWB_SETTINGS_FLASH_ADDR / UWB_SETTINGS_PAGE_SIZE;
    err_code = sd_flash_page_erase(page_number);
    if (err_code != NRF_SUCCESS)
    {
        settings_mark_error();
        return false;
    }

    return true;
}

bool uwb_persistent_settings_write_arm(uint32_t arm_mode)
{
    return settings_start_write(arm_mode, m_settings.name);
}

bool uwb_persistent_settings_write_name(const char *name)
{
    return settings_start_write(m_settings.arm_mode, name);
}

bool uwb_persistent_settings_write_pending(void)
{
    return m_write_state != SETTINGS_WRITE_IDLE;
}

bool uwb_persistent_settings_consume_write_success(void)
{
    if (!m_write_success)
    {
        return false;
    }
    m_write_success = false;
    return true;
}

bool uwb_persistent_settings_consume_write_error(void)
{
    if (!m_write_error)
    {
        return false;
    }
    m_write_error = false;
    return true;
}
