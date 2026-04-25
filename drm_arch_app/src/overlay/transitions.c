#include <render/fbdraw.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "utils/log.h"
#include "config.h"
#include "overlay/overlay.h"
#include "overlay/transitions.h"
#include "driver/drm_warpper.h"
#include "utils/timer.h"
#include "render/layer_animation.h"

#define STB_IMAGE_IMPLEMENTATION
#include "utils/stb_image.h"

static void oltr_callback_free(oltr_callback_t* callback){
    if(callback == NULL){
        return;
    }
    if(callback->end_cb_userdata_on_heap && callback->end_cb_userdata){
        free(callback->end_cb_userdata);
        callback->end_cb_userdata = NULL;
        callback->end_cb_userdata_on_heap = false;
    }
    if(callback->on_heap){
        free(callback);
    }
}

static void oltr_callback_cleanup(void* userdata,bool is_last){
    (void)userdata;
    (void)is_last;
}

static void oltr_callback_cleanup_dtor(void *userdata){
    oltr_callback_t* callback = (oltr_callback_t*)userdata;
    overlay_t* overlay;

    if(callback == NULL){
        return;
    }

    log_trace("oltr_callback_cleanup");
    overlay = callback->owner_overlay;
    if(callback->clear_overlay_on_cleanup && overlay != NULL && overlay->worker.sync_inited){
        pthread_mutex_lock(&overlay->worker.mutex);
        overlay->overlay_timer_handle = 0;
        overlay->overlay_timer_kind = OVERLAY_TIMER_KIND_NONE;
        memset(overlay->layer_animation_handles, 0, sizeof(overlay->layer_animation_handles));
        overlay->layer_animation_handle_count = 0;
        overlay->overlay_used = 0;
        pthread_cond_broadcast(&overlay->worker.idle_cv);
        pthread_mutex_unlock(&overlay->worker.mutex);
    }
    oltr_callback_free(callback);
    log_trace("oltr_callback_cleanup: done");
}

static void oltr_callback_run_now(oltr_callback_t* callback){
    if(callback == NULL){
        return;
    }
    if(callback->middle_cb){
        callback->middle_cb(callback->middle_cb_userdata, true);
    }
    if(callback->end_cb){
        callback->end_cb(callback->end_cb_userdata, true);
    }
}

static void oltr_cancel_callback_timers(overlay_t* overlay,
                                        prts_timer_handle_t middle_cb_handler,
                                        prts_timer_handle_t end_cb_handler,
                                        prts_timer_handle_t callback_cleanup_handler){
    if(middle_cb_handler){
        prts_timer_cancel_sync(middle_cb_handler);
    }
    if(end_cb_handler){
        prts_timer_cancel_sync(end_cb_handler);
    }
    if(callback_cleanup_handler){
        prts_timer_cancel_sync(callback_cleanup_handler);
        if(overlay != NULL && overlay->worker.sync_inited){
            pthread_mutex_lock(&overlay->worker.mutex);
            if(overlay->overlay_timer_handle == callback_cleanup_handler){
                overlay->overlay_timer_handle = 0;
                overlay->overlay_timer_kind = OVERLAY_TIMER_KIND_NONE;
                overlay->overlay_used = 0;
                pthread_cond_broadcast(&overlay->worker.idle_cv);
            }
            pthread_mutex_unlock(&overlay->worker.mutex);
        }
    }
}

