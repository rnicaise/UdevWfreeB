#ifndef PYRO_BUZZER_H
#define PYRO_BUZZER_H

#include <stdint.h>
#include <nrf_gpio.h>

#ifndef PYRO_BUZZER_PIN
#define PYRO_BUZZER_PIN NRF_GPIO_PIN_MAP(1, 5)
#endif

void pyro_buzzer_beep_three_times(void);

/* Short video-game style jingle: rising C-major arpeggio C6-E6-G6-C7
 * (~300 ms, blocking). Played on the vest module when a rule triggers. */
void pyro_buzzer_trigger_jingle(void);

#endif /* PYRO_BUZZER_H */