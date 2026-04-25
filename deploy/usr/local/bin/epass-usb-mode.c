#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>

#define SETTINGS_FILE_PATH "/root/epass_cfg.bin"
#define USBCTL_PATH "/usr/local/bin/usbctl.sh"
#define RUN_DIR "/run/epass"
#define LOCK_PATH "/run/epass/usb-mode.lock"
#define REQUEST_PATH "/run/epass/usb-mode-requested"
#define STATUS_PATH "/run/epass/usb-mode-status"
#define LAST_ERROR_PATH "/run/epass/usb-mode-last-error"
#define LAST_SUCCESS_PATH "/run/epass/usb-mode-last-success"
#define SETTINGS_MAGIC 0x45504153434F4E46ULL
#define SETTINGS_VERSION 2U
#define LOCK_RETRY_USEC 100000
#define LOCK_RETRY_COUNT 50
#define USBCTL_WAIT_USEC 100000
#define USBCTL_WAIT_COUNT 150

typedef enum {
    USB_MODE_MTP = 0,
    USB_MODE_SERIAL = 1,
    USB_MODE_RNDIS = 2,
    USB_MODE_NONE = 3
} usb_mode_t;

typedef struct {
    uint64_t magic;
    uint32_t version;
    int brightness;
    int switch_interval;
    int switch_mode;
    int usb_mode;
    uint32_t ctrl_word;
} settings_disk_t;

static void usage(const char *prog)
{
    fprintf(stderr, "Usage: %s [mtp|serial|rndis|none]\n", prog);
}

static const char *usb_mode_to_name(int usb_mode)
{
    switch (usb_mode) {
    case USB_MODE_MTP:
        return "mtp";
    case USB_MODE_SERIAL:
        return "serial";
    case USB_MODE_RNDIS:
        return "rndis";
    case USB_MODE_NONE:
    default:
        return "none";
    }
}

static int parse_mode_arg(const char *arg)
{
    if (strcmp(arg, "mtp") == 0) {
        return USB_MODE_MTP;
    }
    if (strcmp(arg, "serial") == 0) {
        return USB_MODE_SERIAL;
    }
    if (strcmp(arg, "rndis") == 0) {
        return USB_MODE_RNDIS;
    }
    if (strcmp(arg, "none") == 0 || strcmp(arg, "stop") == 0) {
        return USB_MODE_NONE;
    }
    return -1;
}

static int ensure_run_dir(void)
{
    if (mkdir(RUN_DIR, 0755) == 0 || errno == EEXIST) {
        return 0;
    }

    fprintf(stderr, "epass-usb-mode: failed to create %s: %s\n",
            RUN_DIR, strerror(errno));
    return -1;
}

static int write_text_file(const char *path, const char *text)
{
    FILE *fp;

    fp = fopen(path, "w");
    if (fp == NULL) {
        fprintf(stderr, "epass-usb-mode: failed to open %s: %s\n",
                path, strerror(errno));
        return -1;
    }

    if (fputs(text, fp) == EOF || fputc('\n', fp) == EOF) {
        fprintf(stderr, "epass-usb-mode: failed to write %s: %s\n",
                path, strerror(errno));
        fclose(fp);
        return -1;
    }

    if (fclose(fp) != 0) {
        fprintf(stderr, "epass-usb-mode: failed to close %s: %s\n",
                path, strerror(errno));
        return -1;
    }

    return 0;
}

static void remove_text_file(const char *path)
{
    if (unlink(path) != 0 && errno != ENOENT) {
        fprintf(stderr, "epass-usb-mode: failed to remove %s: %s\n",
                path, strerror(errno));
    }
}

static void read_lock_owner(int fd, char *buf, size_t buf_size)
{
    ssize_t nr;

    if (buf == NULL || buf_size == 0) {
        return;
    }

    buf[0] = '\0';
    if (lseek(fd, 0, SEEK_SET) < 0) {
        return;
    }

    nr = read(fd, buf, buf_size - 1);
    if (nr <= 0) {
        buf[0] = '\0';
        return;
    }

    buf[nr] = '\0';
    while (nr > 0 &&
           (buf[nr - 1] == '\n' || buf[nr - 1] == '\r' || buf[nr - 1] == ' ')) {
        buf[nr - 1] = '\0';
        nr--;
    }
}

