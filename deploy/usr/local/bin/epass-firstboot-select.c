#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/reboot.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define CONFIGURED_MARKER "/etc/epass-firstboot/configured"
#define BOOT_MNT "/run/epass-boot"

#ifndef EPASS_ENABLE_RESIZE
#define EPASS_ENABLE_RESIZE 1
#endif

struct option {
    const char *value;
    const char *label;
};

static const struct option device_revs[] = {
    {"0.2", "HARDWARE 0.2"},
    {"0.3", "HARDWARE 0.3"},
    {"0.5", "HARDWARE 0.5"},
    {"0.6", "HARDWARE 0.6"},
};

static const struct option screens[] = {
    {"laowu", "LAOWU"},
    {"hsd", "HSD"},
    {"boe", "BOE"},
};

static int serial_fd = -1;
static int input_fds[32];
static int input_count = 0;

struct fb_ctx {
    int fd;
    uint8_t *mem;
    size_t len;
    struct fb_fix_screeninfo fix;
    struct fb_var_screeninfo var;
    bool ready;
};

static struct fb_ctx fb = {
    .fd = -1,
    .mem = MAP_FAILED,
};

struct glyph {
    char ch;
    uint8_t rows[7];
};

static const struct glyph font5x7[] = {
    {'0', {0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e}},
    {'1', {0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e}},
    {'2', {0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f}},
    {'3', {0x1e, 0x01, 0x01, 0x0e, 0x01, 0x01, 0x1e}},
    {'4', {0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02}},
    {'5', {0x1f, 0x10, 0x10, 0x1e, 0x01, 0x01, 0x1e}},
    {'6', {0x0e, 0x10, 0x10, 0x1e, 0x11, 0x11, 0x0e}},
    {'7', {0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e}},
    {'9', {0x0e, 0x11, 0x11, 0x0f, 0x01, 0x01, 0x0e}},
    {'A', {0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11}},
    {'B', {0x1e, 0x11, 0x11, 0x1e, 0x11, 0x11, 0x1e}},
    {'C', {0x0e, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0e}},
    {'D', {0x1e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1e}},
    {'E', {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f}},
    {'F', {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10}},
    {'G', {0x0e, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0e}},
    {'H', {0x11, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11}},
    {'I', {0x0e, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0e}},
    {'J', {0x01, 0x01, 0x01, 0x01, 0x11, 0x11, 0x0e}},
    {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}},
    {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f}},
    {'M', {0x11, 0x1b, 0x15, 0x15, 0x11, 0x11, 0x11}},
    {'N', {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}},
    {'O', {0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e}},
    {'P', {0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10}},
    {'Q', {0x0e, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0d}},
    {'R', {0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11}},
    {'S', {0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e}},
    {'T', {0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e}},
    {'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0a, 0x04}},
    {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0a}},
    {'X', {0x11, 0x11, 0x0a, 0x04, 0x0a, 0x11, 0x11}},
    {'Y', {0x11, 0x11, 0x0a, 0x04, 0x04, 0x04, 0x04}},
    {'Z', {0x1f, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1f}},
    {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x0c}},
    {':', {0x00, 0x0c, 0x0c, 0x00, 0x0c, 0x0c, 0x00}},
    {'-', {0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x00}},
    {'/', {0x01, 0x02, 0x04, 0x08, 0x10, 0x00, 0x00}},
};

static void write_fd(int fd, const char *buf, size_t len)
{
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return;
        }
        buf += n;
        len -= (size_t)n;
    }
}

static void say(const char *fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0)
        return;
    size_t len = (size_t)n;
    if (len >= sizeof(buf))
        len = sizeof(buf) - 1;

    write_fd(STDOUT_FILENO, buf, len);
    if (serial_fd >= 0)
        write_fd(serial_fd, buf, len);
}

