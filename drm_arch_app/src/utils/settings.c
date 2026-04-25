#include "utils/settings.h"
#include "utils/log.h"
#include "config.h"
#include <stdlib.h>
#include <sys/wait.h>

static pthread_mutex_t g_usb_mode_worker_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_usb_mode_worker_cond = PTHREAD_COND_INITIALIZER;
static pthread_t g_usb_mode_worker_thread;
static int g_usb_mode_worker_started = 0;
static int g_usb_mode_worker_stop = 0;
static int g_usb_mode_worker_pending = 0;
static usb_mode_t g_usb_mode_pending_value = usb_mode_t_MTP;

void log_settings(settings_t *settings){
    log_info("==> Settings Log <==");
    log_info("magic: %08lx", settings->magic);
    log_info("version: %d", settings->version);
    log_info("brightness: %d", settings->brightness);
    log_info("switch_interval: %d", settings->switch_interval);
    log_info("switch_mode: %d", settings->switch_mode);
    log_info("usb_mode: %d", settings->usb_mode);
    log_info("ctrl.lowbat: %d", settings->ctrl_word.lowbat_trip);
    log_info("ctrl.no_intro: %d", settings->ctrl_word.no_intro_block);
    log_info("ctrl.no_overlay: %d", settings->ctrl_word.no_overlay_block);
}

static void set_brightness(int brightness){
    FILE *f = fopen(SETTINGS_BRIGHTNESS_PATH, "w");
    if (f) {
        fprintf(f, "%d\n", brightness);
        fclose(f);
    } else {
        log_error("Failed to set brightness");
    }
}

static const char *usb_mode_to_name(usb_mode_t usb_mode){
    switch(usb_mode){
        case usb_mode_t_MTP:
            return "mtp";
        case usb_mode_t_SERIAL:
            return "serial";
        case usb_mode_t_RNDIS:
            return "rndis";
        default:
            return "none";
    }
}

static int run_usb_mode_helper(const char *mode_name){
    char cmd[128];
    int ret;

    if (mode_name == NULL || mode_name[0] == '\0') {
        log_error("invalid usb mode helper input");
        return -1;
    }

    ret = snprintf(cmd, sizeof(cmd), "/usr/local/bin/epass-usb-mode %s", mode_name);
    if (ret < 0 || (size_t)ret >= sizeof(cmd)) {
        log_error("usb mode helper command is too long");
        return -1;
    }

    ret = system(cmd);
    if (ret == -1) {
        log_error("failed to execute usb mode helper");
        return -1;
    }

    if (WIFEXITED(ret) && WEXITSTATUS(ret) == 0) {
        return 0;
    }

    if (WIFEXITED(ret)) {
        log_error("usb mode helper exited with status %d", WEXITSTATUS(ret));
    } else if (WIFSIGNALED(ret)) {
        log_error("usb mode helper killed by signal %d", WTERMSIG(ret));
    } else {
        log_error("usb mode helper failed: 0x%x", ret);
    }

    return -1;
}

static int settings_apply_usb_mode_now(usb_mode_t usb_mode){
    const char *mode_name = usb_mode_to_name(usb_mode);
    log_info("apply usb mode: %s", mode_name);

    if (run_usb_mode_helper(mode_name) != 0) {
        log_error("apply usb mode failed: %s", mode_name);
        return -1;
    }

    log_info("apply usb mode ok: %s", mode_name);
    return 0;
}

static void *usb_mode_worker_main(void *userdata){
    (void)userdata;

    while (1) {
        usb_mode_t usb_mode;

        pthread_mutex_lock(&g_usb_mode_worker_mtx);
        while (!g_usb_mode_worker_pending && !g_usb_mode_worker_stop) {
            pthread_cond_wait(&g_usb_mode_worker_cond, &g_usb_mode_worker_mtx);
        }

        if (g_usb_mode_worker_stop && !g_usb_mode_worker_pending) {
            pthread_mutex_unlock(&g_usb_mode_worker_mtx);
            break;
        }

        usb_mode = g_usb_mode_pending_value;
        g_usb_mode_worker_pending = 0;
        pthread_mutex_unlock(&g_usb_mode_worker_mtx);

        settings_apply_usb_mode_now(usb_mode);
    }

    return NULL;
}