static int oltr_schedule_callback_timers(overlay_t* overlay,
                                         oltr_callback_t* callback,
                                         uint64_t middle_delay_us,
                                         uint64_t end_delay_us,
                                         prts_timer_handle_t *out_middle_cb_handler,
                                         prts_timer_handle_t *out_end_cb_handler,
                                         prts_timer_handle_t *out_callback_cleanup_handler){
    int ret;
    prts_timer_handle_t middle_cb_handler = 0;
    prts_timer_handle_t end_cb_handler = 0;
    prts_timer_handle_t callback_cleanup_handler = 0;

    if(overlay == NULL || callback == NULL){
        return -1;
    }

    if(callback->middle_cb){
        ret = prts_timer_create(
            &middle_cb_handler,
            middle_delay_us,
            0,
            1,
            callback->middle_cb,
            callback->middle_cb_userdata
        );
        if(ret != 0){
            log_error("failed to create transition middle callback timer: %d", ret);
            goto fail;
        }
    }

    if(callback->end_cb){
        ret = prts_timer_create(
            &end_cb_handler,
            end_delay_us,
            0,
            1,
            callback->end_cb,
            callback->end_cb_userdata
        );
        if(ret != 0){
            log_error("failed to create transition end callback timer: %d", ret);
            goto fail;
        }
    }

    ret = prts_timer_create_ex(
        &callback_cleanup_handler,
        end_delay_us + 500 * 1000,
        0,
        1,
        oltr_callback_cleanup,
        callback,
        oltr_callback_cleanup_dtor
    );
    if(ret != 0){
        log_error("failed to create transition cleanup timer: %d", ret);
        goto fail;
    }

    callback->owner_overlay = overlay;
    callback->clear_overlay_on_cleanup = true;
    pthread_mutex_lock(&overlay->worker.mutex);
    overlay->overlay_used = 1;
    overlay->overlay_timer_handle = callback_cleanup_handler;
    overlay->overlay_timer_kind = OVERLAY_TIMER_KIND_CLEANUP;
    pthread_mutex_unlock(&overlay->worker.mutex);

    if(out_middle_cb_handler){
        *out_middle_cb_handler = middle_cb_handler;
    }
    if(out_end_cb_handler){
        *out_end_cb_handler = end_cb_handler;
    }
    if(out_callback_cleanup_handler){
        *out_callback_cleanup_handler = callback_cleanup_handler;
    }

    return 0;

fail:
    if(callback_cleanup_handler){
        oltr_cancel_callback_timers(overlay, middle_cb_handler, end_cb_handler, callback_cleanup_handler);
    } else {
        if(middle_cb_handler){
            prts_timer_cancel_sync(middle_cb_handler);
        }
        if(end_cb_handler){
            prts_timer_cancel_sync(end_cb_handler);
        }
        oltr_callback_free(callback);
    }
    return -1;
}

