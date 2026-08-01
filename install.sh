#!/bin/bash
# Install ssd1306-status as a systemd service
# Run this with:  sudo bash install.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SERVICE_FILE="$SCRIPT_DIR/ssd1306-status.service"
PYTHON_SCRIPT="$SCRIPT_DIR/ssd1306_status.py"

echo "=== SSD1306 Status Display Installer ==="

# 1. Make Python script executable
chmod +x "$PYTHON_SCRIPT"
echo "[OK] $PYTHON_SCRIPT is executable"

# 2. Copy service file
cp "$SERVICE_FILE" /etc/systemd/system/ssd1306-status.service
echo "[OK] Service file copied to /etc/systemd/system/"

# 3. Reload systemd
systemctl daemon-reload
echo "[OK] systemd daemon reloaded"

# 4. Enable and start
systemctl enable ssd1306-status.service
echo "[OK] Service enabled (auto-start on boot)"

systemctl start ssd1306-status.service
echo "[OK] Service started"

# 5. Show status
echo ""
echo "=== Service Status ==="
systemctl status ssd1306-status.service --no-pager || true

echo ""
echo "=== Useful Commands ==="
echo "  systemctl status ssd1306-status   # Check status"
echo "  systemctl restart ssd1306-status  # Restart"
echo "  systemctl stop ssd1306-status     # Stop"
echo "  journalctl -u ssd1306-status -f   # Follow logs"
