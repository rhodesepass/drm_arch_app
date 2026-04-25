#pragma once

#include <stdbool.h>
#include <pthread.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint64_t animation_driver_handle_t;
typedef void (*animation_driver_cb)(void *userdata, bool is_last);
typedef void (*animation_driver_userdata_dtor)(void *userdata);

#ifndef ANIMATION_DRIVER_MAX
#define ANIMATION_DRIVER_MAX 1024
#endif

typedef struct {
    uint32_t gen;
    bool active;
    bool reclaim_pending;

    animation_driver_cb cb;
    void *userdata;
    animation_driver_userdata_dtor userdata_dtor;

    uint64_t next_fire_us;
    int64_t remaining;
    uint32_t callbacks_inflight;
} animation_driver_slot_t;

typedef struct animation_driver {
    pthread_mutex_t mtx;
    bool mtx_inited;
    pthread_cond_t cv;
    bool cv_inited;
    bool inited;
    bool stop_requested;
    bool thread_started;
    pthread_t thread;

    uint64_t step_us;
    uint64_t stats_window_start_us;
    uint32_t stats_callbacks_dispatched;

    animation_driver_slot_t slots[ANIMATION_DRIVER_MAX + 1];
    uint16_t free_ids[ANIMATION_DRIVER_MAX];
    uint16_t free_top;
} animation_driver_t;

int animation_driver_init(animation_driver_t *driver, uint64_t step_us);
int animation_driver_destroy(animation_driver_t *driver);

int animation_driver_create(animation_driver_t *driver,
                            animation_driver_handle_t *out,
                            uint64_t start_delay_us,
                            int64_t fire_count,
                            animation_driver_cb cb,
                            void *userdata);

int animation_driver_create_ex(animation_driver_t *driver,
                               animation_driver_handle_t *out,
                               uint64_t start_delay_us,
                               int64_t fire_count,
                               animation_driver_cb cb,
                               void *userdata,
                               animation_driver_userdata_dtor userdata_dtor);

int animation_driver_cancel(animation_driver_t *driver,
                            animation_driver_handle_t handle);
int animation_driver_cancel_sync(animation_driver_t *driver,
                                 animation_driver_handle_t handle);

#ifdef __cplusplus
}
#endif
