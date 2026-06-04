#include "nrfx_power.h"

#include <stdbool.h>

static bool m_initialized;

#if NRF_POWER_HAS_POFCON || defined(__NRFX_DOXYGEN__)
static nrfx_power_pofwarn_event_handler_t m_pof_handler;
#endif

#if NRF_POWER_HAS_SLEEPEVT || defined(__NRFX_DOXYGEN__)
static nrfx_power_sleep_event_handler_t m_sleep_handler;
#endif

#if NRF_POWER_HAS_USBREG || defined(__NRFX_DOXYGEN__)
static nrfx_power_usb_event_handler_t m_usb_handler;

static void nrfx_power_usb_event_dispatch(nrfx_power_usb_evt_t event)
{
    if (m_usb_handler != NULL)
    {
        m_usb_handler(event);
    }
}
#endif

nrfx_power_pofwarn_event_handler_t nrfx_power_pof_handler_get(void)
{
#if NRF_POWER_HAS_POFCON || defined(__NRFX_DOXYGEN__)
    return m_pof_handler;
#else
    return NULL;
#endif
}

#if NRF_POWER_HAS_USBREG || defined(__NRFX_DOXYGEN__)
nrfx_power_usb_event_handler_t nrfx_power_usb_handler_get(void)
{
    return m_usb_handler;
}
#endif

nrfx_err_t nrfx_power_init(nrfx_power_config_t const * p_config)
{
    if (m_initialized)
    {
        return NRFX_ERROR_ALREADY_INITIALIZED;
    }

    m_initialized = true;

    if (p_config != NULL)
    {
        NRF_POWER->DCDCEN = p_config->dcdcen ? 1u : 0u;
    }

    return NRFX_SUCCESS;
}

void nrfx_power_uninit(void)
{
    nrf_power_int_disable(NRF_POWER_INT_USBDETECTED_MASK |
                          NRF_POWER_INT_USBREMOVED_MASK |
                          NRF_POWER_INT_USBPWRRDY_MASK);
    NVIC_DisableIRQ(POWER_CLOCK_IRQn);
    m_initialized = false;
}

#if NRF_POWER_HAS_POFCON || defined(__NRFX_DOXYGEN__)
void nrfx_power_pof_init(nrfx_power_pofwarn_config_t const * p_config)
{
    m_pof_handler = (p_config != NULL) ? p_config->handler : NULL;
}

void nrfx_power_pof_enable(nrfx_power_pofwarn_config_t const * p_config)
{
    m_pof_handler = (p_config != NULL) ? p_config->handler : NULL;
}

void nrfx_power_pof_disable(void)
{
}

void nrfx_power_pof_uninit(void)
{
    m_pof_handler = NULL;
}
#endif

#if NRF_POWER_HAS_SLEEPEVT || defined(__NRFX_DOXYGEN__)
void nrfx_power_sleepevt_init(nrfx_power_sleepevt_config_t const * p_config)
{
    m_sleep_handler = (p_config != NULL) ? p_config->handler : NULL;
}

void nrfx_power_sleepevt_enable(nrfx_power_sleepevt_config_t const * p_config)
{
    m_sleep_handler = (p_config != NULL) ? p_config->handler : NULL;
}

void nrfx_power_sleepevt_disable(void)
{
}

void nrfx_power_sleepevt_uninit(void)
{
    m_sleep_handler = NULL;
}
#endif

#if NRF_POWER_HAS_USBREG || defined(__NRFX_DOXYGEN__)
void nrfx_power_usbevt_init(nrfx_power_usbevt_config_t const * p_config)
{
    m_usb_handler = (p_config != NULL) ? p_config->handler : NULL;
    NRF_POWER->EVENTS_USBDETECTED = 0;
    NRF_POWER->EVENTS_USBREMOVED = 0;
    NRF_POWER->EVENTS_USBPWRRDY = 0;
}

void nrfx_power_usbevt_enable(void)
{
    nrf_power_int_enable(NRF_POWER_INT_USBDETECTED_MASK |
                         NRF_POWER_INT_USBREMOVED_MASK |
                         NRF_POWER_INT_USBPWRRDY_MASK);
    NVIC_ClearPendingIRQ(POWER_CLOCK_IRQn);
    NVIC_EnableIRQ(POWER_CLOCK_IRQn);

    switch (nrfx_power_usbstatus_get())
    {
        case NRFX_POWER_USB_STATE_CONNECTED:
            nrfx_power_usb_event_dispatch(NRFX_POWER_USB_EVT_DETECTED);
            break;

        case NRFX_POWER_USB_STATE_READY:
            nrfx_power_usb_event_dispatch(NRFX_POWER_USB_EVT_DETECTED);
            nrfx_power_usb_event_dispatch(NRFX_POWER_USB_EVT_READY);
            break;

        default:
            break;
    }
}

void nrfx_power_usbevt_disable(void)
{
    nrf_power_int_disable(NRF_POWER_INT_USBDETECTED_MASK |
                          NRF_POWER_INT_USBREMOVED_MASK |
                          NRF_POWER_INT_USBPWRRDY_MASK);
}

void nrfx_power_usbevt_uninit(void)
{
    m_usb_handler = NULL;
    NRF_POWER->EVENTS_USBDETECTED = 0;
    NRF_POWER->EVENTS_USBREMOVED = 0;
    NRF_POWER->EVENTS_USBPWRRDY = 0;
}
#endif

void nrfx_power_irq_handler(void)
{
#if NRF_POWER_HAS_POFCON || defined(__NRFX_DOXYGEN__)
    if ((NRF_POWER->EVENTS_POFWARN != 0u) && (m_pof_handler != NULL))
    {
        NRF_POWER->EVENTS_POFWARN = 0;
        m_pof_handler();
    }
#endif

#if NRF_POWER_HAS_USBREG || defined(__NRFX_DOXYGEN__)
    if ((NRF_POWER->EVENTS_USBDETECTED != 0u) && (m_usb_handler != NULL))
    {
        NRF_POWER->EVENTS_USBDETECTED = 0;
        m_usb_handler(NRFX_POWER_USB_EVT_DETECTED);
    }

    if ((NRF_POWER->EVENTS_USBPWRRDY != 0u) && (m_usb_handler != NULL))
    {
        NRF_POWER->EVENTS_USBPWRRDY = 0;
        m_usb_handler(NRFX_POWER_USB_EVT_READY);
    }

    if ((NRF_POWER->EVENTS_USBREMOVED != 0u) && (m_usb_handler != NULL))
    {
        NRF_POWER->EVENTS_USBREMOVED = 0;
        m_usb_handler(NRFX_POWER_USB_EVT_REMOVED);
    }
#endif

#if NRF_POWER_HAS_SLEEPEVT || defined(__NRFX_DOXYGEN__)
    (void)m_sleep_handler;
#endif
}

void POWER_CLOCK_IRQHandler(void)
{
    nrfx_power_irq_handler();
}