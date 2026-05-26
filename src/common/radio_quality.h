#ifndef RADIO_QUALITY_H
#define RADIO_QUALITY_H

#include <stdbool.h>
#include <stdint.h>

typedef struct
{
    float rx_power_dbm;
    float fp_power_dbm;
    float clock_offset_ppm;
    float peak_to_fp_samples;
    uint8_t fp_conf_level;
    int16_t sts_quality;
    uint8_t score_10;
    uint8_t nlos_score_10;
    bool valid;
    bool nlos_valid;
} radio_quality_t;

void radio_quality_enable_diagnostics(void);
void radio_quality_read(radio_quality_t *quality);

#endif /* RADIO_QUALITY_H */