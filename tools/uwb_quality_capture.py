#!/usr/bin/env python3
"""Capture a UWB CSV stream from macOS serial and generate quality metrics.

Default target is the pure UWB firmware CSV:

    ms,sample,dist,cppm,valid,dist_filt,dist_smooth,rx_fail

The script records until Ctrl-C, then writes:

    out/quality/<label>-<timestamp>.csv
    out/quality/<label>-<timestamp>-summary.txt
    out/quality/<label>-<timestamp>-summary.json

It also accepts the older 32-column GenA CSV enough to compute the common
quality metrics (dist, clock ppm, valid, filtered/smoothed distance).
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import statistics
import subprocess
import sys
import time
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Iterable


PURE_HEADER = [
    "ms",
    "sample",
    "dist",
    "cppm",
    "valid",
    "dist_filt",
    "dist_smooth",
    "rx_fail",
]

LEGACY_HEADER_32 = [
    "ms",
    "sample",
    "dist",
    "rx_power",
    "fp_power",
    "clock_ppm",
    "score",
    "nlos",
    "peak_fp",
    "fp_conf",
    "sts",
    "iax",
    "iay",
    "iaz",
    "rax",
    "ray",
    "raz",
    "resp_acq_ms",
    "init_acq_ms",
    "resp_profile_opt",
    "init_profile_opt",
    "igx",
    "igy",
    "igz",
    "rgx",
    "rgy",
    "rgz",
    "valid",
    "dist_filt",
    "dist_smooth",
    "rload_mv",
    "rload_connected",
]


@dataclass
class Sample:
    ms: int
    sample: int
    dist: float
    cppm: float | None
    valid: bool | None
    dist_filt: float | None
    dist_smooth: float | None
    rx_fail: int | None
    raw: list[str]


def parse_float(value: str) -> float | None:
    try:
        parsed = float(value)
    except (TypeError, ValueError):
        return None
    return parsed if math.isfinite(parsed) else None


def parse_int(value: str) -> int | None:
    try:
        return int(value)
    except (TypeError, ValueError):
        return None


def parse_sample(parts: list[str]) -> Sample | None:
    if len(parts) >= 8:
        ms = parse_int(parts[0])
        sample = parse_int(parts[1])
        dist = parse_float(parts[2])
        if ms is None or sample is None or dist is None:
            return None

        if len(parts) == 8:
            cppm = parse_float(parts[3])
            valid_int = parse_int(parts[4])
            dist_filt = parse_float(parts[5])
            dist_smooth = parse_float(parts[6])
            rx_fail = parse_int(parts[7])
            return Sample(
                ms=ms,
                sample=sample,
                dist=dist,
                cppm=cppm,
                valid=(valid_int != 0) if valid_int is not None else None,
                dist_filt=dist_filt,
                dist_smooth=dist_smooth,
                rx_fail=rx_fail,
                raw=parts,
            )

        if len(parts) >= 32:
            cppm = parse_float(parts[5])
            valid_int = parse_int(parts[27])
            dist_filt = parse_float(parts[28])
            dist_smooth = parse_float(parts[29])
            return Sample(
                ms=ms,
                sample=sample,
                dist=dist,
                cppm=cppm,
                valid=(valid_int != 0) if valid_int is not None else None,
                dist_filt=dist_filt,
                dist_smooth=dist_smooth,
                rx_fail=None,
                raw=parts,
            )

    return None


def percentile(sorted_values: list[float], pct: float) -> float:
    if not sorted_values:
        return math.nan
    if len(sorted_values) == 1:
        return sorted_values[0]
    pos = (len(sorted_values) - 1) * pct
    lo = int(math.floor(pos))
    hi = int(math.ceil(pos))
    if lo == hi:
        return sorted_values[lo]
    weight = pos - lo
    return sorted_values[lo] * (1.0 - weight) + sorted_values[hi] * weight


def pearson(xs: list[float], ys: list[float]) -> float:
    if len(xs) != len(ys) or len(xs) < 2:
        return math.nan
    mean_x = statistics.fmean(xs)
    mean_y = statistics.fmean(ys)
    dx = [x - mean_x for x in xs]
    dy = [y - mean_y for y in ys]
    denom_x = math.sqrt(sum(x * x for x in dx))
    denom_y = math.sqrt(sum(y * y for y in dy))
    if denom_x == 0.0 or denom_y == 0.0:
        return math.nan
    return sum(x * y for x, y in zip(dx, dy)) / (denom_x * denom_y)


def finite(values: Iterable[float | None]) -> list[float]:
    return [value for value in values if value is not None and math.isfinite(value)]


def stats_block(values: list[float]) -> dict[str, float | int]:
    values = [value for value in values if math.isfinite(value)]
    if not values:
        return {
            "count": 0,
            "mean": math.nan,
            "median": math.nan,
            "std": math.nan,
            "mad": math.nan,
            "min": math.nan,
            "max": math.nan,
            "p05": math.nan,
            "p25": math.nan,
            "p50": math.nan,
            "p75": math.nan,
            "p95": math.nan,
            "iqr": math.nan,
            "peak_to_peak": math.nan,
        }

    sorted_values = sorted(values)
    median = percentile(sorted_values, 0.50)
    deviations = sorted(abs(value - median) for value in values)
    p25 = percentile(sorted_values, 0.25)
    p75 = percentile(sorted_values, 0.75)
    return {
        "count": len(values),
        "mean": statistics.fmean(values),
        "median": median,
        "std": statistics.pstdev(values) if len(values) > 1 else 0.0,
        "mad": percentile(deviations, 0.50),
        "min": sorted_values[0],
        "max": sorted_values[-1],
        "p05": percentile(sorted_values, 0.05),
        "p25": p25,
        "p50": median,
        "p75": p75,
        "p95": percentile(sorted_values, 0.95),
        "iqr": p75 - p25,
        "peak_to_peak": sorted_values[-1] - sorted_values[0],
    }


def jump_stats(values: list[float]) -> dict[str, float | int]:
    if len(values) < 2:
        return {
            "count": 0,
            "mean_abs": math.nan,
            "p95_abs": math.nan,
            "max_abs": math.nan,
            "gt_10cm": 0,
            "gt_20cm": 0,
        }
    jumps = [abs(b - a) for a, b in zip(values, values[1:])]
    sorted_jumps = sorted(jumps)
    return {
        "count": len(jumps),
        "mean_abs": statistics.fmean(jumps),
        "p95_abs": percentile(sorted_jumps, 0.95),
        "max_abs": sorted_jumps[-1],
        "gt_10cm": sum(1 for jump in jumps if jump > 0.10),
        "gt_20cm": sum(1 for jump in jumps if jump > 0.20),
    }


def window_stats(samples: list[Sample], signal: str, window_s: float) -> dict[str, float | int]:
    values_by_bucket: dict[int, list[float]] = {}
    for sample in samples:
        value = getattr(sample, signal)
        if value is None or not math.isfinite(value):
            continue
        bucket = int((sample.ms / 1000.0) // window_s)
        values_by_bucket.setdefault(bucket, []).append(value)

    means = [statistics.fmean(values) for values in values_by_bucket.values() if values]
    stds = [statistics.pstdev(values) for values in values_by_bucket.values() if len(values) > 1]
    return {
        "window_s": window_s,
        "windows": len(means),
        "mean_of_means": statistics.fmean(means) if means else math.nan,
        "std_of_means": statistics.pstdev(means) if len(means) > 1 else (0.0 if means else math.nan),
        "mean_window_std": statistics.fmean(stds) if stds else math.nan,
        "min_window_mean": min(means) if means else math.nan,
        "max_window_mean": max(means) if means else math.nan,
        "drift_peak_to_peak": (max(means) - min(means)) if means else math.nan,
    }


def allan_deviation(samples: list[Sample], signal: str, taus_s: list[float]) -> dict[str, float]:
    values = [(sample.ms / 1000.0, getattr(sample, signal)) for sample in samples]
    values = [(t, v) for t, v in values if v is not None and math.isfinite(v)]
    result: dict[str, float] = {}
    if not values:
        return {str(tau): math.nan for tau in taus_s}

    start_t = values[0][0]
    for tau in taus_s:
        buckets: dict[int, list[float]] = {}
        for t, value in values:
            bucket = int((t - start_t) // tau)
            buckets.setdefault(bucket, []).append(value)
        means = [statistics.fmean(buckets[idx]) for idx in sorted(buckets) if buckets[idx]]
        if len(means) < 2:
            result[str(tau)] = math.nan
            continue
        diffs_sq = [(b - a) ** 2 for a, b in zip(means, means[1:])]
        result[str(tau)] = math.sqrt(0.5 * statistics.fmean(diffs_sq))
    return result


def summarize(samples: list[Sample], host_duration_s: float, real_distance_m: float | None) -> dict[str, object]:
    total = len(samples)
    valid_samples = [sample for sample in samples if sample.valid is not False]
    dist_values = [sample.dist for sample in valid_samples]
    dist_all_values = [sample.dist for sample in samples]
    filt_values = finite(sample.dist_filt for sample in valid_samples)
    smooth_values = finite(sample.dist_smooth for sample in valid_samples)
    cppm_values = finite(sample.cppm for sample in samples)

    if len(samples) >= 2:
        fw_span_ms = samples[-1].ms - samples[0].ms
        fw_span_samples = samples[-1].sample - samples[0].sample
        firmware_hz = (fw_span_samples * 1000.0 / fw_span_ms) if fw_span_ms > 0 else math.nan
    else:
        fw_span_ms = 0
        fw_span_samples = 0
        firmware_hz = math.nan

    rx_fail_values = [sample.rx_fail for sample in samples if sample.rx_fail is not None]
    if rx_fail_values:
        rx_fail_delta = rx_fail_values[-1] - rx_fail_values[0]
    else:
        rx_fail_delta = None

    if cppm_values and len(cppm_values) == len(samples):
        corr_dist_cppm = pearson([sample.dist for sample in samples], cppm_values)
    else:
        pairs = [(sample.dist, sample.cppm) for sample in samples if sample.cppm is not None and math.isfinite(sample.cppm)]
        corr_dist_cppm = pearson([dist for dist, _ in pairs], [cppm for _, cppm in pairs])

    dist_stats = stats_block(dist_values)
    if real_distance_m is not None and math.isfinite(dist_stats["mean"]):
        bias_m = float(dist_stats["mean"]) - real_distance_m
    else:
        bias_m = math.nan

    return {
        "samples": total,
        "valid_samples": len(valid_samples),
        "valid_pct": (100.0 * len(valid_samples) / total) if total else 0.0,
        "host_duration_s": host_duration_s,
        "host_hz": (total / host_duration_s) if host_duration_s > 0 else math.nan,
        "firmware_span_ms": fw_span_ms,
        "firmware_span_samples": fw_span_samples,
        "firmware_hz": firmware_hz,
        "rx_fail_delta": rx_fail_delta,
        "real_distance_m": real_distance_m,
        "bias_m": bias_m,
        "dist_all": stats_block(dist_all_values),
        "dist_valid": dist_stats,
        "dist_filt": stats_block(filt_values),
        "dist_smooth": stats_block(smooth_values),
        "jumps_dist_valid": jump_stats(dist_values),
        "cppm": stats_block(cppm_values),
        "corr_dist_cppm": corr_dist_cppm,
        "windows_dist": {
            "1s": window_stats(valid_samples, "dist", 1.0),
            "5s": window_stats(valid_samples, "dist", 5.0),
            "10s": window_stats(valid_samples, "dist", 10.0),
        },
        "allan_dist": allan_deviation(valid_samples, "dist", [0.01, 0.1, 1.0, 5.0]),
    }


def format_m(value: float | int | None) -> str:
    if value is None or not isinstance(value, (float, int)) or not math.isfinite(float(value)):
        return "nan"
    return f"{float(value):.4f} m"


def format_cm(value: float | int | None) -> str:
    if value is None or not isinstance(value, (float, int)) or not math.isfinite(float(value)):
        return "nan"
    return f"{float(value) * 100.0:.2f} cm"


def format_num(value: object, digits: int = 2) -> str:
    if not isinstance(value, (float, int)) or not math.isfinite(float(value)):
        return "nan"
    return f"{float(value):.{digits}f}"


def render_summary(summary: dict[str, object], capture_path: Path, json_path: Path) -> str:
    dist = summary["dist_valid"]
    filt = summary["dist_filt"]
    smooth = summary["dist_smooth"]
    jumps = summary["jumps_dist_valid"]
    cppm = summary["cppm"]
    windows = summary["windows_dist"]
    allan = summary["allan_dist"]

    assert isinstance(dist, dict)
    assert isinstance(filt, dict)
    assert isinstance(smooth, dict)
    assert isinstance(jumps, dict)
    assert isinstance(cppm, dict)
    assert isinstance(windows, dict)
    assert isinstance(allan, dict)

    lines = [
        "UWB quality capture summary",
        "===========================",
        f"capture_csv: {capture_path}",
        f"summary_json: {json_path}",
        "",
        "Acquisition",
        "-----------",
        f"samples: {summary['samples']}",
        f"valid: {summary['valid_samples']} ({format_num(summary['valid_pct'])}%)",
        f"host_duration_s: {format_num(summary['host_duration_s'], 3)}",
        f"host_hz: {format_num(summary['host_hz'], 1)}",
        f"firmware_hz: {format_num(summary['firmware_hz'], 1)}",
        f"firmware_span_ms: {summary['firmware_span_ms']}",
        f"rx_fail_delta: {summary['rx_fail_delta']}",
        "",
        "Distance brute valide",
        "---------------------",
        f"mean: {format_m(dist['mean'])}",
        f"median: {format_m(dist['median'])}",
        f"std_sigma: {format_cm(dist['std'])}",
        f"mad: {format_cm(dist['mad'])}",
        f"p05/p50/p95: {format_m(dist['p05'])} / {format_m(dist['p50'])} / {format_m(dist['p95'])}",
        f"iqr: {format_cm(dist['iqr'])}",
        f"min/max: {format_m(dist['min'])} / {format_m(dist['max'])}",
        f"peak_to_peak: {format_cm(dist['peak_to_peak'])}",
        f"bias_vs_real: {format_cm(summary['bias_m'])}",
        "",
        "Filtres",
        "-------",
        f"dist_filt sigma: {format_cm(filt['std'])}, p05/p95: {format_m(filt['p05'])} / {format_m(filt['p95'])}",
        f"dist_smooth sigma: {format_cm(smooth['std'])}, p05/p95: {format_m(smooth['p05'])} / {format_m(smooth['p95'])}",
        "",
        "Jumps sample-a-sample",
        "---------------------",
        f"mean_abs: {format_cm(jumps['mean_abs'])}",
        f"p95_abs: {format_cm(jumps['p95_abs'])}",
        f"max_abs: {format_cm(jumps['max_abs'])}",
        f"jumps_gt_10cm: {jumps['gt_10cm']}",
        f"jumps_gt_20cm: {jumps['gt_20cm']}",
        "",
        "Clock offset",
        "------------",
        f"cppm mean: {format_num(cppm['mean'], 3)}",
        f"cppm sigma: {format_num(cppm['std'], 3)}",
        f"corr_dist_cppm: {format_num(summary['corr_dist_cppm'], 4)}",
        "",
        "Derive par fenetre",
        "------------------",
    ]

    for name in ("1s", "5s", "10s"):
        block = windows[name]
        lines.append(
            f"{name}: windows={block['windows']} mean_window_std={format_cm(block['mean_window_std'])} "
            f"std_of_means={format_cm(block['std_of_means'])} drift_pp={format_cm(block['drift_peak_to_peak'])}"
        )

    lines.extend([
        "",
        "Allan deviation distance",
        "------------------------",
    ])
    for tau, value in allan.items():
        lines.append(f"tau={tau}s: {format_cm(value)}")

    lines.append("")
    return "\n".join(lines)


def make_output_paths(out_dir: Path, label: str) -> tuple[Path, Path, Path]:
    timestamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    safe_label = "".join(ch if ch.isalnum() or ch in "-_" else "-" for ch in label).strip("-")
    basename = f"{safe_label}-{timestamp}" if safe_label else timestamp
    out_dir.mkdir(parents=True, exist_ok=True)
    return (
        out_dir / f"{basename}.csv",
        out_dir / f"{basename}-summary.txt",
        out_dir / f"{basename}-summary.json",
    )


def write_capture_csv(path: Path, rows: list[list[str]], header: list[str]) -> None:
    with path.open("w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(header)
        writer.writerows(rows)


def write_json(path: Path, payload: dict[str, object]) -> None:
    with path.open("w") as f:
        json.dump(payload, f, indent=2, sort_keys=True, allow_nan=True)
        f.write("\n")


def maybe_reset_target(snr: str | None) -> None:
    if not snr:
        return
    try:
        subprocess.run(
            ["nrfjprog", "--reset", "-f", "NRF52", "--snr", snr],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
            timeout=5,
        )
    except Exception as exc:  # noqa: BLE001 - keep acquisition usable if reset fails.
        print(f"RESET_WARN {exc}")


def capture(args: argparse.Namespace) -> tuple[list[list[str]], list[Sample], float, list[str]]:
    try:
        import serial
    except ModuleNotFoundError:
        print("pyserial is required. Run with ./.venv/bin/python tools/uwb_quality_capture.py or install pyserial.", file=sys.stderr)
        raise

    raw_rows: list[list[str]] = []
    samples: list[Sample] = []
    info_lines: list[str] = []
    malformed = 0
    raw = bytearray()

    ser = serial.Serial(args.port, args.baud, timeout=0.1)
    try:
        ser.reset_input_buffer()
        maybe_reset_target(args.reset_snr)
        print("Acquisition en cours. Appuie sur Ctrl-C pour arreter et generer le rapport.")
        start = time.time()
        last_print = start
        last_sample_count = 0

        while True:
            if args.seconds is not None and (time.time() - start) >= args.seconds:
                break

            chunk = ser.read(4096)
            if chunk:
                raw.extend(chunk)

            while b"\n" in raw:
                line, _, raw = raw.partition(b"\n")
                text = line.decode("ascii", errors="replace").strip()
                if not text:
                    continue
                if text[0].isdigit():
                    parts = text.split(",")
                    parsed = parse_sample(parts)
                    if parsed is None:
                        malformed += 1
                        continue
                    raw_rows.append(parts)
                    samples.append(parsed)
                else:
                    info_lines.append(text)

            now = time.time()
            if now - last_print >= 1.0:
                delta = len(samples) - last_sample_count
                last_sample_count = len(samples)
                last_print = now
                last_dist = samples[-1].dist if samples else math.nan
                print(f"\rrows={len(samples)} rate~{delta:4d}/s last_dist={last_dist:.2f} m", end="", flush=True)
    except KeyboardInterrupt:
        pass
    finally:
        ser.close()

    print()
    duration = time.time() - start if "start" in locals() else 0.0
    if malformed:
        print(f"ignored_malformed_lines={malformed}")
    return raw_rows, samples, duration, info_lines


def choose_header(rows: list[list[str]]) -> list[str]:
    if not rows:
        return PURE_HEADER
    max_len = max(len(row) for row in rows)
    if max_len == 8:
        return PURE_HEADER
    if max_len >= 32:
        return LEGACY_HEADER_32 + [f"extra_{idx}" for idx in range(32, max_len)]
    return [f"col_{idx}" for idx in range(max_len)]


def pad_rows(rows: list[list[str]], width: int) -> list[list[str]]:
    return [row + [""] * (width - len(row)) for row in rows]


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Capture UWB serial CSV and compute quality metrics after stop.")
    parser.add_argument("--port", default="/dev/cu.usbserial-0001", help="serial port (default: /dev/cu.usbserial-0001)")
    parser.add_argument("--baud", type=int, default=1000000, help="serial baudrate (default: 1000000 for pure UWB)")
    parser.add_argument("--seconds", type=float, default=None, help="optional fixed acquisition duration; otherwise stop with Ctrl-C")
    parser.add_argument("--label", default="uwb-quality", help="output file label")
    parser.add_argument("--out-dir", type=Path, default=Path("out/quality"), help="output directory")
    parser.add_argument("--distance-real", type=float, default=None, help="known real distance in meters, used to compute bias")
    parser.add_argument("--reset-snr", default=None, help="optional J-Link SNR to reset before capture, e.g. 802009545")
    return parser


def main() -> int:
    args = build_arg_parser().parse_args()
    capture_path, summary_path, json_path = make_output_paths(args.out_dir, args.label)

    raw_rows, samples, duration, info_lines = capture(args)
    if not samples:
        print("No samples captured; no report written.")
        return 1

    header = choose_header(raw_rows)
    width = len(header)
    write_capture_csv(capture_path, pad_rows(raw_rows, width), header)

    summary = summarize(samples, duration, args.distance_real)
    payload = {
        "capture_csv": str(capture_path),
        "info_lines": info_lines[:50],
        "summary": summary,
    }
    write_json(json_path, payload)

    rendered = render_summary(summary, capture_path, json_path)
    summary_path.write_text(rendered)
    print(rendered)
    print(f"Wrote: {capture_path}")
    print(f"Wrote: {summary_path}")
    print(f"Wrote: {json_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
