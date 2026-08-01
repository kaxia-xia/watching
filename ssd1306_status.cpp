// SPDX-License-Identifier: GPL-2.0
/*
 * ssd1306_status.cpp  —  SSD1306 OLED system status daemon
 *
 * Displays on 128x64 SSD1306 OLED (4 rows x 16 cols):
 *   Line 1: network type + WiFi SSID (UTF-8)
 *   Line 2: CPU % + memory %
 *   Line 3: WebDAV service
 *   Line 4: MP3 fetcher service
 *
 * Writes UTF-8 text to /dev/ssd1306; clears via SSD1306IOC_CLEAR ioctl.
 *
 * Build: g++ -std=c++20 -O2 -Wall -Wextra -o ssd1306_status ssd1306_status.cpp
 */

#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

// ── ioctl ─────────────────────────────────────────────────────────
static constexpr unsigned long SSD1306_IOC_CLEAR = 0x5301;  // _IO('S', 0x01)

static constexpr int COLS   = 16;
static constexpr int REFRESH_S = 3;
static constexpr const char *DEV = "/dev/ssd1306";

// ── tiny UTF-8 helpers (only needed for SSID truncation / pad) ────

static int u8_blen(const char *s) {
    auto c = static_cast<unsigned char>(*s);
    if (c <= 0x7F)     return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

static int u8_width(const std::string &s) {
    int w = 0;
    for (size_t i = 0; i < s.size(); ) {
        int bl = u8_blen(&s[i]);
        w += (bl == 1 && static_cast<unsigned char>(s[i]) <= 0x7F) ? 1 : 2;
        i += bl;
    }
    return w;
}

// truncate to ≤ max display columns, never splitting a multi-byte char
static std::string u8_trunc(const std::string &s, int max_cols) {
    int w = 0;
    size_t i = 0;
    while (i < s.size() && w < max_cols) {
        int bl = u8_blen(&s[i]);
        int cw = (bl == 1 && static_cast<unsigned char>(s[i]) <= 0x7F) ? 1 : 2;
        if (w + cw > max_cols) break;
        w += cw;  i += bl;
    }
    return s.substr(0, i);
}

// strip non-ASCII bytes, keep only printable ASCII
static std::string ascii_only(const std::string &s) {
    std::string r;
    for (unsigned char c : s)
        if (c >= 0x20 && c <= 0x7E) r += (char)c;
    return r;
}

static std::string u8_pad(const std::string &s, int cols) {
    std::string r = s;
    for (int w = u8_width(r); w < cols; ++w) r += ' ';
    return r;
}

// ── helpers ────────────────────────────────────────────────────────

static std::string read_sysfs(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return "";
    char buf[64];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0) return "";
    while (n && (buf[n-1] == '\n' || buf[n-1] == '\r')) --n;
    return std::string(buf, n);
}

static std::string run_cmd(const char *cmd) {
    FILE *p = popen(cmd, "r");
    if (!p) return "";
    char buf[256];
    std::string r;
    if (fgets(buf, sizeof(buf), p)) {
        r = buf;
        while (!r.empty() && (r.back() == '\n' || r.back() == '\r'))
            r.pop_back();
    }
    pclose(p);
    return r;
}

static bool svc_active(const char *name) {
    char cmd[128];
    snprintf(cmd, sizeof(cmd),
             "systemctl is-active --quiet %s 2>/dev/null", name);
    return system(cmd) == 0;
}

// ── data gatherers ─────────────────────────────────────────────────

struct NetInfo { enum { WIFI, WIRED, DOWN } type; std::string ssid; };

static NetInfo get_net() {
    // wlan0 first
    if (read_sysfs("/sys/class/net/wlan0/operstate") == "up" &&
        read_sysfs("/sys/class/net/wlan0/carrier")   == "1") {
        auto ssid = run_cmd("iwgetid wlan0 -r 2>/dev/null");
        if (ssid.empty()) ssid = "?";
        return {NetInfo::WIFI, ssid};
    }
    // eth0
    if (read_sysfs("/sys/class/net/eth0/operstate") == "up" &&
        read_sysfs("/sys/class/net/eth0/carrier")   == "1") {
        return {NetInfo::WIRED, {}};
    }
    return {NetInfo::DOWN, {}};
}

struct CpuSnap {
    unsigned long long usr, nice, sys, idle, iow, irq, sirq;
    auto total() const { return usr + nice + sys + idle + iow + irq + sirq; }
    auto idle2() const { return idle + iow; }
};

