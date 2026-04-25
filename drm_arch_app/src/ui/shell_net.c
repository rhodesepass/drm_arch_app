#define _XOPEN_SOURCE 600

#include "ui/shell_net.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <linux/input.h>
#include <net/if.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "fonts.h"
#include "screens.h"
#include "ui.h"
#include "ui/scr_transition.h"
#include "utils/log.h"
#include "utils/misc.h"
#include "utils/settings.h"
#include "utils/theme.h"

#ifndef IFF_LOWER_UP
#define IFF_LOWER_UP 0
#endif

#define SHELL_TRANSCRIPT_MAX 32768
#define SHELL_READ_CHUNK 512
#define SHELL_PTY_ROWS 26
#define SHELL_PTY_COLS 52
#define NET_REFRESH_PERIOD_MS 1000

typedef struct {
    const char *title;
    const char *command;
    lv_obj_t *button;
} shell_quick_command_t;

typedef struct {
    bool initialized;

    lv_timer_t *shell_timer;
    lv_timer_t *net_timer;

    lv_obj_t *shell_header;
    lv_obj_t *shell_status;
    lv_obj_t *shell_keyboard;
    lv_obj_t *shell_terminal_panel;
    lv_obj_t *shell_terminal;
    lv_obj_t *shell_quick_panel;
    lv_obj_t *shell_footer;
    lv_obj_t *shell_clear_btn;
    lv_obj_t *shell_restart_btn;
    lv_obj_t *shell_back_btn;

    lv_obj_t *net_header;
    lv_obj_t *net_mode_card;
    lv_obj_t *net_status_card;
    lv_obj_t *net_mode;
    lv_obj_t *net_status;
    lv_obj_t *net_detail_panel;
    lv_obj_t *net_detail;
    lv_obj_t *net_action_panel;
    lv_obj_t *net_refresh_btn;
    lv_obj_t *net_rndis_btn;
    lv_obj_t *net_mtp_btn;
    lv_obj_t *net_shell_btn;
    lv_obj_t *net_back_btn;

    char shell_transcript[SHELL_TRANSCRIPT_MAX];
    size_t shell_transcript_len;
    bool shell_dirty;
    bool shell_escape_seq;
    int shell_fd;
    pid_t shell_pid;
    bool shell_running;
} shell_net_ui_t;

static shell_net_ui_t g_shell_net;

static shell_quick_command_t g_shell_quick_commands[] = {
    { "查看 IP", "ip addr", NULL },
    { "查看路由", "ip route", NULL },
    { "切 RNDIS", "/usr/local/bin/epass-usb-mode rndis", NULL },
    { "切 MTP", "/usr/local/bin/epass-usb-mode mtp", NULL },
    { "USB 诊断", "/usr/local/bin/epass-usb-report.sh", NULL },
    { "服务日志", "journalctl -b -u drm-arch-app.service --no-pager | tail -n 80", NULL },
    { "内存", "free -h", NULL },
    { "磁盘", "df -h", NULL }
};

extern objects_t objects;
extern groups_t groups;
extern settings_t g_settings;

static uint64_t shell_net_now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * 1000ULL) + ((uint64_t)ts.tv_nsec / 1000000ULL);
}

static uint32_t mix_rgb(uint32_t a, uint32_t b, uint8_t b_weight)
{
    uint8_t ar = (uint8_t)((a >> 16) & 0xFF);
    uint8_t ag = (uint8_t)((a >> 8) & 0xFF);
    uint8_t ab = (uint8_t)(a & 0xFF);
    uint8_t br = (uint8_t)((b >> 16) & 0xFF);
    uint8_t bg = (uint8_t)((b >> 8) & 0xFF);
    uint8_t bb = (uint8_t)(b & 0xFF);
    uint8_t a_weight = (uint8_t)(100 - b_weight);
    uint32_t rr = ((uint32_t)ar * a_weight + (uint32_t)br * b_weight) / 100U;
    uint32_t rg = ((uint32_t)ag * a_weight + (uint32_t)bg * b_weight) / 100U;
    uint32_t rb = ((uint32_t)ab * a_weight + (uint32_t)bb * b_weight) / 100U;

    return (rr << 16) | (rg << 8) | rb;
}

static bool color_is_dark(uint32_t rgb)
{
    uint8_t r = (uint8_t)((rgb >> 16) & 0xFF);
    uint8_t g = (uint8_t)((rgb >> 8) & 0xFF);
    uint8_t b = (uint8_t)(rgb & 0xFF);
    uint32_t luminance = (uint32_t)r * 299U + (uint32_t)g * 587U + (uint32_t)b * 114U;

    return luminance < 128000U;
}

