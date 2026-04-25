#include <apps/apps.h>
#include <stdio.h>
#include <string.h>
#include <ui/actions_warning.h>
#include <unistd.h>
#include <stdint.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdlib.h>

#include "config.h"
#include "driver/drm_warpper.h"
#include "utils/log.h"
#include "render/mediaplayer.h"
#include "render/animation_driver.h"
#include "render/lvgl_drm_warp.h"
#include "overlay/overlay.h"
#include "utils/timer.h"
#include "render/layer_animation.h"
#include "utils/settings.h"
#include "utils/timer.h"
#include "utils/cacheassets.h"
#include "utils/theme.h"
#include "prts/prts.h"
#include "utils/misc.h"

/* global variables */
drm_warpper_t g_drm_warpper;
mediaplayer_t g_mediaplayer;
lvgl_drm_warp_t g_lvgl_drm_warp;
prts_timer_t g_prts_timer;
animation_driver_t g_animation_driver;
layer_animation_t g_layer_animation;
settings_t g_settings;
overlay_t g_overlay;
cacheassets_t g_cacheassets;
prts_t g_prts;
apps_t g_apps;

buffer_object_t g_video_buf;

int g_running = 1;
int g_exitcode = 0;
bool g_use_sd = false;
static volatile sig_atomic_t g_shutdown_signal = 0;

#define APP_LOG_DIR "/run/epass"
#define APP_LOG_PATH APP_LOG_DIR "/drm_arch_app.log"

static FILE *g_app_log_fp = NULL;

static void cache_default_asset(cacheassets_t *cacheassets, cacheasset_asset_id_t asset_id, const char *default_path)
{
    char resolved_path[256];

    app_theme_resolve_path(default_path, resolved_path, sizeof(resolved_path));
    cacheassets_put_asset(cacheassets, asset_id, resolved_path);
}

static void init_app_log(void)
{
    if (mkdir(APP_LOG_DIR, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "WARN: failed to create %s: %s\n", APP_LOG_DIR, strerror(errno));
        return;
    }

    g_app_log_fp = fopen(APP_LOG_PATH, "w");
    if (g_app_log_fp == NULL) {
        fprintf(stderr, "WARN: failed to open %s: %s\n", APP_LOG_PATH, strerror(errno));
        return;
    }

    if (log_add_fp(g_app_log_fp, LOG_INFO) != 0) {
        fprintf(stderr, "WARN: failed to attach persistent log %s\n", APP_LOG_PATH);
        fclose(g_app_log_fp);
        g_app_log_fp = NULL;
    }
}

void signal_handler(int sig)
{
    g_shutdown_signal = sig;
    g_running = 0;
    g_exitcode = 0;
}

void mount_video_layer_callback(void *userdata,bool is_last){
    drm_warpper_mount_layer(&g_drm_warpper, DRM_WARPPER_LAYER_VIDEO, 0, 0, &g_video_buf);
    drm_warpper_set_layer_coord(&g_drm_warpper, DRM_WARPPER_LAYER_OVERLAY, 0,0);
}


// ============ 组件依赖关系： ============
// AnimationDriver 负责高频动画调度
// LayerAnimation 依赖 AnimationDriver 与 drm warpper
// mediaplayer 依赖 drm warpper
// cacheassets 依赖 mediaplayer 初始化用的那个buffer
// overlay 依赖 drm warpper 与 layer animation
// prts 依赖 overlay
// apps 依赖 prts
// lvgl_drm_warp 依赖 drm_warpper,prts,apps

// ============ 组件初始化顺序： ============
// 1. drm warpper
// 2. prts timer
// 3. animation driver
// 4. settings
// 5. layer animation
// 6. mediaplayer
// 7. cacheassets
// 8. overlay
// 9. prts
// 10. apps
// 11. lvgl_drm_warp

