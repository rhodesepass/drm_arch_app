// PRTS timer implementation

#include "utils/timer.h"

#include <errno.h>
#include <signal.h>
#include <string.h>

#include "utils/log.h"

// 单实例入口（S）：SIGEV_THREAD trampoline 通过它定位到 tm
static prts_timer_t *g_prts_tm_singleton = NULL;

static inline uint32_t handle_id(prts_timer_handle_t h) { return (uint32_t)(h & 0xFFFFFFFFu); }
static inline uint32_t handle_gen(prts_timer_handle_t h) { return (uint32_t)((h >> 32) & 0xFFFFFFFFu); }

// SIGEV_THREAD 只能可靠携带一个 (按值) union sigval，这里用 16bit id + 16bit gen 打包
static inline int pack_sigval_u32(uint16_t id, uint16_t gen)
{
    return (int)(((uint32_t)gen << 16) | (uint32_t)id);
}

static inline void unpack_sigval_u32(int packed, uint16_t *out_id, uint16_t *out_gen)
{
    uint32_t u = (uint32_t)packed;
    *out_id = (uint16_t)(u & 0xFFFFu);
    *out_gen = (uint16_t)((u >> 16) & 0xFFFFu);
}

static inline void us_to_timespec(uint64_t us, struct timespec *out)
{
    out->tv_sec = (time_t)(us / 1000000ULL);
    out->tv_nsec = (long)((us % 1000000ULL) * 1000ULL);
}

static uint64_t prts_timer_now_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)(ts.tv_nsec / 1000ULL);
}

static void prts_timer_reclaim_slot_locked(prts_timer_t *tm, uint16_t id)
{
    prts_timer_slot_t *s;
    prts_timer_userdata_dtor userdata_dtor;
    void *userdata;

    if (!tm || id == 0 || id > PRTS_TIMER_MAX) {
        return;
    }

    s = &tm->slots[id];
    if (!s->reclaim_pending || s->callbacks_inflight != 0) {
        return;
    }

    userdata_dtor = s->userdata_dtor;
    userdata = s->userdata;

    s->cb = NULL;
    s->userdata = NULL;
    s->userdata_dtor = NULL;
    s->interval_us = 0;
    s->remaining = 0;
    s->reclaim_pending = false;

    if (tm->free_top < PRTS_TIMER_MAX) {
        tm->free_ids[tm->free_top++] = id;
    }

    if (userdata_dtor && userdata) {
        userdata_dtor(userdata);
    }
}

static void prts_timer_trampoline(union sigval sv)
{
    prts_timer_t *tm = g_prts_tm_singleton;
    prts_timer_slot_t *s;
    prts_timer_cb cb = NULL;
    void *userdata = NULL;
    bool is_last = false;
    uint16_t id = 0;
    uint16_t gen16 = 0;
    bool emit_stats = false;
    uint32_t stats_callbacks = 0;
    uint64_t now_us;

    if (!tm) {
        return;
    }

    unpack_sigval_u32(sv.sival_int, &id, &gen16);
    if (id == 0 || id > PRTS_TIMER_MAX) {
        return;
    }

    pthread_mutex_lock(&tm->mtx);
    s = &tm->slots[id];

    // gen 只比较低 16bit（与 sival_int 一致）
    if (!tm->inited || !s->active || ((uint16_t)(s->gen & 0xFFFFu) != gen16)) {
        pthread_mutex_unlock(&tm->mtx);
        return;
    }

    tm->callbacks_inflight++;
    s->callbacks_inflight++;
    now_us = prts_timer_now_us();
    if (tm->stats_window_start_us == 0) {
        tm->stats_window_start_us = now_us;
    }
    tm->stats_callbacks_dispatched++;
    if (now_us - tm->stats_window_start_us >= 1000000ULL) {
        stats_callbacks = tm->stats_callbacks_dispatched;
        tm->stats_callbacks_dispatched = 0;
        tm->stats_window_start_us = now_us;
        emit_stats = (stats_callbacks != 0);
    }

    // 计数：先递减，确保在 cancel/destroy 之后的排队回调能因 gen mismatch 直接返回
    if (s->remaining > 0) {
        s->remaining--;
        if (s->remaining == 0) {
            (void)timer_delete(s->t);
            s->active = false;
            s->reclaim_pending = true;
            s->gen++; // 使旧回调线程失效（handle/sigval 均 mismatch）
            is_last = true;
        }
    }

    cb = s->cb;
    userdata = s->userdata;
    pthread_mutex_unlock(&tm->mtx);

    if (emit_stats) {
        log_info("prts_timer:stats callbacks=%u/s", stats_callbacks);
    }

    if (cb) {
        cb(userdata, is_last);
    }

    pthread_mutex_lock(&tm->mtx);
    if (s->callbacks_inflight > 0) {
        s->callbacks_inflight--;
    }
    prts_timer_reclaim_slot_locked(tm, id);
    if (tm->callbacks_inflight > 0) {
        tm->callbacks_inflight--;
    }
    pthread_cond_broadcast(&tm->idle_cv);
    pthread_mutex_unlock(&tm->mtx);
}

