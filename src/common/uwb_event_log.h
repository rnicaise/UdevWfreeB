/*
 * UWB event log ("black box"): full-rate RAM ring of distance + accel
 * samples, frozen and dumped to a dedicated flash region when the arm
 * rule fires (airbag deployment analogue).
 *
 * Flash layout: UWB_EVENTLOG_FLASH_ADDR, UWB_EVENT_LOG_SLOT_COUNT slots of
 * 40 KB (10 pages) each, rotating (oldest seq overwritten). Each slot holds
 * one capture: ~4.5 s of pre-trigger history + ~0.5 s post-trigger at full
 * ranging rate (~400 Hz), 20 bytes per record (distance raw/filt, initiator
 * accel XYZ, responder accel XYZ).
 */
#ifndef UWB_EVENT_LOG_H
#define UWB_EVENT_LOG_H

#include <stdbool.h>
#include <stdint.h>

#define UWB_EVENT_LOG_SLOT_COUNT 3u

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

/* Push one cycle sample into the RAM ring. Distances in metres;
 * valid=false samples store the raw sentinel (ranging failed this cycle,
 * responder accel is then zeros). ia/ra are raw int16 accel XYZ. */
void uwb_event_log_push(uint32_t ms, bool valid, float raw_m, float filt_m,
                        const int16_t ia[3], const int16_t ra[3]);

/* Fire the black-box capture (arm-rule trigger / fire command): freezes
 * the pre-trigger history and keeps recording a short post-trigger tail
 * before dumping to flash. Ignored if a capture is already in progress. */
void uwb_event_log_trigger(uint32_t now_ms);

/* Call once per main-loop iteration: handles post-trigger timeout and
 * starts/retries deferred flash operations. */
void uwb_event_log_service(uint32_t now_ms);

/* True while a capture dump or clear is pending or writing flash. */
bool uwb_event_log_busy(void);

bool uwb_event_log_slot_info(uint32_t slot, uwb_event_log_slot_info_t *info);
bool uwb_event_log_read_record(uint32_t slot, uint32_t idx,
                               uint32_t *ms, int16_t *raw_mm, int16_t *filt_mm,
                               int16_t ia[3], int16_t ra[3]);

/* Request erasing all slots (async when the SoftDevice is enabled). */
bool uwb_event_log_clear(void);

/* One-shot event flags for app-level reporting. */
bool uwb_event_log_consume_triggered(uint32_t *trigger_ms);
bool uwb_event_log_consume_saved(uint32_t *slot, uint32_t *count);
bool uwb_event_log_consume_error(void);
bool uwb_event_log_consume_cleared(void);

#endif