// 渐变过渡，准备完成后无耗时操作，不需要使用worker。
void overlay_transition_fade(overlay_t* overlay,oltr_callback_t* callback,oltr_params_t* params){
    drm_warpper_queue_item_t* item;
    fbdraw_fb_t fbsrc,fbdst;
    fbdraw_rect_t src_rect,dst_rect;
    prts_timer_handle_t middle_cb_handler = 0;
    prts_timer_handle_t end_cb_handler = 0;
    prts_timer_handle_t callback_cleanup_handler = 0;
    animation_driver_handle_t layer_anim_handles[2] = {0};

    if(overlay == NULL || callback == NULL || params == NULL || params->duration <= 0){
        log_error("invalid fade transition params");
        oltr_callback_run_now(callback);
        oltr_callback_free(callback);
        return;
    }

    overlay_cancel_layer_animations(overlay);
    atomic_store(&overlay->request_abort, 0);

    drm_warpper_set_layer_alpha(overlay->drm_warpper, DRM_WARPPER_LAYER_OVERLAY, 0);
    drm_warpper_set_layer_coord(overlay->drm_warpper, DRM_WARPPER_LAYER_OVERLAY, 0, 0);

    // 此处亦有等待vsync的功能。
    // get a free buffer to draw on

    drm_warpper_dequeue_free_item(overlay->drm_warpper, DRM_WARPPER_LAYER_OVERLAY, &item);
    uint32_t* vaddr = (uint32_t*)item->mount.arg0;

    for(int y = 0; y < OVERLAY_HEIGHT; y++){
        for(int x = 0; x < OVERLAY_WIDTH; x++){
            vaddr[x + y * OVERLAY_WIDTH] = params->background_color;
        }
    }

    if(params->image_addr){
        fbsrc.vaddr = params->image_addr;
        fbsrc.width = params->image_w;
        fbsrc.height = params->image_h;

        src_rect.x = 0;
        src_rect.y = 0;
        src_rect.w = params->image_w;
        src_rect.h = params->image_h;

        fbdst.vaddr = vaddr;
        fbdst.width = OVERLAY_WIDTH;
        fbdst.height = OVERLAY_HEIGHT;

        dst_rect.x = OVERLAY_WIDTH / 2  - params->image_w / 2;
        dst_rect.y = OVERLAY_HEIGHT / 2 - params->image_h / 2;
        dst_rect.w = params->image_w;
        dst_rect.h = params->image_h;
        fbdraw_copy_rect(&fbsrc, &fbdst, &src_rect, &dst_rect);
    }

    drm_warpper_enqueue_display_item(overlay->drm_warpper, DRM_WARPPER_LAYER_OVERLAY, item);

    if(oltr_schedule_callback_timers(
        overlay,
        callback,
        params->duration,
        3 * params->duration,
        &middle_cb_handler,
        &end_cb_handler,
        &callback_cleanup_handler
    ) != 0){
        oltr_callback_run_now(callback);
        oltr_callback_free(callback);
        return;
    }

    if(layer_animation_fade_in_ex(
        overlay->layer_animation,
        DRM_WARPPER_LAYER_OVERLAY,
        params->duration,
        0,
        &layer_anim_handles[0]
    ) != 0 || layer_animation_fade_out_ex(
        overlay->layer_animation,
        DRM_WARPPER_LAYER_OVERLAY,
        params->duration,
        2 * params->duration,
        &layer_anim_handles[1]
    ) != 0){
        log_error("failed to create fade transition animations");
        if(layer_anim_handles[0]){
            animation_driver_cancel_sync(overlay->layer_animation->animation_driver,
                                         layer_anim_handles[0]);
        }
        if(layer_anim_handles[1]){
            animation_driver_cancel_sync(overlay->layer_animation->animation_driver,
                                         layer_anim_handles[1]);
        }
        oltr_callback_run_now(callback);
        oltr_cancel_callback_timers(overlay, middle_cb_handler, end_cb_handler, callback_cleanup_handler);
        return;
    }

    overlay_store_layer_animation_handles(overlay, layer_anim_handles, 2);
}