static void write_lock_owner(int fd, const char *mode_name)
{
    char buf[128];
    int len;

    len = snprintf(buf, sizeof(buf), "pid=%ld mode=%s",
                   (long)getpid(), mode_name);
    if (len < 0) {
        return;
    }

    if (ftruncate(fd, 0) != 0) {
        fprintf(stderr, "epass-usb-mode: failed to truncate %s: %s\n",
                LOCK_PATH, strerror(errno));
        return;
    }
    if (lseek(fd, 0, SEEK_SET) < 0) {
        fprintf(stderr, "epass-usb-mode: failed to seek %s: %s\n",
                LOCK_PATH, strerror(errno));
        return;
    }
    if (write(fd, buf, (size_t)len) < 0) {
        fprintf(stderr, "epass-usb-mode: failed to write %s: %s\n",
                LOCK_PATH, strerror(errno));
        return;
    }
    fsync(fd);
}

static void clear_lock_owner(int fd)
{
    if (ftruncate(fd, 0) != 0) {
        fprintf(stderr, "epass-usb-mode: failed to clear %s: %s\n",
                LOCK_PATH, strerror(errno));
        return;
    }
    if (lseek(fd, 0, SEEK_SET) < 0) {
        fprintf(stderr, "epass-usb-mode: failed to rewind %s: %s\n",
                LOCK_PATH, strerror(errno));
        return;
    }
    fsync(fd);
}

static int read_saved_mode(int *usb_mode)
{
    FILE *fp;
    settings_disk_t settings;
    size_t nr;

    if (usb_mode == NULL) {
        return -1;
    }

    fp = fopen(SETTINGS_FILE_PATH, "rb");
    if (fp == NULL) {
        fprintf(stderr,
                "epass-usb-mode: settings missing, defaulting to mtp: %s\n",
                strerror(errno));
        *usb_mode = USB_MODE_MTP;
        return 0;
    }

    memset(&settings, 0, sizeof(settings));
    nr = fread(&settings, 1, sizeof(settings), fp);
    fclose(fp);

    if (nr < offsetof(settings_disk_t, ctrl_word) + sizeof(settings.ctrl_word)) {
        fprintf(stderr,
                "epass-usb-mode: settings too short (%zu bytes), defaulting to mtp\n",
                nr);
        *usb_mode = USB_MODE_MTP;
        return 0;
    }

    if (settings.magic != SETTINGS_MAGIC || settings.version != SETTINGS_VERSION) {
        fprintf(stderr,
                "epass-usb-mode: invalid settings header (magic=%llx version=%u), defaulting to mtp\n",
                (unsigned long long)settings.magic,
                settings.version);
        *usb_mode = USB_MODE_MTP;
        return 0;
    }

    *usb_mode = settings.usb_mode;
    return 0;
}

static int lock_usb_mode(const char *mode_name)
{
    int fd;
    int attempt;
    int flags;
    char owner[128];

    fd = open(LOCK_PATH, O_CREAT | O_RDWR, 0644);
    if (fd < 0) {
        fprintf(stderr, "epass-usb-mode: failed to open %s: %s\n",
                LOCK_PATH, strerror(errno));
        return -1;
    }

    flags = fcntl(fd, F_GETFD);
    if (flags < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) != 0) {
        fprintf(stderr, "epass-usb-mode: failed to set close-on-exec on %s: %s\n",
                LOCK_PATH, strerror(errno));
        close(fd);
        return -1;
    }

    for (attempt = 0; attempt < LOCK_RETRY_COUNT; ++attempt) {
        if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
            write_lock_owner(fd, mode_name);
            return fd;
        }

        if (errno != EWOULDBLOCK && errno != EAGAIN) {
            fprintf(stderr, "epass-usb-mode: failed to lock %s: %s\n",
                    LOCK_PATH, strerror(errno));
            close(fd);
            return -1;
        }

        usleep(LOCK_RETRY_USEC);
    }

    read_lock_owner(fd, owner, sizeof(owner));
    if (owner[0] != '\0') {
        fprintf(stderr,
                "epass-usb-mode: timed out waiting for %s, holder=%s\n",
                LOCK_PATH, owner);
    } else {
        fprintf(stderr,
                "epass-usb-mode: timed out waiting for %s, holder unknown\n",
                LOCK_PATH);
    }

    close(fd);
    return -1;
}

