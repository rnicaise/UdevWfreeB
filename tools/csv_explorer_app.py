#!/usr/bin/env python3
"""
Interactive CSV explorer for UWB exported sessions.

Features:
- Opens old and new CSV formats from the Android app.
- Displays distance timeline and rolling stats.
- Overlays distance over initiator/responder acceleration and phone gyro.
- Shows GPS track colored by distance when location columns are present.

Run:
    streamlit run tools/csv_explorer_app.py
"""

from __future__ import annotations

from dataclasses import dataclass
from glob import glob
from pathlib import Path

import numpy as np
import pandas as pd
import plotly.graph_objects as go
import streamlit as st

RX_QUALITY_COL = "rx_quality_10"
PATH_QUALITY_COL = "path_quality_10"
MULTIPATH_QUALITY_COL = "multipath_quality_10"
CLOCK_QUALITY_COL = "clock_quality_10"
STABILITY_QUALITY_COL = "stability_quality_10"
SMOOTHNESS_QUALITY_COL = "smoothness_quality_10"
TIMING_QUALITY_COL = "timing_quality_10"
DROPOUT_QUALITY_COL = "dropout_quality_10"
LINK_RELIABILITY_COL = "link_reliability_10"
NLOS_QUALITY_COL = "nlos_quality_10"


@dataclass
class Columns:
    ms: str | None
    sample: str | None
    dist_raw: str | None
    dist_plot: str | None
    iax: str | None
    iay: str | None
    iaz: str | None
    rax: str | None
    ray: str | None
    raz: str | None
    gx: str | None
    gy: str | None
    gz: str | None
    speed: str | None
    lat: str | None
    lon: str | None
    alt: str | None
    rx_power: str | None
    fp_power: str | None
    clock_offset: str | None
    signal_quality: str | None
    rx_quality: str | None
    path_quality: str | None
    multipath_quality: str | None
    clock_quality: str | None
    stability_quality: str | None
    smoothness_quality: str | None
    timing_quality: str | None
    dropout_quality: str | None
    link_reliability: str | None
    nlos_quality: str | None
    rolling_std_5s: str | None
    relative_speed: str | None
    instant_hz: str | None
    timing_anomaly_rate: str | None
    valid_rate_5s: str | None
    jump_rate_5s: str | None
    bad_burst_max: str | None
    peak_to_fp: str | None
    fp_conf: str | None
    sts_quality: str | None
    bike_box_position: str | None
    vest_box_position: str | None
    app_preset: str | None
    app_test_profile: str | None
    app_ranging_mode: str | None
    app_rf_channel: str | None
    app_uwb_data_rate_kbps: str | None
    app_acq_period_ms: str | None
    app_median_window: str | None
    connected_role: str | None


@dataclass
class PreparedSession:
    source_name: str
    df: pd.DataFrame
    cols: Columns
    x: pd.Series
    work: pd.DataFrame
    dist_source_col: str
    dist_plot_col: str
    outlier_mask: pd.Series
    dist_vals: pd.Series
    hz_series: pd.Series
    signal_categories: pd.DataFrame
    transmission_categories: pd.DataFrame
    rx_vals: pd.Series | None
    fp_vals: pd.Series | None


def _pick(df: pd.DataFrame, candidates: list[str]) -> str | None:
    for c in candidates:
        if c in df.columns:
            return c
    return None


def detect_columns(df: pd.DataFrame) -> Columns:
    return Columns(
        ms=_pick(df, ["ms", "timestamp_ms", "time_ms"]),
        sample=_pick(df, ["sample", "count", "idx"]),
        dist_raw=_pick(df, ["dist_raw", "dist", "distance", "distance_m"]),
        dist_plot=_pick(df, ["dist_filt", "dist", "dist_raw", "distance", "distance_m"]),
        iax=_pick(df, ["iax", "init_ax", "accel_i_x"]),
        iay=_pick(df, ["iay", "init_ay", "accel_i_y"]),
        iaz=_pick(df, ["iaz", "init_az", "accel_i_z"]),
        rax=_pick(df, ["rax", "resp_ax", "accel_r_x"]),
        ray=_pick(df, ["ray", "resp_ay", "accel_r_y"]),
        raz=_pick(df, ["raz", "resp_az", "accel_r_z"]),
        gx=_pick(df, ["phone_gx", "gyro_x", "gx"]),
        gy=_pick(df, ["phone_gy", "gyro_y", "gy"]),
        gz=_pick(df, ["phone_gz", "gyro_z", "gz"]),
        speed=_pick(df, ["phone_speed_mps", "gps_speed_mps", "speed_mps", "speed"]),
        lat=_pick(df, ["phone_lat", "lat", "latitude"]),
        lon=_pick(df, ["phone_lon", "lon", "longitude"]),
        alt=_pick(df, ["phone_alt_m", "alt", "altitude_m"]),
        rx_power=_pick(df, ["rx_power_dbm", "rssi_dbm", "rx_rssi_dbm", "uwb_rx_power_dbm"]),
        fp_power=_pick(df, ["fp_power_dbm", "first_path_power_dbm", "first_path_dbm", "uwb_fp_power_dbm"]),
        clock_offset=_pick(df, ["clock_offset_ppm", "clock_ppm", "carrier_offset_ppm", "uwb_clock_offset_ppm"]),
        signal_quality=_pick(df, ["signal_quality_10", "quality_10", "uwb_quality_10", "signal_score"]),
        rx_quality=_pick(df, [RX_QUALITY_COL, "rx_score_10"]),
        path_quality=_pick(df, [PATH_QUALITY_COL, "direct_path_quality_10", "path_score_10"]),
        multipath_quality=_pick(df, [MULTIPATH_QUALITY_COL, "multipath_score_10"]),
        clock_quality=_pick(df, [CLOCK_QUALITY_COL, "clock_score_10"]),
        stability_quality=_pick(df, [STABILITY_QUALITY_COL, "stability_score_10"]),
        smoothness_quality=_pick(df, [SMOOTHNESS_QUALITY_COL, "smoothness_score_10"]),
        timing_quality=_pick(df, [TIMING_QUALITY_COL, "timing_score_10"]),
        dropout_quality=_pick(df, [DROPOUT_QUALITY_COL, "dropout_score_10"]),
        link_reliability=_pick(df, [LINK_RELIABILITY_COL, "link_quality_10", "reliability_quality_10"]),
        nlos_quality=_pick(df, [NLOS_QUALITY_COL, "nlos_score_10", "cir_quality_10"]),
        rolling_std_5s=_pick(df, ["rolling_std_5s_m", "std5s_m", "distance_std_5s_m"]),
        relative_speed=_pick(df, ["relative_speed_mps", "rel_speed_mps"]),
        instant_hz=_pick(df, ["instant_hz", "acq_instant_hz"]),
        timing_anomaly_rate=_pick(df, ["timing_anomaly_rate", "dropout_rate"]),
        valid_rate_5s=_pick(df, ["valid_rate_5s", "availability_rate_5s"]),
        jump_rate_5s=_pick(df, ["jump_rate_5s", "speed_spike_rate_5s"]),
        bad_burst_max=_pick(df, ["bad_burst_max", "burst_bad_max"]),
        peak_to_fp=_pick(df, ["peak_to_fp_samples", "peak_fp_gap_samples"]),
        fp_conf=_pick(df, ["fp_conf_level", "first_path_confidence"]),
        sts_quality=_pick(df, ["sts_quality", "sts_quality_index"]),
        bike_box_position=_pick(df, ["bike_box_position", "bike_position", "velo_position"]),
        vest_box_position=_pick(df, ["vest_box_position", "vest_position", "veste_position"]),
        app_preset=_pick(df, ["app_preset", "preset", "acquisition_preset"]),
        app_test_profile=_pick(df, ["app_test_profile", "test_profile", "uwb_test_profile"]),
        app_ranging_mode=_pick(df, ["app_ranging_mode", "ranging_mode"]),
        app_rf_channel=_pick(df, ["app_rf_channel", "rf_channel", "uwb_channel"]),
        app_uwb_data_rate_kbps=_pick(df, ["app_uwb_data_rate_kbps", "uwb_data_rate_kbps"]),
        app_acq_period_ms=_pick(df, ["app_acq_period_ms", "acquisition_period_ms", "acq_period_ms"]),
        app_median_window=_pick(df, ["app_median_window", "median_window"]),
        connected_role=_pick(df, ["connected_role", "role", "uwb_role"]),
    )


def score_high_is_good(series: pd.Series, bad_value: float, good_value: float) -> pd.Series:
    s = pd.to_numeric(series, errors="coerce").astype("float64")
    score = 1.0 + ((s - bad_value) * 9.0 / (good_value - bad_value))
    return score.clip(lower=1.0, upper=10.0)


def score_low_is_good(series: pd.Series, good_value: float, bad_value: float) -> pd.Series:
    s = pd.to_numeric(series, errors="coerce").astype("float64")
    score = 10.0 - ((s - good_value) * 9.0 / (bad_value - good_value))
    return score.clip(lower=1.0, upper=10.0)


def signal_category_scores(df: pd.DataFrame, cols: Columns) -> pd.DataFrame:
    out = pd.DataFrame(index=df.index)

    if cols.rx_quality and cols.rx_quality in df.columns:
        out["RX"] = pd.to_numeric(df[cols.rx_quality], errors="coerce")
    elif cols.rx_power and cols.rx_power in df.columns:
        out["RX"] = score_high_is_good(df[cols.rx_power], bad_value=-35.0, good_value=-5.0)

    if cols.path_quality and cols.path_quality in df.columns:
        out["Direct"] = pd.to_numeric(df[cols.path_quality], errors="coerce")
    elif cols.fp_power and cols.fp_power in df.columns:
        out["Direct"] = score_high_is_good(df[cols.fp_power], bad_value=-45.0, good_value=-10.0)

    if cols.multipath_quality and cols.multipath_quality in df.columns:
        out["Multipath"] = pd.to_numeric(df[cols.multipath_quality], errors="coerce")
    elif cols.rx_power and cols.fp_power and cols.rx_power in df.columns and cols.fp_power in df.columns:
        gap_db = pd.to_numeric(df[cols.rx_power], errors="coerce") - pd.to_numeric(df[cols.fp_power], errors="coerce")
        out["Multipath"] = score_low_is_good(gap_db, good_value=6.0, bad_value=25.0)

    if cols.clock_quality and cols.clock_quality in df.columns:
        out["Clock"] = pd.to_numeric(df[cols.clock_quality], errors="coerce")
    elif cols.clock_offset and cols.clock_offset in df.columns:
        out["Clock"] = score_low_is_good(pd.to_numeric(df[cols.clock_offset], errors="coerce").abs(), good_value=5.0, bad_value=40.0)

    return out


