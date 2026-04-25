#include "prts/prts.h"
#include <dirent.h>
#include <overlay/opinfo.h>
#include <overlay/overlay.h>
#include <overlay/transitions.h>
#include <prts/operators.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/time.h>
#include <ui/actions_warning.h>
#include <utils/timer.h>
#include <fcntl.h>
#include "utils/log.h"
#include <unistd.h>
#include <stdlib.h>
#include "config.h"
#include "utils/settings.h"
#include "vars.h"
#include "render/mediaplayer.h"
#include "ui/scr_transition.h"
#include "utils/misc.h"

extern settings_t g_settings;
extern mediaplayer_t g_mediaplayer;

typedef struct {
    prts_t* prts;
    prts_video_t* video;
    int target_operator_index;
} schedule_video_and_transitions_cb_data_t;

static inline void prts_state_lock(prts_t *prts)
{
    pthread_mutex_lock(&prts->state_mutex);
}

static inline void prts_state_unlock(prts_t *prts)
{
    pthread_mutex_unlock(&prts->state_mutex);
}

static inline bool prts_is_stopping(prts_t *prts)
{
    return prts != NULL && atomic_load(&prts->stopping) != 0;
}

static void prts_clear_timer_handle_locked(prts_timer_handle_t *slot, prts_timer_handle_t handle)
{
    if (slot != NULL && *slot == handle) {
        *slot = 0;
    }
}

static void prts_prepare_parse_log(prts_t *prts, bool truncate_log)
{
    if (truncate_log && prts->parse_log_f != NULL) {
        FILE *fp = freopen(PRTS_OPERATOR_PARSE_LOG, "w", prts->parse_log_f);
        if (fp == NULL) {
            log_error("failed to reopen parse log file: %s", PRTS_OPERATOR_PARSE_LOG);
            prts->parse_log_f = NULL;
        } else {
            prts->parse_log_f = fp;
        }
    }

    if (prts->parse_log_f == NULL) {
        prts->parse_log_f = fopen(PRTS_OPERATOR_PARSE_LOG, "w");
        if (prts->parse_log_f == NULL) {
            log_error("failed to open parse log file: %s", PRTS_OPERATOR_PARSE_LOG);
        }
    }
}

static int prts_scan_catalog(prts_t *prts, bool use_sd, bool truncate_log,
                             bool emit_ui_warning, bool *used_fallback)
{
    int errcnt = 0;

    if (used_fallback != NULL) {
        *used_fallback = false;
    }

    prts_prepare_parse_log(prts, truncate_log);
    prts->operator_count = 0;

    errcnt += prts_operator_scan_assets(prts, PRTS_ASSET_DIR, PRTS_SOURCE_ROOTFS);

    if (use_sd) {
        log_info("==> PRTS will scan SD assets directory: %s", PRTS_ASSET_DIR_SD);
        errcnt += prts_operator_scan_assets(prts, PRTS_ASSET_DIR_SD, PRTS_SOURCE_SD);
    }

    if (errcnt != 0 && emit_ui_warning) {
        ui_warning(UI_WARNING_ASSET_ERROR);
    }
    if (errcnt != 0) {
        log_warn("failed to load assets, error count: %d", errcnt);
    }

    if (prts->operator_count == 0) {
        log_warn("no assets loaded, using fallback");
        if (emit_ui_warning) {
            ui_warning(UI_WARNING_NO_ASSETS);
        }
        prts_operator_try_load(prts, &prts->operators[0], PRTS_FALLBACK_ASSET_DIR,
                               PRTS_SOURCE_ROOTFS, 0);
        prts->operator_count = 1;
        if (used_fallback != NULL) {
            *used_fallback = true;
        }
    }

#ifndef APP_RELEASE
    for (int i = 0; i < prts->operator_count; i++) {
        log_debug("========================");
        log_debug("operator[%d]:", i);
        prts_operator_log_entry(&prts->operators[i]);
    }
#endif // APP_RELEASE

    return errcnt;
}