static void open_outputs(void)
{
    serial_fd = open("/dev/ttyS0", O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (serial_fd >= 0) {
        struct termios tio;
        if (tcgetattr(serial_fd, &tio) == 0) {
            cfmakeraw(&tio);
            cfsetspeed(&tio, B115200);
            tio.c_cflag |= CLOCAL | CREAD;
            tcsetattr(serial_fd, TCSANOW, &tio);
        }
    }
}

static void serial_clear_screen(void)
{
    static const char seq[] = "\033[2J\033[H";

    if (serial_fd >= 0)
        write_fd(serial_fd, seq, sizeof(seq) - 1);
}

static unsigned int bitfield_scale(unsigned int value, unsigned int bits)
{
    if (bits == 0)
        return 0;
    if (bits >= 8)
        return value;
    return (value * ((1u << bits) - 1u) + 127u) / 255u;
}

static uint32_t fb_color(uint8_t r, uint8_t g, uint8_t b)
{
    if (!fb.ready)
        return 0;

    uint32_t color = 0;
    color |= bitfield_scale(r, fb.var.red.length) << fb.var.red.offset;
    color |= bitfield_scale(g, fb.var.green.length) << fb.var.green.offset;
    color |= bitfield_scale(b, fb.var.blue.length) << fb.var.blue.offset;
    if (fb.var.transp.length)
        color |= ((1u << fb.var.transp.length) - 1u) << fb.var.transp.offset;
    return color;
}

static bool fb_open_device(void)
{
    fb.fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
    if (fb.fd < 0)
        return false;
    if (ioctl(fb.fd, FBIOGET_FSCREENINFO, &fb.fix) < 0 ||
        ioctl(fb.fd, FBIOGET_VSCREENINFO, &fb.var) < 0)
        return false;

    fb.len = fb.fix.smem_len;
    if (fb.len == 0)
        fb.len = (size_t)fb.fix.line_length * fb.var.yres_virtual;
    fb.mem = mmap(NULL, fb.len, PROT_READ | PROT_WRITE, MAP_SHARED, fb.fd, 0);
    if (fb.mem == MAP_FAILED)
        return false;

    fb.ready = true;
    return true;
}

static void fb_put_pixel(int x, int y, uint32_t color)
{
    if (!fb.ready || x < 0 || y < 0 || x >= (int)fb.var.xres || y >= (int)fb.var.yres)
        return;

    int bpp = (int)fb.var.bits_per_pixel;
    size_t off = (size_t)y * fb.fix.line_length + (size_t)x * (size_t)bpp / 8;
    if (off + (size_t)bpp / 8 > fb.len)
        return;

    if (bpp == 16) {
        *(uint16_t *)(fb.mem + off) = (uint16_t)color;
    } else if (bpp == 24) {
        fb.mem[off + 0] = (uint8_t)(color >> 0);
        fb.mem[off + 1] = (uint8_t)(color >> 8);
        fb.mem[off + 2] = (uint8_t)(color >> 16);
    } else if (bpp == 32) {
        *(uint32_t *)(fb.mem + off) = color;
    }
}

static void fb_fill_rect(int x, int y, int w, int h, uint32_t color)
{
    if (!fb.ready || w <= 0 || h <= 0)
        return;
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > (int)fb.var.xres)
        w = (int)fb.var.xres - x;
    if (y + h > (int)fb.var.yres)
        h = (int)fb.var.yres - y;
    if (w <= 0 || h <= 0)
        return;

    for (int yy = y; yy < y + h; yy++) {
        for (int xx = x; xx < x + w; xx++)
            fb_put_pixel(xx, yy, color);
    }
}

static void fb_draw_border(int x, int y, int w, int h, int thick, uint32_t color)
{
    fb_fill_rect(x, y, w, thick, color);
    fb_fill_rect(x, y + h - thick, w, thick, color);
    fb_fill_rect(x, y, thick, h, color);
    fb_fill_rect(x + w - thick, y, thick, h, color);
}

static const uint8_t *glyph_rows(char ch)
{
    if (ch >= 'a' && ch <= 'z')
        ch = (char)(ch - 'a' + 'A');
    for (size_t i = 0; i < sizeof(font5x7) / sizeof(font5x7[0]); i++) {
        if (font5x7[i].ch == ch)
            return font5x7[i].rows;
    }
    return NULL;
}

static int text_width(const char *text, int scale)
{
    return (int)strlen(text) * 6 * scale;
}

static void fb_draw_text(int x, int y, const char *text, int scale, uint32_t color)
{
    if (!fb.ready || scale <= 0)
        return;

    for (const char *p = text; *p; p++) {
        if (*p != ' ') {
            const uint8_t *rows = glyph_rows(*p);
            if (rows) {
                for (int row = 0; row < 7; row++) {
                    for (int col = 0; col < 5; col++) {
                        if (rows[row] & (1u << (4 - col)))
                            fb_fill_rect(x + col * scale, y + row * scale, scale, scale, color);
                    }
                }
            }
        }
        x += 6 * scale;
    }
}