def score_timing_hz(series: pd.Series) -> pd.Series:
    hz = pd.to_numeric(series, errors="coerce").astype("float64")
    score = pd.Series(np.nan, index=hz.index, dtype="float64")
    score[(hz >= 40.0) & (hz <= 80.0)] = 10.0
    low = (hz > 10.0) & (hz < 40.0)
    score[low] = 1.0 + ((hz[low] - 10.0) * 9.0 / 30.0)
    high = (hz > 80.0) & (hz < 120.0)
    score[high] = 10.0 - ((hz[high] - 80.0) * 9.0 / 40.0)
    score[(hz <= 10.0) | (hz >= 120.0)] = 1.0
    return score.clip(lower=1.0, upper=10.0)


def rolling_samples_for_seconds(t_sec: pd.Series, seconds: float) -> int:
    t = pd.to_numeric(t_sec, errors="coerce").to_numpy(dtype=np.float64)
    diffs = np.diff(t)
    good = diffs[np.isfinite(diffs) & (diffs > 0.0) & (diffs < 1.0)]
    dt = float(np.nanmedian(good)) if len(good) else 0.02
    return max(3, int(round(seconds / max(dt, 1e-3))))


def rolling_bad_burst_score(bad_mask: pd.Series, window: int = 50) -> pd.Series:
    bad = bad_mask.fillna(False).astype(bool).to_numpy()
    scores = np.full(len(bad), np.nan, dtype=np.float64)
    for i in range(len(bad)):
        start = max(0, i - window + 1)
        current = 0
        max_burst = 0
        for value in bad[start : i + 1]:
            if value:
                current += 1
                max_burst = max(max_burst, current)
            else:
                current = 0
        scores[i] = max(1.0, 10.0 - min(max_burst, 10) * 0.9)
    return pd.Series(scores, index=bad_mask.index)


def transmission_category_scores(df: pd.DataFrame, x: pd.Series, dist_col: str, hz_series: pd.Series, cols: Columns) -> pd.DataFrame:
    out = pd.DataFrame(index=df.index)
    dist = pd.to_numeric(df[dist_col], errors="coerce")
    velocity = pd.to_numeric(df[cols.relative_speed], errors="coerce") if cols.relative_speed and cols.relative_speed in df.columns else compute_relative_velocity(x, dist, smoothing_window=1)

    if cols.stability_quality and cols.stability_quality in df.columns:
        out["Stability"] = pd.to_numeric(df[cols.stability_quality], errors="coerce")
    else:
        window = rolling_samples_for_seconds(x, 5.0)
        rolling_std = dist.rolling(window, min_periods=2).std(ddof=0)
        out["Stability"] = score_low_is_good(rolling_std, good_value=0.05, bad_value=0.50)

    if cols.smoothness_quality and cols.smoothness_quality in df.columns:
        out["Smoothness"] = pd.to_numeric(df[cols.smoothness_quality], errors="coerce")
    elif cols.relative_speed and cols.relative_speed in df.columns:
        out["Smoothness"] = score_low_is_good(pd.to_numeric(df[cols.relative_speed], errors="coerce").abs(), good_value=0.5, bad_value=8.0)
    else:
        out["Smoothness"] = score_low_is_good(velocity.abs(), good_value=0.5, bad_value=8.0)

    if cols.timing_quality and cols.timing_quality in df.columns:
        out["Timing"] = pd.to_numeric(df[cols.timing_quality], errors="coerce")
    elif cols.instant_hz and cols.instant_hz in df.columns:
        out["Timing"] = score_timing_hz(df[cols.instant_hz])
    else:
        out["Timing"] = score_timing_hz(hz_series)

    if cols.dropout_quality and cols.dropout_quality in df.columns:
        out["Dropout"] = pd.to_numeric(df[cols.dropout_quality], errors="coerce")
    elif cols.timing_anomaly_rate and cols.timing_anomaly_rate in df.columns:
        rate = pd.to_numeric(df[cols.timing_anomaly_rate], errors="coerce")
        out["Dropout"] = (10.0 - (rate * 9.0)).clip(lower=1.0, upper=10.0)
    else:
        hz = pd.to_numeric(hz_series, errors="coerce")
        anomaly = hz.notna() & (~hz.between(10.0, 100.0))
        anomaly_rate = anomaly.astype("float64").rolling(50, min_periods=1).mean()
        out["Dropout"] = (10.0 - (anomaly_rate * 9.0)).clip(lower=1.0, upper=10.0)

    if cols.jump_rate_5s and cols.jump_rate_5s in df.columns:
        jump_rate = pd.to_numeric(df[cols.jump_rate_5s], errors="coerce")
    else:
        jump_mask = (velocity.abs() > 10.0) | (dist.diff().abs() > 0.75)
        jump_rate = jump_mask.astype("float64").rolling(50, min_periods=1).mean()
    out["Jump"] = (10.0 - (jump_rate * 9.0)).clip(lower=1.0, upper=10.0)

    if cols.nlos_quality and cols.nlos_quality in df.columns:
        out["NLOS"] = pd.to_numeric(df[cols.nlos_quality], errors="coerce")
    elif cols.peak_to_fp and cols.fp_conf and cols.peak_to_fp in df.columns and cols.fp_conf in df.columns:
        peak_gap_score = score_low_is_good(df[cols.peak_to_fp], good_value=2.0, bad_value=8.0)
        fp_conf_score = score_high_is_good(df[cols.fp_conf], bad_value=4.0, good_value=12.0)
        out["NLOS"] = ((0.65 * peak_gap_score) + (0.35 * fp_conf_score)).clip(lower=1.0, upper=10.0)

    if cols.link_reliability and cols.link_reliability in df.columns:
        out.insert(0, "Link", pd.to_numeric(df[cols.link_reliability], errors="coerce"))
    else:
        if cols.valid_rate_5s and cols.valid_rate_5s in df.columns:
            valid_rate = pd.to_numeric(df[cols.valid_rate_5s], errors="coerce")
        else:
            good_hz = pd.to_numeric(hz_series, errors="coerce")
            expected_hz = float(good_hz.dropna().quantile(0.90)) if good_hz.notna().any() else 60.0
            expected_hz = min(max(expected_hz, 10.0), 80.0)
            valid_rate = (good_hz / expected_hz).clip(lower=0.0, upper=1.0)
        valid_score = (valid_rate * 10.0).clip(lower=1.0, upper=10.0)

        if cols.bad_burst_max and cols.bad_burst_max in df.columns:
            burst_score = score_low_is_good(pd.to_numeric(df[cols.bad_burst_max], errors="coerce"), good_value=0.0, bad_value=10.0)
        else:
            timing_bad = pd.to_numeric(hz_series, errors="coerce").notna() & (~pd.to_numeric(hz_series, errors="coerce").between(10.0, 100.0))
            burst_score = rolling_bad_burst_score(timing_bad | (jump_rate > 0.0), window=50)

        weighted_parts = [
            (out.get("Stability"), 0.25),
            (out.get("Smoothness"), 0.15),
            (valid_score, 0.15),
            (out.get("Jump"), 0.20),
            (burst_score, 0.10),
            (out.get("NLOS"), 0.15),
        ]
        weighted = pd.Series(0.0, index=df.index)
        weights = pd.Series(0.0, index=df.index)
        for series, weight in weighted_parts:
            if series is None:
                continue
            s = pd.to_numeric(series, errors="coerce")
            mask = s.notna()
            weighted.loc[mask] += s.loc[mask] * weight
            weights.loc[mask] += weight
        link = (weighted / weights.replace(0.0, np.nan)).clip(lower=1.0, upper=10.0)
        out.insert(0, "Link", link)

    return out


def gps_speed_from_track(lat: pd.Series, lon: pd.Series, t_sec: pd.Series) -> pd.Series:
    lat_v = np.radians(pd.to_numeric(lat, errors="coerce").to_numpy(dtype=np.float64))
    lon_v = np.radians(pd.to_numeric(lon, errors="coerce").to_numpy(dtype=np.float64))
    t_v = pd.to_numeric(t_sec, errors="coerce").to_numpy(dtype=np.float64)

    speed = np.full_like(t_v, np.nan, dtype=np.float64)
    earth_r_m = 6371000.0

    for i in range(1, len(t_v)):
        if not (np.isfinite(lat_v[i - 1]) and np.isfinite(lat_v[i]) and np.isfinite(lon_v[i - 1]) and np.isfinite(lon_v[i])):
            continue
        dt = t_v[i] - t_v[i - 1]
        if dt <= 0:
            continue

        dlat = lat_v[i] - lat_v[i - 1]
        dlon = lon_v[i] - lon_v[i - 1]
        a = np.sin(dlat / 2.0) ** 2 + np.cos(lat_v[i - 1]) * np.cos(lat_v[i]) * np.sin(dlon / 2.0) ** 2
        c = 2.0 * np.arctan2(np.sqrt(a), np.sqrt(1.0 - a))
        dist_m = earth_r_m * c
        speed[i] = dist_m / dt

    return pd.Series(speed, index=t_sec.index)


def make_monotonic_ms(series: pd.Series) -> pd.Series:
    # pandas>=3 removed fillna(method=...), use explicit ffill() for compatibility.
    vals = pd.to_numeric(series, errors="coerce").ffill().fillna(0).to_numpy(dtype=np.float64)
    out = np.zeros_like(vals, dtype=np.float64)

    if len(vals) == 0:
        return pd.Series(out, index=series.index)

    diffs = np.diff(vals)
    good_diffs = diffs[np.isfinite(diffs) & (diffs > 0) & (diffs < 1000.0)]
    typical_step_ms = float(np.nanmedian(good_diffs)) if len(good_diffs) else 20.0
    max_step_ms = max(1000.0, typical_step_ms * 20.0)

    out[0] = vals[0]
    prev_valid_raw = vals[0]
    for i in range(1, len(vals)):
        v = vals[i]
        delta = v - prev_valid_raw

        if not np.isfinite(v):
            out[i] = out[i - 1] + typical_step_ms
            continue

        if 0.0 <= delta <= max_step_ms:
            out[i] = out[i - 1] + delta
            prev_valid_raw = v
        elif delta < 0.0:
            # RTC reset/wrap: keep time monotonic with a normal sample interval.
            out[i] = out[i - 1] + typical_step_ms
            prev_valid_raw = v
        else:
            # Isolated corrupt timestamp spike: ignore it and keep previous raw baseline.
            out[i] = out[i - 1] + typical_step_ms

    return pd.Series(out, index=series.index)


