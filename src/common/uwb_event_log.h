/*
 * UWB event log ("black box"): full-rate RAM ring of distance samples,
 * frozen and dumped to a dedicated flash region when the distance crosses
 * a trigger threshold (airbag deployment analogue).
 *
 * Flash layout: UWB_EVENTLOG_FLASH_ADDR, UWB_EVENT_LOG_SLOT_COUNT slots of
 * 20 KB (5 pages) each, rotating (oldest seq overwritten). Each slot holds
 * one capture: ~5 s of pre-trigger history + ~1 s post-trigger at full
 * ranging rate (~400 Hz), 8 bytes per record.
 */
#ifndef UWB_EVENT_LOG_H
#define UWB_EVENT_LOG_H

#include <stdbool.h>
#include <stdint.h>

#define UWB_EVENT_LOG_SLOT_COUNT 6u

/* Sentinel raw distance for invalid ranging samples. */
#define UWB_EVENT_LOG_INVALID_MM 0x7FFF

typedef struct
{
    uint32_t seq;
    uint32_t trigger_ms;
    uint32_t count;
    uint32_t pre_count;
} uwb_event_log_slot_info_t;

void uwb_event_log_init(void);

/* Push one ranging sample into the RAM ring and evaluate the trigger.
 * distances in metres; valid=false samples are stored with the sentinel. */
void uwb_event_log_push(uint32_t ms, bool valid, float raw_m, float filt_m);

/* Call once per main-loop iteration: handles post-trigger timeout and
 * starts/retries deferred flash operations. */
void uwb_event_log_service(uint32_t now_ms);

/* True while a capture dump or clear is pending or writing flash. */
bool uwb_event_log_busy(void);

bool uwb_event_log_slot_info(uint32_t slot, uwb_event_log_slot_info_t *info);
bool uwb_event_log_read_record(uint32_t slot, uint32_t idx,
                               uint32_t *ms, int16_t *raw_mm, int16_t *filt_mm);

/* Request erasing all slots (async when the SoftDevice is enabled). */
bool uwb_event_log_clear(void);

/* One-shot event flags for app-level reporting. */
bool uwb_event_log_consume_triggered(uint32_t *trigger_ms);
bool uwb_event_log_consume_saved(uint32_t *slot, uint32_t *count);
bool uwb_event_log_consume_error(void);
bool uwb_event_log_consume_cleared(void);

#endif