static void fb_draw_text_center(int y, const char *text, int scale, uint32_t color)
{
    int x = ((int)fb.var.xres - text_width(text, scale)) / 2;
    if (x < 0)
        x = 0;
    fb_draw_text(x, y, text, scale, color);
}

static void fb_draw_option_box(int x, int y, int w, int h, const char *text,
                               bool selected, bool section_active)
{
    uint32_t panel = fb_color(24, 30, 40);
    uint32_t selected_bg = fb_color(230, 214, 80);
    uint32_t border = section_active ? fb_color(72, 196, 180) : fb_color(70, 78, 96);
    uint32_t fg = selected ? fb_color(10, 12, 18) : fb_color(236, 238, 245);

    fb_fill_rect(x, y, w, h, selected ? selected_bg : panel);
    fb_draw_border(x, y, w, h, section_active ? 3 : 2, border);
    int scale = h >= 34 ? 3 : 2;
    int tx = x + (w - text_width(text, scale)) / 2;
    if (tx < x + 4)
        tx = x + 4;
    fb_draw_text(tx, y + (h - 7 * scale) / 2, text, scale, fg);
}

static void render_fb_menu(int stage, int rev_idx, int screen_idx)
{
    if (!fb.ready)
        return;

    int w = (int)fb.var.xres;
    int h = (int)fb.var.yres;
    int margin = w >= 340 ? 14 : 8;
    int gap = 8;
    int col_w = (w - margin * 2 - gap) / 2;

    uint32_t bg = fb_color(6, 8, 14);
    uint32_t text = fb_color(236, 238, 245);
    uint32_t muted = fb_color(150, 164, 186);
    uint32_t accent = fb_color(72, 196, 180);
    uint32_t warn = fb_color(230, 214, 80);

    fb_fill_rect(0, 0, w, h, bg);
    fb_draw_text_center(10, "ARKEPASS FIRST BOOT", 2, text);
    fb_draw_text_center(32, "KEY1/2 MOVE  KEY3 OK  KEY4 BACK", 1, muted);

    fb_draw_text(margin, 54, stage == 0 ? "HARDWARE SELECT" : "HARDWARE", 2,
                 stage == 0 ? accent : text);
    int hw_y = 78;
    for (int i = 0; i < (int)(sizeof(device_revs) / sizeof(device_revs[0])); i++) {
        int x = margin + (i % 2) * (col_w + gap);
        int y = hw_y + (i / 2) * 42;
        fb_draw_option_box(x, y, col_w, 36, device_revs[i].value,
                           i == rev_idx, stage == 0);
    }

    int screen_label_y = h > 300 ? 174 : 164;
    int screen_y = screen_label_y + 24;
    fb_draw_text(margin, screen_label_y, stage == 1 ? "SCREEN SELECT" : "SCREEN", 2,
                 stage == 1 ? accent : text);
    for (int i = 0; i < (int)(sizeof(screens) / sizeof(screens[0])); i++) {
        fb_draw_option_box(margin, screen_y + i * 32, w - margin * 2, 28,
                           screens[i].label, i == screen_idx, stage == 1);
    }

    if (stage == 2) {
        int box_h = 48;
        int y = h - box_h - 30;
        fb_fill_rect(margin, y, w - margin * 2, box_h, fb_color(42, 44, 52));
        fb_draw_border(margin, y, w - margin * 2, box_h, 3, warn);
        fb_draw_text_center(y + 8, "CONFIRM WITH KEY3 OR Y", 2, warn);
        fb_draw_text_center(y + 30, "B GOES BACK", 1, muted);
    }

    fb_draw_text_center(h - 14, "SERIAL: H/S STAGE  N/P MOVE  Y OK  B BACK", 1, muted);
    msync(fb.mem, fb.len, MS_ASYNC);
}

static void open_inputs(void)
{
    if (input_count > 0)
        return;

    for (int i = 0; i < 32 && input_count < (int)(sizeof(input_fds) / sizeof(input_fds[0])); i++) {
        char path[64];
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd >= 0)
            input_fds[input_count++] = fd;
    }
}

