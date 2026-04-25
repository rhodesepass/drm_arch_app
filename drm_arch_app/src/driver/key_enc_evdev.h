#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

#define KEY_ENC_EVDEV_MAX_DEVICES 8

typedef struct {
    uint16_t code;
    int32_t value;
    bool is_keyboard;
    bool shift;
    bool ctrl;
    bool alt;
} key_enc_evdev_event_t;

typedef bool (*key_enc_evdev_raw_input_cb_t)(const key_enc_evdev_event_t *event);

typedef struct {
    char path[128];
    int fd;
    bool is_keyboard;
} key_enc_evdev_device_t;

typedef struct {
    lv_indev_t * indev;
    void (*input_cb)(uint32_t key);
    key_enc_evdev_raw_input_cb_t raw_input_cb;
    char dev_path[128];
    int evdev_fd;
    key_enc_evdev_device_t devices[KEY_ENC_EVDEV_MAX_DEVICES];
    int device_count;
    int last_key;
    int last_state;
    bool shift_down;
    bool ctrl_down;
    bool alt_down;
    uint64_t next_rescan_ms;
} key_enc_evdev_t;


lv_indev_t * key_enc_evdev_init(key_enc_evdev_t * key_enc_evdev);

void key_enc_evdev_destroy(key_enc_evdev_t * key_enc_evdev);
bool key_enc_evdev_has_keyboard(void);