int main(int argc, char *argv[]){
    uint8_t* cache_buf = NULL;
    const char *exit_stage = "main-loop-exit";
    bool drm_inited = false;
    bool timer_inited = false;
    bool animation_driver_inited = false;
    bool settings_inited = false;
    bool mediaplayer_inited = false;
    bool overlay_inited = false;
    bool prts_inited = false;
    bool apps_inited = false;
    bool lvgl_inited = false;

#define FAIL_WITH_STAGE(stage_name) \
    do { \
        exit_stage = stage_name; \
        g_exitcode = 1; \
        goto cleanup; \
    } while (0)

    if(argc == 2){
        if(strcmp(argv[1], "version") == 0){
            printf("APP_VERSION: %s\n", APP_VERSION_STRING);
            printf("COMPILE_TIME: %s\n", COMPILE_TIME);
            return 0;
        }
        else if(strcmp(argv[1], "sd") == 0){
            g_use_sd = true;
        }
    }

#ifdef APP_RELEASE
    log_warn("Release mode is enabled. Most logs are disabled.");
#endif // APP_RELEASE

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    init_app_log();

    fprintf(stderr,"APP_VERSION: %s\n", APP_VERSION_STRING);
    fprintf(stderr,"COMPILE_TIME: %s\n", COMPILE_TIME);

    fputs(APP_BARNER, stderr);
    log_info("==> Starting EPass DRM APP!");
    if (g_app_log_fp != NULL) {
        log_info("persistent log path: %s", APP_LOG_PATH);
    }

    // ============ DRM Warpper 初始化 ===============
    if (drm_warpper_init(&g_drm_warpper) != 0) {
        FAIL_WITH_STAGE("drm-init");
    }
    drm_inited = true;

    // ============ PRTS 计时器 初始化 ===============
    if (prts_timer_init(&g_prts_timer) != 0) {
        log_error("failed to initialize PRTS timer");
        FAIL_WITH_STAGE("prts-timer-init");
    }
    timer_inited = true;

    if (animation_driver_init(&g_animation_driver, LAYER_ANIMATION_STEP_TIME) != 0) {
        log_error("failed to initialize animation driver");
        FAIL_WITH_STAGE("animation-driver-init");
    }
    animation_driver_inited = true;

    // ============ 设置 初始化 ===============
    settings_init(&g_settings);
    settings_inited = true;
    app_theme_init();

    // ============ 图层动画 初始化 ===============
    layer_animation_init(&g_layer_animation, &g_drm_warpper, &g_animation_driver);

    // ============ Mediaplayer 初始化 ===============
    if (drm_warpper_init_layer(
        &g_drm_warpper, 
        DRM_WARPPER_LAYER_VIDEO, 
        VIDEO_WIDTH, 
        VIDEO_HEIGHT, 
        DRM_WARPPER_LAYER_MODE_MB32_NV12
    ) != 0) {
        log_error("failed to initialize video layer");
        FAIL_WITH_STAGE("video-layer-init");
    }

    // FIXME：
    // 用来跑modeset的buffer，实际上是不用的，这一片内存你也可以拿去干别的
    // 期待有能人帮优化掉这个allocate。
    // 20260110: 这个内存如果用做别的话，modeset的话会显示成很难看的绿色。先闲置把
    if (drm_warpper_allocate_buffer(&g_drm_warpper, DRM_WARPPER_LAYER_VIDEO, &g_video_buf) != 0) {
        log_error("failed to allocate video buffer");
        FAIL_WITH_STAGE("video-buffer-alloc");
    }
    // drm_warpper_mount_layer(&g_drm_warpper, DRM_WARPPER_LAYER_VIDEO, 0, 0, &video_buf);

    /* initialize mediaplayer */
    if (mediaplayer_init(&g_mediaplayer, &g_drm_warpper) != 0) {
        log_error("failed to initialize mediaplayer");
        FAIL_WITH_STAGE("mediaplayer-init");
    }
    mediaplayer_inited = true;

    // 填充video buffer，防止闪烁。
    fill_nv12_buffer_with_color(
        g_video_buf.vaddr, 
        VIDEO_WIDTH, 
        VIDEO_HEIGHT, 
        0xff000000
    );

    // mediaplayer_set_video(&g_mediaplayer, "/assets/MS/loop.mp4");
    // mediaplayer_start(&g_mediaplayer);

    // ============ 缓冲素材 初始化 ===============
    // 原计划是用video层的buffer来缓存素材，但这样会导致第一次切换的时候闪烁，挺难看的...
    // 单独开了一个buffer
    cache_buf = malloc(CACHED_ASSETS_MAX_SIZE);
    if(!cache_buf){
        log_error("failed to allocate cache buffer");
        FAIL_WITH_STAGE("cache-buffer-alloc");
    }
    cacheassets_init(&g_cacheassets, cache_buf, CACHED_ASSETS_MAX_SIZE);
    cache_default_asset(&g_cacheassets, CACHE_ASSETS_AK_BAR, CACHED_ASSETS_ASSET_PATH_AK_BAR);
    cache_default_asset(&g_cacheassets, CACHE_ASSETS_BTM_LEFT_BAR, CACHED_ASSETS_ASSET_PATH_BTM_LEFT_BAR);
    cache_default_asset(&g_cacheassets, CACHE_ASSETS_TOP_LEFT_RECT, CACHED_ASSETS_ASSET_PATH_TOP_LEFT_RECT);
    cache_default_asset(&g_cacheassets, CACHE_ASSETS_TOP_LEFT_RHODES, CACHED_ASSETS_ASSET_PATH_TOP_LEFT_RHODES);
    cache_default_asset(&g_cacheassets, CACHE_ASSETS_TOP_RIGHT_BAR, CACHED_ASSETS_ASSET_PATH_TOP_RIGHT_BAR);
    cache_default_asset(&g_cacheassets, CACHE_ASSETS_TOP_RIGHT_ARROW, CACHED_ASSETS_ASSET_PATH_TOP_RIGHT_ARROW);

    log_info("Cached assets: %d", g_cacheassets.curr_size);

    // ============ OVERLAY 初始化 ===============
    if (drm_warpper_init_layer(
        &g_drm_warpper, 
        DRM_WARPPER_LAYER_OVERLAY, 
        UI_WIDTH, UI_HEIGHT, 
        DRM_WARPPER_LAYER_MODE_ARGB8888
    ) != 0) {
        log_error("failed to initialize overlay layer");
        FAIL_WITH_STAGE("overlay-layer-init");
    }

    if (overlay_init(&g_overlay, &g_drm_warpper, &g_layer_animation) != 0) {
        log_error("failed to initialize overlay");
        FAIL_WITH_STAGE("overlay-init");
    }
    overlay_inited = true;

    // ============ PRTS 初始化===============
    prts_init(&g_prts, &g_overlay, g_use_sd);
    prts_inited = true;

    // ============ APPS 初始化 ===============
    apps_init(&g_apps, &g_prts, g_use_sd);
    apps_inited = true;

    // ============ LVGL 初始化 ===============
    if (drm_warpper_init_layer(
        &g_drm_warpper, 
        DRM_WARPPER_LAYER_UI, 
        UI_WIDTH, UI_HEIGHT, 
        DRM_WARPPER_LAYER_MODE_RGB565
    ) != 0) {
        log_error("failed to initialize UI layer");
        FAIL_WITH_STAGE("ui-layer-init");
    }

    if (lvgl_drm_warp_init(&g_lvgl_drm_warp, &g_drm_warpper,&g_layer_animation,&g_prts,&g_apps) != 0) {
        log_error("failed to initialize LVGL DRM warp");
        FAIL_WITH_STAGE("lvgl-drm-init");
    }
    lvgl_inited = true;
    // drm_warpper_set_layer_coord(&g_drm_warpper, DRM_WARPPER_LAYER_UI, 0, SCREEN_HEIGHT);
    // drm_warpper_set_layer_coord(&g_drm_warpper, DRM_WARPPER_LAYER_OVERLAY, OVERLAY_WIDTH, 0);

    // 如果SD卡插入，但没有启用SD模式，则应该是mount出错了，这边告警
    if(is_sdcard_inserted() && !g_use_sd){
        ui_warning(UI_WARNING_SD_MOUNT_ERROR);
    }


    // ============ 主循环 ===============
    while(g_running){
        int status; 
        pid_t pid;
        // 处理子进程（后台app）退出
        while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
            if (WIFEXITED(status)) {
                log_info("child process %d exited with status %d", pid, WEXITSTATUS(status));
            }
        }
        usleep(1 * 1000 * 1000);
    }

