# Developer Guide

## Current Runtime Status

This repository currently runs a practical SS-TWR workflow, not the older DS-TWR teaching flow.

Current ground truth:
- active ranging mode: SS-TWR only
- distance is computed on the initiator
- the phone must be connected to the initiator over USB serial
- the other board runs the responder firmware
- active test profiles: `FAST_DISTANCE_ONLY` and `TURBO_DISTANCE_ONLY`
- firmware and Android serial baud rate: `460800`
- primary runtime CSV emitted by the initiator: `ms,sample,dist`

If you see older notes describing DS-TWR as the main runtime path, treat them as legacy background only.

---

## 1. System Overview

The project has two embedded roles:
- initiator: starts each ranging exchange, computes distance, sends CSV to Android
- responder: receives the poll and sends the delayed SS response

High-level data flow:

```text
Android phone <-> USB serial <-> Initiator <-> UWB SS-TWR <-> Responder
```

The current live measurement loop is:
1. Initiator sends a POLL frame.
2. Responder schedules and sends a delayed RESPONSE frame.
3. Initiator reads responder timestamps from the RESPONSE.
4. Initiator computes ToF and distance.
5. Initiator writes one CSV line to UART for the Android app.

---

## 2. Project Layout

Main firmware files:
- `src/main.c`
- `src/common/ranging.h`
- `src/common/uwb_profiles.c`
- `src/initiator/main_initiator.c`
- `src/responder/main_responder.c`
- `src/uart/uart_log.c`
- `src/board/custom_board.h`
- `src/platform/deca_spi_dwm3001cdk.c`
- `src/platform/port_dwm3001cdk.c`

Main Android files:
- `android-receiver-app/app/src/main/java/com/qorvo/uwbreceiver/service/UwbForegroundService.kt`
- `android-receiver-app/app/src/main/java/com/qorvo/uwbreceiver/data/CsvParser.kt`
- `android-receiver-app/app/src/main/java/com/qorvo/uwbreceiver/data/UwbModels.kt`
- `android-receiver-app/app/src/main/java/com/qorvo/uwbreceiver/ui/UwbMainScreen.kt`

The Qorvo SDK is vendored in `vendor/sdk`.

---

## 3. Firmware Entry Point

The build selects a role at compile time, then `main()` jumps into the role-specific ranging loop.

Key excerpt from `src/main.c`:

```c
#if defined(UWB_ROLE_INITIATOR)
extern int ds_twr_initiator_custom(void);
#define RANGING_ENTRY ds_twr_initiator_custom
#elif defined(UWB_ROLE_RESPONDER)
extern int ds_twr_responder_custom(void);
#define RANGING_ENTRY ds_twr_responder_custom
#endif

int main(void)
{
   qio_init();
   bsp_board_init(BSP_INIT_LEDS | BSP_INIT_BUTTONS);
   gpio_init();
   nrf52840_dk_spi_init();
   dw_irq_init();
   nrf_delay_ms(2);
   RANGING_ENTRY();

   while (1) { }
}
```

Why it matters:
- `initiator_debug` and `responder_debug` are separate builds
- the runtime role is not selected dynamically
- the ranging loop owns the lifetime of the firmware

---

## 4. Active Runtime Profiles

The current runtime accepts only two hot-switchable test profiles.

Key excerpt from `src/initiator/main_initiator.c`:

```c
static bool is_supported_test_profile(uint8_t profile)
{
   return (profile == UWB_TEST_PROFILE_FAST_DISTANCE_ONLY) ||
         (profile == UWB_TEST_PROFILE_TURBO_DISTANCE_ONLY);
}

static const char *test_profile_name(uint8_t profile)
{
   switch (profile)
   {
      case UWB_TEST_PROFILE_TURBO_DISTANCE_ONLY:
         return "TURBO_DISTANCE_ONLY";
      case UWB_TEST_PROFILE_FAST_DISTANCE_ONLY:
         return "FAST_DISTANCE_ONLY";
      default:
         return "UNKNOWN";
   }
}
```

