#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "overlay/overlay.h"
#include "config.h"
#include "driver/drm_warpper.h"
#include "driver/srgn_drm.h"
#include "utils/log.h"
#include "utils/timer.h"
#include "render/layer_animation.h"
#include "utils/stb_image.h"


int load_img_assets(char *image_path, uint32_t** addr,int* w,int* h){
    int c;

    if(addr == NULL || w == NULL || h == NULL || image_path == NULL){
        log_error("invalid image arguments");
        return -1;
    }

    *addr = NULL;
    *w = 0;
    *h = 0;

    if(image_path[0] == '\0'){
        return 0;
    }

    uint8_t* pixdata = stbi_load(image_path, w, h, &c, 4);
    if(!pixdata){
        log_error("failed to load image: %s", image_path);
        return -1;
    }
    *addr = malloc((*w) * (*h) * 4);
    if(!*addr){
        log_error("failed to malloc memory: %s", image_path);
        stbi_image_free(pixdata);
        pixdata = NULL;
        return -1;
    }
    for(int y = 0; y < (*h); y++){
        for(int x = 0; x < (*w); x++){
            uint32_t bgra_pixel = *((uint32_t *)(pixdata) + x + y * (*w));
            uint32_t rgb_pixel = (bgra_pixel & 0x000000FF) << 16 | (bgra_pixel & 0x0000FF00) | (bgra_pixel & 0x00FF0000) >> 16 | (bgra_pixel & 0xFF000000);
            *((uint32_t *)(*addr) + x + y * (*w)) = rgb_pixel;
        }
    }
    stbi_image_free(pixdata);
    pixdata = NULL;
    return 0;
}

static void overlay_clear_layer_animation_handles_locked(overlay_t* overlay){
    memset(overlay->layer_animation_handles, 0, sizeof(overlay->layer_animation_handles));
    overlay->layer_animation_handle_count = 0;
}

void overlay_store_layer_animation_handles(overlay_t* overlay,const animation_driver_handle_t* handles,int handle_count){
    int stored = 0;

    if(overlay == NULL || !overlay->worker.sync_inited){
        return;
    }

    pthread_mutex_lock(&overlay->worker.mutex);
    overlay_clear_layer_animation_handles_locked(overlay);
    if(handles != NULL && handle_count > 0){
        for(int i = 0; i < handle_count && stored < OVERLAY_LAYER_ANIMATION_HANDLE_MAX; i++){
            if(handles[i] == 0){
                continue;
            }
            overlay->layer_animation_handles[stored++] = handles[i];
        }
    }
    overlay->layer_animation_handle_count = stored;
    pthread_mutex_unlock(&overlay->worker.mutex);
}

void overlay_cancel_layer_animations(overlay_t* overlay){
    animation_driver_handle_t handles[OVERLAY_LAYER_ANIMATION_HANDLE_MAX];
    int handle_count;

    if(overlay == NULL || !overlay->worker.sync_inited){
        return;
    }

    memset(handles, 0, sizeof(handles));

    pthread_mutex_lock(&overlay->worker.mutex);
    handle_count = overlay->layer_animation_handle_count;
    if(handle_count > OVERLAY_LAYER_ANIMATION_HANDLE_MAX){
        handle_count = OVERLAY_LAYER_ANIMATION_HANDLE_MAX;
    }
    for(int i = 0; i < handle_count; i++){
        handles[i] = overlay->layer_animation_handles[i];
    }
    overlay_clear_layer_animation_handles_locked(overlay);
    pthread_mutex_unlock(&overlay->worker.mutex);

    for(int i = 0; i < handle_count; i++){
        if(handles[i] != 0){
            animation_driver_cancel_sync(overlay->layer_animation->animation_driver, handles[i]);
        }
    }
}

uint64_t get_us(void){
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    uint64_t time_us = t.tv_sec * 1000000 + (t.tv_nsec / 1000);
    return time_us;
}


