// 扩列图展示 专用
#include "ui.h"
#include "ui/actions_displayimg.h"
#include "utils/log.h"
#include "vars.h"
#include "ui/scr_transition.h"
#include <src/core/lv_group.h>
#include <dirent.h>
#include <src/lv_api_map_v8.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include "lvgl.h"

static ui_displayimg_t g_displayimg;
static ui_displayimg_t g_displayimg_pending;
static lv_timer_t *g_displayimg_refresh_timer = NULL;
static pthread_mutex_t g_displayimg_refresh_mutex = PTHREAD_MUTEX_INITIALIZER;
static atomic_int g_displayimg_refresh_running = 0;
static atomic_int g_displayimg_refresh_ready = 0;
static uint64_t g_displayimg_refresh_token = 0;

// =========================================
// 自己添加的方法 START
// =========================================

static ui_displayimg_file_type_t displayimg_file_type(const char *filename){
    if (!filename) return UI_DISPLAYIMG_FILE_TYPE_INVALID;
    const char *dot = strrchr(filename, '.');
    if (!dot || dot == filename) return UI_DISPLAYIMG_FILE_TYPE_INVALID;

    // Convert extension to lowercase
    char ext[8] = {0};
    int i = 0;
    dot++; // skip the dot
    while (dot[i] && i < 6) {
        ext[i] = (dot[i] >= 'A' && dot[i] <= 'Z') ? (dot[i] + 32) : dot[i];
        i++;
    }
    ext[i] = '\0';

    if (strcmp(ext, "jpg") == 0 || strcmp(ext, "jpeg") == 0) return UI_DISPLAYIMG_FILE_TYPE_JPG;
    if (strcmp(ext, "png") == 0) return UI_DISPLAYIMG_FILE_TYPE_PNG;
    if (strcmp(ext, "bmp") == 0) return UI_DISPLAYIMG_FILE_TYPE_BMP;
    if (strcmp(ext, "gif") == 0) return UI_DISPLAYIMG_FILE_TYPE_GIF;

    return UI_DISPLAYIMG_FILE_TYPE_INVALID;
}

static uint64_t displayimg_dir_refresh_token(void){
    struct stat st;

    if (stat(DISPLAYIMG_PATH, &st) != 0) {
        return 0;
    }

    return ((uint64_t)st.st_mtime << 32) ^
           (uint64_t)st.st_size ^
           ((uint64_t)st.st_ino << 1);
}

static void display_img(ui_displayimg_file_t *file){
    lv_obj_clean(objects.dispimg_container);
    if(file->is_gif){
        lv_obj_t *gif_obj = lv_gif_create(objects.dispimg_container);
        lv_gif_set_color_format(gif_obj, LV_COLOR_FORMAT_RGB565);
        lv_gif_set_src(gif_obj, file->img_path);
        lv_obj_center(gif_obj);
        lv_obj_move_background(gif_obj);

    }else{
        lv_obj_t *img_obj = lv_image_create(objects.dispimg_container);
        lv_image_set_src(img_obj, file->img_path);
        lv_obj_center(img_obj);
        lv_obj_move_background(img_obj);
    }
    lv_obj_move_background(objects.dispimg_container);

}

static void display_img_clear(void){
    lv_obj_clean(objects.dispimg_container);
    lv_obj_move_background(objects.dispimg_container);
}

static void ui_displayimg_scan(ui_displayimg_t *displayimg){
    DIR *dir;
    struct dirent *entry;

    if (displayimg == NULL) {
        return;
    }

    memset(displayimg, 0, sizeof(*displayimg));

    dir = opendir(DISPLAYIMG_PATH);
    if(dir == NULL){
        log_error("ui_displayimg_scan: open dir %s failed", DISPLAYIMG_PATH);
        return;
    }

    while((entry = readdir(dir)) != NULL){
        if(displayimg->count >= DISPLAYIMG_MAX_COUNT){
            break;
        }
        if(entry->d_type == DT_REG){
            if(displayimg_file_type(entry->d_name) == UI_DISPLAYIMG_FILE_TYPE_INVALID){
                continue;
            }
            snprintf(displayimg->files[displayimg->count].img_path,
                     DISPLAYIMG_MAX_PATH_LENGTH, "A:%s%s",
                     DISPLAYIMG_PATH, entry->d_name);
            displayimg->files[displayimg->count].is_gif =
                (displayimg_file_type(entry->d_name) == UI_DISPLAYIMG_FILE_TYPE_GIF);
            displayimg->count++;
        }
    }
    closedir(dir);
    log_info("ui_displayimg_scan: found %d valid displayimg files", displayimg->count);
}

static void ui_displayimg_apply_snapshot(const ui_displayimg_t *snapshot){
    char current_path[DISPLAYIMG_MAX_PATH_LENGTH];
    bool has_current_path = false;

    if (snapshot == NULL) {
        return;
    }

    memset(current_path, 0, sizeof(current_path));
    if (g_displayimg.count > 0 &&
        g_displayimg.index >= 0 &&
        g_displayimg.index < g_displayimg.count) {
        snprintf(current_path, sizeof(current_path), "%s",
                 g_displayimg.files[g_displayimg.index].img_path);
        has_current_path = true;
    }

    g_displayimg = *snapshot;
    g_displayimg.index = 0;

    if (has_current_path) {
        for (int i = 0; i < g_displayimg.count; i++) {
            if (strcmp(g_displayimg.files[i].img_path, current_path) == 0) {
                g_displayimg.index = i;
                break;
            }
        }
    }

    if (g_displayimg.count > 0) {
        if (g_displayimg.index >= g_displayimg.count) {
            g_displayimg.index = g_displayimg.count - 1;
        }
        display_img(&g_displayimg.files[g_displayimg.index]);
    } else {
        display_img_clear();
    }

    g_displayimg_refresh_token = displayimg_dir_refresh_token();
}