static int run_usbctl(const char *mode_name, int *detail_rc)
{
    pid_t pid;
    int status;
    int attempt;

    if (detail_rc != NULL) {
        *detail_rc = 1;
    }

    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "epass-usb-mode: fork failed: %s\n", strerror(errno));
        if (detail_rc != NULL) {
            *detail_rc = 125;
        }
        return -1;
    }

    if (pid == 0) {
        execl(USBCTL_PATH, USBCTL_PATH, mode_name, (char *)NULL);
        fprintf(stderr, "epass-usb-mode: exec %s failed: %s\n",
                USBCTL_PATH, strerror(errno));
        _exit(127);
    }

    for (attempt = 0; attempt < USBCTL_WAIT_COUNT; ++attempt) {
        pid_t rc;

        rc = waitpid(pid, &status, WNOHANG);
        if (rc == pid) {
            break;
        }
        if (rc < 0) {
            fprintf(stderr, "epass-usb-mode: waitpid failed: %s\n", strerror(errno));
            if (detail_rc != NULL) {
                *detail_rc = 125;
            }
            return -1;
        }
        usleep(USBCTL_WAIT_USEC);
    }

    if (attempt == USBCTL_WAIT_COUNT) {
        fprintf(stderr, "epass-usb-mode: usbctl timed out for mode %s\n",
                mode_name);
        kill(pid, SIGTERM);
        usleep(500000);
        if (waitpid(pid, &status, WNOHANG) == 0) {
            kill(pid, SIGKILL);
            if (waitpid(pid, &status, 0) < 0) {
                fprintf(stderr, "epass-usb-mode: waitpid after SIGKILL failed: %s\n",
                        strerror(errno));
            }
        }
        if (detail_rc != NULL) {
            *detail_rc = 124;
        }
        return -1;
    }

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        if (detail_rc != NULL) {
            *detail_rc = 0;
        }
        return 0;
    }

    if (WIFEXITED(status)) {
        if (detail_rc != NULL) {
            *detail_rc = WEXITSTATUS(status);
        }
        fprintf(stderr, "epass-usb-mode: usbctl exited with status %d\n",
                WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        if (detail_rc != NULL) {
            *detail_rc = 128 + WTERMSIG(status);
        }
        fprintf(stderr, "epass-usb-mode: usbctl killed by signal %d\n",
                WTERMSIG(status));
    } else {
        if (detail_rc != NULL) {
            *detail_rc = 124;
        }
        fprintf(stderr, "epass-usb-mode: usbctl failed, raw status=0x%x\n",
                status);
    }

    return -1;
}

int main(int argc, char **argv)
{
    int usb_mode = USB_MODE_MTP;
    int lock_fd;
    const char *mode_name;
    int rc;
    int detail_rc = 1;
    char status_line[128];

    if (argc > 2) {
        usage(argv[0]);
        return 2;
    }

    if (argc == 2) {
        usb_mode = parse_mode_arg(argv[1]);
        if (usb_mode < 0) {
            usage(argv[0]);
            return 2;
        }
    } else if (read_saved_mode(&usb_mode) != 0) {
        return 1;
    }

    if (ensure_run_dir() != 0) {
        return 1;
    }

    mode_name = usb_mode_to_name(usb_mode);
    write_text_file(REQUEST_PATH, mode_name);
    snprintf(status_line, sizeof(status_line), "pending mode=%s", mode_name);
    write_text_file(STATUS_PATH, status_line);

    lock_fd = lock_usb_mode(mode_name);
    if (lock_fd < 0) {
        snprintf(status_line, sizeof(status_line),
                 "failed mode=%s rc=%d", mode_name, 123);
        write_text_file(STATUS_PATH, status_line);
        write_text_file(LAST_ERROR_PATH, status_line);
        return 1;
    }

    fprintf(stderr, "epass-usb-mode: applying %s\n", mode_name);
    rc = run_usbctl(mode_name, &detail_rc);

    if (rc == 0) {
        snprintf(status_line, sizeof(status_line), "ok mode=%s", mode_name);
        write_text_file(STATUS_PATH, status_line);
        write_text_file(LAST_SUCCESS_PATH, mode_name);
        remove_text_file(LAST_ERROR_PATH);
    } else {
        snprintf(status_line, sizeof(status_line),
                 "failed mode=%s rc=%d", mode_name, detail_rc);
        write_text_file(STATUS_PATH, status_line);
        write_text_file(LAST_ERROR_PATH, status_line);
    }

    clear_lock_owner(lock_fd);
    close(lock_fd);
    return rc == 0 ? 0 : 1;
}