inline static bool should_switch_by_interval(prts_t* prts){
    uint64_t interval_us = 0;
    uint64_t last_switch_time;
    if(atomic_load(&prts->is_auto_switch_blocked) != 0){
        return false;
    }
    switch(g_settings.switch_interval){
        case sw_interval_t_SW_INTERVAL_1MIN:
            interval_us = 1 * 60 * 1000 * 1000;
            break;
        case sw_interval_t_SW_INTERVAL_3MIN:
            interval_us = 3 * 60 * 1000 * 1000;
            break;
        case sw_interval_t_SW_INTERVAL_5MIN:
            interval_us = 5 * 60 * 1000 * 1000;
            break;
        case sw_interval_t_SW_INTERVAL_10MIN:
            interval_us = 10 * 60 * 1000 * 1000;
            break;
        case sw_interval_t_SW_INTERVAL_30MIN:
            interval_us = 30 * 60 * 1000 * 1000;
            break;
        default:
            log_error("invalid switch interval: %d", g_settings.switch_interval);
            return false;
    }


    if(g_settings.switch_mode == sw_mode_t_SW_RANDOM_MANUAL){
        return false;
    }
    else{
        prts_state_lock(prts);
        last_switch_time = prts->last_switch_time;
        prts_state_unlock(prts);
        return get_now_us() - last_switch_time > interval_us;
    }
}

inline static int get_switch_target_index(prts_t* prts){
    int operator_index;

    if(prts->operator_count == 1){
        return 0;
    }

    int target_index = -1;


    prts_state_lock(prts);
    operator_index = prts->operator_index;
    prts_state_unlock(prts);

    if(g_settings.switch_mode == sw_mode_t_SW_MODE_SEQUENCE){
        return (operator_index + 1) % prts->operator_count;
    }
    else if(g_settings.switch_mode == sw_mode_t_SW_MODE_RANDOM){
        do{
            target_index = rand() % prts->operator_count;
        }while(target_index == operator_index);
        return target_index;
    }
    else{
        log_error("invalid switch mode: %d", g_settings.switch_mode);
        return operator_index;
    }
}

static void set_video_cb(void* userdata,bool is_last){
    log_trace("set_video_cb");
    schedule_video_and_transitions_cb_data_t* data = (schedule_video_and_transitions_cb_data_t*)userdata;
    if(data == NULL || data->video == NULL || prts_is_stopping(data->prts)){
        return;
    }
    mediaplayer_switch_video(&g_mediaplayer, data->video->path);
}

extern void mount_video_layer_callback(void *userdata,bool is_last);
static void set_video_mount_layer_cb(void* userdata,bool is_last){
    log_trace("set_video_mount_layer_cb");
    schedule_video_and_transitions_cb_data_t* data = (schedule_video_and_transitions_cb_data_t*)userdata;
    if(data == NULL || data->video == NULL || prts_is_stopping(data->prts)){
        return;
    }
    mediaplayer_switch_video(&g_mediaplayer, data->video->path);

    mount_video_layer_callback(userdata, is_last);
}

// 前向定义
static void schedule_video_and_transitions(prts_t* prts, prts_video_t* video, oltr_params_t* transition, bool is_first_transition, int target_operator_index);

typedef struct {
    prts_t* prts;
    prts_video_t* video;
    oltr_params_t* transition;
    bool is_first_transition;
    int target_operator_index;  // 在调度时捕获目标干员索引
    prts_timer_handle_t timer_handle;
    // 需不需要释放这个结构体
    bool on_heap;
} schedule_video_and_transitions_timer_data_t;

// 用于传递给 schedule_opinfo_timer_cb 的数据结构
typedef struct {
    prts_t* prts;
    int target_operator_index;  // 在调度时捕获目标干员索引
    prts_timer_handle_t timer_handle;
    bool on_heap;
} schedule_opinfo_timer_data_t;

static void schedule_video_and_transitions_timer_data_free(void *userdata)
{
    schedule_video_and_transitions_timer_data_t *data = (schedule_video_and_transitions_timer_data_t *)userdata;

    if (data && data->on_heap) {
        free(data);
    }
}

static void schedule_opinfo_timer_data_free(void *userdata)
{
    schedule_opinfo_timer_data_t *data = (schedule_opinfo_timer_data_t *)userdata;

    if (data && data->on_heap) {
        free(data);
    }
}

// intro视频播放完（达到intro的duration之后）触发的定时器回调。调度transition_loop -> loop video
static void schedule_video_and_transitions_timer_cb(void* userdata,bool is_last){
    schedule_video_and_transitions_timer_data_t* data = (schedule_video_and_transitions_timer_data_t*)userdata;
    prts_t *prts;

    if(data == NULL || data->prts == NULL){
        return;
    }
    prts = data->prts;

    prts_state_lock(prts);
    prts_clear_timer_handle_locked(&prts->transition_delay_timer_handle, data->timer_handle);
    prts_state_unlock(prts);

    if(prts_is_stopping(prts)){
        return;
    }

    prts_state_lock(prts);
    prts->state = PRTS_STATE_TRANSITION_LOOP;
    prts_state_unlock(prts);
    schedule_video_and_transitions(data->prts, data->video, data->transition, data->is_first_transition, data->target_operator_index);
}