// 我们向普瑞塞斯(timer)承诺了一切操作都会尽快完成，也就是说，定时器回调正在运行的时候，不应该再次触发定时器回调。
// 因此，在overlay层进行的一切耗时操作，都需要通过worker来完成
void* overlay_worker_thread(void* arg){
    overlay_t* overlay = (overlay_t*)arg;
    overlay_worker_t* worker = &overlay->worker;
    log_info("==> Overlay Worker Thread Started!");
    while(atomic_load(&worker->running)){
        pthread_mutex_lock(&worker->mutex);

        while(!worker->pending){
            pthread_cond_wait(&worker->cond, &worker->mutex);
            if(!atomic_load(&worker->running)){
                goto worker_end;
            }
        }

        worker->pending = 0;
        worker->in_progress = 1;
        // 这里不是原子性的，如果在耗时操作的时候还是发生了跳帧
        // 我们只能扣掉在这次func里处理过的跳帧，剩下的交给下一次处理。
        int processed_skipped_frame = worker->skipped_frames;

        pthread_mutex_unlock(&worker->mutex);

        // 不要持锁干耗时操作，否则堵塞prts_timer
        worker->func(worker->userdata,processed_skipped_frame);

        pthread_mutex_lock(&worker->mutex);
        worker->skipped_frames -= processed_skipped_frame;
        worker->in_progress = 0;
        pthread_cond_broadcast(&worker->idle_cv);
        pthread_mutex_unlock(&worker->mutex);
    }

worker_end:
    log_info("==> Overlay Worker Thread Ended!");
    return NULL;
}

void overlay_worker_schedule(overlay_t* overlay,void (*func)(void *userdata,int skipped_frames),void* userdata){
    overlay_worker_t* worker = &overlay->worker;
    if(!worker->sync_inited){
        return;
    }
    pthread_mutex_lock(&worker->mutex);
    if(!atomic_load(&worker->running)){
        pthread_mutex_unlock(&worker->mutex);
        return;
    }
    if(!worker->in_progress){
        worker->pending = 1;
        worker->func = func;
        worker->userdata = userdata;
        pthread_cond_signal(&worker->cond);
    }
    else{
        log_warn("overlay worker can't keep up... dropping task");
        worker->skipped_frames++;
    }
    pthread_mutex_unlock(&worker->mutex);
}

