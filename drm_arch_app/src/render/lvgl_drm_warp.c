#include <apps/apps_types.h>
#include <stdbool.h>
#include <stdio.h>
#include <time.h>
#include <ui/actions_apps.h>
#include <ui/actions_displayimg.h>
#include <ui/ipc_helper.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>

#include "render/lvgl_drm_warp.h"
#include "config.h"
#include "render/layer_animation.h"
#include "utils/log.h"
#include "ui.h"
#include "vars.h"
#include "driver/key_enc_evdev.h"
#include "ui/filemanager.h"
#include "ui/actions_warning.h"
#include "ui/actions_oplist.h"
#include "prts/prts.h"
#include "ui/battery.h"
#include "ui/actions_displayimg.h"
#include "ui/actions_confirm.h"
#include "ui/shell_net.h"
#include "utils/theme.h"

#define LVGL_DRM_WARP_MAX_IDLE_MS 16

static void lvgl_drm_warp_release_resources(lvgl_drm_warp_t *lvgl_drm_warp)
{
    if(lvgl_drm_warp->ui_helpers_inited){
        ui_warning_destroy();
        ui_battery_destroy();
        ui_confirm_destroy();
        ui_ipc_helper_destroy();
        ui_apps_destroy();
        ui_shell_net_shutdown();
        lvgl_drm_warp->ui_helpers_inited = false;
    }

    if(lvgl_drm_warp->key_inited){
        key_enc_evdev_destroy(&lvgl_drm_warp->key_enc_evdev);
        lvgl_drm_warp->key_inited = false;
        lvgl_drm_warp->keypad_indev = NULL;
    }

    if(lvgl_drm_warp->disp){
        lv_display_delete(lvgl_drm_warp->disp);
        lvgl_drm_warp->disp = NULL;
    }

    if(lvgl_drm_warp->lv_inited){
        lv_deinit();
        lvgl_drm_warp->lv_inited = false;
    }

    drm_warpper_free_buffer(lvgl_drm_warp->drm_warpper, DRM_WARPPER_LAYER_UI, &lvgl_drm_warp->ui_buf_1);
    drm_warpper_free_buffer(lvgl_drm_warp->drm_warpper, DRM_WARPPER_LAYER_UI, &lvgl_drm_warp->ui_buf_2);
}

static uint32_t lvgl_drm_warp_tick_get_cb(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    uint64_t time_ms = t.tv_sec * 1000 + (t.tv_nsec / 1000000);
    return time_ms;
}

static void lvgl_drm_warp_flush_cb(lv_display_t * disp, const lv_area_t * area, uint8_t * px_map)
{
    (void)area;
    (void)px_map;

    if(!lv_disp_flush_is_last(disp)){
        lv_display_flush_ready(disp);
        return;
    }
    lvgl_drm_warp_t *lvgl_drm_warp = (lvgl_drm_warp_t *)lv_display_get_driver_data(disp);

    if(!lvgl_drm_warp->first_flush_logged){
        lvgl_drm_warp->first_flush_logged = true;
        drm_warpper_request_gui_ready(lvgl_drm_warp->drm_warpper);
        log_info("ui:first-flush-enter");
    }

    // log_info("enqueue display item");

    if(lvgl_drm_warp->curr_draw_buf_idx == 0){
        if(drm_warpper_enqueue_display_item(lvgl_drm_warp->drm_warpper, DRM_WARPPER_LAYER_UI, &lvgl_drm_warp->ui_buf_1_item) != 0){
            log_error("failed to enqueue first UI buffer");
            lv_display_flush_ready(disp);
            return;
        }
    }else{
        if(drm_warpper_enqueue_display_item(lvgl_drm_warp->drm_warpper, DRM_WARPPER_LAYER_UI, &lvgl_drm_warp->ui_buf_2_item) != 0){
            log_error("failed to enqueue second UI buffer");
            lv_display_flush_ready(disp);
            return;
        }
    }
    lvgl_drm_warp->curr_draw_buf_idx = !lvgl_drm_warp->curr_draw_buf_idx;
}

static void lvgl_drm_warp_flush_wait_cb(lv_display_t * disp)
{
    lvgl_drm_warp_t *lvgl_drm_warp = (lvgl_drm_warp_t *)lv_display_get_driver_data(disp);
    drm_warpper_queue_item_t* item;

    drm_warpper_dequeue_free_item(lvgl_drm_warp->drm_warpper, DRM_WARPPER_LAYER_UI, &item);

    if(!lvgl_drm_warp->first_flush_ready_logged){
        lvgl_drm_warp->first_flush_ready_logged = true;
        log_info("ui:first-flush-wait-done");
    }
}