static void schedule_opinfo_timer_cb(void* userdata,bool is_last){
    schedule_opinfo_timer_data_t* data = (schedule_opinfo_timer_data_t*)userdata;
    prts_t* prts = data->prts;
    prts_operator_entry_t* target_operator = &prts->operators[data->target_operator_index];

    prts_state_lock(prts);
    prts_clear_timer_handle_locked(&prts->opinfo_timer_handle, data->timer_handle);
    prts_state_unlock(prts);

    if(prts_is_stopping(prts)){
        return;
    }

    log_info("schedule_opinfo_timer_cb: showing opinfo for operator %d (%s)",
             data->target_operator_index, target_operator->operator_name);

    if(target_operator->opinfo_params.type == OPINFO_TYPE_ARKNIGHTS){
        overlay_opinfo_show_arknights(prts->overlay, &target_operator->opinfo_params);
    }
    else if(target_operator->opinfo_params.type == OPINFO_TYPE_IMAGE){
        overlay_opinfo_show_image(prts->overlay, &target_operator->opinfo_params);
    }
    else{
        log_error("schedule_opinfo_timer_cb: invalid opinfo type: %d", target_operator->opinfo_params.type);
    }
    prts_state_lock(prts);
    prts->state = PRTS_STATE_IDLE;
    prts_state_unlock(prts);
}

static int schedule_opinfo(prts_t* prts,prts_operator_entry_t* target_operator){
    if(prts_is_stopping(prts)){
        return 0;
    }

    if(target_operator->opinfo_params.type != OPINFO_TYPE_NONE){
        schedule_opinfo_timer_data_t* data = malloc(sizeof(schedule_opinfo_timer_data_t));
        if(data == NULL){
            log_error("schedule_opinfo: malloc failed");
            return -1;
        }
        data->prts = prts;
        data->target_operator_index = target_operator->index;
        data->timer_handle = 0;
        data->on_heap = true;

        log_info("schedule_opinfo: scheduling for operator %d (%s)",
                 target_operator->index, target_operator->operator_name);

        prts_timer_handle_t timer_handle;
        int ret = prts_timer_create_ex(&timer_handle,
            target_operator->opinfo_params.appear_time,
            0,
            1,
            schedule_opinfo_timer_cb,
            (void*)data,
            schedule_opinfo_timer_data_free);
        if(ret != 0){
            log_error("schedule_opinfo: failed to create timer: %d, falling back to immediate execution", ret);
            schedule_opinfo_timer_cb((void*)data, true);
            schedule_opinfo_timer_data_free((void*)data);
            return 0;
        }

        prts_state_lock(prts);
        if(prts_is_stopping(prts)){
            prts_state_unlock(prts);
            prts_timer_cancel_sync(timer_handle);
            return 0;
        }
        data->timer_handle = timer_handle;
        prts->opinfo_timer_handle = timer_handle;
        prts_state_unlock(prts);
    }
    return 1;
}

// 让我们捋一下干员切换的时间线。
// ===上一态loop video== | =transition_in== | =intro video== | =transition_loop== | =====OPINFO +loop video ==
//               干员切换|   d*1|  d*2|  d*3|                |   d*1|   d*2|   d*3| appear_time|
//             middle_cb切视频-|            |       middle_cb切视频-|             |
//                            |     end_cb-|                       |      end_cb-|
//                            | <======Intro Video实际播放时长 ===> |
// 
// 所以，我们在end_cb 调用的时候 ,排期transition_loop的时间应该是 
// intro_video.duration - transition_in.duration * 2 - transtion_loop.duration

