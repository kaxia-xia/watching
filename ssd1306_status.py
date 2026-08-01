#!/usr/bin/env python3
"""
SSD1306 System Status Display Daemon
=====================================
Displays real-time system information on a 128x64 SSD1306 OLED (4 rows × 16 cols):
  Line 1: Network type + SSID (WiFi), or "Eth" (wired), or "DOWN"
  Line 2: CPU usage % and Memory usage %
  Line 3: webdav-server.service status
  Line 4: mp3fetcher.service status

Writes UTF-8 text to /dev/ssd1306 (kernel driver presents it as a terminal).
Uses SSD1306IOC_CLEAR ioctl to clear before each refresh.

Intended to run as a systemd service at boot.
"""

import os
import re
import sys
import time
import struct
import fcntl
import subprocess
from pathlib import Path

# ── Constants ────────────────────────────────────────────────────────

DEVICE = "/dev/ssd1306"

# ioctl _IO('S', 0x01) on arm64:
#   _IOC_NONE=0, type='S'=0x53, nr=0x01
#   = (0<<30) | (0x53<<8) | (0x01<<0) = 0x5301
SSD1306_IOC_CLEAR = 0x5301

COLS = 16          # terminal columns
ROWS = 4           # terminal rows
REFRESH_SEC = 3    # seconds between display updates

# ── Display helpers ──────────────────────────────────────────────────

def clear_display(fd: int) -> None:
    """Issue SSD1306IOC_CLEAR ioctl — clears screen and homes cursor."""
    try:
        fcntl.ioctl(fd, SSD1306_IOC_CLEAR)
    except OSError:
        pass  # driver may be temporarily busy


def pad16(text: str) -> str:
    """Pad or truncate a string to exactly 16 bytes (ASCII-safe width).
    Most status text is ASCII, so byte-length ≈ display width."""
    b = text.encode("utf-8", errors="replace")[:COLS]
    return b.ljust(COLS).decode("utf-8", errors="replace")


# ── Data gatherers ───────────────────────────────────────────────────

def get_network_info() -> tuple:
    """
    Returns (type_str, ssid_or_iface, is_up).

    type_str:  "WiFi" | "Eth" | "DOWN"
    ssid_or_iface: SSID for WiFi, interface name for Eth, "" for DOWN
    is_up: True if a link is established
    """
    # --- Check wlan0 first ---
    wlan = Path("/sys/class/net/wlan0")
    if wlan.exists():
        operstate = (wlan / "operstate").read_text().strip()
        carrier = "0"
        cp = wlan / "carrier"
        if cp.exists():
            carrier = cp.read_text().strip()
        if operstate == "up" and carrier == "1":
            # Get SSID
            try:
                r = subprocess.run(
                    ["iwgetid", "wlan0", "-r"],
                    capture_output=True, text=True, timeout=3,
                )
                ssid = r.stdout.strip()
                if not ssid:
                    ssid = "?"
            except Exception:
                ssid = "?"
            return ("WiFi", ssid, True)

    # --- Check eth0 ---
    eth = Path("/sys/class/net/eth0")
    if eth.exists():
        operstate = (eth / "operstate").read_text().strip()
        carrier = "0"
        cp = eth / "carrier"
        if cp.exists():
            carrier = cp.read_text().strip()
        if operstate == "up" and carrier == "1":
            return ("Eth", "eth0", True)

    return ("DOWN", "", False)


def read_cpu_raw() -> list:
    """Read first CPU line from /proc/stat, return list of 7 ints."""
    with open("/proc/stat", "r") as f:
        line = f.readline()
    parts = line.split()
    # cpu  user nice system idle iowait irq softirq ...
    return [int(x) for x in parts[1:8]]


def calc_cpu_pct(prev: list, curr: list) -> float:
    """Calculate CPU usage % between two /proc/stat snapshots."""
    idle_prev = prev[3] + prev[4]   # idle + iowait
    total_prev = sum(prev)
    idle_curr = curr[3] + curr[4]
    total_curr = sum(curr)

    idle_delta = idle_curr - idle_prev
    total_delta = total_curr - total_prev

    if total_delta <= 0:
        return 0.0
    return round((1.0 - idle_delta / total_delta) * 100.0, 1)