static int mkdir_p(const char *path, mode_t mode)
{
    char tmp[256];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(tmp))
        return -1;
    memcpy(tmp, path, len + 1);

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, mode) < 0 && errno != EEXIST)
                return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, mode) < 0 && errno != EEXIST)
        return -1;
    return 0;
}

static int write_text_file(const char *path, const char *text, mode_t mode)
{
    char dirbuf[256];
    char tmp[320];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(dirbuf))
        return -1;
    memcpy(dirbuf, path, len + 1);

    char *slash = strrchr(dirbuf, '/');
    if (!slash)
        return -1;
    if (slash == dirbuf)
        slash[1] = '\0';
    else
        *slash = '\0';

    if (snprintf(tmp, sizeof(tmp), "%s/.tmpXXXXXX", dirbuf) >= (int)sizeof(tmp))
        return -1;

    int fd = mkstemp(tmp);
    if (fd < 0)
        return -1;
    if (fchmod(fd, mode) < 0) {
        close(fd);
        unlink(tmp);
        return -1;
    }

    size_t text_len = strlen(text);
    write_fd(fd, text, text_len);
    if (fsync(fd) < 0 || close(fd) < 0) {
        unlink(tmp);
        return -1;
    }
    if (rename(tmp, path) < 0) {
        unlink(tmp);
        return -1;
    }

    int dirfd = open(dirbuf, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dirfd < 0)
        return -1;
    int rc = fsync(dirfd);
    close(dirfd);
    if (rc < 0)
        return -1;
    return 0;
}

static int touch_file(const char *path)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0)
        return -1;
    if (fsync(fd) < 0 || close(fd) < 0)
        return -1;

    char dirbuf[256];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(dirbuf))
        return -1;
    memcpy(dirbuf, path, len + 1);
    char *slash = strrchr(dirbuf, '/');
    if (!slash)
        return -1;
    if (slash == dirbuf)
        slash[1] = '\0';
    else
        *slash = '\0';

    int dirfd = open(dirbuf, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dirfd < 0)
        return -1;
    int rc = fsync(dirfd);
    close(dirfd);
    if (rc < 0)
        return -1;
    return 0;
}

static int run_cmd(char *const argv[])
{
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        execvp(argv[0], argv);
        _exit(127);
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR)
            return -1;
    }
    if (!WIFEXITED(status))
        return -1;
    return WEXITSTATUS(status);
}

static bool is_mountpoint(const char *path)
{
    FILE *fp = fopen("/proc/mounts", "r");
    if (!fp)
        return false;

    char dev[256], mnt[256], type[64], opts[256];
    bool found = false;
    while (fscanf(fp, "%255s %255s %63s %255s %*d %*d", dev, mnt, type, opts) == 4) {
        if (strcmp(mnt, path) == 0) {
            found = true;
            break;
        }
    }
    fclose(fp);
    return found;
}

static int mount_boot(void)
{
    static const char *devices[] = {
        "/dev/disk/by-label/EPASSBOOT",
        "/dev/mmcblk0p3",
        "/dev/mmcblk1p3",
        "/dev/mmcblk0p1",
        "/dev/mmcblk1p1",
    };

    if (mkdir_p(BOOT_MNT, 0755) < 0)
        return -1;
    if (is_mountpoint(BOOT_MNT))
        return 0;

    for (size_t i = 0; i < sizeof(devices) / sizeof(devices[0]); i++) {
        if (access(devices[i], F_OK) != 0)
            continue;
        char *const argv[] = {
            "mount", "-t", "vfat", "-o", "rw", (char *)devices[i], BOOT_MNT, NULL,
        };
        if (run_cmd(argv) == 0 && is_mountpoint(BOOT_MNT)) {
            say("Mounted boot partition from %s\n", devices[i]);
            return 0;
        }
    }
    return -1;
}

static void unmount_boot(void)
{
    if (is_mountpoint(BOOT_MNT)) {
        char *const argv[] = {"umount", BOOT_MNT, NULL};
        run_cmd(argv);
    }
}

enum action_type {
    ACT_NONE,
    ACT_PREV,
    ACT_NEXT,
    ACT_CONFIRM,
    ACT_BACK,
    ACT_DIGIT,
    ACT_STAGE_HARDWARE,
    ACT_STAGE_SCREEN,
};

