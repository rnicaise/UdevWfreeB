#include "radio_quality.h"

#include <deca_device_api.h>

#define RADIO_QUALITY_DEFAULT_POWER_DBM (-120.0f)
#define RADIO_QUALITY_WEIGHT_RX         (0.35f)
#define RADIO_QUALITY_WEIGHT_FP         (0.35f)
#define RADIO_QUALITY_WEIGHT_GAP        (0.20f)
#define RADIO_QUALITY_WEIGHT_CLOCK      (0.10f)

static float clamp_float(float value, float min_value, float max_value)
{
    if (value < min_value)
    {
        return min_value;
    }
    if (value > max_value)
    {
        return max_value;
    }
    return value;
}

static float score_high_is_good(float value, float bad_value, float good_value)
{
    if (value <= bad_value)
    {
        return 1.0f;
    }
    if (value >= good_value)
    {
        return 10.0f;
    }
    return 1.0f + ((value - bad_value) * 9.0f / (good_value - bad_value));
}

static float score_low_is_good(float value, float good_value, float bad_value)
{
    if (value <= good_value)
    {
        return 10.0f;
    }
    if (value >= bad_value)
    {
        return 1.0f;
    }
    return 10.0f - ((value - good_value) * 9.0f / (bad_value - good_value));
}

static float abs_float(float value)
{
    return (value < 0.0f) ? -value : value;
}

static uint8_t calculate_score_10(float rx_power_dbm, float fp_power_dbm, float clock_offset_ppm, bool valid)
{
    float rx_score;
    float fp_score;
    float gap_score;
    float clock_score;
    float weighted;
    int score;

    if (!valid)
    {
        return 1u;
    }

    /* Close body/bike placement tests often report very strong RX values
     * (-30..0 dBm). Use a tighter scale so orientation changes do not all
     * saturate at 10/10. RSSI says "energy", first-path says "direct path",
     * and the RX/FP gap penalizes multipath/NLOS. */
    rx_score = score_high_is_good(rx_power_dbm, -35.0f, -5.0f);
    fp_score = score_high_is_good(fp_power_dbm, -45.0f, -10.0f);
    gap_score = score_low_is_good(rx_power_dbm - fp_power_dbm, 6.0f, 25.0f);
    clock_score = score_low_is_good(abs_float(clock_offset_ppm), 5.0f, 40.0f);

    weighted = (RADIO_QUALITY_WEIGHT_RX * rx_score) +
               (RADIO_QUALITY_WEIGHT_FP * fp_score) +
               (RADIO_QUALITY_WEIGHT_GAP * gap_score) +
               (RADIO_QUALITY_WEIGHT_CLOCK * clock_score);
    score = (int)(weighted + 0.5f);
    return (uint8_t)clamp_float((float)score, 1.0f, 10.0f);
}

static uint8_t calculate_nlos_score_10(float peak_to_fp_samples, uint8_t fp_conf_level, bool valid)
{
    float peak_gap_score;
    float fp_conf_score;
    float weighted;
    int score;

    if (!valid)
    {
        return 1u;
    }

    /* LOS/direct-path proxy from DW3000 CIR diagnostics:
     * - small peak-first-path gap is better;
     * - high early-first-path confidence is better.
     * This is more useful than RSSI near a metallic bike frame. */
    peak_gap_score = score_low_is_good(peak_to_fp_samples, 2.0f, 8.0f);
    fp_conf_score = score_high_is_good((float)fp_conf_level, 4.0f, 12.0f);
    weighted = (0.65f * peak_gap_score) + (0.35f * fp_conf_score);
    score = (int)(weighted + 0.5f);
    return (uint8_t)clamp_float((float)score, 1.0f, 10.0f);
}

void radio_quality_enable_diagnostics(void)
{
    /* Enables CIA data required by dwt_readdiagnostics_acc(). ALL is needed for
     * first-path/peak/NLOS fields such as EFpConfLevel. */
    dwt_configciadiag((uint8_t)DW_CIA_DIAG_LOG_ALL);
}

void radio_quality_read(radio_quality_t *quality)
{
    dwt_cirdiags_t diag;
    int16_t rssi_q8_8 = 0;
    int16_t fp_q8_8 = 0;
    int16_t sts_quality = 0;
    bool rx_ok = false;
    bool fp_ok = false;
    bool nlos_ok = false;

    if (quality == NULL)
    {
        return;
    }

    quality->rx_power_dbm = RADIO_QUALITY_DEFAULT_POWER_DBM;
    quality->fp_power_dbm = RADIO_QUALITY_DEFAULT_POWER_DBM;
    quality->clock_offset_ppm = (float)dwt_readclockoffset() * (float)CLOCK_OFFSET_PPM_TO_RATIO * 1000000.0f;
    quality->peak_to_fp_samples = 0.0f;
    quality->fp_conf_level = 0u;
    quality->sts_quality = 0;
    quality->score_10 = 1u;
    quality->nlos_score_10 = 1u;
    quality->valid = false;
    quality->nlos_valid = false;

    if (dwt_readdiagnostics_acc(&diag, DWT_ACC_IDX_IP_M) == DWT_SUCCESS)
    {
        if (dwt_calculate_rssi(&diag, DWT_ACC_IDX_IP_M, &rssi_q8_8) == DWT_SUCCESS)
        {
            quality->rx_power_dbm = (float)rssi_q8_8 / 256.0f;
            rx_ok = true;
        }
        if (dwt_calculate_first_path_power(&diag, DWT_ACC_IDX_IP_M, &fp_q8_8) == DWT_SUCCESS)
        {
            quality->fp_power_dbm = (float)fp_q8_8 / 256.0f;
            fp_ok = true;
        }

        quality->peak_to_fp_samples = (float)diag.peakIndex - ((float)diag.FpIndex / 64.0f);
        quality->fp_conf_level = diag.EFpConfLevel;
        nlos_ok = true;
    }

    if (dwt_readstsquality(&sts_quality, 0) >= 0)
    {
        quality->sts_quality = sts_quality;
    }

    quality->valid = rx_ok && fp_ok;
    quality->nlos_valid = nlos_ok;
    quality->score_10 = calculate_score_10(
        quality->rx_power_dbm,
        quality->fp_power_dbm,
        quality->clock_offset_ppm,
        quality->valid);
    quality->nlos_score_10 = calculate_nlos_score_10(
        quality->peak_to_fp_samples,
        quality->fp_conf_level,
        quality->nlos_valid);
}