// 贝塞尔函数移动过渡。 不需要使用worker
void overlay_transition_move(overlay_t* overlay,oltr_callback_t* callback,oltr_params_t* params){
    drm_warpper_queue_item_t* item;
    fbdraw_fb_t fbsrc,fbdst;
    fbdraw_rect_t src_rect,dst_rect;
    prts_timer_handle_t middle_cb_handler = 0;
    prts_timer_handle_t end_cb_handler = 0;
    prts_timer_handle_t callback_cleanup_handler = 0;
    animation_driver_handle_t layer_anim_handles[2] = {0};

    if(overlay == NULL || callback == NULL || params == NULL || params->duration <= 0){
        log_error("invalid move transition params");
        oltr_callback_run_now(callback);
        oltr_callback_free(callback);
        return;
    }

    overlay_cancel_layer_animations(overlay);
    atomic_store(&overlay->request_abort, 0);

    drm_warpper_set_layer_coord(overlay->drm_warpper, DRM_WARPPER_LAYER_OVERLAY, SCREEN_WIDTH, 0);
    drm_warpper_set_layer_alpha(overlay->drm_warpper, DRM_WARPPER_LAYER_OVERLAY, 255);

    // 此处亦有等待vsync的功能。
    // get a free buffer to draw on

    drm_warpper_dequeue_free_item(overlay->drm_warpper, DRM_WARPPER_LAYER_OVERLAY, &item);
    uint32_t* vaddr = (uint32_t*)item->mount.arg0;

    for(int y = 0; y < OVERLAY_HEIGHT; y++){
        for(int x = 0; x < OVERLAY_WIDTH; x++){
            vaddr[x + y * OVERLAY_WIDTH] = params->background_color;
        }
    }

    if(params->image_addr){
        fbsrc.vaddr = params->image_addr;
        fbsrc.width = params->image_w;
        fbsrc.height = params->image_h;
        src_rect.x = 0;
        src_rect.y = 0;
        src_rect.w = params->image_w;
        src_rect.h = params->image_h;
        fbdst.vaddr = vaddr;
        fbdst.width = OVERLAY_WIDTH;
        fbdst.height = OVERLAY_HEIGHT;
        dst_rect.x = OVERLAY_WIDTH / 2  - params->image_w / 2;
        dst_rect.y = OVERLAY_HEIGHT / 2 - params->image_h / 2;
        dst_rect.w = params->image_w;
        dst_rect.h = params->image_h;
        fbdraw_copy_rect(&fbsrc, &fbdst, &src_rect, &dst_rect);
    }
    drm_warpper_enqueue_display_item(overlay->drm_warpper, DRM_WARPPER_LAYER_OVERLAY, item);


    if(oltr_schedule_callback_timers(
        overlay,
        callback,
        params->duration,
        3 * params->duration,
        &middle_cb_handler,
        &end_cb_handler,
        &callback_cleanup_handler
    ) != 0){
        oltr_callback_run_now(callback);
        oltr_callback_free(callback);
        return;
    }

    if(layer_animation_ease_out_move_ex(
        overlay->layer_animation,
        DRM_WARPPER_LAYER_OVERLAY,
        SCREEN_WIDTH, 0,
        0, 0,
        params->duration,
        0,
        &layer_anim_handles[0]
    ) != 0 || layer_animation_ease_in_move_ex(
        overlay->layer_animation,
        DRM_WARPPER_LAYER_OVERLAY,
        0, 0,
        -SCREEN_WIDTH, 0,
        params->duration,
        2 * params->duration,
        &layer_anim_handles[1]
    ) != 0){
        log_error("failed to create move transition animations");
        if(layer_anim_handles[0]){
            animation_driver_cancel_sync(overlay->layer_animation->animation_driver,
                                         layer_anim_handles[0]);
        }
        if(layer_anim_handles[1]){
            animation_driver_cancel_sync(overlay->layer_animation->animation_driver,
                                         layer_anim_handles[1]);
        }
        oltr_callback_run_now(callback);
        oltr_cancel_callback_timers(overlay, middle_cb_handler, end_cb_handler, callback_cleanup_handler);
        return;
    }

    overlay_store_layer_animation_handles(overlay, layer_anim_handles, 2);

}

typedef struct {
    overlay_t* overlay;
    oltr_params_t* params;

    int curr_frame;
    int total_frames;
    int frames_per_stage;

    int image_start_x;
    int image_end_x;

    int image_start_y;


    int* bezeir_values;

    bool middle_cb_called;
    oltr_callback_t* callback;

} swipe_worker_data_t;

typedef enum{
    SWIPE_DRAW_CONTENT,
    SWIPE_DRAW_IDLE,
    SWIPE_DRAW_CLEAR
} swipe_draw_state_t;


static void swipe_cleanup(swipe_worker_data_t* data){
    animation_driver_handle_t timer_handle = 0;

    if(data->overlay->overlay_worker_tick_handle){
        timer_handle = data->overlay->overlay_worker_tick_handle;
        animation_driver_cancel_sync(data->overlay->layer_animation->animation_driver,
                                     timer_handle);
    }
    free(data->bezeir_values);
    data->bezeir_values = NULL;
    pthread_mutex_lock(&data->overlay->worker.mutex);
    data->overlay->overlay_worker_tick_handle = 0;
    data->overlay->overlay_timer_handle = 0;
    data->overlay->overlay_timer_kind = OVERLAY_TIMER_KIND_NONE;
    data->overlay->overlay_used = 0;
    pthread_cond_broadcast(&data->overlay->worker.idle_cv);
    pthread_mutex_unlock(&data->overlay->worker.mutex);
    oltr_callback_free(data->callback);
    free(data);
    return;
}

