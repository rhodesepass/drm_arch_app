#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "driver/key_enc_evdev.h"
#include "lvgl.h"
#include "utils/log.h"
#include "utils/misc.h"

#define KEY_ENC_EVDEV_RESCAN_MS 1000
#define BITS_PER_LONG (sizeof(unsigned long) * 8)
#define NBITS(x) (((x) + BITS_PER_LONG - 1) / BITS_PER_LONG)

static key_enc_evdev_t *g_active_key_input = NULL;

static inline int key_test_bit(const unsigned long *bits, unsigned int bit)
{
    return (bits[bit / BITS_PER_LONG] >> (bit % BITS_PER_LONG)) & 1UL;
}

static uint64_t key_enc_evdev_now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * 1000ULL) + ((uint64_t)ts.tv_nsec / 1000000ULL);
}

static int key_enc_evdev_has_ev_key(int fd)
{
    unsigned long ev_bits[NBITS(EV_MAX + 1)];

    memset(ev_bits, 0, sizeof(ev_bits));
    if (ioctl(fd, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits) < 0) {
        return 0;
    }

    return key_test_bit(ev_bits, EV_KEY);
}

static int key_enc_evdev_is_keyboard_device(int fd)
{
    unsigned long key_bits[NBITS(KEY_MAX + 1)];

    memset(key_bits, 0, sizeof(key_bits));
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) < 0) {
        return 0;
    }

    return key_test_bit(key_bits, KEY_A) &&
           key_test_bit(key_bits, KEY_Z) &&
           key_test_bit(key_bits, KEY_SPACE) &&
           key_test_bit(key_bits, KEY_ENTER);
}

static void key_enc_evdev_close_devices(key_enc_evdev_t *key_enc_evdev)
{
    int i;

    for (i = 0; i < key_enc_evdev->device_count; i++) {
        if (key_enc_evdev->devices[i].fd >= 0) {
            close(key_enc_evdev->devices[i].fd);
            key_enc_evdev->devices[i].fd = -1;
        }
    }

    key_enc_evdev->device_count = 0;
    key_enc_evdev->evdev_fd = -1;
    key_enc_evdev->shift_down = false;
    key_enc_evdev->ctrl_down = false;
    key_enc_evdev->alt_down = false;
}

static int key_enc_evdev_find_device(const key_enc_evdev_t *key_enc_evdev, const char *path)
{
    int i;

    for (i = 0; i < key_enc_evdev->device_count; i++) {
        if (strcmp(key_enc_evdev->devices[i].path, path) == 0) {
            return i;
        }
    }

    return -1;
}

static void key_enc_evdev_remove_device(key_enc_evdev_t *key_enc_evdev, int idx)
{
    int i;

    if (idx < 0 || idx >= key_enc_evdev->device_count) {
        return;
    }

    if (key_enc_evdev->devices[idx].fd >= 0) {
        close(key_enc_evdev->devices[idx].fd);
    }

    for (i = idx; i + 1 < key_enc_evdev->device_count; i++) {
        key_enc_evdev->devices[i] = key_enc_evdev->devices[i + 1];
    }

    key_enc_evdev->device_count--;
    if (key_enc_evdev->device_count > 0) {
        key_enc_evdev->evdev_fd = key_enc_evdev->devices[0].fd;
    } else {
        key_enc_evdev->evdev_fd = -1;
        key_enc_evdev->shift_down = false;
        key_enc_evdev->ctrl_down = false;
        key_enc_evdev->alt_down = false;
    }
}