static void schedule_video_and_transitions_end_cb(void* userdata,bool is_last){
    log_trace("schedule_video_and_transitions_end_cb");
    schedule_video_and_transitions_cb_data_t* cb_data = (schedule_video_and_transitions_cb_data_t*)userdata;
    prts_t* prts = cb_data->prts;
    int target_operator_index = cb_data->target_operator_index;
    prts_operator_entry_t* target_operator = &prts->operators[target_operator_index];

    if(prts_is_stopping(prts)){
        return;
    }

    log_info("schedule_video_and_transitions_end_cb: processing for operator %d (%s)",
             target_operator_index, target_operator->operator_name);

    prts_state_t curr_state;
    prts_state_t next_state = PRTS_STATE_IDLE;

    prts_state_lock(prts);
    curr_state = prts->state;
    prts_state_unlock(prts);


    if(curr_state == PRTS_STATE_TRANSITION_IN){
        // 入场过渡结束，进入intro视频。等intro视频结束后，排期transition_loop -> loop video
        schedule_video_and_transitions_timer_data_t *data = malloc(sizeof(schedule_video_and_transitions_timer_data_t));
        if(data == NULL){
            log_error("schedule_video_and_transitions_end_cb: malloc failed");
            prts_state_lock(prts);
            prts->state = PRTS_STATE_IDLE;
            prts_state_unlock(prts);
            return;
        }
        data->prts = prts;
        data->video = &target_operator->loop_video;
        data->transition = &target_operator->transition_loop;
        data->is_first_transition = false;
        data->target_operator_index = target_operator_index;  // 传递目标干员索引
        data->timer_handle = 0;
        data->on_heap = true;
        int delay = target_operator->intro_video.duration - target_operator->transition_in.duration * 2 - target_operator->transition_loop.duration;
        if(delay < 0){
            log_error("schedule_video_and_transitions_end_cb: delay < 0, delay: %d", delay);
            delay = 100 * 1000;
        }
        prts_timer_handle_t timer_handle;
        int ret = prts_timer_create_ex(&timer_handle, delay, 0, 1, schedule_video_and_transitions_timer_cb, (void*)data,
            schedule_video_and_transitions_timer_data_free);
        if(ret != 0){
            log_error("schedule_video_and_transitions_end_cb: failed to create delay timer: %d, falling back to immediate execution", ret);
            schedule_video_and_transitions_timer_cb((void*)data, true);
            schedule_video_and_transitions_timer_data_free((void*)data);
            return;
        }

        prts_state_lock(prts);
        if(prts_is_stopping(prts)){
            prts_state_unlock(prts);
            prts_timer_cancel_sync(timer_handle);
            return;
        }
        data->timer_handle = timer_handle;
        prts->transition_delay_timer_handle = timer_handle;
        prts_state_unlock(prts);
        next_state = PRTS_STATE_INTRO;
    }
    else if(curr_state == PRTS_STATE_TRANSITION_LOOP){
        // 排期opinfo
        if(target_operator->opinfo_params.type != OPINFO_TYPE_NONE && g_settings.ctrl_word.no_overlay_block == 0){
            if(schedule_opinfo(prts, target_operator) > 0){
                next_state = PRTS_STATE_PRE_OPINFO;
            }
            else{
                next_state = PRTS_STATE_IDLE;
            }
        }
        else{
            next_state = PRTS_STATE_IDLE;
        }
    }
    else{
        log_error("schedule_video_and_transitions_end_cb: invalid state: %d", curr_state);
    }

    prts_state_lock(prts);
    prts->state = next_state;
    prts_state_unlock(prts);
    // 注意：不在这里释放 cb_data
    // oltr_callback_cleanup 或 swipe_cleanup 会统一处理释放
    // 如果在这里释放会导致 double-free 崩溃
}

static oltr_params_t first_transition_params = {
    .type = TRANSITION_TYPE_MOVE,
    .duration = 500 * 1000,
    .image_path = "",
    .image_w = 0,
    .image_h = 0,
    .image_addr = NULL,
    .background_color = 0xFF000000u,
};