static int settings_start_usb_mode_worker(void){
    if (g_usb_mode_worker_started) {
        return 0;
    }

    g_usb_mode_worker_stop = 0;
    g_usb_mode_worker_pending = 0;
    if (pthread_create(&g_usb_mode_worker_thread, NULL, usb_mode_worker_main, NULL) != 0) {
        log_error("failed to start usb mode worker");
        return -1;
    }

    g_usb_mode_worker_started = 1;
    return 0;
}

static void settings_stop_usb_mode_worker(void){
    if (!g_usb_mode_worker_started) {
        return;
    }

    pthread_mutex_lock(&g_usb_mode_worker_mtx);
    g_usb_mode_worker_stop = 1;
    pthread_cond_signal(&g_usb_mode_worker_cond);
    pthread_mutex_unlock(&g_usb_mode_worker_mtx);

    pthread_join(g_usb_mode_worker_thread, NULL);
    g_usb_mode_worker_started = 0;
}

void settings_set_usb_mode(usb_mode_t usb_mode){
    if (!g_usb_mode_worker_started) {
        log_warn("usb mode worker not available, applying mode inline");
        settings_apply_usb_mode_now(usb_mode);
        return;
    }

    log_info("queue usb mode change: %s", usb_mode_to_name(usb_mode));
    pthread_mutex_lock(&g_usb_mode_worker_mtx);
    g_usb_mode_pending_value = usb_mode;
    g_usb_mode_worker_pending = 1;
    pthread_cond_signal(&g_usb_mode_worker_cond);
    pthread_mutex_unlock(&g_usb_mode_worker_mtx);
}

static void settings_save(settings_t *settings){
    FILE *f = fopen(SETTINGS_FILE_PATH, "wb");
    if (!f) {
        log_error("Failed to open settings file for writing");
        return;
    }
    settings->magic = SETTINGS_MAGIC;
    settings->version = SETTINGS_VERSION;
    if (fwrite(settings, SETTINGS_LENGTH, 1, f) != 1) {
        log_error("Failed to write settings");
    }
    fclose(f);

    log_info("setting saved!");
    // log_settings(settings);
}

void settings_init(settings_t *settings){
    FILE *f = fopen(SETTINGS_FILE_PATH, "rb");
    if(f == NULL){
        log_error("failed to open settings file");
    }
    else{
        fread(settings, SETTINGS_LENGTH, 1, f);
        if(settings->magic != SETTINGS_MAGIC){
            log_error("invalid settings file");
        }
        else if(settings->version != SETTINGS_VERSION){
            log_error("invalid settings file version");
        }
        else{
            fclose(f);
            set_brightness(settings->brightness);
            log_info("settings loaded, usb_mode=%d", settings->usb_mode);
            // log_settings(settings);
            pthread_mutex_init(&settings->mtx, NULL);
            if (settings_start_usb_mode_worker() != 0) {
                log_warn("usb mode changes will run inline");
            }
            return;
        }
        fclose(f);
    }

    log_info("creating new settings file");
    settings->magic = SETTINGS_MAGIC;
    settings->version = SETTINGS_VERSION;
    settings->brightness = 5;
    settings->switch_interval = sw_interval_t_SW_INTERVAL_5MIN;
    settings->switch_mode = sw_mode_t_SW_MODE_SEQUENCE;
    settings->usb_mode = usb_mode_t_MTP;
    settings->ctrl_word.lowbat_trip = 1;
    settings->ctrl_word.no_intro_block = 0;
    settings->ctrl_word.no_overlay_block = 0;
    pthread_mutex_init(&settings->mtx, NULL);
    if (settings_start_usb_mode_worker() != 0) {
        log_warn("usb mode changes will run inline");
    }
    settings_save(settings);
    return;
    
}

void settings_destroy(settings_t *settings){
    settings_stop_usb_mode_worker();
    pthread_mutex_destroy(&settings->mtx);
}

void settings_lock(settings_t *settings){
    pthread_mutex_lock(&settings->mtx);
}
void settings_unlock(settings_t *settings){
    pthread_mutex_unlock(&settings->mtx);
}

void settings_update(settings_t *settings){
    settings_save(settings);
    set_brightness(settings->brightness);
}
