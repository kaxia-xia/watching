#!/bin/bash
# 安装 ssd1306-status systemd 服务
# 用法:  sudo bash install.sh

set -e

DIR="$(cd "$(dirname "$0")" && pwd)"
BIN="$DIR/ssd1306_status"
SRC="$DIR/ssd1306_status.cpp"
SVC="$DIR/ssd1306-status.service"

echo "=== SSD1306 系统状态显示 - 安装脚本 ==="

# 1. 编译 (如果需要)
if [ ! -x "$BIN" ] || [ "$SRC" -nt "$BIN" ]; then
    echo "[*] 编译 $SRC ..."
    g++ -std=c++20 -O2 -Wall -Wextra -o "$BIN" "$SRC"
    echo "[OK] 编译完成 → $BIN"
else
    echo "[OK] 二进制已是最新"
fi

# 2. 安装 systemd 服务
cp "$SVC" /etc/systemd/system/ssd1306-status.service
echo "[OK] 服务文件已复制 → /etc/systemd/system/ssd1306-status.service"

systemctl daemon-reload
echo "[OK] systemd 配置已重载"

systemctl enable ssd1306-status.service
echo "[OK] 已设为开机自启"

systemctl restart ssd1306-status.service
echo "[OK] 服务已启动"

echo ""
echo "=== 状态 ==="
systemctl status ssd1306-status.service --no-pager 2>&1 || true

echo ""
echo "=== 常用命令 ==="
echo "  systemctl status ssd1306-status   查看状态"
echo "  systemctl restart ssd1306-status  重启服务"
echo "  systemctl stop ssd1306-status     停止服务"
echo "  journalctl -u ssd1306-status -f   查看日志"
