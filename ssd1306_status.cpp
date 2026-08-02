// SPDX-License-Identifier: GPL-2.0
/*
 * ssd1306_status.cpp  —  SSD1306 OLED system status daemon
 *
 * Displays on 128x64 SSD1306 OLED (4 rows x 16 cols):
 *   Line 1: network type + WiFi SSID
 *   Line 2: CPU % + memory %
 *   Line 3: WebDAV service
 *   Line 4: MP3 fetcher service
 *
 * Incremental refresh: only rewrites lines that actually changed.
 * Uses ANSI arrow-key escapes (ESC [ A / B / C / D) to navigate
 * within the terminal, and \r to return to column 0.
 *
 * Network-change detection: listens on a netlink socket for
 * RTMGRP_LINK & RTMGRP_IPV4_IFADDR events.  When the network
 * state changes the whole screen is repainted immediately.
 *
 * Build: g++ -std=c++20 -O2 -Wall -Wextra -o ssd1306_status ssd1306_status.cpp
 */

#include <sys/ioctl.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <linux/netlink.h>
#include <linux/rtnetlink.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static constexpr unsigned long SSD1306_IOC_CLEAR = 0x5301;  // _IO('S',0x01)
static constexpr int  ROWS       = 4;
static constexpr int  COLS       = 16;
static constexpr int  REFRESH_S  = 3;
static constexpr const char *DEV = "/dev/ssd1306";

// ── ANSI cursor movement ──────────────────────────────────────────
// "\x1b" = ESC, then '[' + letter: A=up B=down C=right D=left
static const char *UP(int n) {
    static char buf[32];
    int pos = 0;
    for (int i = 0; i < n; i++) { buf[pos++]='\x1b'; buf[pos++]='['; buf[pos++]='A'; }
    buf[pos] = '\0';
    return buf;
}
static const char *DN(int n) {
    static char buf[32];
    int pos = 0;
    for (int i = 0; i < n; i++) { buf[pos++]='\x1b'; buf[pos++]='['; buf[pos++]='B'; }
    buf[pos] = '\0';
    return buf;
}

// ── UTF-8 truncation (SSID may be CJK, 2 columns wide) ────────────
static std::string dsp_trunc(const std::string &s, int max_cols) {
    int w = 0; size_t i = 0;
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

// ── sysfs / command helpers ───────────────────────────────────────
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
    char buf[256]; std::string r;
    if (fgets(buf, sizeof(buf), p)) {
        r = buf;
        while (!r.empty() && (r.back()=='\n' || r.back()=='\r')) r.pop_back();
    }
    pclose(p);
    return r;
}
static bool svc_active(const char *name) {
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "systemctl is-active --quiet %s 2>/dev/null", name);
    return system(cmd) == 0;
}

// ── data gatherers ─────────────────────────────────────────────────
struct NetInfo { enum { WIFI, WIRED, DOWN } type; std::string ssid; };
static NetInfo get_net() {
    if (read_sysfs("/sys/class/net/wlan0/operstate") == "up" &&
        read_sysfs("/sys/class/net/wlan0/carrier")   == "1") {
        auto ssid = run_cmd("iwgetid wlan0 -r 2>/dev/null");
        if (ssid.empty()) ssid = "?";
        return {NetInfo::WIFI, ssid};
    }
    if (read_sysfs("/sys/class/net/eth0/operstate") == "up" &&
        read_sysfs("/sys/class/net/eth0/carrier")   == "1")
        return {NetInfo::WIRED, {}};
    return {NetInfo::DOWN, {}};
}

struct CpuSnap { unsigned long long usr, nice, sys, idle, iow, irq, sirq; };
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
    auto dd = b.idle + b.iow - a.idle - a.iow;
    auto dt = (b.usr+b.nice+b.sys+b.idle+b.iow+b.irq+b.sirq)
            - (a.usr+a.nice+a.sys+a.idle+a.iow+a.irq+a.sirq);
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
        else if (sscanf(line, "MemAvailable: %llu", &v) == 1) { avail=v; break; }
    }
    fclose(f);
    return (double)(total - avail) / (double)total * 100.0;
}

// ── build 4 display lines ─────────────────────────────────────────
static void build_lines(std::string l[4],
                        const NetInfo &net, double cpu, double mem,
                        bool webdav, bool mp3) {
    // line 0 – network
    switch (net.type) {
    case NetInfo::WIFI:
        l[0] = "WiFi:" + dsp_trunc(net.ssid, 11); break;
    case NetInfo::WIRED:
        l[0] = "Eth:up"; break;
    default:
        l[0] = "NET:DOWN"; break;
    }

    // line 1 – cpu / mem  (≥100% → "F")
    auto fmt = [](double v) -> std::string {
        if (v >= 100.0) return "F";
        char b[8]; snprintf(b, sizeof(b), "%2.0f%%", v); return b;
    };
    char b1[32];
    snprintf(b1, sizeof(b1), "CPU %s MEM %s", fmt(cpu).c_str(), fmt(mem).c_str());
    l[1] = b1;

    // line 2 – webdav
    l[2] = "webdav  ";
    l[2] += webdav ? " OK" : "DOWN";

    // line 3 – mp3fetcher
    l[3] = "mp3fetch ";
    l[3] += mp3 ? " OK" : "DOWN";
}

