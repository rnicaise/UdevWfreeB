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
from pathlib import Path

import numpy as np
import pandas as pd
import plotly.graph_objects as go
import streamlit as st


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
    )


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
    vals = pd.to_numeric(series, errors="coerce").ffill().fillna(0).to_numpy()
    out = np.zeros_like(vals, dtype=np.float64)
    offset = 0.0
    prev = vals[0]
    for i, v in enumerate(vals):
        if i > 0 and v < prev:
            offset += prev
        out[i] = v + offset
        prev = v
    return pd.Series(out, index=series.index)


def compute_acquisition_hz(t_sec: pd.Series, sample_series: pd.Series | None) -> pd.Series:
    t = pd.to_numeric(t_sec, errors="coerce").to_numpy(dtype=np.float64)
    if sample_series is not None:
        s = pd.to_numeric(sample_series, errors="coerce").to_numpy(dtype=np.float64)
    else:
        s = np.arange(len(t), dtype=np.float64)

    hz = np.full_like(t, np.nan, dtype=np.float64)
    for i in range(1, len(t)):
        dt = t[i] - t[i - 1]
        ds = s[i] - s[i - 1]
        if not np.isfinite(dt) or dt <= 0:
            continue
        if not np.isfinite(ds):
            continue
        hz[i] = ds / dt

    return pd.Series(hz, index=t_sec.index)


def load_csv(file_obj) -> pd.DataFrame:
    df = pd.read_csv(file_obj)
    df.columns = [c.strip() for c in df.columns]
    return df


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


def app() -> None:
    st.set_page_config(page_title="UWB CSV Explorer", layout="wide")
    st.title("UWB CSV Explorer")
    st.caption("Explore distance, acceleration, gyroscope, and GPS from exported CSV sessions.")

    with st.sidebar:
        st.header("Data")
        uploaded = st.file_uploader("CSV file", type=["csv"])
        path_txt = st.text_input("or local path", value="")
        downsample = st.slider("Downsample", min_value=1, max_value=20, value=1, step=1)
        roll_win = st.slider("Rolling mean window", min_value=1, max_value=200, value=1, step=1)

    if uploaded is None and not path_txt.strip():
        st.info("Drop a CSV or provide a local path to start.")
        return

    try:
        if uploaded is not None:
            df = load_csv(uploaded)
            source_name = uploaded.name
        else:
            p = Path(path_txt).expanduser()
            df = load_csv(p)
            source_name = str(p)
    except Exception as exc:
        st.error(f"Failed to read CSV: {exc}")
        return

    if df.empty:
        st.warning("CSV is empty.")
        return

    cols = detect_columns(df)
    if cols.dist_plot is None:
        st.error("No distance column found. Expected one of: dist_filt, dist, dist_raw, distance.")
        return

    if cols.ms is not None:
        t_ms = make_monotonic_ms(df[cols.ms])
        x = t_ms / 1000.0
    else:
        x = pd.Series(np.arange(len(df), dtype=np.float64) / 50.0)

    work = df.copy()
    for c in work.columns:
        if pd.api.types.is_numeric_dtype(work[c]):
            continue
        converted = pd.to_numeric(work[c], errors="coerce")
        # Preserve purely textual columns while converting numeric-like data.
        if converted.notna().any():
            work[c] = converted

    if downsample > 1:
        work = work.iloc[::downsample, :].reset_index(drop=True)
        x = x.iloc[::downsample].reset_index(drop=True)

    if roll_win > 1:
        if cols.dist_plot in work.columns:
            work[cols.dist_plot] = pd.to_numeric(work[cols.dist_plot], errors="coerce").rolling(roll_win, min_periods=1).mean()

    st.subheader("Session Summary")
    c1, c2, c3, c4, c5 = st.columns(5)
    dist_vals = pd.to_numeric(work[cols.dist_plot], errors="coerce")
    hz_series = compute_acquisition_hz(x, work[cols.sample] if cols.sample and cols.sample in work.columns else None)
    c1.metric("Samples", f"{len(work)}")
    c2.metric("Distance mean", f"{dist_vals.mean():.3f} m")
    c3.metric("Distance std", f"{dist_vals.std(ddof=0):.3f} m")
    duration = float(x.iloc[-1] - x.iloc[0]) if len(x) > 1 else 0.0
    c4.metric("Duration", f"{duration:.1f} s")
    c5.metric("Acq mean", f"{hz_series.mean():.2f} Hz")
    st.caption(f"Source: {source_name}")

    st.subheader("Distance")
    fig_dist = go.Figure()
    if cols.dist_raw is not None and cols.dist_raw in work.columns and cols.dist_raw != cols.dist_plot:
        fig_dist.add_trace(go.Scatter(x=x, y=work[cols.dist_raw], mode="lines", name=cols.dist_raw, line=dict(width=1)))
    fig_dist.add_trace(go.Scatter(x=x, y=work[cols.dist_plot], mode="lines", name=cols.dist_plot, line=dict(width=2)))
    fig_dist.update_layout(height=320, margin=dict(l=30, r=30, t=20, b=30), xaxis_title="time (s)", yaxis_title="distance (m)")
    st.plotly_chart(fig_dist, use_container_width=True)

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
        dual_axis_plot(work, x, [cols.iax, cols.iay, cols.iaz], cols.dist_plot, "Initiator accel + distance")
    else:
        st.info("Initiator acceleration columns not found.")

    if all([cols.rax, cols.ray, cols.raz]):
        dual_axis_plot(work, x, [cols.rax, cols.ray, cols.raz], cols.dist_plot, "Responder accel + distance")
    else:
        st.info("Responder acceleration columns not found.")

    st.subheader("Gyroscope and Distance Overlay")
    if all([cols.gx, cols.gy, cols.gz]):
        dual_axis_plot(work, x, [cols.gx, cols.gy, cols.gz], cols.dist_plot, "Phone gyro + distance")
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
                cols.dist_plot: pd.to_numeric(work[cols.dist_plot], errors="coerce"),
            }
        )
        dual_axis_plot(speed_df, x, ["gps_speed_mps"], cols.dist_plot, "GPS speed + distance")
    else:
        st.info("GPS speed unavailable (missing speed column and no valid lat/lon track).")

    st.subheader("GPS")
    if cols.lat and cols.lon and cols.lat in work.columns and cols.lon in work.columns:
        gdf = work[[cols.lat, cols.lon, cols.dist_plot]].copy()
        gdf = gdf.dropna()
        if not gdf.empty:
            fig_gps = go.Figure(
                data=go.Scatter(
                    x=gdf[cols.lon],
                    y=gdf[cols.lat],
                    mode="markers+lines",
                    marker=dict(size=5, color=gdf[cols.dist_plot], colorscale="Turbo", showscale=True, colorbar=dict(title="dist (m)")),
                    name="track",
                )
            )
            fig_gps.update_layout(
                height=380,
                margin=dict(l=30, r=30, t=20, b=30),
                xaxis_title="longitude",
                yaxis_title="latitude",
            )
            st.plotly_chart(fig_gps, use_container_width=True)
        else:
            st.info("GPS columns found but no valid rows.")
    else:
        st.info("GPS columns not found in CSV.")


if __name__ == "__main__":
    app()