// 排期视频和过渡。
// 在第一次过渡时，需要挂载视频图层，并使用move过渡。
// 在非第一次过渡时，需要使用过渡类型对应的过渡效果。
// 如果transition的type为NONE，则直接调用回调函数来切换视频并推进状态机。
// 由于回调函数那边会检定现在的状态，因此：先推进状态机，再来调用这个函数。
static void schedule_video_and_transitions(prts_t* prts, prts_video_t* video, oltr_params_t* transition, bool is_first_transition, int target_operator_index){
    schedule_video_and_transitions_cb_data_t* cb_data;
    if(prts_is_stopping(prts)){
        return;
    }
    oltr_callback_t* callback = malloc(sizeof(oltr_callback_t));
    if(callback == NULL){
        log_error("schedule_video_and_transitions: malloc failed for callback");
        prts_state_lock(prts);
        prts->state = PRTS_STATE_IDLE;
        prts_state_unlock(prts);
        return;
    }
    memset(callback, 0, sizeof(*callback));

    log_trace("schedule_video_and_transitions: video: %s, transition: %d, is_first_transition: %d, target_operator: %d",
              video->path, transition->type, is_first_transition, target_operator_index);

    // 创建 end_cb 的数据结构，包含目标干员索引
    cb_data = malloc(sizeof(*cb_data));
    if(cb_data == NULL){
        log_error("schedule_video_and_transitions: malloc failed for cb_data");
        free(callback);
        prts_state_lock(prts);
        prts->state = PRTS_STATE_IDLE;
        prts_state_unlock(prts);
        return;
    }
    cb_data->prts = prts;
    cb_data->video = video;
    cb_data->target_operator_index = target_operator_index;

    callback->middle_cb_userdata = cb_data;
    callback->end_cb = schedule_video_and_transitions_end_cb;
    callback->end_cb_userdata = cb_data;
    callback->on_heap = true;
    callback->end_cb_userdata_on_heap = true;  // 标记 end_cb_userdata 需要释放

    // 第一次发生过渡时 有两个问题:
    // 1. 需要挂载视频图层（用mount_video_layer_callback）
    // 2. 不能使用fade过渡，否则有bug（强制用move）
    // 3. swipe会需要手动填充像素，不太适合刚开机的情况。
    if(is_first_transition){
        callback->middle_cb = set_video_mount_layer_cb;
        overlay_transition_move(prts->overlay, callback, &first_transition_params);
    }
    else{
        callback->middle_cb = set_video_cb;
        switch(transition->type){
            case TRANSITION_TYPE_FADE:
                overlay_transition_fade(prts->overlay, callback, transition);
                break;
            case TRANSITION_TYPE_MOVE:
                overlay_transition_move(prts->overlay, callback, transition);
                break;
            case TRANSITION_TYPE_SWIPE:
                overlay_transition_swipe(prts->overlay, callback, transition);
                break;
            case TRANSITION_TYPE_NONE:
                // 没有过渡效果时，直接调用回调来切换视频并推进状态机
                if(callback->middle_cb){
                    callback->middle_cb(callback->middle_cb_userdata, true);
                }
                if(callback->end_cb){
                    callback->end_cb(callback->end_cb_userdata, true);
                }
                // 由于没有调用 overlay_transition_*，不会有 cleanup 定时器
                // 需要手动释放 end_cb_userdata
                if(callback->end_cb_userdata_on_heap && callback->end_cb_userdata){
                    free(callback->end_cb_userdata);
                }
                // 释放callback结构体
                if(callback->on_heap){
                    free(callback);
                }
                break;
            default:
                log_error("invalid transition type: %d", transition->type);
                // 即使类型无效，也要调用回调以避免状态机卡住
                if(callback->middle_cb){
                    callback->middle_cb(callback->middle_cb_userdata, true);
                }
                if(callback->end_cb){
                    callback->end_cb(callback->end_cb_userdata, true);
                }
                // 由于没有调用 overlay_transition_*，不会有 cleanup 定时器
                // 需要手动释放 end_cb_userdata
                if(callback->end_cb_userdata_on_heap && callback->end_cb_userdata){
                    free(callback->end_cb_userdata);
                }
                if(callback->on_heap){
                    free(callback);
                }
                break;
        }
    }
}


typedef struct {
    prts_t* prts;
    int target_index;
    prts_operator_entry_t* target_operator;
    bool *is_first_switch;
    prts_timer_handle_t timer_handle;
    bool on_heap;
} switch_operator_secound_stage_data_t;

static void switch_operator_secound_stage_data_free(void *userdata)
{
    switch_operator_secound_stage_data_t *data = (switch_operator_secound_stage_data_t *)userdata;

    if (data && data->on_heap) {
        free(data);
    }
}

static void switch_operator_secound_stage(void* userdata,bool is_last){
    switch_operator_secound_stage_data_t* data = (switch_operator_secound_stage_data_t*)userdata;
    prts_t* prts = data->prts;
    int target_index = data->target_index;
    prts_operator_entry_t* target_operator = data->target_operator;
    bool is_first_switch = *data->is_first_switch;

    prts_state_lock(prts);
    prts_clear_timer_handle_locked(&prts->switch_stage2_timer_handle, data->timer_handle);
    prts_state_unlock(prts);

    if(prts_is_stopping(prts)){
        return;
    }


    // 第一步。 存在intro video，且闭锁入场动画软压板没投
    // 则做全量 transition_in -> intro video -> transition_loop -> loop video
    if(target_operator->intro_video.enabled && g_settings.ctrl_word.no_intro_block == 0){
        prts_state_lock(prts);
        prts->state = PRTS_STATE_TRANSITION_IN;
        prts_state_unlock(prts);
        schedule_video_and_transitions(prts,
            &target_operator->intro_video,
            &target_operator->transition_in,
            is_first_switch,
            target_index
        );
    }
    // 不存在 intro video，则做 transition_in -> loop video
    // 我格式没设计好，所以明明使用的是transition_in 却进入了LOOP状态
    // 这个LOOP状态用于在回调中推进状态机。
    else{
        prts_state_lock(prts);
        prts->state = PRTS_STATE_TRANSITION_LOOP;
        prts_state_unlock(prts);
        schedule_video_and_transitions(prts,
            &target_operator->loop_video,
            &target_operator->transition_in,
            is_first_switch,
            target_index
        );
    }

    prts_state_lock(prts);
    prts->last_switch_time = get_now_us();
    prts->operator_index = target_index;
    prts_state_unlock(prts);
    *data->is_first_switch = false;
}