The same restriction exists in the responder.

Profile constants from `src/common/ranging.h`:

```c
#define UWB_TEST_PROFILE_FAST_DISTANCE_ONLY  0u
#define UWB_TEST_PROFILE_TURBO_DISTANCE_ONLY 5u
#define UWB_TEST_PROFILE_DEFAULT UWB_TEST_PROFILE_TURBO_DISTANCE_ONLY
```

Why it matters:
- old accel-heavy and diagnostics-oriented profiles are not part of the intended current workflow
- the Android app also exposes only these two profiles in the main UI

---

## 5. Current SS-TWR Timing Constants

Important timing values live in `src/common/ranging.h`.

```c
#define POLL_TX_TO_RESP_RX_DLY_UUS  (300 + CPU_PROCESSING_TIME)
#define SS_POLL_RX_TO_RESP_TX_DLY_UUS 900
#define RESP_RX_TIMEOUT_UUS 300
#define RNG_DELAY_MS 1
```

What these control:
- how long the initiator waits before opening RX after the poll
- how long the responder waits before sending the delayed response
- how aggressive the receive timeout is
- the minimum loop delay between measurements

`SS_POLL_RX_TO_RESP_TX_DLY_UUS = 900` is part of the stabilized current setup.

---

## 6. Initiator: Distance Computation and CSV Output

The initiator is the board that computes distance and publishes it to Android.

### 6.1 Minimal CSV writer

Key excerpt from `src/initiator/main_initiator.c`:

```c
static void write_distance_csv(uint32_t ms, uint32_t sample, float distance_m)
{
   char *dst = output_buf;
   float distance_cm_f = distance_m * 100.0f;
   int32_t distance_cm = (int32_t)(distance_cm_f + ((distance_cm_f >= 0.0f) ? 0.5f : -0.5f));

   dst = append_u32(dst, ms);
   *dst++ = ',';
   dst = append_u32(dst, sample);
   *dst++ = ',';
   dst = append_distance_cm(dst, distance_cm);
   *dst = '\0';

   uart_log_write(output_buf);
}
```

Why this is important:
- the hot path uses a lightweight integer formatter
- the main CSV contract is intentionally minimal
- reducing per-sample overhead helps high-rate ranging stability

### 6.2 SS-TWR distance formula actually used

Key excerpt from `src/initiator/main_initiator.c`:

```c
ranging_msg_get_ts(&rx_buffer[RESP_MSG_SS_POLL_RX_TS_IDX], &responder_poll_rx_ts);
ranging_msg_get_ts(&rx_buffer[RESP_MSG_SS_RESP_TX_TS_IDX], &responder_resp_tx_ts);

rtd_init = resp_rx_ts_32 - poll_tx_ts_32;
reply_resp = responder_resp_tx_ts - responder_poll_rx_ts;
clock_offset_ratio = (float)dwt_readclockoffset() * (float)CLOCK_OFFSET_PPM_TO_RATIO;
tof_dtu = ((float)rtd_init - ((float)reply_resp * (1.0f - clock_offset_ratio))) / 2.0f;

distance = tof_dtu * (float)DWT_TIME_UNITS * (float)SPEED_OF_LIGHT;
ranging_count++;

ms = (uint32_t)(((uint64_t)NRF_RTC2->COUNTER * 1000u) / 32768u);
write_distance_csv(ms, ranging_count, distance);
```

Why this is important:
- this is the real SS-TWR computation path in the active build
- clock-offset correction is part of the stabilized solution
- the timestamp source for `ms` is local RTC2 on the initiator

---

## 7. Responder: Delayed SS Response Path

The responder does not publish the main distance CSV in the current workflow. Its critical job is to answer the initiator with the timestamps needed for SS-TWR.

Key excerpt from `src/responder/main_responder.c`:

```c
poll_rx_ts = ranging_get_rx_timestamp_u64();

response_delay_uus = active_profile->responder_ss_poll_rx_to_resp_tx_dly_uus;
resp_tx_time = (poll_rx_ts + (response_delay_uus * UUS_TO_DWT_TIME)) >> 8;
dwt_setdelayedtrxtime(resp_tx_time);
resp_tx_ts = (((uint64_t)(resp_tx_time & 0xFFFFFFFEUL)) << 8) + TX_ANT_DLY;

tx_resp_msg[ALL_MSG_SN_IDX] = frame_seq_nb;
tx_resp_msg[RESP_MSG_CTRL_TEST_PROFILE_IDX] = current_test_profile;
ranging_msg_set_ts(&tx_resp_msg[RESP_MSG_SS_POLL_RX_TS_IDX], poll_rx_ts);
ranging_msg_set_ts(&tx_resp_msg[RESP_MSG_SS_RESP_TX_TS_IDX], resp_tx_ts);
dwt_writetxdata(sizeof(tx_resp_msg), tx_resp_msg, 0);
dwt_writetxfctrl(sizeof(tx_resp_msg) + FCS_LEN, 0, 1);

ret = dwt_starttx(DWT_START_TX_DELAYED);
```

Why this is important:
- the responder schedules a delayed transmit based on the received poll time
- it embeds `poll_rx_ts` and `resp_tx_ts` into the response frame
- these timestamps are what the initiator needs to compute distance

---

## 8. UART Contract

The current serial link is shared between firmware and Android at `460800` baud.

Key excerpt from `src/uart/uart_log.c`:

```c
NRF_UARTE0->CONFIG = 0;
NRF_UARTE0->BAUDRATE = UARTE_BAUDRATE_BAUDRATE_Baud460800;

NRF_UARTE0->ENABLE = UARTE_ENABLE_ENABLE_Enabled;
```

The matching Android constant lives in `UwbForegroundService.kt`:

```kotlin
private const val SERIAL_BAUD_RATE = 460800
```

Why it matters:
- if firmware and Android disagree here, the link is unusable
- `460800` is the stable current operating point

---

## 9. Android Side: Role Enforcement and Sample Filtering

The Android app is aware of the SS-only workflow and prevents applying settings to the wrong board.

Key excerpt from `android-receiver-app/.../UwbForegroundService.kt`:

```kotlin
if (connectedRole == ConnectedUwbRole.RESPONDER) {
   RuntimeStore.setLinkState(RuntimeStore.state.value.linkState, "SS-TWR only: connect USB to initiator")
   return
}
```

The app also rejects implausible distance jumps.

```kotlin
private fun isPlausibleSample(sample: CsvSample): Boolean {
   if (!sample.dist.isFinite() || sample.dist < -5f || sample.dist > 30f) {
      return false
   }

   val previous = lastAcceptedSample ?: return true
   val sampleDelta = sample.sample - previous.sample
   if (sampleDelta <= 0L) {
      return false
   }

   val distanceDelta = kotlin.math.abs(sample.dist - previous.dist)
   return !(sampleDelta <= 3L && distanceDelta > 5f)
}
```

Why it matters:
- this protects the UI against obvious serial glitches or unstable bursts
- the app behavior was tuned for the current higher-rate SS runtime

---

## 10. Android CSV Parsing

The parser accepts the minimal current CSV format while remaining compatible with richer legacy lines.

Key excerpt from `android-receiver-app/.../CsvParser.kt`:

```kotlin
val parts = trimmed.split(',')
if (parts.size < 3) {
   return null
}

CsvSample(
   ms = parts[0].toLong(),
   sample = parts[1].toLong(),
   dist = parts[2].toFloat(),
   ...
)
```

Current primary contract:

```text
ms,sample,dist
1203,57,2.34
```

Why it matters:
- the current initiator path only needs three columns
- the parser still tolerates extended formats with radio metrics and legacy telemetry

---

## 11. Build Commands

From the repository root:

Initiator debug:

```bash
cmake --preset=initiator_debug
cmake --build --preset initiator_debug
```

