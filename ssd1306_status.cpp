// SPDX-License-Identifier: GPL-2.0
/*
 * ssd1306_status.cpp  —  SSD1306 OLED 系统状态显示守护进程
 *
 * 在 128×64 SSD1306 OLED (4行×16列) 上实时显示:
 *   第1行: 网络连接类型 + WiFi SSID
 *   第2行: CPU 使用率 + 内存使用率
 *   第3行: WebDAV 服务状态
 *   第4行: MP3采集 服务状态
 *
 * 通过写入 UTF-8 文本到 /dev/ssd1306 字符设备来驱动 OLED。
 * 使用 SSD1306IOC_CLEAR ioctl 清除屏幕。
 *
 * 编译:  g++ -std=c++20 -O2 -Wall -o ssd1306_status ssd1306_status.cpp
 */

#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

// ── ioctl 常量 ────────────────────────────────────────────────────
// _IO('S', 0x01) = (0<<30) | ('S'<<8) | 0x01 = 0x5301
static constexpr unsigned long SSD1306_IOC_CLEAR = 0x5301;

// 显示几何
static constexpr int DISP_COLS = 16;   // 终端列数 (ASCII列)
static constexpr int DISP_ROWS = 4;    // 终端行数
static constexpr int REFRESH_SEC = 3;

static constexpr const char *DEVICE_PATH = "/dev/ssd1306";

// ── UTF-8 辅助 ────────────────────────────────────────────────────

// 返回一个 UTF-8 字节序列的字节长度 (1..4)
static int utf8_byte_len(const char *s) {
    unsigned char c = static_cast<unsigned char>(*s);
    if (c <= 0x7F) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1; // 非法字节, 跳过
}

// 计算 UTF-8 字符串在 OLED 上占用的显示列数
static int utf8_display_width(const std::string &s) {
    int w = 0;
    size_t i = 0;
    while (i < s.size()) {
        int blen = utf8_byte_len(&s[i]);
        w += (blen == 1 && static_cast<unsigned char>(s[i]) <= 0x7F) ? 1 : 2;
        i += blen;
    }
    return w;
}

// 截断 UTF-8 字符串使其显示宽度 ≤ max_cols
// 返回截断后的字符串 (字节层面截断, 保证不切碎多字节字符)
static std::string utf8_truncate(const std::string &s, int max_cols) {
    int w = 0;
    size_t i = 0;
    while (i < s.size() && w < max_cols) {
        int blen = utf8_byte_len(&s[i]);
        int cw = (blen == 1 && static_cast<unsigned char>(s[i]) <= 0x7F) ? 1 : 2;
        if (w + cw > max_cols) break;
        w += cw;
        i += blen;
    }
    return s.substr(0, i);
}

// 将字符串用空格填充到恰好 max_cols 列宽 (末尾补空格)
static std::string utf8_pad(const std::string &s, int max_cols) {
    int cur = utf8_display_width(s);
    std::string result = s;
    while (cur < max_cols) {
        result += ' ';
        cur++;
    }
    return result;
}

// ── 文件读取辅助 ──────────────────────────────────────────────────

static std::string read_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return "";
    char buf[256];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0) return "";
    buf[n] = '\0';
    // 去除末尾换行
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
        buf[--n] = '\0';
    return std::string(buf, n);
}

// ── 命令执行辅助 ──────────────────────────────────────────────────

// 执行命令并返回 stdout 的首行 (去除末尾换行)
static std::string exec_cmd(const char *cmd) {
    FILE *p = popen(cmd, "r");
    if (!p) return "";
    char buf[256];
    std::string result;
    if (fgets(buf, sizeof(buf), p)) {
        result = buf;
        while (!result.empty() &&
               (result.back() == '\n' || result.back() == '\r'))
            result.pop_back();
    }
    pclose(p);
    return result;
}

// 执行命令, 返回退出码 (静默)
static int exec_cmd_silent(const char *cmd) {
    return system(cmd);
}

// ── 数据采集 ──────────────────────────────────────────────────────

// 网络状态
struct NetInfo {
    enum Type { WIFI, WIRED, DOWN };
    Type type;
    std::string ssid;      // WiFi SSID (UTF-8, 可能含中文)
};

static NetInfo get_net_info() {
    NetInfo info;
    info.type = NetInfo::DOWN;

    // 优先检查 wlan0
    std::string op = read_file("/sys/class/net/wlan0/operstate");
    std::string car = read_file("/sys/class/net/wlan0/carrier");
    if (op == "up" && car == "1") {
        info.type = NetInfo::WIFI;
        info.ssid = exec_cmd("iwgetid wlan0 -r 2>/dev/null");
        if (info.ssid.empty()) info.ssid = "?";
        return info;
    }

    // 检查 eth0
    op = read_file("/sys/class/net/eth0/operstate");
    car = read_file("/sys/class/net/eth0/carrier");
    if (op == "up" && car == "1") {
        info.type = NetInfo::WIRED;
        return info;
    }

    return info;
}

// CPU 使用率 (需要前后两个采样点做差值)
struct CpuSample {
    unsigned long long user, nice, system, idle, iowait, irq, softirq;
    unsigned long long total() const {
        return user + nice + system + idle + iowait + irq + softirq;
    }
    unsigned long long idle_sum() const { return idle + iowait; }
};

static CpuSample read_cpu_sample() {
    CpuSample s{};
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return s;
    // 格式: cpu  user nice system idle iowait irq softirq ...
    int n = fscanf(f, "cpu  %llu %llu %llu %llu %llu %llu %llu",
                   &s.user, &s.nice, &s.system, &s.idle,
                   &s.iowait, &s.irq, &s.softirq);
    (void)n;  // /proc/stat 第一行格式保证正确
    fclose(f);
    return s;
}

