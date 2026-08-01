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

// ── tiny helpers ──────────────────────────────────────────────────

// truncate UTF-8 s to ≤ max_cols display columns (never splits a char)
static std::string dsp_trunc(const std::string &s, int max_cols) {
    int w = 0;
    size_t i = 0;
    while (i < s.size() && w < max_cols) {
        auto c = static_cast<unsigned char>(s[i]);
        int bl, cw;
        if      (c <= 0x7F)           { bl = 1; cw = 1; }
        else if ((c & 0xE0) == 0xC0) { bl = 2; cw = 2; }
        else if ((c & 0xF0) == 0xE0) { bl = 3; cw = 2; }
        else if ((c & 0xF8) == 0xF0) { bl = 4; cw = 2; }
        else                         { bl = 1; cw = 1; }
        if (w + cw > max_cols) break;
        w += cw; i += bl;
    }
    return s.substr(0, i);
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
 * No padding at all — just write content + \n for each line.
 * Padding to 16 triggers driver auto-wrap, then \n does another
 * line advance → double-spacing.  \n alone positions to next row
 * correctly without any auto-wrap side-effects.
 */
static std::string build_screen(const NetInfo &net,
                                 double cpu, double mem,
                                 bool webdav, bool mp3) {
    // line 1 – network
    std::string l1;
    switch (net.type) {
    case NetInfo::WIFI:
        // "WiFi:" = 5 cols,  SSID max 11 cols → total ≤ 16
        l1 = "WiFi:" + dsp_trunc(net.ssid, 11);
        break;
    case NetInfo::WIRED:
        l1 = "Eth:up";
        break;
    default:
        l1 = "NET:DOWN";
        break;
    }

    // line 2 – cpu / mem  (≥100% → "F", else "NN%")
    char b2[32];
    auto fmt = [](double v) -> std::string {
        if (v >= 100.0) return "F";
        char buf[8];
        snprintf(buf, sizeof(buf), "%2.0f%%", v);
        return buf;
    };
    snprintf(b2, sizeof(b2), "CPU %s MEM %s", fmt(cpu).c_str(), fmt(mem).c_str());

    // line 3 – webdav
    std::string l3 = "webdav  ";
    l3 += webdav ? " OK" : "DOWN";

    // line 4 – mp3fetcher
    std::string l4 = "mp3fetch ";
    l4 += mp3 ? " OK" : "DOWN";

    return l1 + "\n" + b2 + "\n" + l3 + "\n" + l4;
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