static CpuSnap cpu_snap() {
    CpuSnap s{};
    FILE *f = fopen("/proc/stat", "r");
    if (f) {
        int n __attribute__((unused)) =
            fscanf(f, "cpu  %llu %llu %llu %llu %llu %llu %llu",
                   &s.usr, &s.nice, &s.sys, &s.idle, &s.iow, &s.irq, &s.sirq);
        fclose(f);
    }
    return s;
}

static double cpu_pct(const CpuSnap &a, const CpuSnap &b) {
    auto dd = b.idle2() - a.idle2();
    auto dt = b.total() - a.total();
    return dt ? (1.0 - (double)dd / (double)dt) * 100.0 : 0.0;
}

static double mem_pct() {
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return 0.0;
    unsigned long long total = 1, avail = 0;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        unsigned long long v;
        if      (sscanf(line, "MemTotal:     %llu", &v) == 1) total = v;
        else if (sscanf(line, "MemAvailable: %llu", &v) == 1) { avail = v; break; }
    }
    fclose(f);
    return (double)(total - avail) / (double)total * 100.0;
}

// ── display formatting ─────────────────────────────────────────────

/*
 * Layout (all ASCII except SSID, 16 cols each):
 *
 *   WiFi:MySSID       ← "WiFi:" (5) + SSID (up to 11 cols)
 *   Eth:up            ← wired
 *   NET:DOWN          ← no link
 *
 *   CPU: 3% MEM:17%   ← max 15 cols
 *   webdav     OK     ← "webdav" (6) + pad + "OK"/"DOWN"
 *   mp3fetch   OK     ← "mp3fetch" (8) + pad + "OK"/"DOWN"
 */
static std::string build_screen(const NetInfo &net,
                                 double cpu, double mem,
                                 bool webdav, bool mp3) {
    // line 1 – network
    std::string l1;
    switch (net.type) {
    case NetInfo::WIFI:
        l1 = "WiFi:" + u8_trunc(ascii_only(net.ssid), 11);   // 5 + ≤11 = 16
        break;
    case NetInfo::WIRED:
        l1 = "Eth:up";
        break;
    default:
        l1 = "NET:DOWN";
        break;
    }
    l1 = u8_pad(l1, COLS);

    // line 2 – cpu / mem  (max: "C:100% M:100%"=14 cols)
    char b2[32];
    snprintf(b2, sizeof(b2), "C:%2.0f%% M:%2.0f%%", cpu, mem);
    std::string l2 = u8_pad(b2, COLS);

    // line 3 – webdav
    std::string l3 = "webdav  ";
    l3 += webdav ? " OK" : "DOWN";
    l3 = u8_pad(l3, COLS);

    // line 4 – mp3fetcher (NO pad — pad would trigger auto-wrap → scroll → blank)
    std::string l4 = "mp3fetch ";
    l4 += mp3 ? " OK" : "DOWN";
    l4 = u8_pad(l4, COLS - 1);   // pad to 15, so col never hits 16

    // No explicit newlines — driver auto-wraps at column 16.
    return l1 + l2 + l3 + l4;
}

// ── main ───────────────────────────────────────────────────────────

int main() {
    printf("ssd1306_status: starting\n");

    sleep(8);   // let network + services settle after boot

    int fd = open(DEV, O_WRONLY);
    if (fd < 0) {
        fprintf(stderr, "ssd1306_status: open %s: %s\n", DEV, strerror(errno));
        return 1;
    }

    auto prev = cpu_snap();

    for (;;) {
        NetInfo net = get_net();
        double mem  = mem_pct();
        bool wd     = svc_active("webdav-server.service");
        bool mp3    = svc_active("mp3fetcher.service");

        auto cur = cpu_snap();
        double c  = cpu_pct(prev, cur);
        prev = cur;

        ioctl(fd, SSD1306_IOC_CLEAR);
        auto out = build_screen(net, c, mem, wd, mp3);

        if (write(fd, out.data(), out.size()) < 0) {
            fprintf(stderr, "ssd1306_status: write: %s\n", strerror(errno));
            close(fd);
            sleep(5);
            fd = open(DEV, O_WRONLY);
            if (fd < 0) {
                fprintf(stderr, "ssd1306_status: waiting for %s...\n", DEV);
                sleep(10);
                continue;
            }
            prev = cpu_snap();
        }

        sleep(REFRESH_S);
    }
}
