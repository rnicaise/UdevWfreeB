#include "uwb_event_log.h"

#include <string.h>

#include "nrf.h"
#include "nrf_error.h"
#include "nrf_sdh.h"
#include "nrf_sdh_soc.h"
#include "nrf_soc.h"

#include "uwb_persistent_settings.h"

#ifndef UWB_EVENTLOG_FLASH_ADDR
#error "UWB_EVENTLOG_FLASH_ADDR must be defined for event log builds"
#endif

#define EVLOG_MAGIC              0x4C425755u /* "UWBL" */
#define EVLOG_VERSION            1u
#define EVLOG_PAGE_SIZE          0x1000u
#define EVLOG_SLOT_SIZE          0x5000u /* 5 pages = 20 KB */
#define EVLOG_HEADER_WORDS       8u
#define EVLOG_MAX_RECORDS        ((EVLOG_SLOT_SIZE - (EVLOG_HEADER_WORDS * 4u)) / 8u) /* 2556 */
#define EVLOG_RING_CAPACITY      EVLOG_MAX_RECORDS

/* Trigger: filtered distance above threshold for N consecutive valid
 * samples. Re-arms when the distance comes back below the re-arm level. */
#define EVLOG_TRIGGER_MM         1000
#define EVLOG_REARM_MM           800
#define EVLOG_TRIGGER_CONSEC     3u
#define EVLOG_POST_RECORDS       400u    /* ~1 s at 400 Hz */
#define EVLOG_POST_TIMEOUT_MS    1500u   /* freeze even if ranging stops */

#define EVLOG_CHUNK_WORDS        256u    /* 1 KB per sd_flash_write */
#define EVLOG_SOC_PRIO           1u

typedef struct
{
    uint32_t ms;
    int16_t  raw_mm;
    int16_t  filt_mm;
} evlog_record_t; /* 8 bytes, 2 words */

typedef struct
{
    uint32_t magic;
    uint32_t version;
    uint32_t seq;
    uint32_t trigger_ms;
    uint32_t count;
    uint32_t pre_count;
    uint32_t crc;
    uint32_t reserved;
} evlog_header_t;

typedef enum
{
    TRIG_WAIT_BELOW = 0, /* wait for distance < re-arm level before arming */
    TRIG_ARMED,
    TRIG_POST            /* triggered, capturing post-trigger samples */
} trig_state_t;

typedef enum
{
    OP_IDLE = 0,
    OP_DUMP_ERASING,
    OP_DUMP_WRITING,
    OP_CLEAR_ERASING
} op_state_t;

static evlog_record_t m_ring[EVLOG_RING_CAPACITY];
static uint32_t m_ring_head;
static uint32_t m_ring_count;

static trig_state_t m_trig_state = TRIG_WAIT_BELOW;
static uint32_t m_consec_above;
static uint32_t m_trigger_ms;
static uint32_t m_post_count;
static uint32_t m_pre_count;

static volatile bool m_dump_pending;   /* ring frozen, waiting to write */
static volatile bool m_clear_pending;
static volatile op_state_t m_op = OP_IDLE;
static uint32_t m_op_page;             /* page index within current op */
static uint32_t m_op_word;             /* next word offset to write */
static uint32_t m_dump_slot;
static uint32_t m_dump_start;          /* ring index of oldest record */
static uint32_t m_dump_count;
static uint32_t m_total_words;
static evlog_header_t m_dump_header;
static uint32_t m_chunk_buf[EVLOG_CHUNK_WORDS];

static volatile bool m_flag_triggered;
static uint32_t m_flag_trigger_ms;
static volatile bool m_flag_saved;
static uint32_t m_flag_saved_slot;
static uint32_t m_flag_saved_count;
static volatile bool m_flag_error;
static volatile bool m_flag_cleared;

static uint32_t slot_addr(uint32_t slot)
{
    return UWB_EVENTLOG_FLASH_ADDR + (slot * EVLOG_SLOT_SIZE);
}