def compute_acquisition_hz(t_sec: pd.Series, sample_series: pd.Series | None) -> pd.Series:
    t = pd.to_numeric(t_sec, errors="coerce").to_numpy(dtype=np.float64)
    if sample_series is not None:
        s = pd.to_numeric(sample_series, errors="coerce").to_numpy(dtype=np.float64)
    else:
        s = np.arange(len(t), dtype=np.float64)

    ds_all = np.diff(s)
    positive_ds = ds_all[np.isfinite(ds_all) & (ds_all > 0) & (ds_all < 1000)]
    typical_ds = float(np.nanmedian(positive_ds)) if len(positive_ds) else 1.0
    max_reasonable_ds = max(10.0, typical_ds * 10.0)

    hz = np.full_like(t, np.nan, dtype=np.float64)
    for i in range(1, len(t)):
        dt = t[i] - t[i - 1]
        ds = s[i] - s[i - 1]
        if not np.isfinite(dt) or dt <= 0:
            continue
        if not np.isfinite(ds) or ds <= 0 or ds > max_reasonable_ds:
            continue
        hz[i] = ds / dt

    return pd.Series(hz, index=t_sec.index)


def compute_relative_velocity(t_sec: pd.Series, distance_m: pd.Series, smoothing_window: int = 1) -> pd.Series:
    """Compute signed relative speed from distance changes, in m/s."""
    t = pd.to_numeric(t_sec, errors="coerce").to_numpy(dtype=np.float64)
    d = pd.to_numeric(distance_m, errors="coerce").to_numpy(dtype=np.float64)
    v = np.full_like(d, np.nan, dtype=np.float64)

    for i in range(1, len(d)):
        dt = t[i] - t[i - 1]
        if not (np.isfinite(dt) and dt > 0.0 and np.isfinite(d[i]) and np.isfinite(d[i - 1])):
            continue
        v[i] = (d[i] - d[i - 1]) / dt

    out = pd.Series(v, index=t_sec.index)
    window = max(1, int(smoothing_window))
    if window > 1:
        out = out.rolling(window, center=True, min_periods=1).median()
    return out