int overlay_init(overlay_t* overlay,drm_warpper_t* drm_warpper,layer_animation_t* layer_animation){
    memset(&overlay->worker, 0, sizeof(overlay->worker));
    overlay->overlay_timer_handle = 0;
    overlay->overlay_worker_tick_handle = 0;
    overlay->overlay_timer_kind = OVERLAY_TIMER_KIND_NONE;
    memset(overlay->layer_animation_handles, 0, sizeof(overlay->layer_animation_handles));
    overlay->layer_animation_handle_count = 0;
    atomic_store(&overlay->request_abort, 0);
    overlay->overlay_used = 0;

    if (pthread_mutex_init(&overlay->worker.mutex, NULL) != 0) {
        log_error("Failed to initialize overlay worker mutex");
        return -1;
    }
    if (pthread_cond_init(&overlay->worker.cond, NULL) != 0) {
        log_error("Failed to initialize overlay worker condition");
        pthread_mutex_destroy(&overlay->worker.mutex);
        return -1;
    }
    if (pthread_cond_init(&overlay->worker.idle_cv, NULL) != 0) {
        log_error("Failed to initialize overlay worker idle condition");
        pthread_cond_destroy(&overlay->worker.cond);
        pthread_mutex_destroy(&overlay->worker.mutex);
        return -1;
    }
    overlay->worker.sync_inited = true;

    drm_warpper_allocate_buffer(drm_warpper, DRM_WARPPER_LAYER_OVERLAY, &overlay->overlay_buf_1);
    drm_warpper_allocate_buffer(drm_warpper, DRM_WARPPER_LAYER_OVERLAY, &overlay->overlay_buf_2);

    memset(overlay->overlay_buf_1.vaddr, 0, OVERLAY_WIDTH * OVERLAY_HEIGHT * 4);
    memset(overlay->overlay_buf_2.vaddr, 0, OVERLAY_WIDTH * OVERLAY_HEIGHT * 4);

    overlay->overlay_buf_1_item.mount.type = DRM_SRGN_ATOMIC_COMMIT_MOUNT_FB_NORMAL;
    overlay->overlay_buf_1_item.mount.arg0 = (uint32_t)overlay->overlay_buf_1.vaddr;
    overlay->overlay_buf_1_item.mount.arg1 = 0;
    overlay->overlay_buf_1_item.mount.arg2 = 0;
    overlay->overlay_buf_1_item.userdata = (void*)&overlay->overlay_buf_1;
    overlay->overlay_buf_1_item.on_heap = false;

    overlay->overlay_buf_2_item.mount.type = DRM_SRGN_ATOMIC_COMMIT_MOUNT_FB_NORMAL;
    overlay->overlay_buf_2_item.mount.arg0 = (uint32_t)overlay->overlay_buf_2.vaddr;
    overlay->overlay_buf_2_item.mount.arg1 = 0;
    overlay->overlay_buf_2_item.mount.arg2 = 0;
    overlay->overlay_buf_2_item.userdata = (void*)&overlay->overlay_buf_2;
    overlay->overlay_buf_2_item.on_heap = false;

    drm_warpper_mount_layer(drm_warpper, DRM_WARPPER_LAYER_OVERLAY, OVERLAY_WIDTH, 0, &overlay->overlay_buf_1);

    // 先把两个buffer都提交一次，形成队列的初始状态（一个显示中，一个等待取回）
    drm_warpper_enqueue_display_item(drm_warpper, DRM_WARPPER_LAYER_OVERLAY, &overlay->overlay_buf_1_item);
    drm_warpper_enqueue_display_item(drm_warpper, DRM_WARPPER_LAYER_OVERLAY, &overlay->overlay_buf_2_item);

    overlay->drm_warpper = drm_warpper;
    overlay->layer_animation = layer_animation;

    atomic_store(&overlay->worker.running, 1);
    if (pthread_create(&overlay->worker.thread, NULL, overlay_worker_thread, overlay) != 0) {
        log_error("Failed to create overlay worker thread");
        atomic_store(&overlay->worker.running, 0);
        drm_warpper_free_buffer(drm_warpper, DRM_WARPPER_LAYER_OVERLAY, &overlay->overlay_buf_1);
        drm_warpper_free_buffer(drm_warpper, DRM_WARPPER_LAYER_OVERLAY, &overlay->overlay_buf_2);
        pthread_cond_destroy(&overlay->worker.idle_cv);
        pthread_cond_destroy(&overlay->worker.cond);
        pthread_mutex_destroy(&overlay->worker.mutex);
        overlay->worker.sync_inited = false;
        return -1;
    }
    overlay->worker.thread_started = true;

    log_info("==> Overlay Initalized!");
    return 0;
}

int overlay_destroy(overlay_t* overlay){
    int has_active_overlay = 0;
    if(overlay->worker.sync_inited){
        pthread_mutex_lock(&overlay->worker.mutex);
        has_active_overlay = (overlay->overlay_used != 0 ||
                              overlay->overlay_timer_handle != 0 ||
                              overlay->overlay_worker_tick_handle != 0 ||
                              overlay->layer_animation_handle_count != 0);
        pthread_mutex_unlock(&overlay->worker.mutex);
    }

    if(has_active_overlay){
        overlay_abort(overlay);
    }
    overlay_cancel_layer_animations(overlay);
    atomic_store(&overlay->request_abort, 1);
    if(overlay->worker.sync_inited){
        pthread_mutex_lock(&overlay->worker.mutex);
        atomic_store(&overlay->worker.running, 0);
        pthread_cond_broadcast(&overlay->worker.cond);
        pthread_cond_broadcast(&overlay->worker.idle_cv);
        pthread_mutex_unlock(&overlay->worker.mutex);
    }
    if(overlay->worker.thread_started){
        pthread_join(overlay->worker.thread, NULL);
        overlay->worker.thread_started = false;
    }
    if(overlay->worker.sync_inited){
        pthread_cond_destroy(&overlay->worker.idle_cv);
        pthread_cond_destroy(&overlay->worker.cond);
        pthread_mutex_destroy(&overlay->worker.mutex);
        overlay->worker.sync_inited = false;
    }
    drm_warpper_free_buffer(overlay->drm_warpper, DRM_WARPPER_LAYER_OVERLAY, &overlay->overlay_buf_1);
    drm_warpper_free_buffer(overlay->drm_warpper, DRM_WARPPER_LAYER_OVERLAY, &overlay->overlay_buf_2);
    return 0;
}