static void set_common_panel_style(lv_obj_t *obj, uint32_t bg_rgb, uint32_t border_rgb)
{
    if (obj == NULL) {
        return;
    }

    lv_obj_set_style_bg_color(obj, lv_color_hex(bg_rgb & 0x00FFFFFFu), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(obj, lv_color_hex(border_rgb & 0x00FFFFFFu), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(obj, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(obj, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
}

static void set_button_style(lv_obj_t *obj, uint32_t bg_rgb, uint32_t text_rgb)
{
    if (obj == NULL) {
        return;
    }

    lv_obj_set_style_bg_color(obj, lv_color_hex(bg_rgb & 0x00FFFFFFu), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(obj, lv_color_hex(text_rgb & 0x00FFFFFFu), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(obj, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(obj, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(obj, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(obj, 4, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(obj, 4, LV_PART_MAIN | LV_STATE_DEFAULT);
}

static void shell_mark_dirty(void)
{
    g_shell_net.shell_dirty = true;
}

static void shell_trim_transcript(void)
{
    size_t excess;
    char *newline;

    if (g_shell_net.shell_transcript_len < SHELL_TRANSCRIPT_MAX - 1) {
        return;
    }

    excess = g_shell_net.shell_transcript_len - (SHELL_TRANSCRIPT_MAX / 2);
    newline = memchr(g_shell_net.shell_transcript + excess,
                     '\n',
                     g_shell_net.shell_transcript_len - excess);
    if (newline != NULL) {
        excess = (size_t)(newline - g_shell_net.shell_transcript) + 1;
    }

    memmove(g_shell_net.shell_transcript,
            g_shell_net.shell_transcript + excess,
            g_shell_net.shell_transcript_len - excess);
    g_shell_net.shell_transcript_len -= excess;
    g_shell_net.shell_transcript[g_shell_net.shell_transcript_len] = '\0';
}

static void shell_append_filtered(const char *buf, size_t len)
{
    size_t i;

    if (buf == NULL || len == 0) {
        return;
    }

    for (i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)buf[i];

        if (g_shell_net.shell_escape_seq) {
            if (ch >= '@' && ch <= '~') {
                g_shell_net.shell_escape_seq = false;
            }
            continue;
        }

        if (ch == 0x1B) {
            g_shell_net.shell_escape_seq = true;
            continue;
        }
        if (ch == '\r' || ch == '\0') {
            continue;
        }
        if (ch < 0x20 && ch != '\n' && ch != '\t') {
            continue;
        }

        if (g_shell_net.shell_transcript_len + 2 >= sizeof(g_shell_net.shell_transcript)) {
            shell_trim_transcript();
        }

        g_shell_net.shell_transcript[g_shell_net.shell_transcript_len++] = (char)ch;
        g_shell_net.shell_transcript[g_shell_net.shell_transcript_len] = '\0';
    }

    shell_mark_dirty();
}

static void shell_append_line(const char *text)
{
    if (text == NULL) {
        return;
    }

    shell_append_filtered(text, strlen(text));
    shell_append_filtered("\n", 1);
}

static void shell_refresh_view(void)
{
    if (!g_shell_net.shell_dirty || g_shell_net.shell_terminal == NULL) {
        return;
    }

    lv_textarea_set_text(g_shell_net.shell_terminal, g_shell_net.shell_transcript);
    lv_textarea_set_cursor_pos(g_shell_net.shell_terminal, LV_TEXTAREA_CURSOR_LAST);
    g_shell_net.shell_dirty = false;
}

static int shell_spawn_session(void)
{
    int master_fd = -1;
    int slave_fd = -1;
    char *slave_name = NULL;
    struct winsize ws;
    pid_t pid;

    if (g_shell_net.shell_running) {
        return 0;
    }

    master_fd = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (master_fd < 0) {
        log_error("shell: posix_openpt failed: %s", strerror(errno));
        shell_append_line("[shell] PTY 创建失败");
        return -1;
    }

    if (grantpt(master_fd) != 0 || unlockpt(master_fd) != 0) {
        log_error("shell: grantpt/unlockpt failed: %s", strerror(errno));
        close(master_fd);
        shell_append_line("[shell] PTY 初始化失败");
        return -1;
    }

    slave_name = ptsname(master_fd);
    if (slave_name == NULL) {
        log_error("shell: ptsname failed: %s", strerror(errno));
        close(master_fd);
        shell_append_line("[shell] PTY 端点解析失败");
        return -1;
    }

    ws.ws_row = SHELL_PTY_ROWS;
    ws.ws_col = SHELL_PTY_COLS;
    ws.ws_xpixel = 0;
    ws.ws_ypixel = 0;

    pid = fork();
    if (pid < 0) {
        log_error("shell: fork failed: %s", strerror(errno));
        close(master_fd);
        shell_append_line("[shell] shell 进程创建失败");
        return -1;
    }

    if (pid == 0) {
        setsid();
        slave_fd = open(slave_name, O_RDWR | O_NOCTTY);
        if (slave_fd < 0) {
            _exit(127);
        }

        ioctl(slave_fd, TIOCSWINSZ, &ws);
        ioctl(slave_fd, TIOCSCTTY, 0);

        dup2(slave_fd, STDIN_FILENO);
        dup2(slave_fd, STDOUT_FILENO);
        dup2(slave_fd, STDERR_FILENO);
        if (slave_fd > STDERR_FILENO) {
            close(slave_fd);
        }
        close(master_fd);

        setenv("TERM", "vt100", 1);
        setenv("PS1", "[epass]# ", 1);
        execl("/bin/sh", "sh", "-i", (char *)NULL);
        _exit(127);
    }

    g_shell_net.shell_fd = master_fd;
    g_shell_net.shell_pid = pid;
    g_shell_net.shell_running = true;
    g_shell_net.shell_escape_seq = false;
    shell_append_line("[shell] root 维护终端已启动");
    return 0;
}

static void shell_stop_session(bool append_notice)
{
    int attempts;
    int status;

    if (!g_shell_net.shell_running && g_shell_net.shell_pid <= 0) {
        return;
    }

    if (g_shell_net.shell_fd >= 0) {
        close(g_shell_net.shell_fd);
        g_shell_net.shell_fd = -1;
    }

    if (g_shell_net.shell_pid > 0) {
        kill(g_shell_net.shell_pid, SIGHUP);
        for (attempts = 0; attempts < 20; attempts++) {
            pid_t waited = waitpid(g_shell_net.shell_pid, &status, WNOHANG);
            if (waited == g_shell_net.shell_pid) {
                break;
            }
            usleep(10000);
        }

        if (waitpid(g_shell_net.shell_pid, &status, WNOHANG) == 0) {
            kill(g_shell_net.shell_pid, SIGTERM);
        }
        waitpid(g_shell_net.shell_pid, &status, WNOHANG);
    }

    g_shell_net.shell_pid = -1;
    g_shell_net.shell_running = false;
    if (append_notice) {
        shell_append_line("[shell] 已关闭");
    }
}

static void shell_write_bytes(const char *buf, size_t len)
{
    ssize_t written;

    if (buf == NULL || len == 0) {
        return;
    }
    if (!g_shell_net.shell_running && shell_spawn_session() != 0) {
        return;
    }
    if (g_shell_net.shell_fd < 0) {
        return;
    }

    written = write(g_shell_net.shell_fd, buf, len);
    if (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        log_error("shell: write failed: %s", strerror(errno));
    }
}

static void shell_send_command(const char *command)
{
    if (command == NULL || command[0] == '\0') {
        return;
    }

    shell_write_bytes(command, strlen(command));
    shell_write_bytes("\n", 1);
}

static int shell_key_to_bytes(const key_enc_evdev_event_t *event, char *out, size_t out_sz)
{
    if (event == NULL || out == NULL || out_sz == 0) {
        return 0;
    }

    if (event->ctrl &&
        event->code >= KEY_A &&
        event->code <= KEY_Z &&
        out_sz >= 1) {
        out[0] = (char)(event->code - KEY_A + 1);
        return 1;
    }

    switch (event->code) {
        case KEY_ENTER:
        case KEY_KPENTER:
            out[0] = '\n';
            return 1;
        case KEY_BACKSPACE:
            out[0] = 0x7F;
            return 1;
        case KEY_TAB:
            out[0] = '\t';
            return 1;
        case KEY_ESC:
            out[0] = 0x1B;
            return 1;
        case KEY_UP:
            safe_strcpy(out, out_sz, "\x1b[A");
            return 3;
        case KEY_DOWN:
            safe_strcpy(out, out_sz, "\x1b[B");
            return 3;
        case KEY_RIGHT:
            safe_strcpy(out, out_sz, "\x1b[C");
            return 3;
        case KEY_LEFT:
            safe_strcpy(out, out_sz, "\x1b[D");
            return 3;
        case KEY_HOME:
            safe_strcpy(out, out_sz, "\x1b[H");
            return 3;
        case KEY_END:
            safe_strcpy(out, out_sz, "\x1b[F");
            return 3;
        case KEY_DELETE:
            safe_strcpy(out, out_sz, "\x1b[3~");
            return 4;
        case KEY_SPACE:
            out[0] = ' ';
            return 1;
        case KEY_MINUS:
            out[0] = event->shift ? '_' : '-';
            return 1;
        case KEY_EQUAL:
            out[0] = event->shift ? '+' : '=';
            return 1;
        case KEY_LEFTBRACE:
            out[0] = event->shift ? '{' : '[';
            return 1;
        case KEY_RIGHTBRACE:
            out[0] = event->shift ? '}' : ']';
            return 1;
        case KEY_BACKSLASH:
            out[0] = event->shift ? '|' : '\\';
            return 1;
        case KEY_SEMICOLON:
            out[0] = event->shift ? ':' : ';';
            return 1;
        case KEY_APOSTROPHE:
            out[0] = event->shift ? '"' : '\'';
            return 1;
        case KEY_GRAVE:
            out[0] = event->shift ? '~' : '`';
            return 1;
        case KEY_COMMA:
            out[0] = event->shift ? '<' : ',';
            return 1;
        case KEY_DOT:
            out[0] = event->shift ? '>' : '.';
            return 1;
        case KEY_SLASH:
            out[0] = event->shift ? '?' : '/';
            return 1;
        case KEY_1:
            out[0] = event->shift ? '!' : '1';
            return 1;
        case KEY_2:
            out[0] = event->shift ? '@' : '2';
            return 1;
        case KEY_3:
            out[0] = event->shift ? '#' : '3';
            return 1;
        case KEY_4:
            out[0] = event->shift ? '$' : '4';
            return 1;
        case KEY_5:
            out[0] = event->shift ? '%' : '5';
            return 1;
        case KEY_6:
            out[0] = event->shift ? '^' : '6';
            return 1;
        case KEY_7:
            out[0] = event->shift ? '&' : '7';
            return 1;
        case KEY_8:
            out[0] = event->shift ? '*' : '8';
            return 1;
        case KEY_9:
            out[0] = event->shift ? '(' : '9';
            return 1;
        case KEY_0:
            out[0] = event->shift ? ')' : '0';
            return 1;
        default:
            break;
    }

    if (event->code >= KEY_A && event->code <= KEY_Z) {
        out[0] = (char)((event->shift ? 'A' : 'a') + (event->code - KEY_A));
        return 1;
    }

    return 0;
}

static void shell_timer_cb(lv_timer_t *timer)
{
    char buf[SHELL_READ_CHUNK];
    int status;

    (void)timer;

    if (!g_shell_net.initialized || ui_get_current_screen() != curr_screen_t_SCREEN_SHELL) {
        return;
    }

    if (g_shell_net.shell_running && g_shell_net.shell_pid > 0) {
        pid_t waited = waitpid(g_shell_net.shell_pid, &status, WNOHANG);
        if (waited == g_shell_net.shell_pid) {
            g_shell_net.shell_running = false;
            g_shell_net.shell_pid = -1;
            if (g_shell_net.shell_fd >= 0) {
                close(g_shell_net.shell_fd);
                g_shell_net.shell_fd = -1;
            }
            shell_append_line("[shell] 子进程已退出");
        }
    }

    if (g_shell_net.shell_running && g_shell_net.shell_fd >= 0) {
        while (1) {
            ssize_t nread = read(g_shell_net.shell_fd, buf, sizeof(buf));
            if (nread > 0) {
                shell_append_filtered(buf, (size_t)nread);
                continue;
            }
            if (nread == 0) {
                g_shell_net.shell_running = false;
                if (g_shell_net.shell_fd >= 0) {
                    close(g_shell_net.shell_fd);
                    g_shell_net.shell_fd = -1;
                }
            }
            break;
        }
    }

    if (g_shell_net.shell_status != NULL) {
        lv_label_set_text(
            g_shell_net.shell_status,
            g_shell_net.shell_running ? "会话: 运行中" : "会话: 未运行"
        );
    }
    if (g_shell_net.shell_keyboard != NULL) {
        lv_label_set_text(
            g_shell_net.shell_keyboard,
            key_enc_evdev_has_keyboard() ? "键盘: 已连接" : "键盘: 未连接，使用下方快捷指令"
        );
    }

    shell_refresh_view();
}

static int read_trimmed_file(const char *path, char *buf, size_t buf_sz)
{
    FILE *fp;
    size_t nread;

    if (buf == NULL || buf_sz == 0 || path == NULL) {
        return -1;
    }

    fp = fopen(path, "r");
    if (fp == NULL) {
        buf[0] = '\0';
        return -1;
    }

    nread = fread(buf, 1, buf_sz - 1, fp);
    fclose(fp);
    buf[nread] = '\0';
    while (nread > 0 &&
           (buf[nread - 1] == '\n' || buf[nread - 1] == '\r' ||
            buf[nread - 1] == ' ' || buf[nread - 1] == '\t')) {
        buf[nread - 1] = '\0';
        nread--;
    }

    return 0;
}

static void append_text_block(char *dst, size_t dst_sz, const char *fmt, ...)
{
    va_list ap;
    size_t used = strlen(dst);
    int written;

    if (used >= dst_sz) {
        return;
    }

    va_start(ap, fmt);
    written = vsnprintf(dst + used, dst_sz - used, fmt, ap);
    va_end(ap);

    if (written < 0) {
        dst[used] = '\0';
    }
}

static void get_nameserver_summary(char *buf, size_t buf_sz)
{
    FILE *fp;
    char line[128];
    int count = 0;

    buf[0] = '\0';
    fp = fopen("/etc/resolv.conf", "r");
    if (fp == NULL) {
        safe_strcpy(buf, buf_sz, "N/A");
        return;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        char addr[64];

        if (sscanf(line, "nameserver %63s", addr) != 1) {
            continue;
        }

        append_text_block(buf, buf_sz, "%s%s", count == 0 ? "" : ", ", addr);
        count++;
        if (count >= 2) {
            break;
        }
    }
    fclose(fp);

    if (count == 0) {
        safe_strcpy(buf, buf_sz, "N/A");
    }
}

static void get_default_route_summary(char *buf, size_t buf_sz)
{
    FILE *fp;
    char line[256];

    buf[0] = '\0';
    fp = fopen("/proc/net/route", "r");
    if (fp == NULL) {
        safe_strcpy(buf, buf_sz, "N/A");
        return;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        char iface[IFNAMSIZ];
        char destination[32];
        char gateway[32];
        unsigned int flags;

        if (sscanf(line, "%15s %31s %31s %X", iface, destination, gateway, &flags) != 4) {
            continue;
        }
        if (strcmp(destination, "00000000") != 0 || (flags & 0x2u) == 0u) {
            continue;
        }

        {
            unsigned long gateway_hex = strtoul(gateway, NULL, 16);
            struct in_addr addr;

            addr.s_addr = gateway_hex;
            snprintf(buf, buf_sz, "%s via %s", iface, inet_ntoa(addr));
        }
        fclose(fp);
        return;
    }
    fclose(fp);

    safe_strcpy(buf, buf_sz, "N/A");
}

static void get_interface_summary(const char *ifname, char *buf, size_t buf_sz, bool *exists_out)
{
    struct ifaddrs *ifaddr;
    struct ifaddrs *ifa;
    int sockfd;
    struct ifreq ifr;
    char ipv4[64] = "N/A";
    char netmask[64] = "";
    char mac[64] = "N/A";
    char mtu_str[16] = "N/A";
    unsigned int flags = 0;
    bool exists = false;

    if (buf == NULL || buf_sz == 0) {
        return;
    }

    if (getifaddrs(&ifaddr) == 0) {
        for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
            if (ifa->ifa_name == NULL || strcmp(ifa->ifa_name, ifname) != 0) {
                continue;
            }

            exists = true;
            flags = ifa->ifa_flags;
            if (ifa->ifa_addr != NULL && ifa->ifa_addr->sa_family == AF_INET) {
                struct sockaddr_in *addr = (struct sockaddr_in *)ifa->ifa_addr;
                struct sockaddr_in *mask = (struct sockaddr_in *)ifa->ifa_netmask;

                inet_ntop(AF_INET, &addr->sin_addr, ipv4, sizeof(ipv4));
                if (mask != NULL) {
                    inet_ntop(AF_INET, &mask->sin_addr, netmask, sizeof(netmask));
                }
            }
        }
        freeifaddrs(ifaddr);
    }

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd >= 0) {
        memset(&ifr, 0, sizeof(ifr));
        safe_strcpy(ifr.ifr_name, sizeof(ifr.ifr_name), ifname);

        if (ioctl(sockfd, SIOCGIFHWADDR, &ifr) == 0) {
            unsigned char *addr = (unsigned char *)ifr.ifr_hwaddr.sa_data;

            snprintf(mac,
                     sizeof(mac),
                     "%02x:%02x:%02x:%02x:%02x:%02x",
                     addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
            exists = true;
        }
        if (ioctl(sockfd, SIOCGIFMTU, &ifr) == 0) {
            snprintf(mtu_str, sizeof(mtu_str), "%d", ifr.ifr_mtu);
        }
        close(sockfd);
    }

    snprintf(
        buf,
        buf_sz,
        "%s\nflags: %s%s%s\nipv4: %s%s%s\nmac: %s\nmtu: %s",
        exists ? "存在" : "未发现",
        (flags & IFF_UP) ? "UP " : "",
        (flags & IFF_RUNNING) ? "RUNNING " : "",
        (flags & IFF_LOWER_UP) ? "LOWER_UP" : "",
        ipv4,
        netmask[0] != '\0' ? " / " : "",
        netmask,
        mac,
        mtu_str
    );

    if (exists_out != NULL) {
        *exists_out = exists;
    }
}

static void list_dir_entries(const char *path, char *buf, size_t buf_sz, bool symlink_targets)
{
    DIR *dir;
    struct dirent *entry;
    int count = 0;

    buf[0] = '\0';
    dir = opendir(path);
    if (dir == NULL) {
        safe_strcpy(buf, buf_sz, "N/A");
        return;
    }

    while ((entry = readdir(dir)) != NULL) {
        char link_path[256];
        char target[256];
        ssize_t nread;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (entry->d_name[0] == '.') {
            continue;
        }

        if (count > 0) {
            append_text_block(buf, buf_sz, ", ");
        }

        if (!symlink_targets) {
            append_text_block(buf, buf_sz, "%s", entry->d_name);
        } else {
            snprintf(link_path, sizeof(link_path), "%s/%s", path, entry->d_name);
            nread = readlink(link_path, target, sizeof(target) - 1);
            if (nread > 0) {
                target[nread] = '\0';
                append_text_block(buf, buf_sz, "%s -> %s", entry->d_name, target);
            } else {
                append_text_block(buf, buf_sz, "%s", entry->d_name);
            }
        }

        count++;
    }
    closedir(dir);

    if (count == 0) {
        safe_strcpy(buf, buf_sz, "(none)");
    }
}

static void refresh_net_view(void)
{
    char requested[64];
    char status[128];
    char last_error[128];
    char last_success[128];
    char udc[128];
    char route[128];
    char nameservers[128];
    char iface_summary[256];
    char config_functions[256];
    char gadget_functions[256];
    char rndis_host[64];
    char rndis_dev[64];
    char mode_card[192];
    char status_card[256];
    char detail[2048];
    bool usb0_exists = false;

    requested[0] = '\0';
    status[0] = '\0';
    last_error[0] = '\0';
    last_success[0] = '\0';
    udc[0] = '\0';
    rndis_host[0] = '\0';
    rndis_dev[0] = '\0';

    read_trimmed_file("/run/epass/usb-mode-requested", requested, sizeof(requested));
    read_trimmed_file("/run/epass/usb-mode-status", status, sizeof(status));
    read_trimmed_file("/run/epass/usb-mode-last-error", last_error, sizeof(last_error));
    read_trimmed_file("/run/epass/usb-mode-last-success", last_success, sizeof(last_success));
    read_trimmed_file("/sys/kernel/config/usb_gadget/g1/UDC", udc, sizeof(udc));
    read_trimmed_file("/sys/kernel/config/usb_gadget/g1/functions/rndis.usb0/host_addr", rndis_host, sizeof(rndis_host));
    read_trimmed_file("/sys/kernel/config/usb_gadget/g1/functions/rndis.usb0/dev_addr", rndis_dev, sizeof(rndis_dev));
    get_default_route_summary(route, sizeof(route));
    get_nameserver_summary(nameservers, sizeof(nameservers));
    get_interface_summary("usb0", iface_summary, sizeof(iface_summary), &usb0_exists);
    list_dir_entries("/sys/kernel/config/usb_gadget/g1/configs/c.1", config_functions, sizeof(config_functions), true);
    list_dir_entries("/sys/kernel/config/usb_gadget/g1/functions", gadget_functions, sizeof(gadget_functions), false);

    if (requested[0] == '\0') {
        safe_strcpy(requested, sizeof(requested), "mtp");
    }
    if (status[0] == '\0') {
        safe_strcpy(status, sizeof(status), "unknown");
    }
    if (udc[0] == '\0') {
        safe_strcpy(udc, sizeof(udc), "N/A");
    }
    if (last_success[0] == '\0') {
        safe_strcpy(last_success, sizeof(last_success), "N/A");
    }
    if (last_error[0] == '\0') {
        safe_strcpy(last_error, sizeof(last_error), "(none)");
    }

    snprintf(mode_card, sizeof(mode_card),
             "请求模式\n%s\n\n应用结果\n%s",
             requested,
             status);

    snprintf(status_card, sizeof(status_card),
             "链路状态\n%s\n\n默认路由\n%s\nDNS %s",
             usb0_exists ? "usb0 可用" : "usb0 未发现",
             route,
             nameservers);

    snprintf(detail, sizeof(detail),
             "USB 模式\n"
             "  request: %s\n"
             "  status: %s\n"
             "  last_success: %s\n"
             "  last_error: %s\n\n"
             "RNDIS 详情\n"
             "  host_mac: %s\n"
             "  device_mac: %s\n"
             "  udc: %s\n"
             "  host_gateway: 192.168.137.1\n"
             "  device_ip: 192.168.137.2/24\n\n"
             "usb0 接口\n%s\n\n"
             "gadget 配置\n"
             "  config_functions: %s\n"
             "  gadget_functions: %s",
             requested,
             status,
             last_success,
             last_error,
             rndis_host[0] != '\0' ? rndis_host : "N/A",
             rndis_dev[0] != '\0' ? rndis_dev : "N/A",
             udc,
             iface_summary,
             config_functions,
             gadget_functions);

    if (g_shell_net.net_mode != NULL) {
        lv_label_set_text(g_shell_net.net_mode, mode_card);
    }

    if (g_shell_net.net_status != NULL) {
        lv_label_set_text(g_shell_net.net_status, status_card);
    }

    if (g_shell_net.net_detail != NULL) {
        lv_label_set_text(g_shell_net.net_detail, detail);
    }
}

static void net_timer_cb(lv_timer_t *timer)
{
    static uint64_t last_refresh_ms = 0;
    uint64_t now_ms = shell_net_now_ms();

    (void)timer;

    if (!g_shell_net.initialized || ui_get_current_screen() != curr_screen_t_SCREEN_NET) {
        return;
    }
    if (now_ms - last_refresh_ms < NET_REFRESH_PERIOD_MS) {
        return;
    }

    refresh_net_view();
    last_refresh_ms = now_ms;
}

static void shell_command_button_cb(lv_event_t *e)
{
    const char *command;

    if (lv_event_get_code(e) != LV_EVENT_PRESSED) {
        return;
    }

    command = (const char *)lv_event_get_user_data(e);
    shell_send_command(command);
}

static void shell_clear_button_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_PRESSED) {
        return;
    }

    g_shell_net.shell_transcript[0] = '\0';
    g_shell_net.shell_transcript_len = 0;
    g_shell_net.shell_escape_seq = false;
    shell_mark_dirty();
    shell_refresh_view();
}

static void shell_restart_button_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_PRESSED) {
        return;
    }

    shell_stop_session(true);
    shell_spawn_session();
}

static void shell_back_button_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_PRESSED) {
        return;
    }

    loadScreen(SCREEN_ID_SYSINFO2);
}

static void net_refresh_button_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_PRESSED) {
        return;
    }

    refresh_net_view();
}