static void* lvgl_drm_warp_thread_entry(void *arg){
    lvgl_drm_warp_t *lvgl_drm_warp = (lvgl_drm_warp_t *)arg;
    log_info("==> LVGL Thread Started!");
    loadScreen(SCREEN_ID_SPINNER);
    while(atomic_load(&lvgl_drm_warp->running)){
        uint32_t idle_time = lv_timer_handler();
        ui_tick();
        if (idle_time > LVGL_DRM_WARP_MAX_IDLE_MS) {
            idle_time = LVGL_DRM_WARP_MAX_IDLE_MS;
        }
        usleep(idle_time * 1000);
    }
    log_info("==> LVGL Thread Ended!");
    return NULL; 
}


extern void screen_key_event_cb(uint32_t key);
extern bool screen_raw_key_event_cb(const key_enc_evdev_event_t *event);
int lvgl_drm_warp_init(lvgl_drm_warp_t *lvgl_drm_warp,drm_warpper_t *drm_warpper,layer_animation_t *layer_animation,prts_t *prts,apps_t *apps){

    lvgl_drm_warp->drm_warpper = drm_warpper;
    lvgl_drm_warp->layer_animation = layer_animation;
    lvgl_drm_warp->thread_started = false;
    lvgl_drm_warp->lv_inited = false;
    lvgl_drm_warp->key_inited = false;
    lvgl_drm_warp->ui_helpers_inited = false;
    lvgl_drm_warp->keypad_indev = NULL;
    lvgl_drm_warp->first_flush_logged = false;
    lvgl_drm_warp->first_flush_ready_logged = false;
    lvgl_drm_warp->key_enc_evdev.evdev_fd = -1;
    if(drm_warpper_allocate_buffer(drm_warpper, DRM_WARPPER_LAYER_UI, &lvgl_drm_warp->ui_buf_1) != 0){
        log_error("failed to allocate first UI buffer");
        return -1;
    }
    if(drm_warpper_allocate_buffer(drm_warpper, DRM_WARPPER_LAYER_UI, &lvgl_drm_warp->ui_buf_2) != 0){
        log_error("failed to allocate second UI buffer");
        drm_warpper_free_buffer(drm_warpper, DRM_WARPPER_LAYER_UI, &lvgl_drm_warp->ui_buf_1);
        return -1;
    }

    // modeset
    if(drm_warpper_mount_layer(drm_warpper, DRM_WARPPER_LAYER_UI, 0, SCREEN_HEIGHT, &lvgl_drm_warp->ui_buf_1) != 0){
        log_error("failed to mount initial UI layer");
        drm_warpper_free_buffer(drm_warpper, DRM_WARPPER_LAYER_UI, &lvgl_drm_warp->ui_buf_2);
        drm_warpper_free_buffer(drm_warpper, DRM_WARPPER_LAYER_UI, &lvgl_drm_warp->ui_buf_1);
        return -1;
    }

    lvgl_drm_warp->ui_buf_1_item.mount.type = DRM_SRGN_ATOMIC_COMMIT_MOUNT_FB_NORMAL;
    lvgl_drm_warp->ui_buf_1_item.mount.arg0 = (uint32_t)lvgl_drm_warp->ui_buf_1.vaddr;
    lvgl_drm_warp->ui_buf_1_item.mount.arg1 = 0;
    lvgl_drm_warp->ui_buf_1_item.mount.arg2 = 0;
    lvgl_drm_warp->ui_buf_1_item.userdata = (void*)&lvgl_drm_warp->ui_buf_1;
    lvgl_drm_warp->ui_buf_1_item.on_heap = false;

    lvgl_drm_warp->ui_buf_2_item.mount.type = DRM_SRGN_ATOMIC_COMMIT_MOUNT_FB_NORMAL;
    lvgl_drm_warp->ui_buf_2_item.mount.arg0 = (uint32_t)lvgl_drm_warp->ui_buf_2.vaddr;
    lvgl_drm_warp->ui_buf_2_item.mount.arg1 = 0;
    lvgl_drm_warp->ui_buf_2_item.mount.arg2 = 0;
    lvgl_drm_warp->ui_buf_2_item.userdata = (void*)&lvgl_drm_warp->ui_buf_2;
    lvgl_drm_warp->ui_buf_2_item.on_heap = false;
    
    lvgl_drm_warp->has_vsync_done = true;

    // 先把buffer提交进去，形成队列的初始状态（有一个buffer等待被free回来）
    // drm_warpper_enqueue_display_item(drm_warpper, DRM_WARPPER_LAYER_UI, &lvgl_drm_warp->ui_buf_1_item);
    if(drm_warpper_enqueue_display_item(drm_warpper, DRM_WARPPER_LAYER_UI, &lvgl_drm_warp->ui_buf_2_item) != 0){
        log_error("failed to enqueue initial UI buffer");
        drm_warpper_free_buffer(drm_warpper, DRM_WARPPER_LAYER_UI, &lvgl_drm_warp->ui_buf_2);
        drm_warpper_free_buffer(drm_warpper, DRM_WARPPER_LAYER_UI, &lvgl_drm_warp->ui_buf_1);
        return -1;
    }
    

    lv_init();
    lvgl_drm_warp->lv_inited = true;
    lv_tick_set_cb(lvgl_drm_warp_tick_get_cb);
    lvgl_drm_warp->curr_draw_buf_idx = 0;

    lv_display_t * disp;
    disp = lv_display_create(UI_WIDTH, UI_HEIGHT);
    if(disp == NULL){
        log_error("failed to create LVGL display");
        drm_warpper_free_buffer(drm_warpper, DRM_WARPPER_LAYER_UI, &lvgl_drm_warp->ui_buf_2);
        drm_warpper_free_buffer(drm_warpper, DRM_WARPPER_LAYER_UI, &lvgl_drm_warp->ui_buf_1);
        return -1;
    }
    lv_display_set_buffers(disp, 
        lvgl_drm_warp->ui_buf_1.vaddr,
        lvgl_drm_warp->ui_buf_2.vaddr, 
        UI_WIDTH * UI_HEIGHT * 2,
        LV_DISPLAY_RENDER_MODE_DIRECT);
    
    lvgl_drm_warp->disp = disp;
    lv_display_set_driver_data(disp, lvgl_drm_warp);
    lv_display_set_flush_cb(disp, lvgl_drm_warp_flush_cb);
    lv_display_set_flush_wait_cb(disp, lvgl_drm_warp_flush_wait_cb);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);

    lvgl_drm_warp->key_enc_evdev.input_cb = screen_key_event_cb;
    lvgl_drm_warp->key_enc_evdev.raw_input_cb = screen_raw_key_event_cb;
    snprintf(lvgl_drm_warp->key_enc_evdev.dev_path, sizeof(lvgl_drm_warp->key_enc_evdev.dev_path), "/dev/input/event%d", 0);
    key_enc_evdev_init(&lvgl_drm_warp->key_enc_evdev);
    lvgl_drm_warp->keypad_indev = lvgl_drm_warp->key_enc_evdev.indev;
    if(lvgl_drm_warp->keypad_indev == NULL){
        log_error("failed to initialize keypad input");
        lvgl_drm_warp_release_resources(lvgl_drm_warp);
        return -1;
    }
    lvgl_drm_warp->key_inited = true;

    ui_create_groups();
    lv_indev_set_group(lvgl_drm_warp->keypad_indev, groups.op);


    // gui_app_create_ui(lvgl_drm_warp);
    ui_init();
    create_filemanager(apps);
    ui_shell_net_init();
    app_theme_refresh_ui();

    // UI 连锁 组件
    ui_warning_init();
    ui_oplist_init(prts);
    ui_battery_init();
    ui_displayimg_init();
    ui_confirm_init();
    ui_ipc_helper_init();
    ui_apps_init(apps);
    lvgl_drm_warp->ui_helpers_inited = true;

    atomic_store(&lvgl_drm_warp->running, 1);
    if (pthread_create(&lvgl_drm_warp->lvgl_thread, NULL, lvgl_drm_warp_thread_entry, lvgl_drm_warp) != 0) {
        log_error("Failed to create LVGL thread");
        atomic_store(&lvgl_drm_warp->running, 0);
        lvgl_drm_warp_release_resources(lvgl_drm_warp);
        return -1;
    }
    lvgl_drm_warp->thread_started = true;
    log_info("==> LVGL warpper Initalized!");
    return 0;
}

void lvgl_drm_warp_stop(lvgl_drm_warp_t *lvgl_drm_warp){
    atomic_store(&lvgl_drm_warp->running, 0);
    if(lvgl_drm_warp->thread_started){
        pthread_join(lvgl_drm_warp->lvgl_thread, NULL);
        lvgl_drm_warp->thread_started = false;
    }
}

void lvgl_drm_warp_destroy(lvgl_drm_warp_t *lvgl_drm_warp){
    lvgl_drm_warp_stop(lvgl_drm_warp);
    lvgl_drm_warp_release_resources(lvgl_drm_warp);
}