static const evlog_header_t *slot_header(uint32_t slot)
{
    return (const evlog_header_t *)(uintptr_t)slot_addr(slot);
}

static uint32_t header_crc(const evlog_header_t *h)
{
    return h->magic ^ h->version ^ h->seq ^ h->trigger_ms ^
           h->count ^ h->pre_count ^ 0xC3C33C3Cu;
}

static bool slot_is_valid(uint32_t slot)
{
    const evlog_header_t *h = slot_header(slot);
    return (h->magic == EVLOG_MAGIC) &&
           (h->version == EVLOG_VERSION) &&
           (h->count <= EVLOG_MAX_RECORDS) &&
           (h->crc == header_crc(h));
}

static int16_t distance_to_mm(float distance_m)
{
    float mm = distance_m * 1000.0f;
    if (mm > 32000.0f)
    {
        mm = 32000.0f;
    }
    else if (mm < -32000.0f)
    {
        mm = -32000.0f;
    }
    return (int16_t)mm;
}

/* Fill nwords of the dump image starting at word offset. The image is:
 * header (8 words) followed by records (2 words each) in chronological
 * order, padded with 0xFFFFFFFF. */
static void dump_fill_words(uint32_t word_offset, uint32_t *dst, uint32_t nwords)
{
    const uint32_t *header_words = (const uint32_t *)&m_dump_header;

    for (uint32_t i = 0u; i < nwords; i++)
    {
        uint32_t w = word_offset + i;
        if (w < EVLOG_HEADER_WORDS)
        {
            dst[i] = header_words[w];
        }
        else
        {
            uint32_t rec = (w - EVLOG_HEADER_WORDS) / 2u;
            if (rec >= m_dump_count)
            {
                dst[i] = 0xFFFFFFFFu;
            }
            else
            {
                uint32_t tmp[2];
                const evlog_record_t *r =
                    &m_ring[(m_dump_start + rec) % EVLOG_RING_CAPACITY];
                memcpy(tmp, r, sizeof(tmp));
                dst[i] = tmp[(w - EVLOG_HEADER_WORDS) & 1u];
            }
        }
    }
}

static uint32_t dump_pages_needed(void)
{
    return ((m_total_words * 4u) + EVLOG_PAGE_SIZE - 1u) / EVLOG_PAGE_SIZE;
}

static void dump_prepare(void)
{
    uint32_t max_seq = 0u;
    uint32_t max_slot = 0u;
    bool found = false;

    for (uint32_t i = 0u; i < UWB_EVENT_LOG_SLOT_COUNT; i++)
    {
        if (slot_is_valid(i) && (slot_header(i)->seq >= max_seq))
        {
            max_seq = slot_header(i)->seq;
            max_slot = i;
            found = true;
        }
    }

    m_dump_slot = found ? ((max_slot + 1u) % UWB_EVENT_LOG_SLOT_COUNT) : 0u;

    memset(&m_dump_header, 0xFF, sizeof(m_dump_header));
    m_dump_header.magic = EVLOG_MAGIC;
    m_dump_header.version = EVLOG_VERSION;
    m_dump_header.seq = max_seq + 1u;
    m_dump_header.trigger_ms = m_trigger_ms;
    m_dump_header.count = m_dump_count;
    m_dump_header.pre_count = m_pre_count;
    m_dump_header.crc = header_crc(&m_dump_header);

    m_total_words = EVLOG_HEADER_WORDS + (m_dump_count * 2u);
}

static void capture_reset_after_dump(void)
{
    m_ring_head = 0u;
    m_ring_count = 0u;
    m_trig_state = TRIG_WAIT_BELOW;
    m_consec_above = 0u;
    m_post_count = 0u;
}

static void dump_complete(void)
{
    m_flag_saved_slot = m_dump_slot;
    m_flag_saved_count = m_dump_count;
    m_flag_saved = true;
    m_dump_pending = false;
    m_op = OP_IDLE;
    capture_reset_after_dump();
}