struct action {
    enum action_type type;
    int digit;
};

static struct action key_action(unsigned short code)
{
    switch (code) {
    case KEY_1:
    case KEY_UP:
    case KEY_LEFT:
        return (struct action){ACT_PREV, -1};
    case KEY_2:
    case KEY_DOWN:
    case KEY_RIGHT:
        return (struct action){ACT_NEXT, -1};
    case KEY_3:
    case KEY_ENTER:
    case KEY_KPENTER:
    case KEY_OK:
    case KEY_SPACE:
    case KEY_POWER:
        return (struct action){ACT_CONFIRM, -1};
    case KEY_4:
    case KEY_BACK:
    case KEY_ESC:
        return (struct action){ACT_BACK, -1};
    default:
        return (struct action){ACT_NONE, -1};
    }
}

static struct action char_action(char c)
{
    if (c >= '1' && c <= '9')
        return (struct action){ACT_DIGIT, c - '1'};
    switch (c) {
    case 'h':
    case 'H':
        return (struct action){ACT_STAGE_HARDWARE, -1};
    case 's':
    case 'S':
        return (struct action){ACT_STAGE_SCREEN, -1};
    case '\r':
    case '\n':
    case 'y':
    case 'Y':
        return (struct action){ACT_CONFIRM, -1};
    case 'n':
    case 'N':
    case 'j':
    case 'J':
    case '+':
        return (struct action){ACT_NEXT, -1};
    case 'p':
    case 'P':
    case 'k':
    case 'K':
    case '-':
        return (struct action){ACT_PREV, -1};
    case 'b':
    case 'B':
    case 0x1b:
        return (struct action){ACT_BACK, -1};
    default:
        return (struct action){ACT_NONE, -1};
    }
}

static struct action wait_action(void)
{
    for (;;) {
        open_inputs();

        fd_set rfds;
        FD_ZERO(&rfds);
        int maxfd = -1;
        if (serial_fd >= 0) {
            FD_SET(serial_fd, &rfds);
            maxfd = serial_fd;
        }
        for (int i = 0; i < input_count; i++) {
            FD_SET(input_fds[i], &rfds);
            if (input_fds[i] > maxfd)
                maxfd = input_fds[i];
        }

        if (maxfd < 0) {
            say("No serial or input device is ready yet. Waiting...\n");
            sleep(1);
            continue;
        }

        int rc = select(maxfd + 1, &rfds, NULL, NULL, NULL);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            sleep(1);
            continue;
        }

        if (serial_fd >= 0 && FD_ISSET(serial_fd, &rfds)) {
            char buf[32];
            ssize_t n = read(serial_fd, buf, sizeof(buf));
            for (ssize_t i = 0; i < n; i++) {
                struct action a = char_action(buf[i]);
                if (a.type != ACT_NONE)
                    return a;
            }
        }

        for (int i = 0; i < input_count; i++) {
            if (!FD_ISSET(input_fds[i], &rfds))
                continue;
            struct input_event ev;
            while (read(input_fds[i], &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
                if (ev.type != EV_KEY || ev.value != 1)
                    continue;
                struct action a = key_action(ev.code);
                if (a.type != ACT_NONE)
                    return a;
            }
        }
    }
}

static void render_serial_menu(int stage, int rev_idx, int screen_idx)
{
    serial_clear_screen();
    say("\n--- ArkEPass first boot ---\n");
    say("Use device keys: 1/2 move, 3 confirm, 4 back.\n");
    say("Serial fallback: h hardware, s screen, n/p move, y confirm, b back.\n");
    say("Hardware: %s%s\n", device_revs[rev_idx].value, stage == 0 ? " <" : "");
    say("Screen:   %s%s\n", screens[screen_idx].value, stage == 1 ? " <" : "");
    if (stage == 2)
        say("Confirm selection with y/Enter, or b to go back.\n");
}

static void render_menu(int stage, int rev_idx, int screen_idx)
{
    render_fb_menu(stage, rev_idx, screen_idx);
    render_serial_menu(stage, rev_idx, screen_idx);
}

