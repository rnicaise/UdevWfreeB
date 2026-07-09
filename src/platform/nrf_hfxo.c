#include "nrf_hfxo.h"

#include <nrf_clock.h>

bool nrf_hfxo_is_running(void)
{
    return nrf_clock_hf_is_running(NRF_CLOCK_HFCLK_HIGH_ACCURACY);
}

void nrf_hfxo_start_blocking(void)
{
    if (nrf_hfxo_is_running())
    {
        return;
    }

    nrf_clock_event_clear(NRF_CLOCK_EVENT_HFCLKSTARTED);
    nrf_clock_task_trigger(NRF_CLOCK_TASK_HFCLKSTART);
    while (!nrf_hfxo_is_running())
    {
    }
}