static void op_fail(void)
{
    m_flag_error = true;
    m_dump_pending = false;
    m_clear_pending = false;
    m_op = OP_IDLE;
    capture_reset_after_dump();
}

/* -- Blocking NVMC path (SoftDevice disabled, e.g. armed boot) -- */

static void nvmc_wait_ready(void)
{
    while (NRF_NVMC->READY == NVMC_READY_READY_Busy)
    {
    }
}

static void nvmc_erase_page(uint32_t addr)
{
    nvmc_wait_ready();
    NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Een;
    nvmc_wait_ready();
    NRF_NVMC->ERASEPAGE = addr;
    nvmc_wait_ready();
    NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Ren;
    nvmc_wait_ready();
}

static void nvmc_dump(void)
{
    uint32_t base = slot_addr(m_dump_slot);
    uint32_t pages = dump_pages_needed();

    for (uint32_t p = 0u; p < pages; p++)
    {
        nvmc_erase_page(base + (p * EVLOG_PAGE_SIZE));
    }

    nvmc_wait_ready();
    NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Wen;
    nvmc_wait_ready();
    for (uint32_t w = 0u; w < m_total_words; w += EVLOG_CHUNK_WORDS)
    {
        uint32_t n = m_total_words - w;
        if (n > EVLOG_CHUNK_WORDS)
        {
            n = EVLOG_CHUNK_WORDS;
        }
        dump_fill_words(w, m_chunk_buf, n);
        volatile uint32_t *dst = (volatile uint32_t *)(uintptr_t)(base + (w * 4u));
        for (uint32_t i = 0u; i < n; i++)
        {
            dst[i] = m_chunk_buf[i];
            nvmc_wait_ready();
        }
    }
    NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Ren;
    nvmc_wait_ready();

    dump_complete();
}

static void nvmc_clear(void)
{
    for (uint32_t p = 0u; p < (UWB_EVENT_LOG_SLOT_COUNT * (EVLOG_SLOT_SIZE / EVLOG_PAGE_SIZE)); p++)
    {
        nvmc_erase_page(UWB_EVENTLOG_FLASH_ADDR + (p * EVLOG_PAGE_SIZE));
    }
    m_clear_pending = false;
    m_op = OP_IDLE;
    m_flag_cleared = true;
}

/* -- Async sd_flash path (SoftDevice enabled) -- */

static bool sd_start_dump(void)
{
    uint32_t page = slot_addr(m_dump_slot) / EVLOG_PAGE_SIZE;
    m_op_page = 0u;
    m_op = OP_DUMP_ERASING;
    if (sd_flash_page_erase(page) != NRF_SUCCESS)
    {
        m_op = OP_IDLE;
        return false; /* busy: retry from service() */
    }
    return true;
}

static bool sd_start_clear(void)
{
    uint32_t page = UWB_EVENTLOG_FLASH_ADDR / EVLOG_PAGE_SIZE;
    m_op_page = 0u;
    m_op = OP_CLEAR_ERASING;
    if (sd_flash_page_erase(page) != NRF_SUCCESS)
    {
        m_op = OP_IDLE;
        return false;
    }
    return true;
}

static void sd_write_next_chunk(void)
{
    uint32_t n = m_total_words - m_op_word;
    if (n > EVLOG_CHUNK_WORDS)
    {
        n = EVLOG_CHUNK_WORDS;
    }
    dump_fill_words(m_op_word, m_chunk_buf, n);
    uint32_t addr = slot_addr(m_dump_slot) + (m_op_word * 4u);
    if (sd_flash_write((uint32_t *)(uintptr_t)addr, m_chunk_buf, n) != NRF_SUCCESS)
    {
        op_fail();
    }
}