static void key_enc_evdev_scan_devices(key_enc_evdev_t *key_enc_evdev)
{
    DIR *dir;
    struct dirent *entry;
    char found_paths[KEY_ENC_EVDEV_MAX_DEVICES][128];
    int found_count = 0;
    int i;

    dir = opendir("/dev/input");
    if (dir == NULL) {
        key_enc_evdev_close_devices(key_enc_evdev);
        key_enc_evdev->next_rescan_ms = key_enc_evdev_now_ms() + KEY_ENC_EVDEV_RESCAN_MS;
        return;
    }

    while ((entry = readdir(dir)) != NULL && found_count < KEY_ENC_EVDEV_MAX_DEVICES) {
        char path[128];
        int fd;

        if (strncmp(entry->d_name, "event", 5) != 0) {
            continue;
        }

        snprintf(path, sizeof(path), "/dev/input/%s", entry->d_name);
        fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            continue;
        }

        if (!key_enc_evdev_has_ev_key(fd)) {
            close(fd);
            continue;
        }

        close(fd);
        safe_strcpy(found_paths[found_count], sizeof(found_paths[found_count]), path);
        found_count++;
    }
    closedir(dir);

    for (i = key_enc_evdev->device_count - 1; i >= 0; i--) {
        int keep = 0;
        int j;

        for (j = 0; j < found_count; j++) {
            if (strcmp(key_enc_evdev->devices[i].path, found_paths[j]) == 0) {
                keep = 1;
                break;
            }
        }

        if (!keep) {
            key_enc_evdev_remove_device(key_enc_evdev, i);
        }
    }

    for (i = 0; i < found_count; i++) {
        int fd;

        if (key_enc_evdev_find_device(key_enc_evdev, found_paths[i]) >= 0) {
            continue;
        }
        if (key_enc_evdev->device_count >= KEY_ENC_EVDEV_MAX_DEVICES) {
            break;
        }

        fd = open(found_paths[i], O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            continue;
        }

        if (!key_enc_evdev_has_ev_key(fd)) {
            close(fd);
            continue;
        }

        safe_strcpy(
            key_enc_evdev->devices[key_enc_evdev->device_count].path,
            sizeof(key_enc_evdev->devices[key_enc_evdev->device_count].path),
            found_paths[i]
        );
        key_enc_evdev->devices[key_enc_evdev->device_count].fd = fd;
        key_enc_evdev->devices[key_enc_evdev->device_count].is_keyboard =
            key_enc_evdev_is_keyboard_device(fd) ? true : false;
        key_enc_evdev->device_count++;
    }

    if (key_enc_evdev->device_count > 0) {
        key_enc_evdev->evdev_fd = key_enc_evdev->devices[0].fd;
    } else {
        key_enc_evdev->evdev_fd = -1;
        key_enc_evdev->shift_down = false;
        key_enc_evdev->ctrl_down = false;
        key_enc_evdev->alt_down = false;
    }

    key_enc_evdev->next_rescan_ms = key_enc_evdev_now_ms() + KEY_ENC_EVDEV_RESCAN_MS;
}

static int key_enc_evdev_process_key(uint16_t code, bool is_keyboard, bool shift_down)
{
    if (!is_keyboard) {
        switch(code) {
            case KEY_1:
                return LV_KEY_LEFT;
            case KEY_2:
                return LV_KEY_RIGHT;
            case KEY_3:
                return LV_KEY_ENTER;
            case KEY_4:
                return LV_KEY_ESC;
            case KEY_0:
                return LV_KEY_END;
            default:
                return 0;
        }
    }

    switch(code) {
        case KEY_LEFT:
        case KEY_UP:
            return LV_KEY_LEFT;
        case KEY_RIGHT:
        case KEY_DOWN:
            return LV_KEY_RIGHT;
        case KEY_TAB:
            return shift_down ? LV_KEY_LEFT : LV_KEY_RIGHT;
        case KEY_ENTER:
        case KEY_KPENTER:
            return LV_KEY_ENTER;
        case KEY_ESC:
            return LV_KEY_ESC;
        case KEY_END:
            return LV_KEY_END;
        default:
            return 0;
    }
}