def get_mem_pct() -> float:
    """Return memory usage % from /proc/meminfo (uses MemAvailable)."""
    total = 1
    available = 0
    with open("/proc/meminfo", "r") as f:
        for line in f:
            if line.startswith("MemTotal:"):
                total = int(line.split()[1])
            elif line.startswith("MemAvailable:"):
                available = int(line.split()[1])
                break
            elif line.startswith("MemFree:") and available == 0:
                # fallback if MemAvailable not seen yet
                pass
    used = total - available
    return round(used / total * 100.0, 1)


def check_service(name: str) -> bool:
    """Return True if the systemd service is active."""
    try:
        rc = subprocess.call(
            ["systemctl", "is-active", "--quiet", name],
            timeout=5,
        )
        return rc == 0
    except Exception:
        return False


# ── Display formatting ───────────────────────────────────────────────

def format_lines(
    net_type: str,
    net_id: str,
    net_up: bool,
    cpu_pct: float,
    mem_pct: float,
    webdav_ok: bool,
    mp3_ok: bool,
) -> str:
    """Build a 4-line string (16 cols each) for the OLED."""

    # Line 1: Network
    if net_type == "WiFi":
        # SSID is UTF-8; CJK chars are 2 columns wide on display.
        # "W:" = 2 cols, leaving 14 cols for SSID.
        # Truncate to 14 bytes (≈ ASCII-safe), driver handles CJK width.
        ssid_bytes = net_id.encode("utf-8", errors="replace")[:14]
        ssid = ssid_bytes.decode("utf-8", errors="replace")
        line1 = f"W:{ssid}"
    elif net_type == "Eth":
        line1 = "E:eth0  UP"
    else:
        line1 = "NET:  DOWN"

    line1 = pad16(line1)

    # Line 2: CPU + Memory
    line2 = f"CPU:{cpu_pct:5.1f}% M:{mem_pct:4.1f}%"
    line2 = pad16(line2)

    # Line 3: webdav
    s = "OK" if webdav_ok else "DOWN"
    line3 = f"webdav:    {s}"
    line3 = pad16(line3)

    # Line 4: mp3fetcher
    s = "OK" if mp3_ok else "DOWN"
    line4 = f"mp3fetch:  {s}"
    line4 = pad16(line4)

    return "\n".join([line1, line2, line3, line4])


# ── Main loop ────────────────────────────────────────────────────────

def main() -> None:
    print("ssd1306_status: starting system status display daemon", flush=True)

    # Wait for system to settle after boot (network, services up)
    time.sleep(8)

    # Open SSD1306 device
    try:
        fd = os.open(DEVICE, os.O_WRONLY)
    except OSError as e:
        print(f"ssd1306_status: cannot open {DEVICE}: {e}", file=sys.stderr)
        sys.exit(1)

    # Prime CPU measurement: first sample now, second after other checks
    prev_cpu = read_cpu_raw()

    while True:
        try:
            # ── Gather everything ──
            net_type, net_id, net_up = get_network_info()
            mem_pct = get_mem_pct()
            webdav_ok = check_service("webdav-server.service")
            mp3_ok = check_service("mp3fetcher.service")

            # CPU delta since last iteration
            curr_cpu = read_cpu_raw()
            cpu_pct = calc_cpu_pct(prev_cpu, curr_cpu)
            prev_cpu = curr_cpu

            # ── Format & write ──
            clear_display(fd)
            output = format_lines(net_type, net_id, net_up,
                                  cpu_pct, mem_pct, webdav_ok, mp3_ok)
            os.write(fd, output.encode("utf-8"))

        except OSError as e:
            print(f"ssd1306_status: device error: {e}", file=sys.stderr)
            # Try to reopen
            try:
                os.close(fd)
            except Exception:
                pass
            try:
                fd = os.open(DEVICE, os.O_WRONLY)
                prev_cpu = read_cpu_raw()
            except OSError:
                print("ssd1306_status: waiting for device...", file=sys.stderr)
                time.sleep(10)
                continue

        except Exception as e:
            print(f"ssd1306_status: unexpected error: {e}", file=sys.stderr)

        time.sleep(REFRESH_SEC)


if __name__ == "__main__":
    main()