static void evlog_soc_evt_handler(uint32_t evt_id, void *p_context)
{
    (void)p_context;

    if (m_op == OP_IDLE)
    {
        return;
    }

    if (evt_id == NRF_EVT_FLASH_OPERATION_ERROR)
    {
        op_fail();
        return;
    }

    if (evt_id != NRF_EVT_FLASH_OPERATION_SUCCESS)
    {
        return;
    }

    if (m_op == OP_DUMP_ERASING)
    {
        m_op_page++;
        if (m_op_page < dump_pages_needed())
        {
            uint32_t page = (slot_addr(m_dump_slot) + (m_op_page * EVLOG_PAGE_SIZE)) / EVLOG_PAGE_SIZE;
            if (sd_flash_page_erase(page) != NRF_SUCCESS)
            {
                op_fail();
            }
        }
        else
        {
            m_op = OP_DUMP_WRITING;
            m_op_word = 0u;
            sd_write_next_chunk();
        }
        return;
    }

    if (m_op == OP_DUMP_WRITING)
    {
        uint32_t n = m_total_words - m_op_word;
        if (n > EVLOG_CHUNK_WORDS)
        {
            n = EVLOG_CHUNK_WORDS;
        }
        m_op_word += n;
        if (m_op_word < m_total_words)
        {
            sd_write_next_chunk();
        }
        else
        {
            dump_complete();
        }
        return;
    }

    if (m_op == OP_CLEAR_ERASING)
    {
        m_op_page++;
        if (m_op_page < (UWB_EVENT_LOG_SLOT_COUNT * (EVLOG_SLOT_SIZE / EVLOG_PAGE_SIZE)))
        {
            uint32_t page = (UWB_EVENTLOG_FLASH_ADDR + (m_op_page * EVLOG_PAGE_SIZE)) / EVLOG_PAGE_SIZE;
            if (sd_flash_page_erase(page) != NRF_SUCCESS)
            {
                op_fail();
            }
        }
        else
        {
            m_clear_pending = false;
            m_op = OP_IDLE;
            m_flag_cleared = true;
        }
        return;
    }
}

NRF_SDH_SOC_OBSERVER(m_evlog_soc_observer, EVLOG_SOC_PRIO, evlog_soc_evt_handler, NULL);

/* -- Capture logic -- */

static void request_dump(void)
{
    /* Freeze the ring: snapshot start/count for the writer. */
    m_dump_count = m_ring_count;
    m_dump_start = (m_ring_head + EVLOG_RING_CAPACITY - m_ring_count) % EVLOG_RING_CAPACITY;
    m_dump_pending = true;
}

void uwb_event_log_init(void)
{
    m_ring_head = 0u;
    m_ring_count = 0u;
    m_trig_state = TRIG_WAIT_BELOW;
    m_consec_above = 0u;
    m_dump_pending = false;
    m_clear_pending = false;
    m_op = OP_IDLE;
}

void uwb_event_log_push(uint32_t ms, bool valid, float raw_m, float filt_m)
{
    evlog_record_t rec;

    if (m_dump_pending || (m_op != OP_IDLE))
    {
        return; /* ring frozen while dumping */
    }

    rec.ms = ms;
    rec.raw_mm = valid ? distance_to_mm(raw_m) : (int16_t)UWB_EVENT_LOG_INVALID_MM;
    rec.filt_mm = distance_to_mm(filt_m);

    m_ring[m_ring_head] = rec;
    m_ring_head = (m_ring_head + 1u) % EVLOG_RING_CAPACITY;
    if (m_ring_count < EVLOG_RING_CAPACITY)
    {
        m_ring_count++;
    }

    if (m_trig_state == TRIG_WAIT_BELOW)
    {
        if (valid && (rec.filt_mm < EVLOG_REARM_MM))
        {
            m_trig_state = TRIG_ARMED;
            m_consec_above = 0u;
        }
    }
    else if (m_trig_state == TRIG_ARMED)
    {
        if (valid && (rec.filt_mm >= EVLOG_TRIGGER_MM))
        {
            m_consec_above++;
            if (m_consec_above >= EVLOG_TRIGGER_CONSEC)
            {
                m_trig_state = TRIG_POST;
                m_trigger_ms = ms;
                m_post_count = 0u;
                m_pre_count = m_ring_count;
                m_flag_trigger_ms = ms;
                m_flag_triggered = true;
            }
        }
        else
        {
            m_consec_above = 0u;
        }
    }
    else /* TRIG_POST */
    {
        m_post_count++;
        if (m_post_count >= EVLOG_POST_RECORDS)
        {
            request_dump();
        }
    }
}

