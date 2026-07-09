#ifndef NRF_HFXO_H
#define NRF_HFXO_H

#include <stdbool.h>

void nrf_hfxo_start_blocking(void);
bool nrf_hfxo_is_running(void);

#endif