static void key_enc_evdev_read_cb(lv_indev_t * indev, lv_indev_data_t * data)
{
    key_enc_evdev_t * key_enc_evdev = (key_enc_evdev_t *)lv_indev_get_driver_data(indev);
    uint64_t now_ms = key_enc_evdev_now_ms();

    if (now_ms >= key_enc_evdev->next_rescan_ms) {
        key_enc_evdev_scan_devices(key_enc_evdev);
    }

    while (1) {
        int i;

        for (i = 0; i < key_enc_evdev->device_count; i++) {
            struct input_event in = { 0 };
            key_enc_evdev_event_t raw_event;
            int mapped_key;
            int bytes_read = read(key_enc_evdev->devices[i].fd, &in, sizeof(in));

            if (bytes_read <= 0) {
                if (bytes_read < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                    key_enc_evdev->next_rescan_ms = 0;
                }
                continue;
            }

            if (in.type != EV_KEY) {
                continue;
            }

            if (in.code == KEY_LEFTSHIFT || in.code == KEY_RIGHTSHIFT) {
                key_enc_evdev->shift_down = (in.value != 0);
            } else if (in.code == KEY_LEFTCTRL || in.code == KEY_RIGHTCTRL) {
                key_enc_evdev->ctrl_down = (in.value != 0);
            } else if (in.code == KEY_LEFTALT || in.code == KEY_RIGHTALT) {
                key_enc_evdev->alt_down = (in.value != 0);
            }

            raw_event.code = in.code;
            raw_event.value = in.value;
            raw_event.is_keyboard = key_enc_evdev->devices[i].is_keyboard;
            raw_event.shift = key_enc_evdev->shift_down;
            raw_event.ctrl = key_enc_evdev->ctrl_down;
            raw_event.alt = key_enc_evdev->alt_down;

            if (key_enc_evdev->raw_input_cb != NULL &&
                key_enc_evdev->raw_input_cb(&raw_event)) {
                continue;
            }

            mapped_key = key_enc_evdev_process_key(
                in.code,
                key_enc_evdev->devices[i].is_keyboard,
                key_enc_evdev->shift_down
            );
            if (mapped_key == 0) {
                continue;
            }

            if (in.value == 1 || in.value == 2) {
                data->key = mapped_key;
                data->state = LV_INDEV_STATE_PRESSED;
                if (key_enc_evdev->input_cb != NULL) {
                    key_enc_evdev->input_cb(data->key);
                }
                key_enc_evdev->last_key = data->key;
                key_enc_evdev->last_state = data->state;
                return;
            }

            data->state = LV_INDEV_STATE_RELEASED;
            data->key = mapped_key;
            key_enc_evdev->last_key = data->key;
            key_enc_evdev->last_state = data->state;
            return;
        }
        break;
    }

    data->key = key_enc_evdev->last_key;
    data->state = key_enc_evdev->last_state;
}

lv_indev_t * key_enc_evdev_init(key_enc_evdev_t * key_enc_evdev)
{
    lv_indev_t * indev = lv_indev_create();

    memset(key_enc_evdev->devices, 0, sizeof(key_enc_evdev->devices));
    key_enc_evdev->device_count = 0;
    key_enc_evdev->last_key = 0;
    key_enc_evdev->last_state = LV_INDEV_STATE_RELEASED;
    key_enc_evdev->shift_down = false;
    key_enc_evdev->ctrl_down = false;
    key_enc_evdev->alt_down = false;
    key_enc_evdev->next_rescan_ms = 0;

    lv_indev_set_type(indev, LV_INDEV_TYPE_ENCODER);
    lv_indev_set_read_cb(indev, key_enc_evdev_read_cb);
    key_enc_evdev->indev = indev;
    lv_indev_set_driver_data(indev, key_enc_evdev);

    key_enc_evdev_scan_devices(key_enc_evdev);
    g_active_key_input = key_enc_evdev;
    return indev;
}

void key_enc_evdev_destroy(key_enc_evdev_t * key_enc_evdev)
{
    key_enc_evdev_close_devices(key_enc_evdev);
    if(key_enc_evdev->indev){
        lv_indev_delete(key_enc_evdev->indev);
        key_enc_evdev->indev = NULL;
    }
    if (g_active_key_input == key_enc_evdev) {
        g_active_key_input = NULL;
    }
}

bool key_enc_evdev_has_keyboard(void)
{
    int i;

    if (g_active_key_input == NULL) {
        return false;
    }

    for (i = 0; i < g_active_key_input->device_count; i++) {
        if (g_active_key_input->devices[i].is_keyboard) {
            return true;
        }
    }

    return false;
}
