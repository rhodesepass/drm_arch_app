#include "render/animation_driver.h"

#include <errno.h>
#include <string.h>
#include <time.h>

#include "utils/log.h"

typedef struct {
    animation_driver_userdata_dtor userdata_dtor;
    void *userdata;
} animation_driver_reclaim_t;

static inline uint32_t animation_handle_id(animation_driver_handle_t handle)
{
    return (uint32_t)(handle & 0xFFFFFFFFu);
}

static inline uint32_t animation_handle_gen(animation_driver_handle_t handle)
{
    return (uint32_t)((handle >> 32) & 0xFFFFFFFFu);
}

static uint64_t animation_driver_now_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)(ts.tv_nsec / 1000ULL);
}

static void animation_driver_abs_us_to_timespec(uint64_t abs_us,
                                                struct timespec *out)
{
    out->tv_sec = (time_t)(abs_us / 1000000ULL);
    out->tv_nsec = (long)((abs_us % 1000000ULL) * 1000ULL);
}

static void animation_driver_run_reclaim(animation_driver_reclaim_t *reclaim)
{
    if (reclaim == NULL) {
        return;
    }

    if (reclaim->userdata_dtor != NULL && reclaim->userdata != NULL) {
        reclaim->userdata_dtor(reclaim->userdata);
    }

    reclaim->userdata_dtor = NULL;
    reclaim->userdata = NULL;
}

static void animation_driver_reclaim_slot_locked(animation_driver_t *driver,
                                                 uint16_t id,
                                                 animation_driver_reclaim_t *reclaim)
{
    animation_driver_slot_t *slot;

    if (driver == NULL || reclaim == NULL || id == 0 || id > ANIMATION_DRIVER_MAX) {
        return;
    }

    slot = &driver->slots[id];
    if (!slot->reclaim_pending || slot->callbacks_inflight != 0) {
        return;
    }

    reclaim->userdata_dtor = slot->userdata_dtor;
    reclaim->userdata = slot->userdata;

    slot->cb = NULL;
    slot->userdata = NULL;
    slot->userdata_dtor = NULL;
    slot->next_fire_us = 0;
    slot->remaining = 0;
    slot->reclaim_pending = false;

    if (driver->free_top < ANIMATION_DRIVER_MAX) {
        driver->free_ids[driver->free_top++] = id;
    }
}

static int animation_driver_cancel_locked(animation_driver_t *driver,
                                          uint32_t id,
                                          uint32_t gen,
                                          uint32_t *out_wait_gen,
                                          animation_driver_reclaim_t *reclaim)
{
    animation_driver_slot_t *slot;

    if (driver == NULL || reclaim == NULL || id == 0 || id > ANIMATION_DRIVER_MAX) {
        return -EINVAL;
    }

    slot = &driver->slots[id];
    if (!driver->inited || !slot->active || slot->gen != gen) {
        return 0;
    }

    slot->active = false;
    slot->reclaim_pending = true;
    slot->gen++;
    if (out_wait_gen != NULL) {
        *out_wait_gen = slot->gen;
    }

    animation_driver_reclaim_slot_locked(driver, (uint16_t)id, reclaim);
    return 0;
}

static void animation_driver_maybe_log_stats_locked(animation_driver_t *driver,
                                                    uint64_t now_us)
{
    uint32_t callbacks;

    if (driver->stats_window_start_us == 0) {
        driver->stats_window_start_us = now_us;
        return;
    }

    if (now_us - driver->stats_window_start_us < 1000000ULL) {
        return;
    }

    callbacks = driver->stats_callbacks_dispatched;
    driver->stats_callbacks_dispatched = 0;
    driver->stats_window_start_us = now_us;

    if (callbacks != 0) {
        log_info("animation_driver: callbacks=%u/s", callbacks);
    }
}

