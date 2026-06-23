#!/usr/bin/env zsh
set -euo pipefail

if [[ $# -lt 2 ]]; then
  print "Usage: $0 <package.zip> <serial-port> [baud]" >&2
  print "Example: $0 out/initiator.zip /dev/cu.usbserial-0001 115200" >&2
  print "Set DFU_SEND_BOOT_CMD=0 if the target is already in bootloader mode." >&2
  exit 2
fi

package_zip="$1"
serial_port="$2"
baud_rate="${3:-${DFU_BAUD:-115200}}"
flow_control="${DFU_FLOW_CONTROL:-0}"
prn="${DFU_PRN:-0}"
timeout_sec="${DFU_TIMEOUT:-30}"
connect_delay_sec="${DFU_CONNECT_DELAY:-0}"
send_boot_cmd="${DFU_SEND_BOOT_CMD:-1}"
app_baud_rate="${DFU_APP_BAUD:-460800}"
attempts="${DFU_ATTEMPTS:-4}"
script_dir="$(cd "$(dirname "$0")" && pwd)"
repo_root="$(cd "$script_dir/../.." && pwd)"
python_bin="${DFU_PYTHON:-$repo_root/.venv/bin/python}"

if [[ ! -x "$python_bin" ]]; then
  python_bin="python3"
fi

if [[ "$send_boot_cmd" != "0" ]]; then
  "$python_bin" - \
    "$serial_port" \
    "$app_baud_rate" \
    "$package_zip" \
    "$baud_rate" \
    "$flow_control" \
    "$prn" \
    "$timeout_sec" \
    "$connect_delay_sec" \
    "$attempts" <<'PY'
import serial
import subprocess
import sys
import time

port = sys.argv[1]
baud = int(sys.argv[2])
package_zip = sys.argv[3]
dfu_baud = sys.argv[4]
flow_control = sys.argv[5]
prn = sys.argv[6]
timeout_sec = sys.argv[7]
connect_delay_sec = sys.argv[8]
attempts = int(sys.argv[9])

cmd = [
    "nrfutil", "nrf5sdk-tools", "dfu", "serial",
    "--package", package_zip,
    "--port", port,
    "--baud-rate", dfu_baud,
    "--flow-control", flow_control,
    "--packet-receipt-notification", prn,
    "--timeout", timeout_sec,
    "--connect-delay", connect_delay_sec,
]

def enter_bootloader():
  ser = serial.Serial(port, baud, timeout=0.02)
  ser.reset_input_buffer()

  raw = bytearray()
  start = time.time()
  next_send = 0.0

  while time.time() - start < 5.0:
    now = time.time()
    if now >= next_send:
      ser.write(b"\nBOOT,DFU\n")
      ser.flush()
      next_send = now + 0.25

    chunk = ser.read(512)
    if not chunk:
      continue

    raw.extend(chunk)
    while b"\n" in raw:
      line, _, raw = raw.partition(b"\n")
      text = line.decode("ascii", "replace").strip()
      if text == "ACK,BOOT_DFU":
        ser.close()
        return True

  ser.close()
  return False

last_exit = 1
for attempt in range(1, attempts + 1):
  print(f"DFU attempt {attempt}/{attempts}: entering bootloader")
  if not enter_bootloader():
    print("BOOT,DFU was not acknowledged", file=sys.stderr)
    time.sleep(2.0)
    continue

  print("BOOT,DFU acknowledged")
  last_exit = subprocess.call(cmd)
  if last_exit == 0:
    sys.exit(0)

  time.sleep(4.0)

sys.exit(last_exit)
PY
else
  nrfutil nrf5sdk-tools dfu serial \
    --package "$package_zip" \
    --port "$serial_port" \
    --baud-rate "$baud_rate" \
    --flow-control "$flow_control" \
    --packet-receipt-notification "$prn" \
    --timeout "$timeout_sec" \
    --connect-delay "$connect_delay_sec"
fi