Responder debug:

```bash
cmake --preset=responder_debug
cmake --build --preset responder_debug
```

Android app debug APK:

```bash
cd android-receiver-app
./gradlew assembleDebug
```

---

## 12. Flash Commands

Examples with `nrfjprog`:

Initiator:

```bash
nrfjprog --snr 760221448 --program build/initiator_debug/uwb_initiator.hex --sectorerase --verify --reset
```

Responder:

```bash
nrfjprog --snr 760220908 --program build/responder_debug/uwb_responder.hex --sectorerase --verify --reset
```

Custom board through J-Link EDU Mini:

```bash
nrfjprog --snr 802009545 --family NRF52 --program build/responder_debug/uwb_responder.hex --sectorerase --verify --reset
```

Notes:
- recurring `SeggerBackend` `-256` lines can still appear during successful operations
- the final success criteria are program, verify, and reset completion
- the J-Link EDU Mini needs valid target power and VTref; it does not power the board for you

---

## 13. Quick Validation Checklist

After flashing both boards:
1. Connect the phone to the initiator, not the responder.
2. Launch the Android app.
3. Confirm the app detects the role and allows settings application.
4. Confirm live CSV is flowing.
5. Move the boards and verify distance updates.
6. Switch between `FAST_DISTANCE_ONLY` and `TURBO_DISTANCE_ONLY`.

If something is wrong:
- no serial data: check baud, cable, OTG, and which board is connected
- no ranging: check that initiator and responder builds were not swapped
- unstable distance: recheck power, antenna setup, and any timing changes

---

## 14. Throughput Tuning History (>70 Hz)

This table summarizes what has already been tuned in this repository and what has not been lowered yet.

| Lever | Current value | Tried already | Observed effect | Stability risk |
| --- | --- | --- | --- | --- |
| `RNG_DELAY_MS` | `1` | Yes | Major frequency gain versus older higher delays | Medium if other parts are also aggressive |
| Initiator hot-path logs/formatting | minimal CSV (`ms,sample,dist`) | Yes | Better sustained rate | Low |
| UART end-to-end baud | `460800` | Yes (higher baud also tested in project history) | Good compromise for stability | Medium at very high baud |
| Android plausibility filtering | enabled | Yes | Fewer visible spikes/glitches | Low |
| `POLL_TX_TO_RESP_RX_DLY_UUS` | `300 + CPU_PROCESSING_TIME` | No documented reduction in git history | Unknown | High |
| `SS_POLL_RX_TO_RESP_TX_DLY_UUS` | `900` | No documented reduction in git history | Unknown | High |
| `RESP_RX_TIMEOUT_UUS` | `300` | No documented reduction in git history | Unknown | Medium to High |

Interpretation:
- the project already used software-path optimizations to push frequency up
- the three radio timing constants above have not been reduced in a documented commit history yet
- those three are the most sensitive levers for the next frequency step and also the easiest way to re-introduce instability

---

## 15. What Is Still Legacy

Some source files still contain DS-TWR branches, extra telemetry paths, or historical profile definitions.

That does not mean they are the preferred runtime path.

For current development, prioritize these truths:
- SS-TWR drives the live measurement workflow
- initiator computes distance
- responder mainly provides delayed timestamped responses
- Android is meant to be attached to the initiator
- `460800` is the current stable serial configuration

---

## 16. Recommended Files to Read First

If you want to understand the current system quickly, read in this order:
1. `src/main.c`
2. `src/common/ranging.h`
3. `src/initiator/main_initiator.c`
4. `src/responder/main_responder.c`
5. `src/uart/uart_log.c`
6. `android-receiver-app/app/src/main/java/com/qorvo/uwbreceiver/service/UwbForegroundService.kt`
7. `android-receiver-app/app/src/main/java/com/qorvo/uwbreceiver/data/CsvParser.kt`

That path gives you the current boot flow, timing configuration, SS response path, distance computation, UART output, and Android ingestion logic.
