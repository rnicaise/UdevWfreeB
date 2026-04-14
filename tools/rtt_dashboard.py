#!/usr/bin/env python3
"""
rtt_dashboard.py — Dashboard temps réel : Distance + Accéléromètres (initiator & responder)

Lit la sortie RTT du responder via JLinkRTTLogger et affiche 3 graphiques :
  1. Distance UWB (m) vs temps
  2. Accéléromètre initiator X/Y/Z (mg) vs temps
  3. Accéléromètre responder X/Y/Z (mg) vs temps

CSV attendu : ms,sample,dist,iax,iay,iaz,rax,ray,raz

Usage:
    source .venv/bin/activate
    python tools/rtt_dashboard.py [--serial 760220908]
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
ts_buf = deque(maxlen=MAX_POINTS)      # temps (s)
dist_buf = deque(maxlen=MAX_POINTS)    # distance (m)
iax_buf = deque(maxlen=MAX_POINTS)     # initiator accel X
iay_buf = deque(maxlen=MAX_POINTS)
iaz_buf = deque(maxlen=MAX_POINTS)
rax_buf = deque(maxlen=MAX_POINTS)     # responder accel X
ray_buf = deque(maxlen=MAX_POINTS)
raz_buf = deque(maxlen=MAX_POINTS)
lock = threading.Lock()
stats = {"count": 0, "last_hz": 0.0, "hz_t0": None, "hz_n0": 0}

# CSV: ms,sample,dist,iax,iay,iaz,rax,ray,raz
CSV_RE = re.compile(
    r"^(\d+),(\d+),([\d.+-]+),([-\d]+),([-\d]+),([-\d]+),([-\d]+),([-\d]+),([-\d]+)"
)


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
        "-RTTSearchRanges", "0x20000000 0x20020000",
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

    time.sleep(2)
    print("Connected — reading RTT...\n")

    with open(tmp.name, "r") as f:
        while proc.poll() is None:
            line = f.readline()
            if not line:
                time.sleep(0.005)
                continue

            line = line.strip()
            m = CSV_RE.match(line)
            if not m:
                continue

            ms = int(m.group(1))
            sample = int(m.group(2))
            dist = float(m.group(3))
            iax, iay, iaz = int(m.group(4)), int(m.group(5)), int(m.group(6))
            rax, ray, raz = int(m.group(7)), int(m.group(8)), int(m.group(9))

            t_sec = ms / 1000.0

            with lock:
                ts_buf.append(t_sec)
                dist_buf.append(dist)
                iax_buf.append(iax)
                iay_buf.append(iay)
                iaz_buf.append(iaz)
                rax_buf.append(rax)
                ray_buf.append(ray)
                raz_buf.append(raz)
                stats["count"] = sample

                # Hz calculation
                now = time.monotonic()
                if stats["hz_t0"] is None:
                    stats["hz_t0"] = now
                    stats["hz_n0"] = sample
                elif now - stats["hz_t0"] > 1.0:
                    dt = now - stats["hz_t0"]
                    dn = sample - stats["hz_n0"]
                    stats["last_hz"] = dn / dt
                    stats["hz_t0"] = now
                    stats["hz_n0"] = sample


def main():
    parser = argparse.ArgumentParser(description="UWB + Accel dashboard via RTT")
    parser.add_argument("--serial", "-s", default="760220908",
                        help="J-Link serial (default: 760220908 = responder)")
    args = parser.parse_args()

    # Start RTT reader thread
    reader = threading.Thread(target=rtt_reader, args=(args.serial,), daemon=True)
    reader.start()

    # ── Matplotlib ──
    fig, (ax_dist, ax_iaccel, ax_raccel) = plt.subplots(3, 1, figsize=(12, 8), sharex=True)
    fig.canvas.manager.set_window_title("UWB Dashboard — Distance + Accéléromètres")
    fig.subplots_adjust(hspace=0.25)

    # Distance plot
    ln_dist, = ax_dist.plot([], [], "b-", linewidth=1, label="Distance")
    ax_dist.set_ylabel("Distance (m)")
    ax_dist.set_title("Distance UWB")
    ax_dist.grid(True, alpha=0.3)
    ax_dist.legend(loc="upper right")
    txt_dist = ax_dist.text(0.02, 0.92, "", transform=ax_dist.transAxes, fontsize=9,
                            verticalalignment="top", bbox=dict(boxstyle="round", alpha=0.8, facecolor="wheat"))

    # Initiator accel plot
    ln_iax, = ax_iaccel.plot([], [], "r-", linewidth=0.8, label="X")
    ln_iay, = ax_iaccel.plot([], [], "g-", linewidth=0.8, label="Y")
    ln_iaz, = ax_iaccel.plot([], [], "b-", linewidth=0.8, label="Z")
    ax_iaccel.set_ylabel("Accel (mg)")
    ax_iaccel.set_title("Accéléromètre Initiator")
    ax_iaccel.grid(True, alpha=0.3)
    ax_iaccel.legend(loc="upper right", ncol=3)

    # Responder accel plot
    ln_rax, = ax_raccel.plot([], [], "r-", linewidth=0.8, label="X")
    ln_ray, = ax_raccel.plot([], [], "g-", linewidth=0.8, label="Y")
    ln_raz, = ax_raccel.plot([], [], "b-", linewidth=0.8, label="Z")
    ax_raccel.set_ylabel("Accel (mg)")
    ax_raccel.set_xlabel("Temps (s)")
    ax_raccel.set_title("Accéléromètre Responder")
    ax_raccel.grid(True, alpha=0.3)
    ax_raccel.legend(loc="upper right", ncol=3)

    def update(frame):
        with lock:
            if not ts_buf:
                return (ln_dist, ln_iax, ln_iay, ln_iaz, ln_rax, ln_ray, ln_raz, txt_dist)

            t = list(ts_buf)
            d = list(dist_buf)
            ix, iy, iz = list(iax_buf), list(iay_buf), list(iaz_buf)
            rx, ry, rz = list(rax_buf), list(ray_buf), list(raz_buf)
            cnt = stats["count"]
            hz = stats["last_hz"]

        # Distance
        ln_dist.set_data(t, d)
        ax_dist.set_xlim(t[0], t[-1] + 0.1)
        if d:
            dmin, dmax = min(d), max(d)
            margin = max(0.05, (dmax - dmin) * 0.2)
            ax_dist.set_ylim(dmin - margin, dmax + margin)
        txt_dist.set_text(f"#{cnt}  {hz:.0f} Hz  {d[-1]:.3f} m")

        # Initiator accel
        ln_iax.set_data(t, ix)
        ln_iay.set_data(t, iy)
        ln_iaz.set_data(t, iz)
        all_i = ix + iy + iz
        if all_i:
            imin, imax = min(all_i), max(all_i)
            margin = max(20, (imax - imin) * 0.1)
            ax_iaccel.set_ylim(imin - margin, imax + margin)
        ax_iaccel.set_xlim(t[0], t[-1] + 0.1)

        # Responder accel
        ln_rax.set_data(t, rx)
        ln_ray.set_data(t, ry)
        ln_raz.set_data(t, rz)
        all_r = rx + ry + rz
        if all_r:
            rmin, rmax = min(all_r), max(all_r)
            margin = max(20, (rmax - rmin) * 0.1)
            ax_raccel.set_ylim(rmin - margin, rmax + margin)
        ax_raccel.set_xlim(t[0], t[-1] + 0.1)

        return (ln_dist, ln_iax, ln_iay, ln_iaz, ln_rax, ln_ray, ln_raz, txt_dist)

    ani = animation.FuncAnimation(fig, update, interval=50, blit=False, cache_frame_data=False)
    plt.tight_layout()
    plt.show()


if __name__ == "__main__":
    main()