int prts_timer_init(prts_timer_t *tm)
{
    if (!tm) return -EINVAL;

    // 单实例约束：已存在其他实例则拒绝
    if (g_prts_tm_singleton && g_prts_tm_singleton != tm) {
        return -EBUSY;
    }

    if (!tm->mtx_inited) {
        memset(tm, 0, sizeof(*tm));
        int ret = pthread_mutex_init(&tm->mtx, NULL);
        if (ret != 0) return -ret;
        tm->mtx_inited = true;
        ret = pthread_cond_init(&tm->idle_cv, NULL);
        if (ret != 0) {
            pthread_mutex_destroy(&tm->mtx);
            tm->mtx_inited = false;
            return -ret;
        }
        tm->idle_cv_inited = true;
    }

    pthread_mutex_lock(&tm->mtx);
    tm->callbacks_inflight = 0;
    tm->stats_window_start_us = prts_timer_now_us();
    tm->stats_callbacks_dispatched = 0;
    tm->free_top = 0;
    for (uint16_t i = 1; i <= PRTS_TIMER_MAX; i++) {
        tm->slots[i].active = false;
        tm->slots[i].reclaim_pending = false;
        tm->slots[i].gen = 1; // 从 1 开始，避免 0 作为特殊值
        tm->slots[i].callbacks_inflight = 0;
        tm->slots[i].cb = NULL;
        tm->slots[i].userdata = NULL;
        tm->slots[i].userdata_dtor = NULL;
        tm->free_ids[tm->free_top++] = i;
    }
    tm->inited = true;
    g_prts_tm_singleton = tm;
    pthread_mutex_unlock(&tm->mtx);
    log_info("==> PRTS Timer Initialized!");
    return 0;
}

static int prts_timer_cancel_locked(prts_timer_t *tm, uint32_t id, uint32_t gen, uint32_t *out_wait_gen)
{
    prts_timer_slot_t *s;

    if (!tm || id == 0 || id > PRTS_TIMER_MAX) return -EINVAL;

    s = &tm->slots[id];
    if (!tm->inited || !s->active || s->gen != gen) {
        return 0; // safe no-op
    }

    (void)timer_delete(s->t);
    s->active = false;
    s->reclaim_pending = true;
    s->gen++; // invalidate outstanding callbacks/handles
    if (out_wait_gen) {
        *out_wait_gen = s->gen;
    }
    prts_timer_reclaim_slot_locked(tm, (uint16_t)id);
    return 0;
}

int prts_timer_cancel(prts_timer_handle_t handle)
{
    prts_timer_t *tm = g_prts_tm_singleton;
    uint32_t id;
    uint32_t gen;
    int ret;

    if (!tm) return -EINVAL;

    id = handle_id(handle);
    gen = handle_gen(handle);
    if (id == 0 || id > PRTS_TIMER_MAX) return 0; // safe no-op

    pthread_mutex_lock(&tm->mtx);
    ret = prts_timer_cancel_locked(tm, id, gen, NULL);
    pthread_cond_broadcast(&tm->idle_cv);
    pthread_mutex_unlock(&tm->mtx);
    return ret;
}

int prts_timer_cancel_sync(prts_timer_handle_t handle)
{
    prts_timer_t *tm = g_prts_tm_singleton;
    uint32_t id;
    uint32_t gen;
    uint32_t wait_gen = 0;
    prts_timer_slot_t *s;
    int ret;

    if (!tm) return -EINVAL;

    id = handle_id(handle);
    gen = handle_gen(handle);
    if (id == 0 || id > PRTS_TIMER_MAX) return 0;

    pthread_mutex_lock(&tm->mtx);
    s = &tm->slots[id];
    if (!s->active && s->reclaim_pending && s->callbacks_inflight != 0 && s->gen == gen + 1) {
        wait_gen = s->gen;
    }
    ret = prts_timer_cancel_locked(tm, id, gen, &wait_gen);
    if (ret != 0) {
        pthread_mutex_unlock(&tm->mtx);
        return ret;
    }

    while (tm->slots[id].gen == wait_gen && tm->slots[id].callbacks_inflight != 0) {
        pthread_cond_wait(&tm->idle_cv, &tm->mtx);
    }

    prts_timer_reclaim_slot_locked(tm, (uint16_t)id);
    pthread_mutex_unlock(&tm->mtx);
    return 0;
}