// 每一帧绘制的时候来调用。 来自 overlay_worker 线程
// 双缓冲的建议按照状态机+绘制的方法来写。
// 先计算这次要画哪些东西，然后绘制，最后交换buffer。
static void swipe_worker(void *userdata,int skipped_frames){
    swipe_worker_data_t* data = (swipe_worker_data_t*)userdata;
    // log_trace("swipe_worker: skipped_frames=%d,curr_frame=%d,total_frames=%d", skipped_frames, data->curr_frame, data->total_frames);
    
    // 是否要求我们退出
    if(atomic_load(&data->overlay->request_abort)){
        swipe_cleanup(data);
        log_debug("swipe worker: request abort");
        return;
    }
    
    drm_warpper_set_layer_coord(data->overlay->drm_warpper, DRM_WARPPER_LAYER_OVERLAY, 0, 0);
    drm_warpper_queue_item_t* item;
    drm_warpper_dequeue_free_item(data->overlay->drm_warpper, DRM_WARPPER_LAYER_OVERLAY, &item);
    uint32_t* vaddr = (uint32_t*)item->mount.arg0;


    // 状态转移 START
    swipe_draw_state_t draw_state;
    int draw_start_x;
    int draw_end_x;


    if (data->curr_frame < data->frames_per_stage){
        draw_state = SWIPE_DRAW_CONTENT;

        draw_start_x = data->bezeir_values[data->curr_frame];
        if(data->curr_frame < 2){
            draw_start_x = 0;    
        }
        // double buffer，一次性需要推进两帧的内容
        if(data->curr_frame + 2 < data->frames_per_stage){
            draw_end_x = data->bezeir_values[data->curr_frame + 2];
        }
        else{
            draw_end_x = UI_WIDTH;
        }
    }
    else if (data->curr_frame < 2 * data->frames_per_stage){
        draw_state = SWIPE_DRAW_IDLE;
        // stub to make compiler happy.
        draw_start_x = 0;
        draw_end_x = 0;
        if(!data->middle_cb_called){
            if(data->callback->middle_cb){
                data->callback->middle_cb(data->callback->middle_cb_userdata, false);
            }
            data->middle_cb_called = true;
        }
    }
    else{
        draw_state = SWIPE_DRAW_CLEAR;
        draw_start_x = data->bezeir_values[data->curr_frame - 2 * data->frames_per_stage];
        // 进入本状态的头两帧（两个buffer）都从最左边画起
        if(data->curr_frame - 2 * data->frames_per_stage < 2){
            draw_start_x = 0;
        }
        if(data->curr_frame + 2 < 3 * data->frames_per_stage){
            draw_end_x = data->bezeir_values[data->curr_frame - 2 * data->frames_per_stage + 2];
        }
        else{
            draw_end_x = UI_WIDTH;
        }
    }


    int draw_image_start_x;
    int draw_image_end_x;
    int draw_image_w;

    if(draw_state == SWIPE_DRAW_CONTENT){
        if(draw_start_x < data->image_start_x){
            draw_image_start_x = data->image_start_x;
        }
        else{
            draw_image_start_x = draw_start_x;
        }
        if(draw_end_x > data->image_end_x){
            draw_image_end_x = data->image_end_x;
        }
        else{
            draw_image_end_x = draw_end_x;
        }

        draw_image_w = draw_image_end_x - draw_image_start_x;
        if(draw_image_w < 0){
            draw_image_w = 0;
        }
    }
    // 状态转移 END

    // 绘制 START
    fbdraw_fb_t fbsrc,fbdst;
    fbdraw_rect_t src_rect,dst_rect;

    fbdst.vaddr = vaddr;
    fbdst.width = OVERLAY_WIDTH;
    fbdst.height = OVERLAY_HEIGHT;

    if(draw_state == SWIPE_DRAW_CONTENT){
        // 填充颜色
        dst_rect.x = draw_start_x;
        dst_rect.y = 0;
        dst_rect.w = draw_end_x - draw_start_x;
        dst_rect.h = OVERLAY_HEIGHT;
        fbdraw_fill_rect(&fbdst, &dst_rect, data->params->background_color);

        // 绘制图片
        if(data->params->image_addr != NULL && data->params->image_w > 0 && data->params->image_h > 0 && draw_image_w > 0){
            fbsrc.vaddr = data->params->image_addr;
            fbsrc.width = data->params->image_w;
            fbsrc.height = data->params->image_h;

            src_rect.x = draw_image_start_x - data->image_start_x;
            src_rect.y = 0;
            src_rect.w = draw_image_w;
            src_rect.h = data->params->image_h;

            dst_rect.x = draw_image_start_x;
            dst_rect.y = data->image_start_y;
            dst_rect.w = draw_image_w;
            dst_rect.h = OVERLAY_HEIGHT;
            fbdraw_copy_rect(&fbsrc, &fbdst, &src_rect, &dst_rect);
        }
    }
    else if(draw_state == SWIPE_DRAW_CLEAR){
        // 填充颜色
        dst_rect.x = draw_start_x;
        dst_rect.y = 0;
        dst_rect.w = draw_end_x - draw_start_x;
        dst_rect.h = OVERLAY_HEIGHT;
        fbdraw_fill_rect(&fbdst, &dst_rect, 0x00000000);
    }

    drm_warpper_enqueue_display_item(data->overlay->drm_warpper, DRM_WARPPER_LAYER_OVERLAY, item);
    data->curr_frame ++;
    if(data->curr_frame >= data->total_frames){
        if(data->callback->end_cb){
            data->callback->end_cb(data->callback->end_cb_userdata, true);
        }
        swipe_cleanup(data);
        return;
    }

}