// ── netlink socket for network-change detection ────────────────────
// Returns a non-blocking netlink socket subscribed to link & IPv4
// address change multicasts.  Returns -1 on failure (caller falls
// back to pure timer-driven refresh).
static int netlink_open() {
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0) {
        perror("ssd1306_status: netlink socket");
        return -1;
    }

    struct sockaddr_nl addr{};
    addr.nl_family = AF_NETLINK;
    addr.nl_groups = RTMGRP_LINK | RTMGRP_IPV4_IFADDR;

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("ssd1306_status: netlink bind");
        close(fd);
        return -1;
    }

    // Make non-blocking so poll() works predictably
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    return fd;
}

// Drain all pending netlink messages.  Returns true if at least one
// message was read (meaning a network event happened).
static bool netlink_drain(int nl_fd) {
    char buf[4096];
    bool got_event = false;
    for (;;) {
        ssize_t n = recv(nl_fd, buf, sizeof(buf), 0);
        if (n > 0) {
            got_event = true;
            continue;               // drain the whole queue
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        if (n == 0) break;
        break;                      // real error → bail
    }
    return got_event;
}

// ── main ───────────────────────────────────────────────────────────
int main() {
    printf("ssd1306_status: starting\n");
    sleep(8);

    int fd = open(DEV, O_WRONLY);
    if (fd < 0) {
        fprintf(stderr, "ssd1306_status: open %s: %s\n", DEV, strerror(errno));
        return 1;
    }

    // Try to open netlink for network-change notifications.
    // If it fails we fall back to pure polling — still works fine.
    int nl_fd = netlink_open();
    if (nl_fd >= 0)
        printf("ssd1306_status: netlink listener active\n");
    else
        printf("ssd1306_status: netlink unavailable, polling only\n");

    auto prev_cpu = cpu_snap();
    std::string old[ROWS];          // previous frame contents
    std::string cur[ROWS];          // new frame contents
    bool     first = true;
    int      cursor_row = 0;        // where we think the hw cursor is

    struct pollfd pfds[2];
    int nfds = 0;

    // pfd[0] = netlink (optional)
    if (nl_fd >= 0) {
        pfds[nfds].fd     = nl_fd;
        pfds[nfds].events = POLLIN;
        nfds++;
    }
    // timer-only fallback: poll with no fds but a timeout
    // (poll supports nfds==0, it just sleeps for the timeout).

    for (;;) {
        // ── poll: wait for netlink event or timeout ────────────
        int ret = poll(pfds, nfds, REFRESH_S * 1000);
        if (ret < 0) {
            if (errno == EINTR) continue;  // signal, just loop
            perror("ssd1306_status: poll");
            sleep(REFRESH_S);
        }

        // Check if netlink has data → network state changed
        bool net_changed = false;
        if (nl_fd >= 0 && (pfds[0].revents & POLLIN)) {
            net_changed = netlink_drain(nl_fd);
        }

        // If network changed, force a full-screen repaint
        if (net_changed) {
            first = true;
            printf("ssd1306_status: network change detected, refresh\n");
        }

        // ── gather data ────────────────────────────────────────
        NetInfo net = get_net();
        double  mem = mem_pct();
        bool    wd  = svc_active("webdav-server.service");
        bool    mp3 = svc_active("mp3fetcher.service");

        auto cpu_now = cpu_snap();
        double cp = cpu_pct(prev_cpu, cpu_now);
        prev_cpu = cpu_now;

        build_lines(cur, net, cp, mem, wd, mp3);

        if (first) {
            // full paint
            ioctl(fd, SSD1306_IOC_CLEAR);
            std::string all =
                cur[0] + "\n" + cur[1] + "\n" + cur[2] + "\n" + cur[3];
            if (write(fd, all.data(), all.size()) < 0) goto reopen;
            for (int i = 0; i < ROWS; i++) old[i] = cur[i];
            cursor_row = 3;
            first = false;
        } else {
            for (int r = 0; r < ROWS; r++) {
                if (cur[r] != old[r]) {
                    int d = r - cursor_row;
                    std::string nav;
                    if (d < 0)      nav  = UP(-d);
                    else if (d > 0) nav  = DN(d);
                    nav += '\r';
                    std::string line = cur[r];
                    int old_len = (int)old[r].size();
                    int new_len = (int)line.size();
                    if (new_len < old_len) line.append(old_len - new_len, ' ');

                    if (write(fd, nav.data(),  nav.size())  < 0) goto reopen;
                    if (write(fd, line.data(), line.size()) < 0) goto reopen;
                    cursor_row = r;
                    old[r] = cur[r];
                }
            }
        }

        continue;

    reopen:
        fprintf(stderr, "ssd1306_status: write error: %s\n", strerror(errno));
        close(fd);
        sleep(5);
        fd = open(DEV, O_WRONLY);
        if (fd < 0) {
            fprintf(stderr, "ssd1306_status: waiting for %s...\n", DEV);
            sleep(10);
            continue;
        }
        prev_cpu = cpu_snap();
        first = true;   // force full repaint after reopen
    }

    // unreachable, but clean up anyway
    if (nl_fd >= 0) close(nl_fd);
    close(fd);
}
