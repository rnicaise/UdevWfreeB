#!/usr/bin/env python3
"""
ble_plot.py — Visualisation temps réel de la distance UWB via BLE

Scanne les paquets BLE advertising du device "UWB" (responder DWM3001CDK)
et affiche la distance sur un graphique matplotlib en temps réel.

Usage:
    pip install bleak matplotlib
    python tools/ble_plot.py

Le device "UWB" émet des ADV_NONCONN_IND avec Manufacturer Specific Data:
    Company ID: 0xFFFF
    Payload: [dist_cm_lo, dist_cm_hi, count_lo, count_hi]
"""

import asyncio
import struct
import sys
from collections import deque
from datetime import datetime

try:
    from bleak import BleakScanner
except ImportError:
    print("pip install bleak")
    sys.exit(1)

try:
    import matplotlib.pyplot as plt
    import matplotlib.animation as animation
except ImportError:
    print("pip install matplotlib")
    sys.exit(1)

# ── Config ──
DEVICE_NAME = "UWB"
COMPANY_ID = 0xFFFF
MAX_POINTS = 200  # Nombre de points affichés

# ── Data buffers ──
timestamps = deque(maxlen=MAX_POINTS)
distances = deque(maxlen=MAX_POINTS)
counts = deque(maxlen=MAX_POINTS)
last_count = [0]
t0 = [None]


def parse_manufacturer_data(mfr_data: dict):
    """Parse manufacturer specific data from BLE advertisement."""
    if COMPANY_ID not in mfr_data:
        return None, None

    raw = mfr_data[COMPANY_ID]
    if len(raw) < 4:
        return None, None

    dist_cm = struct.unpack_from("<h", raw, 0)[0]  # int16 LE
    count = struct.unpack_from("<H", raw, 2)[0]     # uint16 LE

    return dist_cm / 100.0, count


def detection_callback(device, advertisement_data):
    """Called for each BLE advertisement detected."""
    if device.name != DEVICE_NAME:
        return

    mfr = advertisement_data.manufacturer_data
    if not mfr:
        return

    distance_m, count = parse_manufacturer_data(mfr)
    if distance_m is None:
        return

    # Skip duplicates (same counter value)
    if count == last_count[0] and len(distances) > 0:
        return
    last_count[0] = count

    now = datetime.now()
    if t0[0] is None:
        t0[0] = now

    elapsed = (now - t0[0]).total_seconds()
    timestamps.append(elapsed)
    distances.append(distance_m)
    counts.append(count)

    print(f"  #{count:5d}  dist={distance_m:6.2f} m  RSSI={advertisement_data.rssi} dBm")


async def run_scanner():
    """Start BLE scanning in background."""
    scanner = BleakScanner(detection_callback=detection_callback)
    await scanner.start()
    print(f"Scanning for BLE device '{DEVICE_NAME}'...")
    print("Waiting for data...\n")

    # Keep running until cancelled
    try:
        while True:
            await asyncio.sleep(0.1)
    except asyncio.CancelledError:
        await scanner.stop()


def main():
    # Start BLE scanner in background thread
    loop = asyncio.new_event_loop()

    import threading
    scanner_thread = threading.Thread(
        target=lambda: loop.run_until_complete(run_scanner()),
        daemon=True,
    )
    scanner_thread.start()

    # ── Matplotlib real-time plot ──
    fig, ax = plt.subplots(figsize=(10, 5))
    fig.canvas.manager.set_window_title("UWB Distance — BLE Monitor")
    (line,) = ax.plot([], [], "b-o", markersize=3, linewidth=1.5, label="Distance")

    ax.set_xlabel("Temps (s)")
    ax.set_ylabel("Distance (m)")
    ax.set_title("Distance UWB en temps réel (via BLE)")
    ax.legend(loc="upper right")
    ax.grid(True, alpha=0.3)

    # Stats text
    stats_text = ax.text(
        0.02, 0.95, "", transform=ax.transAxes,
        fontsize=9, verticalalignment="top",
        bbox=dict(boxstyle="round", facecolor="wheat", alpha=0.5),
    )

    def update(frame):
        if len(timestamps) < 2:
            return (line, stats_text)

        t = list(timestamps)
        d = list(distances)

        line.set_data(t, d)
        ax.set_xlim(max(0, t[-1] - 30), t[-1] + 1)

        d_min = min(d[-50:]) if len(d) >= 50 else min(d)
        d_max = max(d[-50:]) if len(d) >= 50 else max(d)
        margin = max(0.1, (d_max - d_min) * 0.2)
        ax.set_ylim(d_min - margin, d_max + margin)

        # Stats
        recent = d[-20:] if len(d) >= 20 else d
        avg = sum(recent) / len(recent)
        std = (sum((x - avg) ** 2 for x in recent) / len(recent)) ** 0.5
        stats_text.set_text(
            f"Dernière: {d[-1]:.2f} m\n"
            f"Moyenne:  {avg:.2f} m\n"
            f"Écart-type: {std:.3f} m\n"
            f"Mesures: {counts[-1] if counts else 0}"
        )

        return (line, stats_text)

    ani = animation.FuncAnimation(fig, update, interval=100, blit=False, cache_frame_data=False)
    plt.tight_layout()
    plt.show()


if __name__ == "__main__":
    main()