static void *ui_displayimg_refresh_worker(void *userdata){
    ui_displayimg_t snapshot;
    (void)userdata;

    log_info("displayimg: background scan started");
    ui_displayimg_scan(&snapshot);
    pthread_mutex_lock(&g_displayimg_refresh_mutex);
    g_displayimg_pending = snapshot;
    pthread_mutex_unlock(&g_displayimg_refresh_mutex);
    atomic_store(&g_displayimg_refresh_ready, 1);
    atomic_store(&g_displayimg_refresh_running, 0);
    log_info("displayimg: background scan finished");
    return NULL;
}

static void ui_displayimg_apply_pending_refresh(void){
    ui_displayimg_t snapshot;

    if (atomic_load(&g_displayimg_refresh_ready) == 0) {
        return;
    }

    if (ui_get_current_screen() != curr_screen_t_SCREEN_DISPLAYIMG) {
        return;
    }

    pthread_mutex_lock(&g_displayimg_refresh_mutex);
    snapshot = g_displayimg_pending;
    pthread_mutex_unlock(&g_displayimg_refresh_mutex);

    atomic_store(&g_displayimg_refresh_ready, 0);
    ui_displayimg_apply_snapshot(&snapshot);
}

static void ui_displayimg_refresh_timer_cb(lv_timer_t *timer){
    (void)timer;
    ui_displayimg_apply_pending_refresh();
}

void ui_displayimg_init(){
    ui_displayimg_scan(&g_displayimg);
    g_displayimg_refresh_token = displayimg_dir_refresh_token();
    if(g_displayimg.count != 0){
        display_img(&g_displayimg.files[0]);
    } else {
        display_img_clear();
    }
    if (g_displayimg_refresh_timer == NULL) {
        g_displayimg_refresh_timer = lv_timer_create(ui_displayimg_refresh_timer_cb, 250, NULL);
    }
    return;
}

void ui_displayimg_request_refresh(void){
    pthread_t worker;
    uint64_t token;

    if (atomic_load(&g_displayimg_refresh_ready) != 0 ||
        atomic_load(&g_displayimg_refresh_running) != 0) {
        return;
    }

    token = displayimg_dir_refresh_token();
    if (token == g_displayimg_refresh_token) {
        return;
    }

    atomic_store(&g_displayimg_refresh_running, 1);
    if (pthread_create(&worker, NULL, ui_displayimg_refresh_worker, NULL) != 0) {
        log_error("displayimg: failed to create background refresh thread");
        atomic_store(&g_displayimg_refresh_running, 0);
        ui_displayimg_scan(&g_displayimg);
        g_displayimg_refresh_token = displayimg_dir_refresh_token();
        if (g_displayimg.count != 0) {
            display_img(&g_displayimg.files[g_displayimg.index]);
        } else {
            display_img_clear();
        }
        return;
    }
    pthread_detach(worker);
}

void ui_displayimg_key_event(uint32_t key){
    if(g_displayimg.count == 0){
        return;
    }
    switch(key){
        case LV_KEY_LEFT:
            g_displayimg.index--;
            if(g_displayimg.index < 0){
                g_displayimg.index = 0;
            }
            break;
        case LV_KEY_RIGHT:
            g_displayimg.index++;
            if(g_displayimg.index >= g_displayimg.count){
                g_displayimg.index = g_displayimg.count - 1;
            }
            break;
        default:
            break;
    }
    display_img(&g_displayimg.files[g_displayimg.index]);
}

void ui_displayimg_force_dispimg(const char *path){
    ui_displayimg_file_type_t type = displayimg_file_type(path);
    if(type == UI_DISPLAYIMG_FILE_TYPE_INVALID){
        return;
    }
    static ui_displayimg_file_t file;
    snprintf(file.img_path, DISPLAYIMG_MAX_PATH_LENGTH, "A:%s", path);
    file.is_gif = (type == UI_DISPLAYIMG_FILE_TYPE_GIF);
    display_img(&file);
}

// =========================================
// EEZ 回调 START
// =========================================

const char *get_var_displayimg_size_lbl(){
    static char buf[128];
    snprintf(buf, sizeof(buf), "%d/%d", g_displayimg.index + 1, g_displayimg.count);
    return buf;
}
void set_var_displayimg_size_lbl(const char *value){
    return;
}

void action_displayimg_key(lv_event_t * e){
    log_debug("action_displayimg_key");
}

void action_show_dispimg(lv_event_t * e){
    log_debug("action_show_dispimg");
    lv_obj_t* obj = lv_event_get_target(e);
    lv_obj_remove_state(obj, LV_STATE_PRESSED);
    ui_schedule_screen_transition(curr_screen_t_SCREEN_DISPLAYIMG);
}

bool get_var_dispimg_hide_warning(){
    return g_displayimg.count != 0;
}

void set_var_dispimg_hide_warning(bool value){
    return;
}
const char *get_var_dispimg_path(){
    if(g_displayimg.count == 0){
        return "ERROR";
    }
    return g_displayimg.files[g_displayimg.index].img_path;
}

void set_var_dispimg_path(const char *value){
    return;
}