static void *animation_driver_thread(void *arg)
{
    animation_driver_t *driver = (animation_driver_t *)arg;
    uint64_t next_wake_us;

    log_info("==> Animation Driver Started!");

    next_wake_us = animation_driver_now_us() + driver->step_us;
    while (1) {
        struct timespec wake_ts;
        uint64_t now_us;

        animation_driver_abs_us_to_timespec(next_wake_us, &wake_ts);
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &wake_ts, NULL) == EINTR) {
        }

        pthread_mutex_lock(&driver->mtx);
        if (driver->stop_requested) {
            pthread_mutex_unlock(&driver->mtx);
            break;
        }

        now_us = animation_driver_now_us();
        for (uint16_t id = 1; id <= ANIMATION_DRIVER_MAX; id++) {
            while (1) {
                animation_driver_slot_t *slot;
                animation_driver_cb cb;
                void *userdata;
                bool is_last = false;
                animation_driver_reclaim_t reclaim = {0};

                slot = &driver->slots[id];
                if (!slot->active || slot->cb == NULL || now_us < slot->next_fire_us) {
                    break;
                }

                cb = slot->cb;
                userdata = slot->userdata;
                slot->callbacks_inflight++;
                driver->stats_callbacks_dispatched++;

                if (slot->remaining > 0) {
                    slot->remaining--;
                    if (slot->remaining == 0) {
                        slot->active = false;
                        slot->reclaim_pending = true;
                        slot->gen++;
                        is_last = true;
                    }
                }

                if (slot->active) {
                    slot->next_fire_us += driver->step_us;
                }

                pthread_mutex_unlock(&driver->mtx);
                cb(userdata, is_last);
                pthread_mutex_lock(&driver->mtx);

                if (slot->callbacks_inflight > 0) {
                    slot->callbacks_inflight--;
                }
                animation_driver_reclaim_slot_locked(driver, id, &reclaim);
                pthread_cond_broadcast(&driver->cv);
                pthread_mutex_unlock(&driver->mtx);
                animation_driver_run_reclaim(&reclaim);
                pthread_mutex_lock(&driver->mtx);

                if (driver->stop_requested) {
                    break;
                }
                now_us = animation_driver_now_us();
            }

            if (driver->stop_requested) {
                break;
            }
        }

        animation_driver_maybe_log_stats_locked(driver, animation_driver_now_us());
        pthread_mutex_unlock(&driver->mtx);

        now_us = animation_driver_now_us();
        while (next_wake_us <= now_us) {
            next_wake_us += driver->step_us;
        }
    }

    log_info("==> Animation Driver Ended!");
    return NULL;
}

int animation_driver_init(animation_driver_t *driver, uint64_t step_us)
{
    int ret;

    if (driver == NULL || step_us == 0) {
        return -EINVAL;
    }

    memset(driver, 0, sizeof(*driver));

    ret = pthread_mutex_init(&driver->mtx, NULL);
    if (ret != 0) {
        return -ret;
    }
    driver->mtx_inited = true;

    ret = pthread_cond_init(&driver->cv, NULL);
    if (ret != 0) {
        pthread_mutex_destroy(&driver->mtx);
        driver->mtx_inited = false;
        return -ret;
    }
    driver->cv_inited = true;

    pthread_mutex_lock(&driver->mtx);
    driver->step_us = step_us;
    driver->stats_window_start_us = animation_driver_now_us();
    driver->stats_callbacks_dispatched = 0;
    driver->stop_requested = false;
    driver->free_top = 0;
    for (uint16_t i = 1; i <= ANIMATION_DRIVER_MAX; i++) {
        driver->slots[i].gen = 1;
        driver->free_ids[driver->free_top++] = i;
    }
    driver->inited = true;
    pthread_mutex_unlock(&driver->mtx);

    ret = pthread_create(&driver->thread, NULL, animation_driver_thread, driver);
    if (ret != 0) {
        pthread_mutex_lock(&driver->mtx);
        driver->inited = false;
        pthread_mutex_unlock(&driver->mtx);
        pthread_cond_destroy(&driver->cv);
        pthread_mutex_destroy(&driver->mtx);
        driver->cv_inited = false;
        driver->mtx_inited = false;
        return -ret;
    }

    driver->thread_started = true;
    return 0;
}

int animation_driver_destroy(animation_driver_t *driver)
{
    if (driver == NULL) {
        return -EINVAL;
    }

    if (driver->mtx_inited) {
        pthread_mutex_lock(&driver->mtx);
        driver->stop_requested = true;
        pthread_cond_broadcast(&driver->cv);
        pthread_mutex_unlock(&driver->mtx);
    }

    if (driver->thread_started) {
        pthread_join(driver->thread, NULL);
        driver->thread_started = false;
    }

    if (driver->mtx_inited) {
        pthread_mutex_lock(&driver->mtx);
        for (uint16_t id = 1; id <= ANIMATION_DRIVER_MAX; id++) {
            animation_driver_reclaim_t reclaim = {0};
            animation_driver_slot_t *slot = &driver->slots[id];

            if (slot->active) {
                slot->active = false;
                slot->reclaim_pending = true;
                slot->gen++;
            }

            animation_driver_reclaim_slot_locked(driver, id, &reclaim);
            pthread_mutex_unlock(&driver->mtx);
            animation_driver_run_reclaim(&reclaim);
            pthread_mutex_lock(&driver->mtx);
        }
        driver->inited = false;
        pthread_mutex_unlock(&driver->mtx);
    }

    return 0;
}