static void switch_operator(prts_t* prts,int target_index){
    static bool is_first_switch = true;
    int curr_operator_index;

    prts_operator_entry_t* target_operator = &prts->operators[target_index];
    prts_operator_entry_t* curr_operator;


    if(prts_is_stopping(prts)){
        return;
    }

    prts_state_lock(prts);
    if(prts->state != PRTS_STATE_IDLE){
        prts_state_t curr_state = prts->state;
        prts_state_unlock(prts);
        log_error("switch_operator: prts is not idle?? curr_state: %d", curr_state);
        return;
    }
    prts->state = PRTS_STATE_SWITCH_PENDING;
    curr_operator_index = prts->operator_index;
    prts_state_unlock(prts);
    curr_operator = &prts->operators[curr_operator_index];

    log_info("switching operator from %s to %s", curr_operator->operator_name, target_operator->operator_name);

    // 卸载当前干员。此时overlay播放消失动画，但是mediaplayer还在运行。
    if(!is_first_switch){
        overlay_abort(prts->overlay);
        overlay_opinfo_free_image(&curr_operator->opinfo_params);
        overlay_transition_free_image(&curr_operator->transition_in);
        overlay_transition_free_image(&curr_operator->transition_loop);
    }

    switch_operator_secound_stage_data_t *data = malloc(sizeof(switch_operator_secound_stage_data_t));
    if(data == NULL){
        log_error("switch_operator: malloc failed");
        prts_state_lock(prts);
        prts->state = PRTS_STATE_IDLE;
        prts_state_unlock(prts);
        return;
    }
    data->prts = prts;
    data->target_index = target_index;
    data->target_operator = target_operator;
    data->is_first_switch = &is_first_switch;
    data->timer_handle = 0;
    data->on_heap = true;

    // 先排期 overlay_abort结束后的操作（第二阶段）
    // 一旦调用第二阶段的schedule代码 就会立刻把buffer覆盖。
    // 我们要等overlay先结束之后 再进入第二阶段。
    bool run_stage2_now = false;
    prts_timer_handle_t timer_handle;
    int ret = prts_timer_create_ex(
        &timer_handle, 
        UI_LAYER_ANIMATION_DURATION, 
        0, 
        1, 
        switch_operator_secound_stage, 
        (void*)data,
        switch_operator_secound_stage_data_free
    );
    if(ret != 0){
        log_error("switch_operator: failed to create stage-2 timer: %d, falling back to immediate execution", ret);
        run_stage2_now = true;
    } else {
        prts_state_lock(prts);
        if(prts_is_stopping(prts)){
            prts_state_unlock(prts);
            prts_timer_cancel_sync(timer_handle);
            switch_operator_secound_stage_data_free((void*)data);
            return;
        }
        data->timer_handle = timer_handle;
        prts->switch_stage2_timer_handle = timer_handle;
        prts_state_unlock(prts);
    }
    
    // 加载新干员
    overlay_transition_load_image(&target_operator->transition_in);
    overlay_transition_load_image(&target_operator->transition_loop);
    overlay_opinfo_load_image(&target_operator->opinfo_params);

    if(run_stage2_now){
        switch_operator_secound_stage((void*)data, true);
        switch_operator_secound_stage_data_free((void*)data);
    }

}



static void prts_tick_cb(void* userdata,bool is_last){
    prts_t* prts = (prts_t*)userdata;
    prts_request_t* req;


    // 如果 这一次tick中 需要处理多个干员切换，我们只处理最后一次
    // 以防止切换堆叠到一起的情况。
    int target_operator_index = -1;

    // 处理所有的对普瑞塞斯的请求。
    while(spsc_bq_try_pop(&prts->req_queue, (void**)&req) == 0){
        switch(req->type){
            case PRTS_REQUEST_SET_OPERATOR:
                target_operator_index = req->operator_index;
                break;
            default:
                log_error("invalid request type: %d", req->type);
                break;
        }

        if(req->on_heap){
            free(req);
        }
    }

    if(prts_is_stopping(prts)){
        return;
    }

    settings_lock(&g_settings);
    bool interval_sw = should_switch_by_interval(prts);
    // 应当发生干员切换
    if(target_operator_index != -1 || interval_sw){
        bool not_idle;
        // 如果PRTS正在处理干员切换，则告警
        prts_state_lock(prts);
        not_idle = (prts->state != PRTS_STATE_IDLE);
        prts_state_unlock(prts);
        if(not_idle){
            ui_warning(UI_WARNING_PRTS_CONFLICT);
            log_warn("prts is busy, skip switch");
            settings_unlock(&g_settings);
            return;
        }
    }
    else{
        settings_unlock(&g_settings);
        return;
    }

    // 由 时间触发
    if(target_operator_index == -1){
        if(!ui_is_hidden()){
            log_warn("switch_operator: ui is not hidden, skip switch");
            settings_unlock(&g_settings);
            return;
        }
        target_operator_index = get_switch_target_index(prts);
    }

    switch_operator(prts, target_operator_index);

    settings_unlock(&g_settings);

    return;
}