static double calc_cpu_pct(const CpuSample &prev, const CpuSample &curr) {
    unsigned long long idle_d = curr.idle_sum() - prev.idle_sum();
    unsigned long long total_d = curr.total() - prev.total();
    if (total_d == 0) return 0.0;
    return (1.0 - (double)idle_d / (double)total_d) * 100.0;
}

// 内存使用率 (基于 MemAvailable)
static double get_mem_pct() {
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return 0.0;

    unsigned long long total = 1, available = 0;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        unsigned long long val;
        if (sscanf(line, "MemTotal: %llu kB", &val) == 1)
            total = val;
        else if (sscanf(line, "MemAvailable: %llu kB", &val) == 1) {
            available = val;
            break;
        }
    }
    fclose(f);
    if (available == 0) return 0.0;
    return (double)(total - available) / (double)total * 100.0;
}

// 检查 systemd 服务是否 active
static bool check_service(const char *svc) {
    char cmd[128];
    snprintf(cmd, sizeof(cmd),
             "systemctl is-active --quiet %s 2>/dev/null", svc);
    return exec_cmd_silent(cmd) == 0;
}

// ── 格式化输出 ────────────────────────────────────────────────────

/*
 * 构建 4 行显示字符串 (每行 16 列), 行间用 \n 分隔。
 *
 *  行 列数预算 (CJK=2列, ASCII=1列):
 *    "无线:卡夏的手机"      → 5 + 5×2 = 15 ✅
 *    "有线:已连接"          → 5 + 2×2 = 9  ✅
 *    "网络:已断开"          → 5 + 2×2 = 9  ✅
 *    "C:90% 内:85%"         → 最坏 15, pad→16 ✅
 *    "WebDAV:运行中"        → 7 + 3×2 = 13 ✅
 *    "MP3采集:运行中"       → 8 + 3×2 = 14 ✅
 */
static std::string format_display(const NetInfo &net,
                                   double cpu_pct, double mem_pct,
                                   bool webdav_ok, bool mp3_ok) {
    // ── 第 1 行: 网络 ──
    std::string line1;
    switch (net.type) {
    case NetInfo::WIFI: {
        // "无线:" = 5 列, 剩下 11 列给 SSID (最多 5 个 CJK 汉字)
        std::string ssid_short = utf8_truncate(net.ssid, 11);
        if (ssid_short.empty()) ssid_short = "?";
        line1 = "无线:" + ssid_short;
        break;
    }
    case NetInfo::WIRED:
        line1 = "有线:已连接";
        break;
    default:
        line1 = "网络:已断开";
        break;
    }
    line1 = utf8_pad(line1, DISP_COLS);

    // ── 第 2 行: CPU + 内存 ──
    // "C: 2% 内:18%"=13列, "C:100% 内:100%"=15列, pad 到 16
    char buf2[64];
    snprintf(buf2, sizeof(buf2),
             "C:%2.0f%% 内:%2.0f%%", cpu_pct, mem_pct);
    std::string line2 = utf8_pad(buf2, DISP_COLS);

    // ── 第 3 行: WebDAV ──
    std::string line3 = webdav_ok ? "WebDAV:运行中" : "WebDAV:已停止";
    line3 = utf8_pad(line3, DISP_COLS);

    // ── 第 4 行: MP3 采集 ──
    std::string line4 = mp3_ok ? "MP3采集:运行中" : "MP3采集:已停止";
    line4 = utf8_pad(line4, DISP_COLS);

    return line1 + "\n" + line2 + "\n" + line3 + "\n" + line4;
}

// ── 主循环 ────────────────────────────────────────────────────────

int main() {
    printf("ssd1306_status: OLED 系统状态守护进程启动\n");

    // 等待系统就绪 (网络、服务启动)
    sleep(8);

    // 打开 SSD1306 设备
    int fd = open(DEVICE_PATH, O_WRONLY);
    if (fd < 0) {
        fprintf(stderr, "ssd1306_status: 无法打开 %s: %s\n",
                DEVICE_PATH, strerror(errno));
        return 1;
    }

    // 首次 CPU 采样
    CpuSample prev_cpu = read_cpu_sample();

    for (;;) {
        // ── 采集数据 ──
        NetInfo net = get_net_info();
        double mem_pct = get_mem_pct();
        bool webdav_ok = check_service("webdav-server.service");
        bool mp3_ok = check_service("mp3fetcher.service");

        CpuSample curr_cpu = read_cpu_sample();
        double cpu_pct = calc_cpu_pct(prev_cpu, curr_cpu);
        prev_cpu = curr_cpu;

        // ── 格式化 & 写入 ──
        ioctl(fd, SSD1306_IOC_CLEAR);                     // 清屏
        std::string output = format_display(net, cpu_pct, mem_pct,
                                            webdav_ok, mp3_ok);
        ssize_t written = write(fd, output.data(), output.size());
        if (written < 0) {
            fprintf(stderr, "ssd1306_status: 写入失败: %s\n",
                    strerror(errno));
            // 尝试重新打开设备
            close(fd);
            sleep(5);
            fd = open(DEVICE_PATH, O_WRONLY);
            if (fd < 0) {
                fprintf(stderr, "ssd1306_status: 等待设备恢复...\n");
                sleep(10);
                continue;
            }
            prev_cpu = read_cpu_sample();
        }

        sleep(REFRESH_SEC);
    }

    close(fd);
    return 0;
}
