#!/bin/bash
# Install ssd1306-status systemd service
# Usage:  sudo bash install.sh

set -e

DIR="$(cd "$(dirname "$0")" && pwd)"
BIN="$DIR/ssd1306_status"
SRC="$DIR/ssd1306_status.cpp"
SVC="$DIR/ssd1306-status.service"

echo "=== SSD1306 Status Display - Installer ==="

# 1. Build (if needed)
if [ ! -x "$BIN" ] || [ "$SRC" -nt "$BIN" ]; then
    echo "[*] Building $SRC ..."
    g++ -std=c++20 -O2 -Wall -Wextra -o "$BIN" "$SRC"
    echo "[OK] Build done → $BIN"
else
    echo "[OK] Binary is up-to-date"
fi

# 2. Install systemd unit
cp "$SVC" /etc/systemd/system/ssd1306-status.service
echo "[OK] Unit file → /etc/systemd/system/ssd1306-status.service"

systemctl daemon-reload
echo "[OK] systemd reloaded"

systemctl enable ssd1306-status.service
echo "[OK] Enabled for auto-start"

systemctl restart ssd1306-status.service
echo "[OK] Service started"

echo ""
echo "=== Status ==="
systemctl status ssd1306-status.service --no-pager 2>&1 || true

echo ""
echo "=== Commands ==="
echo "  systemctl status   ssd1306-status"
echo "  systemctl restart  ssd1306-status"
echo "  systemctl stop     ssd1306-status"
echo "  journalctl -u ssd1306-status -f"