void prts_request_set_operator(prts_t* prts,int operator_index){
    if(prts == NULL || prts_is_stopping(prts)){
        return;
    }

    prts_request_t* req = malloc(sizeof(prts_request_t));
    if(req == NULL){
        log_error("prts_request_set_operator: malloc failed");
        return;
    }
    req->type = PRTS_REQUEST_SET_OPERATOR;
    req->operator_index = operator_index;
    req->on_heap = true;
    spsc_bq_push(&prts->req_queue, (void *)req);
}


void prts_init(prts_t* prts, overlay_t* overlay, bool use_sd){
    log_info("==> PRTS Initializing...");
    prts->overlay = overlay;
    prts->state_mutex_inited = false;
    prts->timer_handle = 0;
    prts->switch_stage2_timer_handle = 0;
    prts->transition_delay_timer_handle = 0;
    prts->opinfo_timer_handle = 0;
    atomic_store(&prts->stopping, 0);
    prts->parse_log_f = NULL;

    prts_scan_catalog(prts, use_sd, true, true, NULL);

    atomic_store(&prts->is_auto_switch_blocked, 0);
    pthread_mutex_init(&prts->state_mutex, NULL);
    prts->state_mutex_inited = true;

    spsc_bq_init(&prts->req_queue, 10);

    log_info("==> PRTS will perform first switch...");
    // 进行第一次干员切换
    prts_state_lock(prts);
    prts->state = PRTS_STATE_IDLE;
    prts->operator_index = 0;
    prts_state_unlock(prts);
    switch_operator(prts, 0);
    
    int ret = prts_timer_create(
        &prts->timer_handle, 
        0,
        PRTS_TICK_PERIOD, 
        -1, 
        prts_tick_cb, 
        prts
    );
    if(ret != 0){
        prts->timer_handle = 0;
        log_error("prts_init: failed to create tick timer: %d", ret);
    }

    log_info("==> PRTS Initalized!");

}

int prts_catalog_snapshot_scan(prts_catalog_snapshot_t *snapshot, bool use_sd)
{
    prts_t temp_prts;
    bool used_fallback = false;

    if (snapshot == NULL) {
        return -1;
    }

    memset(snapshot, 0, sizeof(*snapshot));
    memset(&temp_prts, 0, sizeof(temp_prts));

    temp_prts.parse_log_f = fopen(PRTS_OPERATOR_PARSE_LOG, "w");
    if (temp_prts.parse_log_f == NULL) {
        log_error("failed to open parse log file: %s", PRTS_OPERATOR_PARSE_LOG);
    }

    snapshot->errcnt = prts_scan_catalog(&temp_prts, use_sd, false, false,
                                         &used_fallback);
    snapshot->operator_count = temp_prts.operator_count;
    snapshot->used_fallback = used_fallback;
    memcpy(snapshot->operators, temp_prts.operators, sizeof(snapshot->operators));

    if (temp_prts.parse_log_f != NULL) {
        fclose(temp_prts.parse_log_f);
        temp_prts.parse_log_f = NULL;
    }

    return snapshot->errcnt;
}