static int write_selection(const char *rev, const char *screen)
{
    char boot_env[320];
    char hw_conf[256];

    if (mount_boot() < 0) {
        say("ERROR: could not mount EPASSBOOT boot partition.\n");
        return -1;
    }

    if (mkdir_p(BOOT_MNT "/epass", 0755) < 0 ||
        mkdir_p("/etc/epass", 0755) < 0 ||
        mkdir_p("/etc/epass-firstboot", 0755) < 0) {
        say("ERROR: could not create firstboot directories.\n");
        unmount_boot();
        return -1;
    }

    snprintf(boot_env, sizeof(boot_env),
             "device_rev=%s\nscreen=%s\nepass_firstboot=0\nepass_resize_pending=%d\nepass_resize_stage=%s\nepass_boot_mode=%s\n",
             rev,
             screen,
             EPASS_ENABLE_RESIZE ? 1 : 0,
             EPASS_ENABLE_RESIZE ? "partition" : "done",
             EPASS_ENABLE_RESIZE ? "resize" : "normal");
    snprintf(hw_conf, sizeof(hw_conf),
             "DEVICE_REV=%s\nSCREEN=%s\n", rev, screen);

    if (write_text_file(BOOT_MNT "/epass/boot.env", boot_env, 0644) < 0 ||
        write_text_file("/etc/epass/hardware.conf", hw_conf, 0644) < 0) {
        say("ERROR: failed while writing firstboot config.\n");
        unmount_boot();
        return -1;
    }

    char screen_text[64];
    snprintf(screen_text, sizeof(screen_text), "%s\n", screen);
    if (write_text_file("/etc/screen_type", screen_text, 0644) < 0 ||
        touch_file(CONFIGURED_MARKER) < 0) {
        say("ERROR: failed while writing firstboot markers.\n");
        unmount_boot();
        return -1;
    }

    sync();
    unmount_boot();
    sync();
    return 0;
}

static bool firstboot_needed(void)
{
    return access(CONFIGURED_MARKER, F_OK) != 0;
}

static int find_option(const struct option *options, size_t count, const char *value, int fallback)
{
    for (size_t i = 0; i < count; i++) {
        if (strcmp(options[i].value, value) == 0)
            return (int)i;
    }
    return fallback;
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--needed") == 0)
        return firstboot_needed() ? 0 : 1;
    if (!firstboot_needed())
        return 0;

    open_outputs();
    fb_open_device();
    say("ArkEPass first boot setup starting. Screen UI is on /dev/fb0 when available.\n");

    int stage = 0;
    int rev_idx = find_option(device_revs, sizeof(device_revs) / sizeof(device_revs[0]), "0.6", 3);
    int screen_idx = find_option(screens, sizeof(screens) / sizeof(screens[0]), "laowu", 0);

    for (;;) {
        render_menu(stage, rev_idx, screen_idx);
        struct action a = wait_action();
        int count = stage == 0 ? (int)(sizeof(device_revs) / sizeof(device_revs[0])) :
                    stage == 1 ? (int)(sizeof(screens) / sizeof(screens[0])) : 0;

        if (a.type == ACT_STAGE_HARDWARE) {
            stage = 0;
            continue;
        }
        if (a.type == ACT_STAGE_SCREEN) {
            stage = 1;
            continue;
        }

        if (stage == 0 || stage == 1) {
            int *idx = stage == 0 ? &rev_idx : &screen_idx;
            if (a.type == ACT_NEXT)
                *idx = (*idx + 1) % count;
            else if (a.type == ACT_PREV)
                *idx = (*idx + count - 1) % count;
            else if (a.type == ACT_DIGIT && a.digit >= 0 && a.digit < count)
                *idx = a.digit;
            else if (a.type == ACT_CONFIRM)
                stage++;
            else if (a.type == ACT_BACK && stage > 0)
                stage--;
            continue;
        }

        if (a.type == ACT_BACK) {
            stage = 1;
            continue;
        }
        if (a.type != ACT_CONFIRM)
            continue;

        if (write_selection(device_revs[rev_idx].value, screens[screen_idx].value) == 0)
            break;

        say("Press Enter/y to retry.\n");
        while (wait_action().type != ACT_CONFIRM) {
        }
    }

    if (EPASS_ENABLE_RESIZE)
        say("Configuration saved. Requesting reboot before shared data resize...\n");
    else
        say("Configuration saved. Requesting reboot into normal mode...\n");
    render_fb_menu(2, rev_idx, screen_idx);
    sync();
    sleep(1);
    return 0;
}