// 这个终止函数的调用者一般是 PRTS自己的timer回调
// PRTS那边定时器周期比较长，而且这边request stop，最差情况下应该是多渲染一帧之后再退出
// 现在我们还是用轮询的方式来做的。
// 如果卡住prts太久，导致那边定时器被双重触发，就需要想个办法处理一下。
void overlay_abort(overlay_t* overlay){
    prts_timer_handle_t timer_handle = 0;
    animation_driver_handle_t hide_handle = 0;
    overlay_timer_kind_t timer_kind = OVERLAY_TIMER_KIND_NONE;
    int has_overlay_runtime = 0;
    int has_layer_animations = 0;

    pthread_mutex_lock(&overlay->worker.mutex);
    has_overlay_runtime = (overlay->overlay_used != 0 ||
                           overlay->overlay_timer_handle != 0 ||
                           overlay->overlay_worker_tick_handle != 0 ||
                           overlay->worker.in_progress ||
                           overlay->worker.pending);
    has_layer_animations = (overlay->layer_animation_handle_count != 0);
    if(!has_overlay_runtime && !has_layer_animations){
        pthread_mutex_unlock(&overlay->worker.mutex);
        log_warn("overlay is not used, skip abort");
        return;
    }
    overlay->overlay_used = 0;
    timer_handle = overlay->overlay_timer_handle;
    timer_kind = overlay->overlay_timer_kind;
    pthread_mutex_unlock(&overlay->worker.mutex);

    atomic_store(&overlay->request_abort, 1);
    overlay_cancel_layer_animations(overlay);

    if(!has_overlay_runtime){
        return;
    }

    if(timer_handle != 0 && timer_kind == OVERLAY_TIMER_KIND_CLEANUP){
        prts_timer_cancel_sync(timer_handle);
        pthread_mutex_lock(&overlay->worker.mutex);
        if(overlay->overlay_timer_handle == timer_handle){
            overlay->overlay_timer_handle = 0;
            overlay->overlay_timer_kind = OVERLAY_TIMER_KIND_NONE;
            pthread_cond_broadcast(&overlay->worker.idle_cv);
        }
        pthread_mutex_unlock(&overlay->worker.mutex);
    }

    if(layer_animation_ease_in_out_move_ex(
        overlay->layer_animation,
        DRM_WARPPER_LAYER_OVERLAY,
        0, 0,
        0, OVERLAY_HEIGHT,
        UI_LAYER_ANIMATION_DURATION, 0,
        &hide_handle
    ) == 0){
        overlay_store_layer_animation_handles(overlay, &hide_handle, 1);
    } else {
        log_error("failed to start overlay abort hide animation");
    }

    pthread_mutex_lock(&overlay->worker.mutex);
    while(overlay->overlay_timer_handle ||
          overlay->overlay_worker_tick_handle ||
          overlay->worker.in_progress ||
          overlay->worker.pending){
        pthread_cond_wait(&overlay->worker.idle_cv, &overlay->worker.mutex);
    }
    pthread_mutex_unlock(&overlay->worker.mutex);
    return;
}