int prts_timer_destroy(prts_timer_t *tm)
{
    if (!tm) return -EINVAL;

    pthread_mutex_lock(&tm->mtx);
    if (!tm->inited) {
        pthread_mutex_unlock(&tm->mtx);
        return 0;
    }

    // cancel all active timers
    for (uint32_t id = 1; id <= PRTS_TIMER_MAX; id++) {
        prts_timer_slot_t *s = &tm->slots[id];
        if (s->active) {
            (void)timer_delete(s->t);
            s->active = false;
            s->reclaim_pending = true;
            s->gen++;
            prts_timer_reclaim_slot_locked(tm, (uint16_t)id);
        }
    }

    tm->inited = false;
    if (g_prts_tm_singleton == tm) {
        g_prts_tm_singleton = NULL;
    }

    while (tm->callbacks_inflight != 0) {
        pthread_cond_wait(&tm->idle_cv, &tm->mtx);
    }

    for (uint32_t id = 1; id <= PRTS_TIMER_MAX; id++) {
        prts_timer_reclaim_slot_locked(tm, (uint16_t)id);
    }

    pthread_mutex_unlock(&tm->mtx);
    return 0;
}

int prts_timer_create(prts_timer_handle_t *out,
                      uint64_t start_delay_us,
                      uint64_t interval_us,
                      int64_t fire_count,
                      prts_timer_cb cb,
                      void *userdata)
{
    return prts_timer_create_ex(out, start_delay_us, interval_us, fire_count, cb, userdata, NULL);
}

int prts_timer_create_ex(prts_timer_handle_t *out,
                         uint64_t start_delay_us,
                         uint64_t interval_us,
                         int64_t fire_count,
                         prts_timer_cb cb,
                         void *userdata,
                         prts_timer_userdata_dtor userdata_dtor)
{
    prts_timer_t *tm = g_prts_tm_singleton;
    uint16_t id16;
    prts_timer_slot_t *s;
    struct sigevent sev;
    struct itimerspec its;
    timer_t t;
    int rc;

    if (!tm || !out || !cb) return -EINVAL;
    if (fire_count == 0) return -EINVAL;
    if (fire_count < -1) return -EINVAL;
    if (fire_count == -1 && interval_us == 0) return -EINVAL;
    if (fire_count > 1 && interval_us == 0) return -EINVAL;

    pthread_mutex_lock(&tm->mtx);
    if (!tm->inited) {
        pthread_mutex_unlock(&tm->mtx);
        return -EINVAL;
    }
    if (g_prts_tm_singleton != tm) {
        pthread_mutex_unlock(&tm->mtx);
        return -EBUSY;
    }
    if (tm->free_top == 0) {
        pthread_mutex_unlock(&tm->mtx);
        return -ENOMEM;
    }

    id16 = tm->free_ids[--tm->free_top];
    s = &tm->slots[id16];

    // 生成新的 gen（确保低 16bit 变化，避免 sival_int 复用导致旧线程误命中）
    s->gen++;
    if ((uint16_t)(s->gen & 0xFFFFu) == 0) {
        s->gen++; // 避免低16位为0
    }

    s->cb = cb;
    s->userdata = userdata;
    s->userdata_dtor = userdata_dtor;
    s->interval_us = interval_us;
    s->remaining = fire_count;
    s->callbacks_inflight = 0;
    s->reclaim_pending = false;
    s->active = true;

    memset(&sev, 0, sizeof(sev));
    sev.sigev_notify = SIGEV_THREAD;
    sev.sigev_notify_function = prts_timer_trampoline;
    sev.sigev_value.sival_int = pack_sigval_u32(id16, (uint16_t)(s->gen & 0xFFFFu));

    rc = timer_create(CLOCK_MONOTONIC, &sev, &t);
    if (rc != 0) {
        int err = errno;
        s->active = false;
        s->cb = NULL;
        s->userdata = NULL;
        s->userdata_dtor = NULL;
        s->reclaim_pending = false;
        s->gen++;
        tm->free_ids[tm->free_top++] = id16;
        pthread_mutex_unlock(&tm->mtx);
        log_error("prts_timer_create: timer_create failed: %s(%d)", strerror(err), err);
        return -err;
    }
    s->t = t;

    if (start_delay_us == 0) {
        start_delay_us = (interval_us != 0) ? interval_us : 1;
    }

    memset(&its, 0, sizeof(its));
    us_to_timespec(start_delay_us, &its.it_value);
    if (fire_count == 1) {
        its.it_interval.tv_sec = 0;
        its.it_interval.tv_nsec = 0;
    } else {
        us_to_timespec(interval_us, &its.it_interval);
    }

    rc = timer_settime(s->t, 0, &its, NULL);
    if (rc != 0) {
        int err = errno;
        (void)timer_delete(s->t);
        s->active = false;
        s->cb = NULL;
        s->userdata = NULL;
        s->userdata_dtor = NULL;
        s->reclaim_pending = false;
        s->gen++;
        tm->free_ids[tm->free_top++] = id16;
        pthread_mutex_unlock(&tm->mtx);
        log_error("prts_timer_create: timer_settime failed: %s(%d)", strerror(err), err);
        return -err;
    }

    *out = (((uint64_t)s->gen) << 32) | (uint64_t)id16;
    pthread_mutex_unlock(&tm->mtx);
    return 0;
}
