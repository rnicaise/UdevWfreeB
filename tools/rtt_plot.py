#!/usr/bin/env python3
"""
rtt_plot.py — Visualisation temps réel de la distance UWB via RTT (SWD)

Lit la sortie RTT du responder (CSV: count,distance,tof) via JLinkRTTLogger
et affiche la distance sur un graphique matplotlib en temps réel.

Beaucoup plus rapide que le BLE (pas de throttle CoreBluetooth).

Usage:
    source .venv/bin/activate
    python tools/rtt_plot.py [--serial 760220908]

Nécessite JLinkRTTLogger dans le PATH (installé avec J-Link Software).
"""

import argparse
import re
import subprocess
import sys
import tempfile
import threading
import time
from collections import deque

try:
    import matplotlib.pyplot as plt
    import matplotlib.animation as animation
except ImportError:
    print("pip install matplotlib")
    sys.exit(1)

# ── Config ──
MAX_POINTS = 500
DEVICE = "NRF52833_XXAA"

# ── Data buffers ──
timestamps = deque(maxlen=MAX_POINTS)
distances = deque(maxlen=MAX_POINTS)
counts = deque(maxlen=MAX_POINTS)
lock = threading.Lock()
t0 = [None]

# Regex for CSV line: count,distance,tof
CSV_RE = re.compile(r"^(\d+),([\d.+-]+),([\d.eE+-]+)")


def rtt_reader(serial: str):
    """Read RTT output from JLinkRTTLogger and parse CSV lines."""
    tmp = tempfile.NamedTemporaryFile(suffix=".log", delete=False)
    tmp.close()

    cmd = [
        "JLinkRTTLogger",
        "-Device", DEVICE,
        "-If", "SWD",
        "-Speed", "4000",
        "-RTTChannel", "0",
    ]
    if serial:
        cmd += ["-SelectEmuBySN", serial]
    cmd.append(tmp.name)

    print(f"Starting: {' '.join(cmd)}")
    proc = subprocess.Popen(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )

    # Wait for RTT to connect then read the log file
    time.sleep(2)
    print("Connected — reading RTT...\n")

    with open(tmp.name, "r") as f:
        while proc.poll() is None:
            line = f.readline()
            if not line:
                time.sleep(0.01)
                continue

            line = line.strip()
            m = CSV_RE.match(line)
            if not m:
                if line and not line.startswith("#"):
                    pass  # skip non-CSV lines
                continue

            count = int(m.group(1))
            distance = float(m.group(2))

            now = time.monotonic()
            if t0[0] is None:
                t0[0] = now

            elapsed = now - t0[0]

            with lock:
                timestamps.append(elapsed)
                distances.append(distance)
                counts.append(count)

            print(f"  #{count:5d}  dist={distance:6.2f} m")


def main():
    parser = argparse.ArgumentParser(description="UWB distance plot via RTT")
    parser.add_argument("--serial", "-s", default="760220908",
                        help="J-Link serial number (default: 760220908 = responder)")
    args = parser.parse_args()

    # Start RTT reader thread
    reader = threading.Thread(target=rtt_reader, args=(args.serial,), daemon=True)
    reader.start()

    # ── Matplotlib real-time plot ──
    fig, ax = plt.subplots(figsize=(10, 5))
    fig.canvas.manager.set_window_title("UWB Distance — RTT Monitor")
    (line,) = ax.plot([], [], "b-o", markersize=2, linewidth=1, label="Distance")

    ax.set_xlabel("Temps (s)")
    ax.set_ylabel("Distance (m)")
    ax.set_title("Distance UWB en temps réel (via RTT)")
    ax.legend(loc="upper right")
    ax.grid(True, alpha=0.3)

    stats_text = ax.text(
        0.02, 0.95, "", transform=ax.transAxes,
        fontsize=9, verticalalignment="top",
        bbox=dict(boxstyle="round", facecolor="wheat", alpha=0.5),
    )

    def update(frame):
        with lock:
            if len(timestamps) < 2:
                return (line, stats_text)

            t = list(timestamps)
            d = list(distances)
            c = list(counts)

        line.set_data(t, d)
        ax.set_xlim(max(0, t[-1] - 30), t[-1] + 1)

        recent = d[-50:] if len(d) >= 50 else d
        d_min, d_max = min(recent), max(recent)
        margin = max(0.1, (d_max - d_min) * 0.2)
        ax.set_ylim(d_min - margin, d_max + margin)

        avg = sum(recent) / len(recent)
        std = (sum((x - avg) ** 2 for x in recent) / len(recent)) ** 0.5

        # Rate calculation
        if len(t) >= 10:
            rate = (len(t) - 1) / (t[-1] - t[0]) if t[-1] > t[0] else 0
        else:
            rate = 0

        stats_text.set_text(
            f"Dernière: {d[-1]:.2f} m\n"
            f"Moyenne:  {avg:.2f} m\n"
            f"Écart-type: {std:.3f} m\n"
            f"Mesures: {c[-1]}\n"
            f"Rate: {rate:.1f} Hz"
        )

        return (line, stats_text)

    ani = animation.FuncAnimation(fig, update, interval=50, blit=False, cache_frame_data=False)
    plt.tight_layout()
    plt.show()


if __name__ == "__main__":
    main()