int prts_catalog_apply_snapshot(prts_t *prts,
                                const prts_catalog_snapshot_t *snapshot)
{
    prts_operator_entry_t old_operators[PRTS_OPERATORS_MAX];
    uuid_t current_uuid;
    bool has_current_uuid = false;
    bool current_found = false;
    prts_state_t curr_state;
    int old_count;
    int target_index = 0;

    if (prts == NULL || snapshot == NULL || prts_is_stopping(prts)) {
        return -1;
    }

    prts_state_lock(prts);
    curr_state = prts->state;
    if (curr_state != PRTS_STATE_IDLE) {
        prts_state_unlock(prts);
        log_warn("prts_catalog_apply_snapshot: skip reload, state=%d", curr_state);
        return -1;
    }

    old_count = prts->operator_count;
    memcpy(old_operators, prts->operators, sizeof(old_operators));
    if (old_count > 0 &&
        prts->operator_index >= 0 &&
        prts->operator_index < old_count) {
        current_uuid = old_operators[prts->operator_index].uuid;
        has_current_uuid = true;
    }
    prts_state_unlock(prts);

    if (snapshot->errcnt != 0) {
        ui_warning(UI_WARNING_ASSET_ERROR);
    }
    if (snapshot->used_fallback) {
        ui_warning(UI_WARNING_NO_ASSETS);
    }

    memset(prts->operators, 0, sizeof(prts->operators));
    memcpy(prts->operators, snapshot->operators, sizeof(snapshot->operators));
    prts->operator_count = snapshot->operator_count;

    for (int i = 0; i < prts->operator_count; i++) {
        if (has_current_uuid &&
            uuid_compare(&prts->operators[i].uuid, &current_uuid)) {
            target_index = i;
            current_found = true;
            break;
        }
    }

    for (int i = 0; i < old_count; i++) {
        overlay_opinfo_free_image(&old_operators[i].opinfo_params);
        overlay_transition_free_image(&old_operators[i].transition_in);
        overlay_transition_free_image(&old_operators[i].transition_loop);
    }

    if (has_current_uuid && prts->operator_count > 0 &&
        !uuid_compare(&prts->operators[target_index].uuid, &current_uuid)) {
        log_warn("prts_reload_catalog: current operator removed, fallback to index 0");
        target_index = 0;
    }

    prts_state_lock(prts);
    prts->state = PRTS_STATE_IDLE;
    prts->operator_index = target_index;
    prts_state_unlock(prts);

    if (has_current_uuid && !current_found) {
        switch_operator(prts, target_index);
    }

    log_info("==> PRTS catalog reloaded! %d operators loaded", prts->operator_count);
    return snapshot->errcnt;
}

int prts_reload_catalog(prts_t *prts, bool use_sd)
{
    prts_catalog_snapshot_t snapshot;

    if (prts == NULL || prts_is_stopping(prts)) {
        return -1;
    }

    prts_catalog_snapshot_scan(&snapshot, use_sd);
    return prts_catalog_apply_snapshot(prts, &snapshot);
}

void prts_stop(prts_t* prts){
    prts_timer_handle_t handles[4] = {0};
    int handle_count = 0;

    if(prts == NULL){
        return;
    }

    atomic_store(&prts->stopping, 1);

    if(prts->state_mutex_inited){
        prts_state_lock(prts);
        handles[handle_count++] = prts->timer_handle;
        handles[handle_count++] = prts->switch_stage2_timer_handle;
        handles[handle_count++] = prts->transition_delay_timer_handle;
        handles[handle_count++] = prts->opinfo_timer_handle;
        prts->timer_handle = 0;
        prts->switch_stage2_timer_handle = 0;
        prts->transition_delay_timer_handle = 0;
        prts->opinfo_timer_handle = 0;
        prts->state = PRTS_STATE_IDLE;
        prts_state_unlock(prts);
    } else {
        handles[handle_count++] = prts->timer_handle;
        handles[handle_count++] = prts->switch_stage2_timer_handle;
        handles[handle_count++] = prts->transition_delay_timer_handle;
        handles[handle_count++] = prts->opinfo_timer_handle;
        prts->timer_handle = 0;
        prts->switch_stage2_timer_handle = 0;
        prts->transition_delay_timer_handle = 0;
        prts->opinfo_timer_handle = 0;
    }

    for(int i = 0; i < handle_count; i++){
        if(handles[i] != 0){
            prts_timer_cancel_sync(handles[i]);
        }
    }
}

void prts_destroy(prts_t* prts){
    prts_request_t* req;

    prts_stop(prts);

    if(prts->parse_log_f != NULL){
        fclose(prts->parse_log_f);
        prts->parse_log_f = NULL;
    }

    while(spsc_bq_try_pop(&prts->req_queue, (void**)&req) == 0){
        if(req && req->on_heap){
            free(req);
        }
    }
    spsc_bq_destroy(&prts->req_queue);

    if(prts->state_mutex_inited){
        pthread_mutex_destroy(&prts->state_mutex);
        prts->state_mutex_inited = false;
    }

    for(int i = 0; i < prts->operator_count; i++){
        overlay_opinfo_free_image(&prts->operators[i].opinfo_params);
        overlay_transition_free_image(&prts->operators[i].transition_in);
        overlay_transition_free_image(&prts->operators[i].transition_loop);
    }
}