void uwb_event_log_service(uint32_t now_ms)
{
    if ((m_trig_state == TRIG_POST) && !m_dump_pending &&
        ((uint32_t)(now_ms - m_trigger_ms) >= EVLOG_POST_TIMEOUT_MS))
    {
        request_dump();
    }

    if (m_op != OP_IDLE)
    {
        return;
    }

    /* Never interleave with a persistent-settings flash write: both state
     * machines listen to the same SoC flash events. */
    if (uwb_persistent_settings_write_pending())
    {
        return;
    }

    if (m_dump_pending)
    {
        dump_prepare();
        if (!nrf_sdh_is_enabled())
        {
            nvmc_dump();
        }
        else
        {
            (void)sd_start_dump(); /* on busy, retried next call */
        }
        return;
    }

    if (m_clear_pending)
    {
        if (!nrf_sdh_is_enabled())
        {
            nvmc_clear();
        }
        else
        {
            (void)sd_start_clear();
        }
    }
}

bool uwb_event_log_busy(void)
{
    return m_dump_pending || m_clear_pending || (m_op != OP_IDLE);
}

bool uwb_event_log_slot_info(uint32_t slot, uwb_event_log_slot_info_t *info)
{
    if ((slot >= UWB_EVENT_LOG_SLOT_COUNT) || !slot_is_valid(slot))
    {
        return false;
    }

    const evlog_header_t *h = slot_header(slot);
    info->seq = h->seq;
    info->trigger_ms = h->trigger_ms;
    info->count = h->count;
    info->pre_count = h->pre_count;
    return true;
}

bool uwb_event_log_read_record(uint32_t slot, uint32_t idx,
                               uint32_t *ms, int16_t *raw_mm, int16_t *filt_mm)
{
    if ((slot >= UWB_EVENT_LOG_SLOT_COUNT) || !slot_is_valid(slot) ||
        (idx >= slot_header(slot)->count))
    {
        return false;
    }

    const evlog_record_t *records =
        (const evlog_record_t *)(uintptr_t)(slot_addr(slot) + (EVLOG_HEADER_WORDS * 4u));
    *ms = records[idx].ms;
    *raw_mm = records[idx].raw_mm;
    *filt_mm = records[idx].filt_mm;
    return true;
}

bool uwb_event_log_clear(void)
{
    if (uwb_event_log_busy())
    {
        return false;
    }
    m_clear_pending = true;
    return true;
}

bool uwb_event_log_consume_triggered(uint32_t *trigger_ms)
{
    if (!m_flag_triggered)
    {
        return false;
    }
    m_flag_triggered = false;
    *trigger_ms = m_flag_trigger_ms;
    return true;
}

bool uwb_event_log_consume_saved(uint32_t *slot, uint32_t *count)
{
    if (!m_flag_saved)
    {
        return false;
    }
    m_flag_saved = false;
    *slot = m_flag_saved_slot;
    *count = m_flag_saved_count;
    return true;
}

bool uwb_event_log_consume_error(void)
{
    if (!m_flag_error)
    {
        return false;
    }
    m_flag_error = false;
    return true;
}

bool uwb_event_log_consume_cleared(void)
{
    if (!m_flag_cleared)
    {
        return false;
    }
    m_flag_cleared = false;
    return true;
}