static void net_apply_usb_mode(usb_mode_t usb_mode)
{
    settings_lock(&g_settings);
    g_settings.usb_mode = usb_mode;
    settings_unlock(&g_settings);
    settings_update(&g_settings);
    settings_set_usb_mode(usb_mode);
}

static void net_rndis_button_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_PRESSED) {
        return;
    }

    net_apply_usb_mode(usb_mode_t_RNDIS);
    refresh_net_view();
}

static void net_mtp_button_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_PRESSED) {
        return;
    }

    net_apply_usb_mode(usb_mode_t_MTP);
    refresh_net_view();
}

static void net_shell_button_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_PRESSED) {
        return;
    }

    loadScreen(SCREEN_ID_SHELL);
}

static void net_back_button_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_PRESSED) {
        return;
    }

    loadScreen(SCREEN_ID_SYSINFO2);
}

static lv_obj_t *create_action_button(lv_obj_t *parent, int x, int y, int w, int h, const char *text)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_t *label;

    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, w, h);
    label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    lv_obj_set_style_text_font(label, &ui_font_sourcesans_reg_14, LV_PART_MAIN | LV_STATE_DEFAULT);
    return button;
}

static void create_shell_screen_widgets(void)
{
    size_t i;
    lv_obj_t *label;

    lv_obj_clean(objects.shell_container);

    g_shell_net.shell_header = lv_obj_create(objects.shell_container);
    lv_obj_set_pos(g_shell_net.shell_header, 12, 12);
    lv_obj_set_size(g_shell_net.shell_header, 336, 84);
    lv_obj_set_scrollbar_mode(g_shell_net.shell_header, LV_SCROLLBAR_MODE_OFF);

    label = lv_label_create(g_shell_net.shell_header);
    lv_label_set_text(label, "Shell");
    lv_obj_set_pos(label, 12, 6);
    lv_obj_set_style_text_font(label, &ui_font_sourceselif_heavy_24, LV_PART_MAIN | LV_STATE_DEFAULT);

    label = lv_label_create(g_shell_net.shell_header);
    lv_label_set_text(label, "外接键盘可直接输入；没有键盘时，使用下方维护命令");
    lv_obj_set_pos(label, 12, 36);
    lv_obj_set_size(label, 310, LV_SIZE_CONTENT);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(label, &ui_font_sourcesans_reg_14, LV_PART_MAIN | LV_STATE_DEFAULT);

    g_shell_net.shell_status = lv_label_create(g_shell_net.shell_header);
    lv_label_set_text(g_shell_net.shell_status, "会话: 未运行");
    lv_obj_set_pos(g_shell_net.shell_status, 12, 62);
    lv_obj_set_style_text_font(g_shell_net.shell_status, &ui_font_sourcesans_reg_14, LV_PART_MAIN | LV_STATE_DEFAULT);

    g_shell_net.shell_keyboard = lv_label_create(g_shell_net.shell_header);
    lv_label_set_text(g_shell_net.shell_keyboard, "键盘: 未连接");
    lv_obj_set_pos(g_shell_net.shell_keyboard, 168, 62);
    lv_obj_set_style_text_font(g_shell_net.shell_keyboard, &ui_font_sourcesans_reg_14, LV_PART_MAIN | LV_STATE_DEFAULT);

    g_shell_net.shell_terminal_panel = lv_obj_create(objects.shell_container);
    lv_obj_set_pos(g_shell_net.shell_terminal_panel, 12, 108);
    lv_obj_set_size(g_shell_net.shell_terminal_panel, 336, 308);
    lv_obj_set_scrollbar_mode(g_shell_net.shell_terminal_panel, LV_SCROLLBAR_MODE_OFF);

    g_shell_net.shell_terminal = lv_textarea_create(g_shell_net.shell_terminal_panel);
    lv_obj_set_pos(g_shell_net.shell_terminal, 8, 8);
    lv_obj_set_size(g_shell_net.shell_terminal, 320, 292);
    lv_textarea_set_one_line(g_shell_net.shell_terminal, false);
    lv_textarea_set_text(g_shell_net.shell_terminal, "");
    lv_textarea_set_cursor_pos(g_shell_net.shell_terminal, LV_TEXTAREA_CURSOR_LAST);
    lv_obj_set_style_text_font(g_shell_net.shell_terminal, &ui_font_sourcesans_reg_14, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(g_shell_net.shell_terminal, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(g_shell_net.shell_terminal, LV_OBJ_FLAG_CLICK_FOCUSABLE);

    g_shell_net.shell_quick_panel = lv_obj_create(objects.shell_container);
    lv_obj_set_pos(g_shell_net.shell_quick_panel, 12, 428);
    lv_obj_set_size(g_shell_net.shell_quick_panel, 336, 136);
    lv_obj_set_scrollbar_mode(g_shell_net.shell_quick_panel, LV_SCROLLBAR_MODE_OFF);

    label = lv_label_create(g_shell_net.shell_quick_panel);
    lv_label_set_text(label, "快捷操作");
    lv_obj_set_pos(label, 12, 10);
    lv_obj_set_style_text_font(label, &ui_font_sourcesans_reg_14, LV_PART_MAIN | LV_STATE_DEFAULT);

    for (i = 0; i < sizeof(g_shell_quick_commands) / sizeof(g_shell_quick_commands[0]); i++) {
        int x = (int)(12 + (i % 2) * 156);
        int y = (int)(36 + (i / 2) * 24);

        g_shell_quick_commands[i].button = create_action_button(
            g_shell_net.shell_quick_panel,
            x,
            y,
            146,
            22,
            g_shell_quick_commands[i].title
        );
        lv_obj_add_event_cb(
            g_shell_quick_commands[i].button,
            shell_command_button_cb,
            LV_EVENT_PRESSED,
            (void *)g_shell_quick_commands[i].command
        );
    }

    g_shell_net.shell_footer = lv_obj_create(objects.shell_container);
    lv_obj_set_pos(g_shell_net.shell_footer, 12, 574);
    lv_obj_set_size(g_shell_net.shell_footer, 336, 54);
    lv_obj_set_scrollbar_mode(g_shell_net.shell_footer, LV_SCROLLBAR_MODE_OFF);

    g_shell_net.shell_clear_btn = create_action_button(g_shell_net.shell_footer, 12, 7, 98, 40, "清屏");
    lv_obj_add_event_cb(g_shell_net.shell_clear_btn, shell_clear_button_cb, LV_EVENT_PRESSED, NULL);

    g_shell_net.shell_restart_btn = create_action_button(g_shell_net.shell_footer, 119, 7, 98, 40, "重开");
    lv_obj_add_event_cb(g_shell_net.shell_restart_btn, shell_restart_button_cb, LV_EVENT_PRESSED, NULL);

    g_shell_net.shell_back_btn = create_action_button(g_shell_net.shell_footer, 226, 7, 98, 40, "返回");
    lv_obj_add_event_cb(g_shell_net.shell_back_btn, shell_back_button_cb, LV_EVENT_PRESSED, NULL);
}

static void create_net_screen_widgets(void)
{
    lv_obj_t *label;

    lv_obj_clean(objects.net_container);

    g_shell_net.net_header = lv_obj_create(objects.net_container);
    lv_obj_set_pos(g_shell_net.net_header, 12, 12);
    lv_obj_set_size(g_shell_net.net_header, 336, 72);
    lv_obj_set_scrollbar_mode(g_shell_net.net_header, LV_SCROLLBAR_MODE_OFF);

    label = lv_label_create(g_shell_net.net_header);
    lv_label_set_text(label, "网络 / USB");
    lv_obj_set_pos(label, 12, 6);
    lv_obj_set_style_text_font(label, &ui_font_sourceselif_heavy_24, LV_PART_MAIN | LV_STATE_DEFAULT);

    label = lv_label_create(g_shell_net.net_header);
    lv_label_set_text(label, "查看 RNDIS 链路、地址、路由和 gadget 状态，并直接切换 USB 模式");
    lv_obj_set_pos(label, 12, 40);
    lv_obj_set_size(label, 312, LV_SIZE_CONTENT);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(label, &ui_font_sourcesans_reg_14, LV_PART_MAIN | LV_STATE_DEFAULT);

    g_shell_net.net_mode_card = lv_obj_create(objects.net_container);
    lv_obj_set_pos(g_shell_net.net_mode_card, 12, 96);
    lv_obj_set_size(g_shell_net.net_mode_card, 162, 76);
    lv_obj_set_scrollbar_mode(g_shell_net.net_mode_card, LV_SCROLLBAR_MODE_OFF);

    g_shell_net.net_mode = lv_label_create(g_shell_net.net_mode_card);
    lv_label_set_text(g_shell_net.net_mode, "请求模式\nmtp\n\n应用结果\nunknown");
    lv_obj_set_pos(g_shell_net.net_mode, 10, 8);
    lv_obj_set_size(g_shell_net.net_mode, 142, LV_SIZE_CONTENT);
    lv_label_set_long_mode(g_shell_net.net_mode, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(g_shell_net.net_mode, &ui_font_sourcesans_reg_14, LV_PART_MAIN | LV_STATE_DEFAULT);

    g_shell_net.net_status_card = lv_obj_create(objects.net_container);
    lv_obj_set_pos(g_shell_net.net_status_card, 186, 96);
    lv_obj_set_size(g_shell_net.net_status_card, 162, 76);
    lv_obj_set_scrollbar_mode(g_shell_net.net_status_card, LV_SCROLLBAR_MODE_OFF);

    g_shell_net.net_status = lv_label_create(g_shell_net.net_status_card);
    lv_label_set_text(g_shell_net.net_status, "链路状态\nusb0 未发现");
    lv_obj_set_pos(g_shell_net.net_status, 10, 8);
    lv_obj_set_size(g_shell_net.net_status, 142, LV_SIZE_CONTENT);
    lv_label_set_long_mode(g_shell_net.net_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(g_shell_net.net_status, &ui_font_sourcesans_reg_14, LV_PART_MAIN | LV_STATE_DEFAULT);

    g_shell_net.net_detail_panel = lv_obj_create(objects.net_container);
    lv_obj_set_pos(g_shell_net.net_detail_panel, 12, 184);
    lv_obj_set_size(g_shell_net.net_detail_panel, 336, 288);

    g_shell_net.net_detail = lv_label_create(g_shell_net.net_detail_panel);
    lv_obj_set_pos(g_shell_net.net_detail, 10, 10);
    lv_obj_set_size(g_shell_net.net_detail, 316, LV_SIZE_CONTENT);
    lv_label_set_long_mode(g_shell_net.net_detail, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(g_shell_net.net_detail, &ui_font_sourcesans_reg_14, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(g_shell_net.net_detail, "loading...");

    g_shell_net.net_action_panel = lv_obj_create(objects.net_container);
    lv_obj_set_pos(g_shell_net.net_action_panel, 12, 484);
    lv_obj_set_size(g_shell_net.net_action_panel, 336, 144);
    lv_obj_set_scrollbar_mode(g_shell_net.net_action_panel, LV_SCROLLBAR_MODE_OFF);

    g_shell_net.net_refresh_btn = create_action_button(g_shell_net.net_action_panel, 12, 18, 98, 40, "刷新");
    lv_obj_add_event_cb(g_shell_net.net_refresh_btn, net_refresh_button_cb, LV_EVENT_PRESSED, NULL);

    g_shell_net.net_rndis_btn = create_action_button(g_shell_net.net_action_panel, 119, 18, 98, 40, "切 RNDIS");
    lv_obj_add_event_cb(g_shell_net.net_rndis_btn, net_rndis_button_cb, LV_EVENT_PRESSED, NULL);

    g_shell_net.net_mtp_btn = create_action_button(g_shell_net.net_action_panel, 226, 18, 98, 40, "切 MTP");
    lv_obj_add_event_cb(g_shell_net.net_mtp_btn, net_mtp_button_cb, LV_EVENT_PRESSED, NULL);

    g_shell_net.net_shell_btn = create_action_button(g_shell_net.net_action_panel, 12, 78, 152, 40, "进入 Shell");
    lv_obj_add_event_cb(g_shell_net.net_shell_btn, net_shell_button_cb, LV_EVENT_PRESSED, NULL);

    g_shell_net.net_back_btn = create_action_button(g_shell_net.net_action_panel, 172, 78, 152, 40, "返回");
    lv_obj_add_event_cb(g_shell_net.net_back_btn, net_back_button_cb, LV_EVENT_PRESSED, NULL);
}

void ui_shell_net_init(void)
{
    memset(&g_shell_net, 0, sizeof(g_shell_net));
    g_shell_net.shell_fd = -1;
    g_shell_net.shell_pid = -1;

    create_shell_screen_widgets();
    create_net_screen_widgets();

    g_shell_net.shell_timer = lv_timer_create(shell_timer_cb, 80, NULL);
    g_shell_net.net_timer = lv_timer_create(net_timer_cb, 250, NULL);
    g_shell_net.initialized = true;

    shell_append_line("[shell] 外接键盘可直接输入；无键盘时使用快捷指令");
    ui_shell_net_refresh_theme();
    refresh_net_view();
}

void ui_shell_net_on_screen_loaded(curr_screen_t screen)
{
    size_t i;

    if (!g_shell_net.initialized) {
        return;
    }

    if (screen != curr_screen_t_SCREEN_SHELL) {
        shell_stop_session(false);
    }

    if (screen == curr_screen_t_SCREEN_SHELL) {
        lv_group_remove_all_objs(groups.op);
        lv_group_set_wrap(groups.op, false);
        for (i = 0; i < sizeof(g_shell_quick_commands) / sizeof(g_shell_quick_commands[0]); i++) {
            lv_group_add_obj(groups.op, g_shell_quick_commands[i].button);
        }
        lv_group_add_obj(groups.op, g_shell_net.shell_clear_btn);
        lv_group_add_obj(groups.op, g_shell_net.shell_restart_btn);
        lv_group_add_obj(groups.op, g_shell_net.shell_back_btn);
        lv_group_focus_obj(g_shell_quick_commands[0].button);
        shell_spawn_session();
        shell_refresh_view();
        return;
    }

    if (screen == curr_screen_t_SCREEN_NET) {
        lv_group_remove_all_objs(groups.op);
        lv_group_set_wrap(groups.op, false);
        lv_group_add_obj(groups.op, g_shell_net.net_refresh_btn);
        lv_group_add_obj(groups.op, g_shell_net.net_rndis_btn);
        lv_group_add_obj(groups.op, g_shell_net.net_mtp_btn);
        lv_group_add_obj(groups.op, g_shell_net.net_shell_btn);
        lv_group_add_obj(groups.op, g_shell_net.net_back_btn);
        lv_group_focus_obj(g_shell_net.net_refresh_btn);
        refresh_net_view();
    }
}

bool ui_shell_net_handle_raw_key(const key_enc_evdev_event_t *event)
{
    char buf[8];
    int len;

    if (!g_shell_net.initialized || event == NULL) {
        return false;
    }
    if (ui_get_current_screen() != curr_screen_t_SCREEN_SHELL || !event->is_keyboard) {
        return false;
    }

    if (event->value == 0) {
        return true;
    }

    len = shell_key_to_bytes(event, buf, sizeof(buf));
    if (len > 0) {
        shell_write_bytes(buf, (size_t)len);
    }
    return true;
}

void ui_shell_net_refresh_theme(void)
{
    uint32_t bg = app_theme_get_bg_color();
    uint32_t text = app_theme_get_text_color();
    uint32_t primary = app_theme_get_primary_color();
    uint32_t secondary = app_theme_get_secondary_color();
    uint32_t panel = mix_rgb(bg, text, app_theme_is_dark() ? 14 : 8);
    uint32_t panel_strong = mix_rgb(bg, text, app_theme_is_dark() ? 20 : 10);
    uint32_t terminal = mix_rgb(bg, 0x000000u, app_theme_is_dark() ? 28 : 12);
    uint32_t primary_text = color_is_dark(primary) ? 0xFFFFFFu : 0x111111u;
    uint32_t secondary_text = color_is_dark(secondary) ? 0xFFFFFFu : 0x111111u;

    if (!g_shell_net.initialized) {
        return;
    }

    set_common_panel_style(g_shell_net.shell_header, panel, panel_strong);
    set_common_panel_style(g_shell_net.shell_terminal_panel, terminal, panel_strong);
    set_common_panel_style(g_shell_net.shell_quick_panel, panel, panel_strong);
    set_common_panel_style(g_shell_net.shell_footer, panel, panel_strong);
    set_common_panel_style(g_shell_net.net_header, panel, panel_strong);
    set_common_panel_style(g_shell_net.net_mode_card, panel, panel_strong);
    set_common_panel_style(g_shell_net.net_status_card, panel, panel_strong);
    set_common_panel_style(g_shell_net.net_detail_panel, panel, panel_strong);
    set_common_panel_style(g_shell_net.net_action_panel, panel, panel_strong);

    if (g_shell_net.shell_terminal != NULL) {
        lv_obj_set_style_bg_color(g_shell_net.shell_terminal, lv_color_hex(terminal & 0x00FFFFFFu), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(g_shell_net.shell_terminal, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(g_shell_net.shell_terminal, lv_color_hex(text & 0x00FFFFFFu), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(g_shell_net.shell_terminal, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    }

    set_button_style(g_shell_net.shell_clear_btn, panel_strong, text);
    set_button_style(g_shell_net.shell_restart_btn, secondary, secondary_text);
    set_button_style(g_shell_net.shell_back_btn, primary, primary_text);

    set_button_style(g_shell_net.net_refresh_btn, panel_strong, text);
    set_button_style(g_shell_net.net_rndis_btn, primary, primary_text);
    set_button_style(g_shell_net.net_mtp_btn, secondary, secondary_text);
    set_button_style(g_shell_net.net_shell_btn, primary, primary_text);
    set_button_style(g_shell_net.net_back_btn, panel_strong, text);

    if (g_shell_net.shell_status != NULL) {
        lv_obj_set_style_text_color(g_shell_net.shell_status, lv_color_hex(text & 0x00FFFFFFu), LV_PART_MAIN | LV_STATE_DEFAULT);
    }
    if (g_shell_net.shell_keyboard != NULL) {
        lv_obj_set_style_text_color(g_shell_net.shell_keyboard, lv_color_hex(text & 0x00FFFFFFu), LV_PART_MAIN | LV_STATE_DEFAULT);
    }
    if (g_shell_net.net_mode != NULL) {
        lv_obj_set_style_text_color(g_shell_net.net_mode, lv_color_hex(text & 0x00FFFFFFu), LV_PART_MAIN | LV_STATE_DEFAULT);
    }
    if (g_shell_net.net_status != NULL) {
        lv_obj_set_style_text_color(g_shell_net.net_status, lv_color_hex(text & 0x00FFFFFFu), LV_PART_MAIN | LV_STATE_DEFAULT);
    }
    if (g_shell_net.net_detail != NULL) {
        lv_obj_set_style_text_color(g_shell_net.net_detail, lv_color_hex(text & 0x00FFFFFFu), LV_PART_MAIN | LV_STATE_DEFAULT);
    }
}

void ui_shell_net_shutdown(void)
{
    if (!g_shell_net.initialized) {
        return;
    }

    shell_stop_session(false);
    if (g_shell_net.shell_timer != NULL) {
        lv_timer_delete(g_shell_net.shell_timer);
        g_shell_net.shell_timer = NULL;
    }
    if (g_shell_net.net_timer != NULL) {
        lv_timer_delete(g_shell_net.net_timer);
        g_shell_net.net_timer = NULL;
    }
    g_shell_net.initialized = false;
}