// 定时器回调。来自普瑞塞斯 的 rt 启动的 sigev_thread 线程。
static void swipe_timer_cb(void *userdata,bool is_last){
    swipe_worker_data_t* data = (swipe_worker_data_t*)userdata;
    overlay_worker_schedule(data->overlay,swipe_worker,data);
}

// 类似drm_app的过渡效果，但是使用贝塞尔，需要使用worker。
void overlay_transition_swipe(overlay_t* overlay,oltr_callback_t* callback,oltr_params_t* params){
    drm_warpper_queue_item_t* item;
    uint32_t* vaddr;
    swipe_worker_data_t* swipe_worker_data;
    int frames_per_stage;
    int total_frames;

    if(overlay == NULL || callback == NULL || params == NULL || params->duration <= 0){
        log_error("invalid swipe transition params");
        oltr_callback_run_now(callback);
        oltr_callback_free(callback);
        return;
    }

    overlay_cancel_layer_animations(overlay);
    frames_per_stage = params->duration / OVERLAY_ANIMATION_STEP_TIME;
    total_frames = 3 * params->duration / OVERLAY_ANIMATION_STEP_TIME;
    if(frames_per_stage <= 0 || total_frames <= 0){
        log_error("invalid swipe transition duration: %d", params->duration);
        oltr_callback_run_now(callback);
        oltr_callback_free(callback);
        return;
    }


    // 清空双缓冲buffer
    drm_warpper_dequeue_free_item(overlay->drm_warpper, DRM_WARPPER_LAYER_OVERLAY, &item);
    vaddr = (uint32_t*)item->mount.arg0;
    memset(vaddr, 0, OVERLAY_WIDTH * OVERLAY_HEIGHT * 4);
    drm_warpper_enqueue_display_item(overlay->drm_warpper, DRM_WARPPER_LAYER_OVERLAY, item);

    // 刚刚挂上去的buffer已经是空buffer了 可以上坐标了
    drm_warpper_set_layer_alpha(overlay->drm_warpper, DRM_WARPPER_LAYER_OVERLAY, 255);
    drm_warpper_set_layer_coord(overlay->drm_warpper, DRM_WARPPER_LAYER_OVERLAY, 0, 0);
    
    drm_warpper_dequeue_free_item(overlay->drm_warpper, DRM_WARPPER_LAYER_OVERLAY, &item);
    vaddr = (uint32_t*)item->mount.arg0;
    memset(vaddr, 0, OVERLAY_WIDTH * OVERLAY_HEIGHT * 4);
    drm_warpper_enqueue_display_item(overlay->drm_warpper, DRM_WARPPER_LAYER_OVERLAY, item);
    



    swipe_worker_data = calloc(1, sizeof(*swipe_worker_data));
    if(swipe_worker_data == NULL){
        log_error("malloc failed??");
        oltr_callback_free(callback);
        return;
    }

    swipe_worker_data->overlay = overlay;
    swipe_worker_data->curr_frame = 0;
    swipe_worker_data->frames_per_stage = frames_per_stage;
    swipe_worker_data->total_frames = total_frames;
    swipe_worker_data->middle_cb_called = false;
    swipe_worker_data->callback = callback;
    swipe_worker_data->params = params;
    swipe_worker_data->image_start_x = UI_WIDTH / 2 - params->image_w / 2;
    swipe_worker_data->image_end_x = UI_WIDTH / 2 + params->image_w / 2;
    swipe_worker_data->image_start_y = UI_HEIGHT / 2 - params->image_h / 2;

    swipe_worker_data->bezeir_values = malloc(swipe_worker_data->frames_per_stage * sizeof(int));
    if(swipe_worker_data->bezeir_values == NULL){
        log_error("malloc failed??");
        free(swipe_worker_data);
        oltr_callback_free(callback);
        return;
    }

    int32_t ctlx1 = LV_BEZIER_VAL_FLOAT(0.42);
    int32_t ctly1 = LV_BEZIER_VAL_FLOAT(0);
    int32_t ctx2 = LV_BEZIER_VAL_FLOAT(0.58);
    int32_t cty2 = LV_BEZIER_VAL_FLOAT(1);


    for(int i = 0; i < swipe_worker_data->frames_per_stage; i++){
        uint32_t t = lv_map(i, 0, swipe_worker_data->frames_per_stage, 0, LV_BEZIER_VAL_MAX);
        int32_t step = lv_cubic_bezier(t, ctlx1, ctly1, ctx2, cty2);
        int32_t new_value;
        new_value = step * UI_WIDTH;
        new_value = new_value >> LV_BEZIER_VAL_SHIFT;
        swipe_worker_data->bezeir_values[i] = new_value;
    }

    atomic_store(&overlay->request_abort, 0);

    // 我们在这里设置永远触发，其实是在worker里面注销定时器。
    // 我们要保证，就算有跳帧发生，最后一次触发的事件也能传到我们的回调里面
    // 在那里处理资源回收的问题。

    // 不在定时器回调用is_last处理资源回收的原因是，worker和定时器回调是两个线程
    // 如果worker在运行的时候你free了它的内存，就会直接UAF。
    animation_driver_handle_t timer_handle = 0;
    if(animation_driver_create(
        overlay->layer_animation->animation_driver,
        &timer_handle,
        0,
        -1,
        swipe_timer_cb,
        swipe_worker_data
    ) != 0){
        log_error("failed to create swipe transition animation driver handle");
        drm_warpper_set_layer_coord(overlay->drm_warpper, DRM_WARPPER_LAYER_OVERLAY, 0, OVERLAY_HEIGHT);
        oltr_callback_run_now(callback);
        swipe_cleanup(swipe_worker_data);
        return;
    }

    pthread_mutex_lock(&overlay->worker.mutex);
    overlay->overlay_worker_tick_handle = timer_handle;
    overlay->overlay_timer_kind = OVERLAY_TIMER_KIND_WORKER;
    overlay->overlay_used = 1;
    pthread_mutex_unlock(&overlay->worker.mutex);

}

void overlay_transition_load_image(oltr_params_t* params){
    if(params == NULL){
        return;
    }
    if(params->type == TRANSITION_TYPE_NONE){
        return;
    }
    if(load_img_assets(params->image_path, &params->image_addr, &params->image_w, &params->image_h) != 0){
        log_warn("(transition) failed to load image: %s", params->image_path);
    }
    log_debug("(transition) loaded image: %s, w: %d, h: %d", params->image_path, params->image_w, params->image_h);
}

void overlay_transition_free_image(oltr_params_t* params){
    if(params == NULL){
        return;
    }
    if(params->image_addr){
        free(params->image_addr);
        params->image_addr = NULL;
        log_debug("(transition) freed image: %s", params->image_path);
    }
}