def clean_distance_spikes(
    series: pd.Series,
    *,
    max_abs_m: float,
    hampel_window: int,
    hampel_sigma: float,
    min_spike_dev_m: float,
) -> tuple[pd.Series, pd.Series]:
    """Return a display-safe distance series and a boolean spike mask.

    The raw recording is kept intact; this only creates a filtered copy for charts and stats.
    It catches impossible absolute values and isolated local spikes using a Hampel-style
    rolling median/MAD detector.
    """
    s = pd.to_numeric(series, errors="coerce").astype("float64")
    outliers = pd.Series(False, index=s.index)

    if max_abs_m > 0:
        outliers |= s.abs() > max_abs_m

    window = max(3, int(hampel_window))
    if window % 2 == 0:
        window += 1

    if window > 1 and hampel_sigma > 0:
        min_periods = max(3, window // 3)
        local_median = s.rolling(window, center=True, min_periods=min_periods).median()
        abs_dev = (s - local_median).abs()
        local_mad = abs_dev.rolling(window, center=True, min_periods=min_periods).median()
        threshold = (hampel_sigma * 1.4826 * local_mad).clip(lower=min_spike_dev_m)
        outliers |= abs_dev > threshold

    clean = s.mask(outliers)
    if clean.notna().any():
        clean = clean.interpolate(limit_direction="both").ffill().bfill()
    else:
        clean = s
        outliers[:] = False

    return clean, outliers.fillna(False)


def load_csv(file_obj) -> pd.DataFrame:
    df = pd.read_csv(file_obj)
    df.columns = [c.strip() for c in df.columns]
    return df


def parse_local_csv_paths(path_txt: str) -> list[Path]:
    paths: list[Path] = []
    raw_tokens = path_txt.replace(";", "\n").replace(",", "\n").splitlines()
    for token in raw_tokens:
        token = token.strip()
        if not token:
            continue

        expanded = str(Path(token).expanduser())
        if any(ch in expanded for ch in "*?["):
            matches = [Path(p) for p in sorted(glob(expanded))]
        else:
            p = Path(expanded)
            if p.is_dir():
                matches = sorted(p.glob("*.csv"))
            else:
                matches = [p]

        for p in matches:
            if p not in paths:
                paths.append(p)
    return paths


def coerce_numeric_columns(df: pd.DataFrame) -> pd.DataFrame:
    work = df.copy()
    for c in work.columns:
        if pd.api.types.is_numeric_dtype(work[c]):
            continue
        converted = pd.to_numeric(work[c], errors="coerce")
        # Preserve purely textual columns while converting numeric-like data.
        if converted.notna().any():
            work[c] = converted
    return work


def prepare_session(
    df: pd.DataFrame,
    source_name: str,
    *,
    downsample: int,
    roll_win: int,
    filter_spikes: bool,
    max_abs_m: float,
    hampel_window: int,
    hampel_sigma: float,
    min_spike_dev_m: float,
) -> PreparedSession:
    if df.empty:
        raise ValueError("CSV is empty")

    cols = detect_columns(df)
    if cols.dist_plot is None:
        raise ValueError("No distance column found. Expected one of: dist_filt, dist, dist_raw, distance.")

    if cols.ms is not None:
        t_ms = make_monotonic_ms(df[cols.ms])
        x = t_ms / 1000.0
    else:
        x = pd.Series(np.arange(len(df), dtype=np.float64) / 50.0)

    work = coerce_numeric_columns(df)
    dist_source_col = cols.dist_plot
    dist_plot_col = dist_source_col
    outlier_mask = pd.Series(False, index=work.index)

    if filter_spikes:
        dist_plot_col = "dist_clean"
        work[dist_plot_col], outlier_mask = clean_distance_spikes(
            work[dist_source_col],
            max_abs_m=float(max_abs_m),
            hampel_window=int(hampel_window),
            hampel_sigma=float(hampel_sigma),
            min_spike_dev_m=float(min_spike_dev_m),
        )

    if downsample > 1:
        work = work.iloc[::downsample, :].reset_index(drop=True)
        x = x.iloc[::downsample].reset_index(drop=True)
        outlier_mask = outlier_mask.iloc[::downsample].reset_index(drop=True)

    if roll_win > 1 and dist_plot_col in work.columns:
        work[dist_plot_col] = pd.to_numeric(work[dist_plot_col], errors="coerce").rolling(roll_win, min_periods=1).mean()

    dist_vals = pd.to_numeric(work[dist_plot_col], errors="coerce")
    hz_series = compute_acquisition_hz(x, work[cols.sample] if cols.sample and cols.sample in work.columns else None)
    signal_categories = signal_category_scores(work, cols)
    transmission_categories = transmission_category_scores(work, x, dist_plot_col, hz_series, cols)
    rx_vals = pd.to_numeric(work[cols.rx_power], errors="coerce") if cols.rx_power and cols.rx_power in work.columns else None
    fp_vals = pd.to_numeric(work[cols.fp_power], errors="coerce") if cols.fp_power and cols.fp_power in work.columns else None

    return PreparedSession(
        source_name=source_name,
        df=df,
        cols=cols,
        x=x,
        work=work,
        dist_source_col=dist_source_col,
        dist_plot_col=dist_plot_col,
        outlier_mask=outlier_mask,
        dist_vals=dist_vals,
        hz_series=hz_series,
        signal_categories=signal_categories,
        transmission_categories=transmission_categories,
        rx_vals=rx_vals,
        fp_vals=fp_vals,
    )


def _mean_score(df: pd.DataFrame, col: str) -> float:
    if col not in df.columns:
        return np.nan
    vals = pd.to_numeric(df[col], errors="coerce")
    return float(vals.mean()) if vals.notna().any() else np.nan


def _weighted_score(parts: list[tuple[float, float]]) -> float:
    weighted = 0.0
    weights = 0.0
    for value, weight in parts:
        if np.isfinite(value):
            weighted += value * weight
            weights += weight
    return weighted / weights if weights > 0.0 else np.nan


def _score_scalar_low_is_good(value: float, good_value: float, bad_value: float) -> float:
    if not np.isfinite(value):
        return np.nan
    score = 10.0 - ((value - good_value) * 9.0 / (bad_value - good_value))
    return float(np.clip(score, 1.0, 10.0))


def _max_consecutive_true(mask: pd.Series) -> int:
    max_burst = 0
    current = 0
    for value in mask.fillna(False).astype(bool).to_numpy():
        if value:
            current += 1
            max_burst = max(max_burst, current)
        else:
            current = 0
    return int(max_burst)


def _max_true_burst_duration_s(mask: pd.Series, t_sec: pd.Series) -> float:
    bad = mask.fillna(False).astype(bool).to_numpy()
    t = pd.to_numeric(t_sec, errors="coerce").to_numpy(dtype=np.float64)
    max_duration = 0.0
    start_idx: int | None = None
    end_idx: int | None = None

    for i, value in enumerate(bad):
        if value:
            if start_idx is None:
                start_idx = i
            end_idx = i
            continue

        if start_idx is not None and end_idx is not None and np.isfinite(t[start_idx]) and np.isfinite(t[end_idx]):
            max_duration = max(max_duration, float(t[end_idx] - t[start_idx]))
        start_idx = None
        end_idx = None

    if start_idx is not None and end_idx is not None and np.isfinite(t[start_idx]) and np.isfinite(t[end_idx]):
        max_duration = max(max_duration, float(t[end_idx] - t[start_idx]))

    return max(0.0, max_duration)


def local_distance_mad(t_sec: pd.Series, distance_m: pd.Series, window_s: float) -> pd.Series:
    """Rolling local MAD of detrended distance, useful for short-term jitter."""
    dist = pd.to_numeric(distance_m, errors="coerce").astype("float64")
    window = rolling_samples_for_seconds(t_sec, max(0.2, float(window_s)))
    min_periods = max(3, window // 3)
    local_median = dist.rolling(window, center=True, min_periods=min_periods).median()
    abs_dev = (dist - local_median).abs()
    return abs_dev.rolling(window, center=True, min_periods=min_periods).median()


def compute_fast_instability(
    t_sec: pd.Series,
    distance_m: pd.Series,
    *,
    improbable_speed_mps: float,
    jump_distance_m: float,
    local_window_s: float,
) -> tuple[dict[str, float | int], pd.DataFrame]:
    """Compute dynamic KPIs that highlight fast-mode local instability.

    These are intentionally based on raw point-to-point behavior rather than global
    distance standard deviation, so they expose jitter, jumps, and bad bursts.
    """
    dist = pd.to_numeric(distance_m, errors="coerce").astype("float64")
    t = pd.to_numeric(t_sec, errors="coerce").astype("float64")
    dt = t.diff()
    abs_delta = dist.diff().abs()
    raw_velocity = compute_relative_velocity(t, dist, smoothing_window=1)
    abs_speed = raw_velocity.abs()
    valid_transition = dt.gt(0.0) & dt.notna() & dist.notna() & dist.shift(1).notna()

    speed_spike_mask = (abs_speed > float(improbable_speed_mps)) & valid_transition if improbable_speed_mps > 0 else pd.Series(False, index=dist.index)
    distance_jump_mask = (abs_delta > float(jump_distance_m)) & valid_transition if jump_distance_m > 0 else pd.Series(False, index=dist.index)
    bad_mask = (speed_spike_mask | distance_jump_mask).fillna(False)
    valid_count = int(valid_transition.sum())

    local_mad = local_distance_mad(t, dist, local_window_s)
    dt_ms = dt * 1000.0
    dt_median_ms = float(dt_ms[dt_ms.gt(0.0)].median()) if dt_ms.gt(0.0).any() else np.nan
    inter_sample_jitter_ms = (dt_ms - dt_median_ms).abs() if np.isfinite(dt_median_ms) else pd.Series(np.nan, index=dist.index)

    jump_rate_pct = float(distance_jump_mask.sum() * 100.0 / valid_count) if valid_count else np.nan
    impossible_speed_rate_pct = float(speed_spike_mask.sum() * 100.0 / valid_count) if valid_count else np.nan
    bad_rate_pct = float(bad_mask.sum() * 100.0 / valid_count) if valid_count else np.nan
    max_bad_burst_samples = _max_consecutive_true(bad_mask)
    max_bad_burst_s = _max_true_burst_duration_s(bad_mask, t)

    delta_p95 = float(abs_delta[valid_transition].quantile(0.95)) if abs_delta[valid_transition].notna().any() else np.nan
    delta_p99 = float(abs_delta[valid_transition].quantile(0.99)) if abs_delta[valid_transition].notna().any() else np.nan
    local_mad_p95 = float(local_mad.quantile(0.95)) if local_mad.notna().any() else np.nan
    speed_p95 = float(abs_speed[valid_transition].quantile(0.95)) if abs_speed[valid_transition].notna().any() else np.nan
    speed_p99 = float(abs_speed[valid_transition].quantile(0.99)) if abs_speed[valid_transition].notna().any() else np.nan
    acq_gap_p95_ms = float(dt_ms[dt_ms.gt(0.0)].quantile(0.95)) if dt_ms.gt(0.0).any() else np.nan
    inter_sample_jitter_p95_ms = float(inter_sample_jitter_ms.quantile(0.95)) if inter_sample_jitter_ms.notna().any() else np.nan

    stability_score_10 = _weighted_score(
        [
            (_score_scalar_low_is_good(jump_rate_pct, good_value=0.0, bad_value=15.0), 0.20),
            (_score_scalar_low_is_good(impossible_speed_rate_pct, good_value=0.0, bad_value=10.0), 0.25),
            (_score_scalar_low_is_good(delta_p95, good_value=0.05, bad_value=0.75), 0.20),
            (_score_scalar_low_is_good(local_mad_p95, good_value=0.03, bad_value=0.30), 0.20),
            (_score_scalar_low_is_good(float(max_bad_burst_samples), good_value=0.0, bad_value=10.0), 0.15),
        ]
    )
    instability_index = float(((10.0 - stability_score_10) / 9.0) * 100.0) if np.isfinite(stability_score_10) else np.nan

    metrics: dict[str, float | int] = {
        "fast_stability_10": stability_score_10,
        "fast_instability_index": instability_index,
        "jump_rate_pct": jump_rate_pct,
        "impossible_speed_rate_pct": impossible_speed_rate_pct,
        "bad_dynamic_rate_pct": bad_rate_pct,
        "delta_dist_p95_m": delta_p95,
        "delta_dist_p99_m": delta_p99,
        "rolling_mad_p95_m": local_mad_p95,
        "rel_speed_p95_mps": speed_p95,
        "rel_speed_p99_mps": speed_p99,
        "max_bad_burst_samples": max_bad_burst_samples,
        "max_bad_burst_s": max_bad_burst_s,
        "acq_gap_p95_ms": acq_gap_p95_ms,
        "inter_sample_jitter_p95_ms": inter_sample_jitter_p95_ms,
    }
    details = pd.DataFrame(
        {
            "abs_delta_m": abs_delta,
            "abs_speed_mps": abs_speed,
            "rolling_mad_m": local_mad,
            "speed_spike": speed_spike_mask,
            "distance_jump": distance_jump_mask,
            "bad_dynamic_point": bad_mask,
            "inter_sample_jitter_ms": inter_sample_jitter_ms,
        },
        index=dist.index,
    )
    return metrics, details


def _first_value(df: pd.DataFrame, col: str | None) -> object:
    if col is None or col not in df.columns:
        return ""
    values = df[col].dropna()
    return values.iloc[0] if not values.empty else ""


def _fmt_number(value: object, fmt: str) -> str:
    try:
        numeric = float(value)
    except (TypeError, ValueError):
        return "--"
    return fmt.format(numeric) if np.isfinite(numeric) else "--"


def _session_label(session: PreparedSession) -> str:
    parts = [session.source_name]
    preset = _first_value(session.work, session.cols.app_preset)
    channel = _first_value(session.work, session.cols.app_rf_channel)
    bike = _first_value(session.work, session.cols.bike_box_position)
    vest = _first_value(session.work, session.cols.vest_box_position)

    if str(preset) not in ("", "nan"):
        parts.append(str(preset))
    if str(channel) not in ("", "nan"):
        parts.append(f"ch{channel}")
    if str(bike) not in ("", "nan") or str(vest) not in ("", "nan"):
        parts.append(f"bike={bike}, vest={vest}")

    return " · ".join(parts)


def _relative_x(session: PreparedSession) -> pd.Series:
    if session.x.empty:
        return session.x
    x = pd.to_numeric(session.x, errors="coerce")
    first = x.dropna().iloc[0] if x.notna().any() else 0.0
    return x - first


def summarize_for_comparison(
    session: PreparedSession,
    improbable_speed_mps: float,
    jump_distance_m: float,
    local_instability_window_s: float,
) -> dict[str, float | int | str]:
    raw_velocity = compute_relative_velocity(session.x, session.work[session.dist_source_col], smoothing_window=1)
    abs_raw_velocity = raw_velocity.abs()
    raw_speed_spikes = abs_raw_velocity > float(improbable_speed_mps) if improbable_speed_mps > 0 else pd.Series(False, index=session.work.index)
    duration = float(session.x.iloc[-1] - session.x.iloc[0]) if len(session.x) > 1 else 0.0
    spike_rate = float(raw_speed_spikes.mean()) if len(raw_speed_spikes) else np.nan
    fast_metrics, _ = compute_fast_instability(
        session.x,
        session.work[session.dist_source_col],
        improbable_speed_mps=improbable_speed_mps,
        jump_distance_m=jump_distance_m,
        local_window_s=local_instability_window_s,
    )

    clock_spike_corr = np.nan
    if session.cols.clock_offset and session.cols.clock_offset in session.work.columns:
        clock_delta = pd.to_numeric(session.work[session.cols.clock_offset], errors="coerce").diff().abs().fillna(0.0)
        spike_numeric = raw_speed_spikes.astype("float64")
        if clock_delta.std(ddof=0) > 0.0 and spike_numeric.std(ddof=0) > 0.0:
            clock_spike_corr = float(np.corrcoef(clock_delta.to_numpy(dtype=np.float64), spike_numeric.to_numpy(dtype=np.float64))[0, 1])

    link_avg = _mean_score(session.transmission_categories, "Link")
    stability_avg = _mean_score(session.transmission_categories, "Stability")
    jump_avg = _mean_score(session.transmission_categories, "Jump")
    nlos_avg = _mean_score(session.transmission_categories, "NLOS")
    timing_avg = _mean_score(session.transmission_categories, "Timing")
    dropout_avg = _mean_score(session.transmission_categories, "Dropout")
    comparison_score = _weighted_score(
        [
            (link_avg, 0.35),
            (stability_avg, 0.20),
            (jump_avg, 0.20),
            (nlos_avg, 0.15),
            (timing_avg, 0.10),
        ]
    )

    return {
        "session": session.source_name,
        "bike_box_position": _first_value(session.work, session.cols.bike_box_position),
        "vest_box_position": _first_value(session.work, session.cols.vest_box_position),
        "app_preset": _first_value(session.work, session.cols.app_preset),
        "app_test_profile": _first_value(session.work, session.cols.app_test_profile),
        "app_rf_channel": _first_value(session.work, session.cols.app_rf_channel),
        "app_acq_period_ms": _first_value(session.work, session.cols.app_acq_period_ms),
        "app_median_window": _first_value(session.work, session.cols.app_median_window),
        "app_ranging_mode": _first_value(session.work, session.cols.app_ranging_mode),
        "connected_role": _first_value(session.work, session.cols.connected_role),
        "score_10": comparison_score,
        "link_avg": link_avg,
        "stability_avg": stability_avg,
        "jump_avg": jump_avg,
        "nlos_avg": nlos_avg,
        "timing_avg": timing_avg,
        "dropout_avg": dropout_avg,
        "fast_stability_10": fast_metrics["fast_stability_10"],
        "fast_instability_index": fast_metrics["fast_instability_index"],
        "distance_std_m": float(session.dist_vals.std(ddof=0)) if session.dist_vals.notna().any() else np.nan,
        "raw_speed_spikes": int(raw_speed_spikes.sum()),
        "raw_speed_spike_rate_pct": spike_rate * 100.0 if np.isfinite(spike_rate) else np.nan,
        "jump_rate_pct": fast_metrics["jump_rate_pct"],
        "impossible_speed_rate_pct": fast_metrics["impossible_speed_rate_pct"],
        "bad_dynamic_rate_pct": fast_metrics["bad_dynamic_rate_pct"],
        "delta_dist_p95_m": fast_metrics["delta_dist_p95_m"],
        "delta_dist_p99_m": fast_metrics["delta_dist_p99_m"],
        "rolling_mad_p95_m": fast_metrics["rolling_mad_p95_m"],
        "rel_speed_p99_mps": float(abs_raw_velocity.quantile(0.99)) if abs_raw_velocity.notna().any() else np.nan,
        "max_bad_burst_samples": fast_metrics["max_bad_burst_samples"],
        "max_bad_burst_s": fast_metrics["max_bad_burst_s"],
        "acq_gap_p95_ms": fast_metrics["acq_gap_p95_ms"],
        "inter_sample_jitter_p95_ms": fast_metrics["inter_sample_jitter_p95_ms"],
        "acq_hz_mean": float(session.hz_series.mean()) if session.hz_series.notna().any() else np.nan,
        "duration_s": duration,
        "samples": len(session.work),
        "filtered_spikes": int(session.outlier_mask.sum()),
        "clock_spike_corr": clock_spike_corr,
    }


def comparison_dataframe(
    sessions: list[PreparedSession],
    improbable_speed_mps: float,
    jump_distance_m: float,
    local_instability_window_s: float,
) -> pd.DataFrame:
    rows = [summarize_for_comparison(session, improbable_speed_mps, jump_distance_m, local_instability_window_s) for session in sessions]
    return pd.DataFrame(rows).sort_values("score_10", ascending=False, na_position="last").reset_index(drop=True)


def plot_multi_session_overlays(
    sessions: list[PreparedSession],
    *,
    distance_delta_from_start: bool,
    velocity_roll_win: int,
    velocity_axis_limit: float,
    improbable_speed_mps: float,
    jump_distance_m: float,
    local_instability_window_s: float,
) -> None:
    st.subheader("Visual Overlay")
    st.caption("Toutes les sessions sont recalées à t=0 pour comparer visuellement les courbes sur le même graphique.")

    distance_fig = go.Figure()
    for session in sessions:
        x_rel = _relative_x(session)
        y = pd.to_numeric(session.work[session.dist_plot_col], errors="coerce")
        if distance_delta_from_start and y.notna().any():
            y = y - float(y.dropna().iloc[0])
        distance_fig.add_trace(
            go.Scatter(
                x=x_rel,
                y=y,
                mode="lines",
                name=_session_label(session),
                line=dict(width=2),
            )
        )
    distance_fig.update_layout(
        height=360,
        margin=dict(l=30, r=30, t=20, b=40),
        xaxis_title="time from session start (s)",
        yaxis_title="Δ distance from start (m)" if distance_delta_from_start else "distance (m)",
        legend=dict(orientation="h", yanchor="bottom", y=1.02, xanchor="left", x=0),
    )
    st.plotly_chart(distance_fig, use_container_width=True)

    jitter_fig = go.Figure()
    for session in sessions:
        _, details = compute_fast_instability(
            session.x,
            session.work[session.dist_source_col],
            improbable_speed_mps=float(improbable_speed_mps),
            jump_distance_m=float(jump_distance_m),
            local_window_s=float(local_instability_window_s),
        )
        jitter_fig.add_trace(
            go.Scatter(
                x=_relative_x(session),
                y=details["rolling_mad_m"],
                mode="lines",
                name=_session_label(session),
                line=dict(width=2),
            )
        )
    jitter_fig.update_layout(
        height=320,
        margin=dict(l=30, r=30, t=20, b=40),
        xaxis_title="time from session start (s)",
        yaxis_title=f"rolling MAD {local_instability_window_s:.1f}s (m)",
        legend=dict(orientation="h", yanchor="bottom", y=1.02, xanchor="left", x=0),
    )
    st.plotly_chart(jitter_fig, use_container_width=True)

    speed_fig = go.Figure()
    for session in sessions:
        velocity = compute_relative_velocity(session.x, session.work[session.dist_plot_col], smoothing_window=int(velocity_roll_win))
        speed_fig.add_trace(
            go.Scatter(
                x=_relative_x(session),
                y=velocity,
                mode="lines",
                name=_session_label(session),
                line=dict(width=1.8),
            )
        )
    speed_yaxis = dict(title="relative speed (m/s)")
    if velocity_axis_limit > 0:
        speed_yaxis["range"] = [-float(velocity_axis_limit), float(velocity_axis_limit)]
    speed_fig.update_layout(
        height=320,
        margin=dict(l=30, r=30, t=20, b=40),
        xaxis_title="time from session start (s)",
        yaxis=speed_yaxis,
        legend=dict(orientation="h", yanchor="bottom", y=1.02, xanchor="left", x=0),
    )
    st.plotly_chart(speed_fig, use_container_width=True)

    quality_fig = go.Figure()
    for session in sessions:
        if "Link" in session.transmission_categories:
            quality_fig.add_trace(
                go.Scatter(
                    x=_relative_x(session),
                    y=session.transmission_categories["Link"],
                    mode="lines",
                    name=f"{_session_label(session)} · Link",
                    line=dict(width=2.2),
                )
            )
        if "Jump" in session.transmission_categories:
            quality_fig.add_trace(
                go.Scatter(
                    x=_relative_x(session),
                    y=session.transmission_categories["Jump"],
                    mode="lines",
                    name=f"{_session_label(session)} · Jump",
                    line=dict(width=1.2, dash="dot"),
                    opacity=0.75,
                )
            )
    quality_fig.update_layout(
        height=320,
        margin=dict(l=30, r=30, t=20, b=40),
        xaxis_title="time from session start (s)",
        yaxis=dict(title="quality score (/10)", range=[0, 10.5]),
        legend=dict(orientation="h", yanchor="bottom", y=1.02, xanchor="left", x=0),
    )
    st.plotly_chart(quality_fig, use_container_width=True)


def dual_axis_plot(df: pd.DataFrame, x: pd.Series, primary_cols: list[str], dist_col: str, title: str):
    fig = go.Figure()
    colors = ["#2f7ed8", "#0d233a", "#8bbc21"]
    for i, c in enumerate(primary_cols):
        fig.add_trace(
            go.Scatter(
                x=x,
                y=df[c],
                name=c,
                mode="lines",
                line=dict(width=1.5, color=colors[i % len(colors)]),
                yaxis="y1",
            )
        )

    fig.add_trace(
        go.Scatter(
            x=x,
            y=df[dist_col],
            name=dist_col,
            mode="lines",
            line=dict(width=2, color="#f45b5b"),
            yaxis="y2",
        )
    )

    fig.update_layout(
        title=title,
        xaxis=dict(title="time (s)"),
        yaxis=dict(title="sensor"),
        yaxis2=dict(title="distance (m)", overlaying="y", side="right"),
        legend=dict(orientation="h", yanchor="bottom", y=1.02, xanchor="left", x=0),
        margin=dict(l=30, r=30, t=50, b=30),
        height=380,
    )
    st.plotly_chart(fig, use_container_width=True)


def map_zoom_for_bounds(lat: pd.Series, lon: pd.Series) -> float:
    lat_vals = pd.to_numeric(lat, errors="coerce")
    lon_vals = pd.to_numeric(lon, errors="coerce")
    lat_span = float(lat_vals.max() - lat_vals.min()) if lat_vals.notna().any() else 0.0
    lon_span = float(lon_vals.max() - lon_vals.min()) if lon_vals.notna().any() else 0.0
    span = max(lat_span, lon_span)
    if not np.isfinite(span) or span <= 0.0:
        return 16.0
    return float(np.clip(8.0 - np.log2(span), 3.0, 18.0))


def app() -> None:
    st.set_page_config(page_title="UWB CSV Explorer", layout="wide")
    st.title("UWB CSV Explorer")
    st.caption("Explore distance stability, transmission quality, acceleration, gyroscope, GPS, and optional radio diagnostics from exported CSV sessions.")

    with st.sidebar:
        st.header("Data")
        uploaded_files = st.file_uploader("CSV file(s)", type=["csv"], accept_multiple_files=True)
        path_txt = st.text_area("or local path(s) / glob", value="", help="One CSV per line, a folder, or a glob such as ~/Downloads/*.csv")
        downsample = st.slider("Downsample", min_value=1, max_value=20, value=1, step=1)
        roll_win = st.slider("Rolling mean window", min_value=1, max_value=200, value=1, step=1)
        st.header("Distance cleanup")
        filter_spikes = st.checkbox("Filter display spikes", value=True)
        max_abs_m = st.number_input("Hard distance limit |m|", min_value=0.0, max_value=100000.0, value=20.0, step=1.0)
        hampel_window = st.slider("Local spike window", min_value=3, max_value=101, value=21, step=2)
        hampel_sigma = st.slider("Local spike sigma", min_value=2.0, max_value=12.0, value=6.0, step=0.5)
        min_spike_dev_m = st.number_input("Min spike deviation (m)", min_value=0.0, max_value=20.0, value=0.75, step=0.05)
        show_outliers = st.checkbox("Show removed spikes", value=True)
        robust_axis = st.checkbox("Robust distance y-axis", value=True)
        st.header("Relative speed")
        velocity_roll_win = st.slider("Velocity median window", min_value=1, max_value=101, value=7, step=2)
        velocity_axis_limit = st.number_input("Velocity y-axis limit |m/s|", min_value=0.0, max_value=500.0, value=20.0, step=1.0)
        improbable_speed_mps = st.number_input("Improbable speed threshold |m/s|", min_value=0.0, max_value=200.0, value=10.0, step=1.0)
        jump_distance_m = st.number_input("Jump distance threshold Δm", min_value=0.0, max_value=10.0, value=0.50, step=0.05)
        local_instability_window_s = st.number_input("Local instability window (s)", min_value=0.2, max_value=10.0, value=2.0, step=0.2)
        show_raw_velocity = st.checkbox("Show raw velocity", value=False)

    if not uploaded_files and not path_txt.strip():
        st.info("Drop one or more CSV files, or provide local paths/globs to start.")
        return

    loaded: list[tuple[str, pd.DataFrame]] = []
    load_errors: list[str] = []

    for uploaded in uploaded_files or []:
        try:
            loaded.append((uploaded.name, load_csv(uploaded)))
        except Exception as exc:
            load_errors.append(f"{uploaded.name}: {exc}")

    for p in parse_local_csv_paths(path_txt):
        try:
            loaded.append((p.name, load_csv(p)))
        except Exception as exc:
            load_errors.append(f"{p}: {exc}")

    for err in load_errors:
        st.warning(f"Failed to read CSV: {err}")

    if not loaded:
        st.error("No readable CSV file found.")
        return

    prepared_sessions: list[PreparedSession] = []
    prepare_errors: list[str] = []
    for source_name, loaded_df in loaded:
        try:
            prepared_sessions.append(
                prepare_session(
                    loaded_df,
                    source_name,
                    downsample=int(downsample),
                    roll_win=int(roll_win),
                    filter_spikes=bool(filter_spikes),
                    max_abs_m=float(max_abs_m),
                    hampel_window=int(hampel_window),
                    hampel_sigma=float(hampel_sigma),
                    min_spike_dev_m=float(min_spike_dev_m),
                )
            )
        except Exception as exc:
            prepare_errors.append(f"{source_name}: {exc}")

    for err in prepare_errors:
        st.warning(f"Skipped CSV: {err}")

    if not prepared_sessions:
        st.error("No valid UWB CSV session found.")
        return

    comparison = comparison_dataframe(
        prepared_sessions,
        float(improbable_speed_mps),
        float(jump_distance_m),
        float(local_instability_window_s),
    )
    selected_source = prepared_sessions[0].source_name

    if len(prepared_sessions) > 1:
        st.subheader("Session Comparison")
        best = comparison.iloc[0]
        if np.isfinite(best["score_10"]):
            st.success(f"Best overall: {best['session']} · {best['score_10']:.1f}/10")
        else:
            st.info("Comparison loaded, but no score could be computed from the available columns.")

        display_cols = [
            "session",
            "bike_box_position",
            "vest_box_position",
            "app_preset",
            "app_test_profile",
            "app_rf_channel",
            "app_acq_period_ms",
            "app_median_window",
            "app_ranging_mode",
            "connected_role",
            "score_10",
            "link_avg",
            "stability_avg",
            "jump_avg",
            "nlos_avg",
            "fast_stability_10",
            "fast_instability_index",
            "distance_std_m",
            "raw_speed_spikes",
            "raw_speed_spike_rate_pct",
            "jump_rate_pct",
            "impossible_speed_rate_pct",
            "delta_dist_p95_m",
            "rolling_mad_p95_m",
            "max_bad_burst_samples",
            "max_bad_burst_s",
            "rel_speed_p99_mps",
            "acq_gap_p95_ms",
            "inter_sample_jitter_p95_ms",
            "acq_hz_mean",
            "duration_s",
            "samples",
            "clock_spike_corr",
        ]
        st.dataframe(
            comparison[display_cols].style.format(
                {
                    "score_10": "{:.1f}",
                    "link_avg": "{:.1f}",
                    "stability_avg": "{:.1f}",
                    "jump_avg": "{:.1f}",
                    "nlos_avg": "{:.1f}",
                    "fast_stability_10": "{:.1f}",
                    "fast_instability_index": "{:.0f}",
                    "distance_std_m": "{:.3f}",
                    "raw_speed_spike_rate_pct": "{:.2f}%",
                    "jump_rate_pct": "{:.2f}%",
                    "impossible_speed_rate_pct": "{:.2f}%",
                    "delta_dist_p95_m": "{:.3f}",
                    "rolling_mad_p95_m": "{:.3f}",
                    "max_bad_burst_s": "{:.2f}",
                    "rel_speed_p99_mps": "{:.2f}",
                    "acq_gap_p95_ms": "{:.0f}",
                    "inter_sample_jitter_p95_ms": "{:.0f}",
                    "acq_hz_mean": "{:.1f}",
                    "duration_s": "{:.1f}",
                    "clock_spike_corr": "{:.2f}",
                }
            ),
            use_container_width=True,
            hide_index=True,
        )

        score_fig = go.Figure()
        score_metrics = ["score_10", "link_avg", "stability_avg", "jump_avg", "nlos_avg"]
        for metric in score_metrics:
            if comparison[metric].notna().any():
                score_fig.add_trace(go.Bar(x=comparison["session"], y=comparison[metric], name=metric))
        score_fig.update_layout(
            height=340,
            margin=dict(l=30, r=30, t=20, b=80),
            barmode="group",
            xaxis_title="session",
            yaxis=dict(title="score (/10)", range=[0, 10.5]),
            legend=dict(orientation="h", yanchor="bottom", y=1.02, xanchor="left", x=0),
        )
        st.plotly_chart(score_fig, use_container_width=True)

        instability_fig = go.Figure()
        for metric in ["fast_instability_index", "jump_rate_pct", "impossible_speed_rate_pct", "bad_dynamic_rate_pct"]:
            if metric in comparison.columns and comparison[metric].notna().any():
                instability_fig.add_trace(go.Bar(x=comparison["session"], y=comparison[metric], name=metric))
        instability_fig.update_layout(
            height=320,
            margin=dict(l=30, r=30, t=20, b=80),
            barmode="group",
            xaxis_title="session",
            yaxis=dict(title="dynamic instability (% / index)", range=[0, 100]),
            legend=dict(orientation="h", yanchor="bottom", y=1.02, xanchor="left", x=0),
        )
        st.plotly_chart(instability_fig, use_container_width=True)

        penalty_fig = go.Figure()
        penalty_fig.add_trace(go.Bar(x=comparison["session"], y=comparison["distance_std_m"], name="distance_std_m", yaxis="y1"))
        penalty_fig.add_trace(go.Bar(x=comparison["session"], y=comparison["rolling_mad_p95_m"], name="rolling_mad_p95_m", yaxis="y1"))
        penalty_fig.add_trace(go.Bar(x=comparison["session"], y=comparison["raw_speed_spikes"], name="raw_speed_spikes", yaxis="y2"))
        penalty_fig.update_layout(
            height=320,
            margin=dict(l=30, r=30, t=20, b=80),
            barmode="group",
            xaxis_title="session",
            yaxis=dict(title="distance jitter (m)"),
            yaxis2=dict(title="raw speed spikes", overlaying="y", side="right"),
            legend=dict(orientation="h", yanchor="bottom", y=1.02, xanchor="left", x=0),
        )
        st.plotly_chart(penalty_fig, use_container_width=True)
        st.caption("Score global = moyenne pondérée de Link, Stability, Jump, NLOS et Timing. Fast instability met en avant les sauts locaux, vitesses impossibles, jitter court terme et rafales de mauvais points.")

        distance_delta_from_start = st.checkbox(
            "Overlay distance as Δ from session start",
            value=False,
            help="Active ce mode si les runs ne démarrent pas exactement à la même distance mais suivent le même profil de trajet.",
        )
        plot_multi_session_overlays(
            prepared_sessions,
            distance_delta_from_start=bool(distance_delta_from_start),
            velocity_roll_win=int(velocity_roll_win),
            velocity_axis_limit=float(velocity_axis_limit),
            improbable_speed_mps=float(improbable_speed_mps),
            jump_distance_m=float(jump_distance_m),
            local_instability_window_s=float(local_instability_window_s),
        )

        selected_source = st.selectbox("Detailed session", comparison["session"].tolist())

    selected_session = next((session for session in prepared_sessions if session.source_name == selected_source), prepared_sessions[0])
    df = selected_session.df
    cols = selected_session.cols
    x = selected_session.x
    work = selected_session.work
    dist_source_col = selected_session.dist_source_col
    dist_plot_col = selected_session.dist_plot_col
    outlier_mask = selected_session.outlier_mask
    dist_vals = selected_session.dist_vals
    hz_series = selected_session.hz_series
    signal_categories = selected_session.signal_categories
    transmission_categories = selected_session.transmission_categories
    rx_vals = selected_session.rx_vals
    fp_vals = selected_session.fp_vals
    source_name = selected_session.source_name

    st.subheader("Session Summary")
    c1, c2, c3, c4, c5, c6, c7, c8 = st.columns(8)
    quality_vals = pd.to_numeric(work[cols.signal_quality], errors="coerce") if cols.signal_quality and cols.signal_quality in work.columns else None
    c1.metric("Samples", f"{len(work)}")
    c2.metric("Distance mean", f"{dist_vals.mean():.3f} m")
    c3.metric("Distance std", f"{dist_vals.std(ddof=0):.3f} m")
    duration = float(x.iloc[-1] - x.iloc[0]) if len(x) > 1 else 0.0
    c4.metric("Duration", f"{duration:.1f} s")
    c5.metric("Acq mean", f"{hz_series.mean():.2f} Hz")
    c6.metric("Spikes filtered", f"{int(outlier_mask.sum())}")
    link_vals = transmission_categories["Link"] if "Link" in transmission_categories else None
    jump_vals = transmission_categories["Jump"] if "Jump" in transmission_categories else None
    c7.metric("Link avg", f"{link_vals.mean():.1f}/10" if link_vals is not None and link_vals.notna().any() else "--")
    c8.metric("Jump avg", f"{jump_vals.mean():.1f}/10" if jump_vals is not None and jump_vals.notna().any() else "--")
    st.caption(f"Source: {source_name}")

    st.subheader("Distance")
    fig_dist = go.Figure()
    if filter_spikes:
        fig_dist.add_trace(
            go.Scatter(
                x=x,
                y=work[dist_source_col],
                mode="lines",
                name=f"{dist_source_col} (original)",
                line=dict(width=1, color="#999999"),
                opacity=0.35,
            )
        )
    elif cols.dist_raw is not None and cols.dist_raw in work.columns and cols.dist_raw != dist_plot_col:
        fig_dist.add_trace(go.Scatter(x=x, y=work[cols.dist_raw], mode="lines", name=cols.dist_raw, line=dict(width=1)))
    fig_dist.add_trace(go.Scatter(x=x, y=work[dist_plot_col], mode="lines", name=dist_plot_col, line=dict(width=2)))
    if show_outliers and outlier_mask.any():
        fig_dist.add_trace(
            go.Scatter(
                x=x[outlier_mask],
                y=pd.to_numeric(work.loc[outlier_mask, dist_source_col], errors="coerce"),
                mode="markers",
                name="removed spikes",
                marker=dict(size=7, color="#d62728", symbol="x"),
            )
        )

    yaxis = dict(title="distance (m)")
    if robust_axis and dist_vals.notna().any():
        lo, hi = dist_vals.quantile([0.001, 0.999])
        if np.isfinite(lo) and np.isfinite(hi) and hi > lo:
            pad = max(0.1, float(hi - lo) * 0.08)
            yaxis["range"] = [float(lo - pad), float(hi + pad)]

    fig_dist.update_layout(height=320, margin=dict(l=30, r=30, t=20, b=30), xaxis_title="time (s)", yaxis=yaxis)
    st.plotly_chart(fig_dist, use_container_width=True)

    st.subheader("Transmission Quality")
    if not transmission_categories.empty:
        r1, r2, r3, r4 = st.columns(4)
        link_cat = transmission_categories["Link"] if "Link" in transmission_categories else None
        stab_cat = transmission_categories["Stability"] if "Stability" in transmission_categories else None
        smooth_cat = transmission_categories["Smoothness"] if "Smoothness" in transmission_categories else None
        timing_cat = transmission_categories["Timing"] if "Timing" in transmission_categories else None
        drop_cat = transmission_categories["Dropout"] if "Dropout" in transmission_categories else None
        jump_cat = transmission_categories["Jump"] if "Jump" in transmission_categories else None
        nlos_cat = transmission_categories["NLOS"] if "NLOS" in transmission_categories else None
        r1.metric("Link mean", f"{link_cat.mean():.1f}/10" if link_cat is not None and link_cat.notna().any() else "--")
        r2.metric("Stability mean", f"{stab_cat.mean():.1f}/10" if stab_cat is not None and stab_cat.notna().any() else "--")
        r3.metric("Jump mean", f"{jump_cat.mean():.1f}/10" if jump_cat is not None and jump_cat.notna().any() else "--")
        r4.metric("NLOS/CIR mean", f"{nlos_cat.mean():.1f}/10" if nlos_cat is not None and nlos_cat.notna().any() else "--")

        d1, d2, d3, d4 = st.columns(4)
        d1.metric("Smoothness mean", f"{smooth_cat.mean():.1f}/10" if smooth_cat is not None and smooth_cat.notna().any() else "--")
        d2.metric("Timing mean", f"{timing_cat.mean():.1f}/10" if timing_cat is not None and timing_cat.notna().any() else "--")
        d3.metric("Dropout mean", f"{drop_cat.mean():.1f}/10" if drop_cat is not None and drop_cat.notna().any() else "--")
        if cols.valid_rate_5s and cols.valid_rate_5s in work.columns:
            valid_rate_vals = pd.to_numeric(work[cols.valid_rate_5s], errors="coerce")
            d4.metric("Valid rate", f"{valid_rate_vals.mean() * 100.0:.0f}%" if valid_rate_vals.notna().any() else "--")
        elif cols.bad_burst_max and cols.bad_burst_max in work.columns:
            burst_vals = pd.to_numeric(work[cols.bad_burst_max], errors="coerce")
            d4.metric("Bad burst max", f"{burst_vals.max():.0f}" if burst_vals.notna().any() else "--")
        else:
            d4.metric("Valid rate", "--")

        fig_quality = go.Figure()
        colors = {
            "Link": "#111111",
            "Stability": "#2ca02c",
            "Smoothness": "#1f77b4",
            "Timing": "#9467bd",
            "Dropout": "#ff7f0e",
            "Jump": "#d62728",
            "NLOS": "#17becf",
        }
        for col in transmission_categories.columns:
            fig_quality.add_trace(
                go.Scatter(
                    x=x,
                    y=transmission_categories[col],
                    mode="lines",
                    name=col,
                    line=dict(width=2.5, color=colors.get(col, None)),
                )
            )
        if show_outliers and outlier_mask.any() and "Stability" in transmission_categories:
            fig_quality.add_trace(
                go.Scatter(
                    x=x[outlier_mask],
                    y=transmission_categories.loc[outlier_mask, "Stability"],
                    mode="markers",
                    name="distance spikes",
                    marker=dict(size=8, color="#d62728", symbol="x"),
                )
            )
        fig_quality.update_layout(
            height=300,
            margin=dict(l=30, r=30, t=20, b=30),
            xaxis_title="time (s)",
            yaxis=dict(title="transmission score (/10)", range=[0, 10.5]),
            legend=dict(orientation="h", yanchor="bottom", y=1.02, xanchor="left", x=0),
        )
        st.plotly_chart(fig_quality, use_container_width=True)

        st.caption("Lecture: Link = score pondéré global; Stability = écart-type distance sur ~5s; Jump = sauts/vitesses impossibles; NLOS/CIR = indice multipath/non-ligne-de-vue; Timing/Dropout = cadence et trous d'acquisition.")
    else:
        st.info("Transmission quality unavailable.")

    st.subheader("Fast Instability KPIs")
    fast_metrics, fast_details = compute_fast_instability(
        x,
        work[dist_source_col],
        improbable_speed_mps=float(improbable_speed_mps),
        jump_distance_m=float(jump_distance_m),
        local_window_s=float(local_instability_window_s),
    )
    f1, f2, f3, f4 = st.columns(4)
    f1.metric("Fast stability", _fmt_number(fast_metrics["fast_stability_10"], "{:.1f}/10"))
    f2.metric("Instability index", _fmt_number(fast_metrics["fast_instability_index"], "{:.0f}/100"))
    f3.metric("Jump rate", _fmt_number(fast_metrics["jump_rate_pct"], "{:.2f}%"))
    f4.metric("Impossible speed", _fmt_number(fast_metrics["impossible_speed_rate_pct"], "{:.2f}%"))

    f5, f6, f7, f8 = st.columns(4)
    f5.metric("Δdist p95", _fmt_number(fast_metrics["delta_dist_p95_m"], "{:.3f} m"))
    f6.metric("Rolling MAD p95", _fmt_number(fast_metrics["rolling_mad_p95_m"], "{:.3f} m"))
    f7.metric(
        "Bad burst max",
        f"{fast_metrics['max_bad_burst_samples']} pts / {_fmt_number(fast_metrics['max_bad_burst_s'], '{:.2f} s')}",
    )
    f8.metric("Timing jitter p95", _fmt_number(fast_metrics["inter_sample_jitter_p95_ms"], "{:.0f} ms"))

    fig_fast = go.Figure()
    fig_fast.add_trace(
        go.Scatter(
            x=x,
            y=fast_details["abs_delta_m"],
            mode="lines",
            name="|Δdistance raw|",
            line=dict(width=1.5, color="#7b3294"),
        )
    )
    fig_fast.add_trace(
        go.Scatter(
            x=x,
            y=fast_details["rolling_mad_m"],
            mode="lines",
            name=f"rolling MAD {local_instability_window_s:.1f}s",
            line=dict(width=2.5, color="#008837"),
        )
    )
    if jump_distance_m > 0:
        fig_fast.add_hline(y=float(jump_distance_m), line_dash="dash", line_color="#d62728", annotation_text="jump threshold")
    bad_dynamic = fast_details["bad_dynamic_point"].fillna(False).astype(bool)
    if show_outliers and bad_dynamic.any():
        fig_fast.add_trace(
            go.Scatter(
                x=x[bad_dynamic],
                y=fast_details.loc[bad_dynamic, "abs_delta_m"],
                mode="markers",
                name="dynamic bad point",
                marker=dict(size=8, color="#d62728", symbol="x"),
            )
        )
    fig_fast.update_layout(
        height=310,
        margin=dict(l=30, r=30, t=20, b=30),
        xaxis_title="time (s)",
        yaxis_title="distance change / local jitter (m)",
        legend=dict(orientation="h", yanchor="bottom", y=1.02, xanchor="left", x=0),
    )
    st.plotly_chart(fig_fast, use_container_width=True)
    st.caption("À utiliser quand la standard deviation globale est proche: ces KPI mesurent les sauts point-à-point, la vitesse impossible, le jitter local et les rafales de mauvais points. Plus l'Instability index est élevé, plus le mode est instable.")

    st.subheader("NLOS / CIR Diagnostics")
    nlos_cols = [c for c in [cols.nlos_quality, cols.peak_to_fp, cols.fp_conf, cols.sts_quality] if c and c in work.columns]
    if nlos_cols:
        n1, n2, n3, n4 = st.columns(4)
        if cols.nlos_quality and cols.nlos_quality in work.columns:
            nlos_vals = pd.to_numeric(work[cols.nlos_quality], errors="coerce")
            n1.metric("NLOS mean", f"{nlos_vals.mean():.1f}/10" if nlos_vals.notna().any() else "--")
        else:
            nlos_vals = transmission_categories["NLOS"] if "NLOS" in transmission_categories else None
            n1.metric("NLOS mean", f"{nlos_vals.mean():.1f}/10" if nlos_vals is not None and nlos_vals.notna().any() else "--")
        if cols.peak_to_fp and cols.peak_to_fp in work.columns:
            peak_gap_vals = pd.to_numeric(work[cols.peak_to_fp], errors="coerce")
            n2.metric("Peak-FP gap", f"{peak_gap_vals.mean():.2f} spl" if peak_gap_vals.notna().any() else "--")
        else:
            n2.metric("Peak-FP gap", "--")
        if cols.fp_conf and cols.fp_conf in work.columns:
            fp_conf_vals = pd.to_numeric(work[cols.fp_conf], errors="coerce")
            n3.metric("FP confidence", f"{fp_conf_vals.mean():.1f}" if fp_conf_vals.notna().any() else "--")
        else:
            n3.metric("FP confidence", "--")
        if cols.sts_quality and cols.sts_quality in work.columns:
            sts_vals = pd.to_numeric(work[cols.sts_quality], errors="coerce")
            n4.metric("STS quality", f"{sts_vals.mean():.0f}" if sts_vals.notna().any() else "--")
        else:
            n4.metric("STS quality", "--")

        if len(nlos_cols) > 0:
            dual_axis_plot(work, x, nlos_cols, dist_plot_col, "NLOS/CIR diagnostics + distance")
        st.caption("Peak-FP gap élevé ou FP confidence faible indiquent souvent du multipath/NLOS, fréquent près du corps ou d'un cadre métallique.")
    else:
        st.info("NLOS/CIR diagnostics columns not found.")

    st.subheader("Radio Raw Diagnostics")
    radio_cols = [c for c in [cols.rx_power, cols.fp_power, cols.clock_offset, cols.signal_quality, cols.rx_quality, cols.path_quality, cols.multipath_quality, cols.clock_quality] if c and c in work.columns]
    if radio_cols:
        r1, r2, r3 = st.columns(3)
        r1.metric("RX mean", f"{rx_vals.mean():.1f} dBm" if rx_vals is not None and rx_vals.notna().any() else "--")
        r2.metric("FP mean", f"{fp_vals.mean():.1f} dBm" if fp_vals is not None and fp_vals.notna().any() else "--")
        if rx_vals is not None and fp_vals is not None and (rx_vals.notna().any() or fp_vals.notna().any()):
            r3.metric("RX-FP gap", f"{(rx_vals - fp_vals).mean():.1f} dB")
        else:
            r3.metric("RX-FP gap", "--")

        power_cols = [c for c in [cols.rx_power, cols.fp_power] if c and c in work.columns]
        if power_cols:
            dual_axis_plot(work, x, power_cols, dist_plot_col, "RX/first-path power + distance")
        if cols.clock_offset and cols.clock_offset in work.columns:
            dual_axis_plot(work, x, [cols.clock_offset], dist_plot_col, "Clock offset + distance")

            clock_vals = pd.to_numeric(work[cols.clock_offset], errors="coerce")
            clock_delta = clock_vals.diff().abs()
            raw_velocity_for_clock = compute_relative_velocity(x, work[dist_source_col], smoothing_window=1)
            clock_speed_spikes = raw_velocity_for_clock.abs() > float(improbable_speed_mps) if improbable_speed_mps > 0 else pd.Series(False, index=work.index)
            spike_delta = clock_delta[clock_speed_spikes]
            normal_delta = clock_delta[~clock_speed_spikes]

            corr_value = None
            clock_for_corr = clock_delta.fillna(0.0)
            spike_numeric = clock_speed_spikes.astype("float64")
            if clock_for_corr.std(ddof=0) > 0.0 and spike_numeric.std(ddof=0) > 0.0:
                corr_value = float(np.corrcoef(clock_for_corr.to_numpy(dtype=np.float64), spike_numeric.to_numpy(dtype=np.float64))[0, 1])

            j1, j2, j3, j4 = st.columns(4)
            j1.metric("Clock std", f"{clock_vals.std(ddof=0):.3f} ppm" if clock_vals.notna().any() else "--")
            j2.metric("Δclock p95", f"{clock_delta.quantile(0.95):.3f} ppm" if clock_delta.notna().any() else "--")
            j3.metric("Δclock on spikes", f"{spike_delta.median():.3f} ppm" if spike_delta.notna().any() else "--")
            j4.metric("Clock/spike corr", f"{corr_value:.2f}" if corr_value is not None and np.isfinite(corr_value) else "--")

            fig_clock_jitter = go.Figure()
            fig_clock_jitter.add_trace(
                go.Scatter(
                    x=x,
                    y=clock_delta,
                    mode="lines",
                    name="|Δ clock_offset|",
                    line=dict(width=1.8, color="#2f7ed8"),
                )
            )
            if show_outliers and clock_speed_spikes.any():
                fig_clock_jitter.add_trace(
                    go.Scatter(
                        x=x[clock_speed_spikes],
                        y=clock_delta[clock_speed_spikes],
                        mode="markers",
                        name="raw impossible speed",
                        marker=dict(size=7, color="#d62728", symbol="x"),
                    )
                )
            fig_clock_jitter.update_layout(
                height=260,
                margin=dict(l=30, r=30, t=20, b=30),
                xaxis_title="time (s)",
                yaxis_title="|Δ clock offset| (ppm/sample)",
                legend=dict(orientation="h", yanchor="bottom", y=1.02, xanchor="left", x=0),
            )
            st.plotly_chart(fig_clock_jitter, use_container_width=True)
        st.caption("Ces métriques RF sont indicatives seulement; pour comparer orientation/positionnement, privilégier la section Transmission Quality.")
    else:
        st.info("Radio diagnostics columns not found.")

    st.subheader("Instant Relative Speed")
    velocity = compute_relative_velocity(x, work[dist_plot_col], smoothing_window=int(velocity_roll_win))
    raw_velocity = compute_relative_velocity(x, work[dist_source_col], smoothing_window=1)
    abs_velocity = velocity.abs()
    speed_outliers = raw_velocity.abs() > float(improbable_speed_mps) if improbable_speed_mps > 0 else pd.Series(False, index=velocity.index)

    v1, v2, v3 = st.columns(3)
    v1.metric("Rel speed p95", f"{abs_velocity.quantile(0.95):.2f} m/s")
    v2.metric("Rel speed p99", f"{abs_velocity.quantile(0.99):.2f} m/s")
    v3.metric("Raw speed spikes", f"{int(speed_outliers.sum())}")

    fig_vel = go.Figure()
    if show_raw_velocity:
        fig_vel.add_trace(
            go.Scatter(
                x=x,
                y=raw_velocity,
                mode="lines",
                name="raw Δdistance/Δt",
                line=dict(width=1, color="#999999"),
                opacity=0.35,
            )
        )
    fig_vel.add_trace(
        go.Scatter(
            x=x,
            y=velocity,
            mode="lines",
            name="relative speed (filtered distance)",
            line=dict(width=2, color="#7b3294"),
        )
    )
    if show_outliers and speed_outliers.any():
        fig_vel.add_trace(
            go.Scatter(
                x=x[speed_outliers],
                y=raw_velocity[speed_outliers].clip(lower=-float(velocity_axis_limit), upper=float(velocity_axis_limit)) if velocity_axis_limit > 0 else raw_velocity[speed_outliers],
                mode="markers",
                name="raw impossible speed",
                marker=dict(size=7, color="#d62728", symbol="x"),
            )
        )

    velocity_yaxis = dict(title="relative speed (m/s)")
    if velocity_axis_limit > 0:
        velocity_yaxis["range"] = [-float(velocity_axis_limit), float(velocity_axis_limit)]
    fig_vel.update_layout(height=320, margin=dict(l=30, r=30, t=20, b=30), xaxis_title="time (s)", yaxis=velocity_yaxis)
    st.plotly_chart(fig_vel, use_container_width=True)

    st.subheader("Acquisition Frequency")
    hz_plot = hz_series
    if roll_win > 1:
        hz_plot = hz_plot.rolling(roll_win, min_periods=1).mean()
    fig_hz = go.Figure()
    fig_hz.add_trace(go.Scatter(x=x, y=hz_plot, mode="lines", name="acq_hz", line=dict(width=2, color="#5b8c5a")))
    fig_hz.update_layout(height=300, margin=dict(l=30, r=30, t=20, b=30), xaxis_title="time (s)", yaxis_title="Hz")
    st.plotly_chart(fig_hz, use_container_width=True)

    st.subheader("Acceleration and Distance Overlay")
    if all([cols.iax, cols.iay, cols.iaz]):
        dual_axis_plot(work, x, [cols.iax, cols.iay, cols.iaz], dist_plot_col, "Initiator accel + distance")
    else:
        st.info("Initiator acceleration columns not found.")

    if all([cols.rax, cols.ray, cols.raz]):
        dual_axis_plot(work, x, [cols.rax, cols.ray, cols.raz], dist_plot_col, "Responder accel + distance")
    else:
        st.info("Responder acceleration columns not found.")

    st.subheader("Gyroscope and Distance Overlay")
    if all([cols.gx, cols.gy, cols.gz]):
        dual_axis_plot(work, x, [cols.gx, cols.gy, cols.gz], dist_plot_col, "Phone gyro + distance")
    else:
        st.info("Gyroscope columns not found in CSV.")

    st.subheader("GPS Speed and Distance Overlay")
    gps_speed = None
    if cols.speed and cols.speed in work.columns:
        gps_speed = pd.to_numeric(work[cols.speed], errors="coerce")
    elif cols.lat and cols.lon and cols.lat in work.columns and cols.lon in work.columns:
        gps_speed = gps_speed_from_track(work[cols.lat], work[cols.lon], x)

    if gps_speed is not None and gps_speed.notna().any():
        if roll_win > 1:
            gps_speed = gps_speed.rolling(roll_win, min_periods=1).mean()
        speed_df = pd.DataFrame(
            {
                "gps_speed_mps": gps_speed,
                dist_plot_col: pd.to_numeric(work[dist_plot_col], errors="coerce"),
            }
        )
        dual_axis_plot(speed_df, x, ["gps_speed_mps"], dist_plot_col, "GPS speed + distance")
    else:
        st.info("GPS speed unavailable (missing speed column and no valid lat/lon track).")

    st.subheader("GPS")
    if cols.lat and cols.lon and cols.lat in work.columns and cols.lon in work.columns:
        gdf = work[[cols.lat, cols.lon, dist_plot_col]].copy()
        gdf[cols.lat] = pd.to_numeric(gdf[cols.lat], errors="coerce")
        gdf[cols.lon] = pd.to_numeric(gdf[cols.lon], errors="coerce")
        gdf[dist_plot_col] = pd.to_numeric(gdf[dist_plot_col], errors="coerce")
        gdf = gdf.dropna(subset=[cols.lat, cols.lon])
        gdf = gdf[gdf[cols.lat].between(-90.0, 90.0) & gdf[cols.lon].between(-180.0, 180.0)]
        if not gdf.empty:
            center_lat = float(gdf[cols.lat].mean())
            center_lon = float(gdf[cols.lon].mean())
            fig_gps = go.Figure()
            fig_gps.add_trace(
                go.Scattermapbox(
                    lat=gdf[cols.lat],
                    lon=gdf[cols.lon],
                    mode="lines",
                    line=dict(width=4, color="#555555"),
                    name="route",
                    hoverinfo="skip",
                )
            )
            fig_gps.add_trace(
                go.Scattermapbox(
                    lat=gdf[cols.lat],
                    lon=gdf[cols.lon],
                    mode="markers",
                    marker=dict(size=7, color=gdf[dist_plot_col], colorscale="Turbo", showscale=True, colorbar=dict(title="dist (m)")),
                    customdata=np.stack([gdf[dist_plot_col].to_numpy(dtype=np.float64)], axis=-1),
                    hovertemplate="lat=%{lat:.6f}<br>lon=%{lon:.6f}<br>dist=%{customdata[0]:.2f} m<extra></extra>",
                    name="distance",
                )
            )
            fig_gps.add_trace(
                go.Scattermapbox(
                    lat=[float(gdf[cols.lat].iloc[0]), float(gdf[cols.lat].iloc[-1])],
                    lon=[float(gdf[cols.lon].iloc[0]), float(gdf[cols.lon].iloc[-1])],
                    mode="markers+text",
                    marker=dict(size=13, color=["#2ca02c", "#d62728"]),
                    text=["Start", "End"],
                    textposition="top center",
                    name="start/end",
                )
            )
            fig_gps.update_layout(
                height=520,
                margin=dict(l=30, r=30, t=20, b=30),
                mapbox=dict(
                    style="open-street-map",
                    center=dict(lat=center_lat, lon=center_lon),
                    zoom=map_zoom_for_bounds(gdf[cols.lat], gdf[cols.lon]),
                ),
                legend=dict(orientation="h", yanchor="bottom", y=1.02, xanchor="left", x=0),
            )
            st.plotly_chart(fig_gps, use_container_width=True)
            st.caption("Carte OpenStreetMap: la ligne grise est le trajet, les points sont colorés par distance UWB.")
        else:
            st.info("GPS columns found but no valid rows.")
    else:
        st.info("GPS columns not found in CSV.")


if __name__ == "__main__":
    app()