int animation_driver_create(animation_driver_t *driver,
                            animation_driver_handle_t *out,
                            uint64_t start_delay_us,
                            int64_t fire_count,
                            animation_driver_cb cb,
                            void *userdata)
{
    return animation_driver_create_ex(driver,
                                      out,
                                      start_delay_us,
                                      fire_count,
                                      cb,
                                      userdata,
                                      NULL);
}

int animation_driver_create_ex(animation_driver_t *driver,
                               animation_driver_handle_t *out,
                               uint64_t start_delay_us,
                               int64_t fire_count,
                               animation_driver_cb cb,
                               void *userdata,
                               animation_driver_userdata_dtor userdata_dtor)
{
    animation_driver_slot_t *slot;
    uint16_t id;
    uint64_t now_us;

    if (driver == NULL || out == NULL || cb == NULL) {
        return -EINVAL;
    }
    if (fire_count == 0 || fire_count < -1) {
        return -EINVAL;
    }

    pthread_mutex_lock(&driver->mtx);
    if (!driver->inited) {
        pthread_mutex_unlock(&driver->mtx);
        return -EINVAL;
    }
    if (driver->free_top == 0) {
        pthread_mutex_unlock(&driver->mtx);
        return -ENOMEM;
    }

    id = driver->free_ids[--driver->free_top];
    slot = &driver->slots[id];

    slot->gen++;
    if (slot->gen == 0) {
        slot->gen++;
    }

    now_us = animation_driver_now_us();
    if (start_delay_us == 0) {
        start_delay_us = driver->step_us;
    }

    slot->active = true;
    slot->reclaim_pending = false;
    slot->cb = cb;
    slot->userdata = userdata;
    slot->userdata_dtor = userdata_dtor;
    slot->next_fire_us = now_us + start_delay_us;
    slot->remaining = fire_count;
    slot->callbacks_inflight = 0;

    *out = (((uint64_t)slot->gen) << 32) | (uint64_t)id;
    pthread_mutex_unlock(&driver->mtx);
    return 0;
}

int animation_driver_cancel(animation_driver_t *driver,
                            animation_driver_handle_t handle)
{
    uint32_t id;
    uint32_t gen;
    int ret;
    animation_driver_reclaim_t reclaim = {0};

    if (driver == NULL) {
        return -EINVAL;
    }

    id = animation_handle_id(handle);
    gen = animation_handle_gen(handle);
    if (id == 0 || id > ANIMATION_DRIVER_MAX) {
        return 0;
    }

    pthread_mutex_lock(&driver->mtx);
    ret = animation_driver_cancel_locked(driver, id, gen, NULL, &reclaim);
    pthread_cond_broadcast(&driver->cv);
    pthread_mutex_unlock(&driver->mtx);

    animation_driver_run_reclaim(&reclaim);
    return ret;
}

int animation_driver_cancel_sync(animation_driver_t *driver,
                                 animation_driver_handle_t handle)
{
    uint32_t id;
    uint32_t gen;
    uint32_t wait_gen = 0;
    int ret;
    animation_driver_reclaim_t reclaim = {0};
    bool self_cancel = false;

    if (driver == NULL) {
        return -EINVAL;
    }

    id = animation_handle_id(handle);
    gen = animation_handle_gen(handle);
    if (id == 0 || id > ANIMATION_DRIVER_MAX) {
        return 0;
    }

    if (driver->thread_started && pthread_equal(pthread_self(), driver->thread)) {
        self_cancel = true;
    }

    pthread_mutex_lock(&driver->mtx);
    ret = animation_driver_cancel_locked(driver, id, gen, &wait_gen, &reclaim);
    if (ret != 0) {
        pthread_mutex_unlock(&driver->mtx);
        return ret;
    }

    if (!self_cancel) {
        while (driver->slots[id].gen == wait_gen &&
               driver->slots[id].callbacks_inflight != 0) {
            pthread_cond_wait(&driver->cv, &driver->mtx);
        }
    }

    animation_driver_reclaim_slot_locked(driver, (uint16_t)id, &reclaim);
    pthread_mutex_unlock(&driver->mtx);

    animation_driver_run_reclaim(&reclaim);
    return 0;
}
