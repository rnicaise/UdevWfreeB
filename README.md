# UWB Ranging Firmware Architecture (SS-TWR)

This repository contains embedded firmware for a two-board UWB ranging system based on DW3000 + nRF52.

README.md and README_DEV.md are intentionally identical in this branch.
The goal is to provide a technical architecture guide (not a beginner C tutorial).

## 1) Current Runtime Scope

Current production-oriented behavior:
- SS-TWR is the active live workflow
- distance is computed on the initiator
- responder provides delayed response timestamps
- initiator outputs CSV on UART at 460800 baud

Important note:
- The codebase still contains some DS-oriented compatibility paths and legacy profile options.
- The active high-rate workflow is SS-TWR with FAST/TURBO test profiles.

## 2) Firmware Architecture Map

Main code areas:
- `src/main.c`: role entrypoint selection (`UWB_ROLE_INITIATOR` or `UWB_ROLE_RESPONDER`)
- `src/initiator/main_initiator.c`: initiator ranging loop, SS distance computation, CSV output
- `src/responder/main_responder.c`: responder RX/TX scheduling and timestamp embedding
- `src/common/ranging.h`: shared protocol constants and timing macros
- `src/common/uwb_profiles.c`: radio profile definitions and rate/channel mapping
- `src/common/uwb_profiles.h`: runtime profile structure and API
- `src/uart/uart_log.c`: UART TX/RX implementation
- `src/platform/*` and `src/board/*`: board-level hardware adaptation

Build-level structure:
- `CMakePresets.json`: role-specific build presets
- `CMakeLists.txt`: top-level firmware build
- `vendor/sdk`: vendored platform and DW3 SDK components

## 3) Where the SS-TWR Logic Lives

### Initiator side (distance computation)
Primary file:
- `src/initiator/main_initiator.c`

Key SS-TWR path:
1. Receive RESPONSE frame
2. Read responder timestamps from response payload
3. Compute round-trip and reply delay
4. Apply clock-offset correction
5. Convert ToF to meters
6. Emit CSV (`ms,sample,dist`)

The hot-path function used for output formatting is `write_distance_csv(...)`.

### Responder side (timestamped delayed response)
Primary file:
- `src/responder/main_responder.c`

Key SS-TWR responder path:
1. Receive POLL
2. Capture `poll_rx_ts`
3. Schedule delayed TX based on profile (`responder_ss_poll_rx_to_resp_tx_dly_uus`)
4. Embed `poll_rx_ts` and `resp_tx_ts` in response
5. Send delayed RESPONSE

## 4) Profile System and Where It Is Defined

Profile definitions are centralized in:
- `src/common/uwb_profiles.c`

The static `profiles[]` table defines per-profile:
- PHY config (`dwt_config_t`)
- initiator and responder timing delays/timeouts
- preamble timeout
- nominal data rate field (`data_rate_kbps`)

Current profile options in this table:
- `UWB_PROFILE_OPT_6M8_STABLE_CH5`
- `UWB_PROFILE_OPT_6M8_STABLE_CH9`
- `UWB_PROFILE_OPT_850K_ROBUST`

### About `UWB_PROFILE_OPT_850K_ROBUST`

This profile is defined with robust PHY/timing values, including:
- `DWT_BR_850K`
- `DWT_PLEN_1024`
- `DWT_PAC32`
- longer RX/TX delays and timeouts

In `uwb_profile_opt_for_channel_rate_kbps(...)`:
- rates `<= 850` map to `UWB_PROFILE_OPT_850K_ROBUST`
- otherwise, channel 9 maps to 6M8 CH9
- default is 6M8 CH5

So this robust profile is still part of the runtime profile layer even if the fastest SS workflow typically runs at 6M8.

## 5) Test Profiles vs PHY Profiles

There are two concept layers that are easy to confuse:

1. Test profiles (application behavior):
- examples: `FAST_DISTANCE_ONLY`, `TURBO_DISTANCE_ONLY`
- used to control runtime behavior, sampling strategy, and hot-path overhead

2. PHY/runtime radio profiles (`uwb_profiles.c`):
- examples: `6M8_STABLE_CH5`, `6M8_STABLE_CH9`, `850K_ROBUST`
- used to configure DW3000 PHY + timing

Both layers influence observed performance and robustness.

## 6) Key Timing Constants

Global shared timing constants are in:
- `src/common/ranging.h`

Examples used by active SS path:
- `POLL_TX_TO_RESP_RX_DLY_UUS`
- `SS_POLL_RX_TO_RESP_TX_DLY_UUS`
- `RESP_RX_TIMEOUT_UUS`
- `RNG_DELAY_MS`

These constants feed profile timing in `uwb_profiles.c` and ultimately shape throughput/stability tradeoffs.

## 7) UART Contract

Current primary runtime CSV output:
- `ms,sample,dist`

UART configuration:
- firmware side configured in `src/uart/uart_log.c`
- current operating baud is 460800

## 8) Build and Flash

Build from repository root:

```bash
cmake --preset=initiator_debug
cmake --build --preset initiator_debug

cmake --preset=responder_debug
cmake --build --preset responder_debug
```

Typical output files:
- `build/initiator_debug/uwb_initiator.hex`
- `build/responder_debug/uwb_responder.hex`

Flash examples:

```bash
nrfjprog --snr <INITIATOR_SNR> --program build/initiator_debug/uwb_initiator.hex --sectorerase --verify --reset
nrfjprog --snr <RESPONDER_SNR> --program build/responder_debug/uwb_responder.hex --sectorerase --verify --reset
```

## 9) Quick Code Reading Order (for reviewers)

If you need a fast architecture walkthrough:
1. `src/main.c`
2. `src/common/ranging.h`
3. `src/common/uwb_profiles.c`
4. `src/initiator/main_initiator.c`
5. `src/responder/main_responder.c`
6. `src/uart/uart_log.c`

This order gives role selection, timing/profile model, SS ranging logic, and output path.
