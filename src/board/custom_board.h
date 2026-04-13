/*
 * custom_board.h — Board definition for DWM3001CDK (nRF52833 + DW3000)
 *
 * This file maps the Nordic BSP (LEDs, buttons) and the ARDUINO_*_PIN
 * macros used by the Qorvo SDK port.h to the actual DWM3001CDK pins.
 *
 * DW3000 SPI wiring on DWM3001CDK:
 *   SCK  = P0.03    MOSI = P0.08    MISO = P0.29    CS = P1.06
 *   IRQ  = P1.02    RST  = P0.25    WAKEUP = P1.19 (internal)
 *   SPI instance = SPIM3
 */

#ifndef CUSTOM_BOARD_H
#define CUSTOM_BOARD_H

#ifdef __cplusplus
extern "C" {
#endif

#include "nrf_gpio.h"

/* ── LEDs (active low) ── */
#define LEDS_NUMBER    4

#define LED_1          NRF_GPIO_PIN_MAP(0, 4)
#define LED_2          NRF_GPIO_PIN_MAP(0, 5)
#define LED_3          NRF_GPIO_PIN_MAP(0, 22)
#define LED_4          NRF_GPIO_PIN_MAP(0, 14)
#define LED_START      LED_1
#define LED_STOP       LED_4

#define LEDS_ACTIVE_STATE 0

#define LEDS_LIST { LED_1, LED_2, LED_3, LED_4 }

#define LEDS_INV_MASK  LEDS_MASK

#define BSP_LED_0      NRF_GPIO_PIN_MAP(0, 4)
#define BSP_LED_1      NRF_GPIO_PIN_MAP(0, 5)
#define BSP_LED_2      NRF_GPIO_PIN_MAP(0, 22)
#define BSP_LED_3      NRF_GPIO_PIN_MAP(0, 14)

/* ── Buttons ── */
#define BUTTONS_NUMBER 1

#define BUTTON_1       NRF_GPIO_PIN_MAP(0, 2)
#define BUTTON_PULL    NRF_GPIO_PIN_PULLUP

#define BUTTONS_ACTIVE_STATE 0

#define BUTTONS_LIST { BUTTON_1 }

#define BSP_BUTTON_0   BUTTON_1

/* ── UART ── */
#define RX_PIN_NUMBER  15
#define TX_PIN_NUMBER  19
#define CTS_PIN_NUMBER 0xFFFFFFFF
#define RTS_PIN_NUMBER 0xFFFFFFFF
#define HWFC           false

/*
 * ── ARDUINO_*_PIN mapping for DWM3001CDK ──
 *
 * The Qorvo SDK port.h uses ARDUINO_*_PIN to define DW3000 connections:
 *   DW3000_CLK_Pin  = ARDUINO_13_PIN → SCK  = P0.03
 *   DW3000_MOSI_Pin = ARDUINO_11_PIN → MOSI = P0.08
 *   DW3000_MISO_Pin = ARDUINO_12_PIN → MISO = P0.29
 *   DW3000_CS_Pin   = ARDUINO_10_PIN → CS   = P1.06
 *   DW3000_IRQn_Pin = ARDUINO_8_PIN  → IRQ  = P1.02
 *   DW3000_RESET_Pin= ARDUINO_7_PIN  → RST  = P0.25
 *   DW3000_WAKEUP   = ARDUINO_9_PIN  → WU   = P1.19 (module-internal)
 *   DW3000_IRQ2n    = ARDUINO_6_PIN  → unused
 *
 * SPI2 pins (second DW3000 — not present on DWM3001CDK):
 *   DW3000_CLK2_Pin  is hardcoded in port.h as NRF_GPIO_PIN_MAP(0, 7)
 *   DW3000_CS2_Pin   = ARDUINO_2_PIN
 *   DW3000_MOSI2_Pin = ARDUINO_4_PIN
 *   DW3000_MISO2_Pin = ARDUINO_5_PIN
 */
#define ARDUINO_13_PIN              NRF_GPIO_PIN_MAP(0, 3)   /* SPI SCK */
#define ARDUINO_12_PIN              NRF_GPIO_PIN_MAP(0, 29)  /* SPI MISO */
#define ARDUINO_11_PIN              NRF_GPIO_PIN_MAP(0, 8)   /* SPI MOSI */
#define ARDUINO_10_PIN              NRF_GPIO_PIN_MAP(1, 6)   /* SPI CS */
#define ARDUINO_9_PIN               NRF_GPIO_PIN_MAP(1, 9)   /* WAKEUP (safe P1 pin) */
#define ARDUINO_8_PIN               NRF_GPIO_PIN_MAP(1, 2)   /* IRQ */
#define ARDUINO_7_PIN               NRF_GPIO_PIN_MAP(0, 25)  /* RESET */
#define ARDUINO_6_PIN               NRF_GPIO_PIN_MAP(1, 7)   /* IRQ2 (unused) */
#define ARDUINO_5_PIN               NRF_GPIO_PIN_MAP(1, 5)   /* SPI2 MISO (unused) */
#define ARDUINO_4_PIN               NRF_GPIO_PIN_MAP(1, 4)   /* SPI2 MOSI (unused) */
#define ARDUINO_3_PIN               NRF_GPIO_PIN_MAP(1, 3)   /* (unused) */
#define ARDUINO_2_PIN               NRF_GPIO_PIN_MAP(1, 1)   /* SPI2 CS (unused) */
#define ARDUINO_1_PIN               NRF_GPIO_PIN_MAP(1, 0)   /* (unused) */
#define ARDUINO_0_PIN               NRF_GPIO_PIN_MAP(0, 31)  /* (unused) */

#define ARDUINO_A0_PIN              3
#define ARDUINO_A1_PIN              4
#define ARDUINO_A2_PIN              28
#define ARDUINO_A3_PIN              29
#define ARDUINO_A4_PIN              30
#define ARDUINO_A5_PIN              31

#ifdef __cplusplus
}
#endif

#endif /* CUSTOM_BOARD_H */