cleanup:
    /* cleanup */
    if (g_shutdown_signal != 0) {
        log_info("received signal %d, shutting down", g_shutdown_signal);
    }
    log_info("app-exit code=%d stage=%s", g_exitcode, exit_stage);
    log_info("==> Shutting down EPass DRM APP!");

    if (lvgl_inited) {
        lvgl_drm_warp_stop(&g_lvgl_drm_warp);
    }

    if (prts_inited) {
        prts_stop(&g_prts);
    }

    if (apps_inited) {
        apps_destroy(&g_apps);
        apps_inited = false;
    }

    if (overlay_inited) {
        overlay_abort(&g_overlay);
    }

    if (mediaplayer_inited) {
        mediaplayer_stop(&g_mediaplayer);
    }

    if (prts_inited) {
        prts_destroy(&g_prts);
        prts_inited = false;
    }

    if (timer_inited) {
        prts_timer_destroy(&g_prts_timer);
        timer_inited = false;
    }

    if (drm_inited) {
        drm_warpper_stop_display_thread(&g_drm_warpper);
    }

    if (lvgl_inited) {
        lvgl_drm_warp_destroy(&g_lvgl_drm_warp);
        lvgl_inited = false;
    }

    if (overlay_inited) {
        overlay_destroy(&g_overlay);
        overlay_inited = false;
    }

    if (animation_driver_inited) {
        animation_driver_destroy(&g_animation_driver);
        animation_driver_inited = false;
    }

    if (mediaplayer_inited) {
        mediaplayer_destroy(&g_mediaplayer);
        mediaplayer_inited = false;
    }

    if (drm_inited) {
        drm_warpper_free_buffer(&g_drm_warpper, DRM_WARPPER_LAYER_VIDEO, &g_video_buf);
        drm_warpper_destroy(&g_drm_warpper);
        drm_inited = false;
    }

    if (settings_inited) {
        settings_destroy(&g_settings);
        settings_inited = false;
    }

    free(cache_buf);
#undef FAIL_WITH_STAGE
    return g_exitcode;
}
