// UI 屏幕过渡 相关处理方法
#include "ui/scr_transition.h"
#include "config.h"
#include "render/layer_animation.h"
#include "render/lvgl_drm_warp.h"
#include "utils/timer.h"
#include "utils/log.h"
#include "vars.h"
#include "screens.h"
#include "ui.h"
#include "ui/actions_settings.h"
#include "ui/filemanager.h"
#include "ui/actions_oplist.h"
#include "ui/actions_apps.h"
#include "ui/scr_transition.h"
#include <ui/actions_displayimg.h>
#include "ui/actions_confirm.h"
#include "ui/shell_net.h"
#include "utils/theme.h"
#include "lvgl.h"
#include <stdint.h>
#include <string.h>

extern int g_running;
extern int g_exitcode;

static curr_screen_t g_cur_scr = curr_screen_t_SCREEN_SPINNER;
static bool g_ui_hidden = true;
static lv_timer_t *g_delayed_screen_timer = NULL;
#define UI_SCREEN_HISTORY_MAX 8
static curr_screen_t g_screen_history[UI_SCREEN_HISTORY_MAX];
static uint32_t g_screen_history_size = 0;

// =========================================
// 自己添加的方法 START
// =========================================

inline static int get_screen_target_y(curr_screen_t screen){
    switch(screen){
        case curr_screen_t_SCREEN_OPLIST:
            return UI_OPLIST_Y;
        case curr_screen_t_SCREEN_MAINMENU:
            return UI_MAINMENU_Y;
        case curr_screen_t_SCREEN_WARNING:
            return UI_WARNING_Y;
        case curr_screen_t_SCREEN_SPINNER:
            return UI_HEIGHT;
        case curr_screen_t_SCREEN_DISPLAYIMG:
            return 0;
        case curr_screen_t_SCREEN_SETTINGS:
            return 0;
        case curr_screen_t_SCREEN_SYSINFO:
            return 0;
        case curr_screen_t_SCREEN_SYSINFO2:
            return 0;
        case curr_screen_t_SCREEN_FILEMANAGER:
            return 0;
        case curr_screen_t_SCREEN_CONFIRM:
            return UI_CONFIRM_Y;
        case curr_screen_t_SCREEN_APPLIST:
            return 0;
        default:
            return 0;
    }
}

inline static void load_screen(curr_screen_t screen){
    switch(screen){
        case curr_screen_t_SCREEN_OPLIST:
            loadScreen(SCREEN_ID_OPLIST);
            break;
        case curr_screen_t_SCREEN_MAINMENU:
            loadScreen(SCREEN_ID_MAINMENU);
            break;
        case curr_screen_t_SCREEN_WARNING:
            loadScreen(SCREEN_ID_WARNING);
            break;
        case curr_screen_t_SCREEN_SPINNER:
            loadScreen(SCREEN_ID_SPINNER);
            break;
        case curr_screen_t_SCREEN_DISPLAYIMG:
            loadScreen(SCREEN_ID_DISPLAYIMG);
            break;
        case curr_screen_t_SCREEN_SETTINGS:
            loadScreen(SCREEN_ID_SETTINGS);
            break;
        case curr_screen_t_SCREEN_SYSINFO:
            loadScreen(SCREEN_ID_SYSINFO);
            break;
        case curr_screen_t_SCREEN_SYSINFO2:
            loadScreen(SCREEN_ID_SYSINFO2);
            break;
        case curr_screen_t_SCREEN_FILEMANAGER:
            loadScreen(SCREEN_ID_FILEMANAGER);
            break;
        case curr_screen_t_SCREEN_CONFIRM:
            loadScreen(SCREEN_ID_CONFIRM);
            break;
        case curr_screen_t_SCREEN_APPLIST:
            loadScreen(SCREEN_ID_APPLIST);
            break;
        case curr_screen_t_SCREEN_SHELL:
            loadScreen(SCREEN_ID_SHELL);
            break;
        case curr_screen_t_SCREEN_NET:
            loadScreen(SCREEN_ID_NET);
            break;
    }
}

static void ui_history_push(curr_screen_t screen){
    if (g_screen_history_size > 0 &&
        g_screen_history[g_screen_history_size - 1] == screen) {
        return;
    }

    if (g_screen_history_size >= UI_SCREEN_HISTORY_MAX) {
        memmove(&g_screen_history[0],
                &g_screen_history[1],
                sizeof(g_screen_history[0]) * (UI_SCREEN_HISTORY_MAX - 1));
        g_screen_history_size = UI_SCREEN_HISTORY_MAX - 1;
    }

    g_screen_history[g_screen_history_size++] = screen;
}

void ui_clear_screen_history(void){
    g_screen_history_size = 0;
}

void ui_push_screen_transition(curr_screen_t to_screen){
    if (g_cur_scr != to_screen) {
        ui_history_push(g_cur_scr);
    }
    ui_schedule_screen_transition(to_screen);
}

void ui_pop_screen_transition(curr_screen_t fallback_screen){
    curr_screen_t to_screen = fallback_screen;

    while (g_screen_history_size > 0) {
        curr_screen_t candidate = g_screen_history[--g_screen_history_size];
        if (candidate != g_cur_scr) {
            to_screen = candidate;
            break;
        }
    }

    ui_schedule_screen_transition(to_screen);
}


static void delayed_load_screen_cb(lv_timer_t *timer){
    curr_screen_t screen = (curr_screen_t)(uintptr_t)lv_timer_get_user_data(timer);

    g_delayed_screen_timer = NULL;
    load_screen(screen);
}

void ui_schedule_screen_transition(curr_screen_t to_screen){
    lv_display_t * disp = lv_display_get_default();
    lvgl_drm_warp_t* lvgl_drm_warp = (lvgl_drm_warp_t*)lv_display_get_driver_data(disp);
    
    int current_y = get_screen_target_y(g_cur_scr);
    int target_y = get_screen_target_y(to_screen);

    if(g_delayed_screen_timer != NULL){
        lv_timer_delete(g_delayed_screen_timer);
        g_delayed_screen_timer = NULL;
    }

    // 从spinner 到 其他任何屏幕，(除告警页）都做二阶段动画
    if(g_cur_scr == curr_screen_t_SCREEN_SPINNER && to_screen != curr_screen_t_SCREEN_WARNING && to_screen != curr_screen_t_SCREEN_CONFIRM){
        g_ui_hidden = false;
        g_delayed_screen_timer = lv_timer_create(
            delayed_load_screen_cb,
            UI_LAYER_ANIMATION_INTRO_LOADSCREEN_DELAY / 1000,
            (void*)to_screen
        );
        lv_timer_set_repeat_count(g_delayed_screen_timer, 1);
        layer_animation_ease_in_out_move(
            lvgl_drm_warp->layer_animation, 
            DRM_WARPPER_LAYER_UI, 
            0, SCREEN_HEIGHT, 
            0, UI_SPINNER_INTRO_Y, 
            UI_LAYER_ANIMATION_INTRO_SPINNER_TRANSITION_DURATION, 0);
    
        layer_animation_ease_in_out_move(
            lvgl_drm_warp->layer_animation, 
            DRM_WARPPER_LAYER_UI, 
            0, UI_SPINNER_INTRO_Y, 
            0, target_y, 
            UI_LAYER_ANIMATION_INTRO_DEST_TRANSITION_DURATION, 
            UI_LAYER_ANIMATION_INTRO_DEST_TRANSITION_DELAY);
    }
    else{
        // 其他情况，直接过渡。
        if(to_screen != curr_screen_t_SCREEN_SPINNER){
            g_ui_hidden = false;
        }
        if(current_y != target_y){
            layer_animation_ease_in_out_move(
                lvgl_drm_warp->layer_animation, 
                DRM_WARPPER_LAYER_UI, 
                0, current_y, 
                0, target_y, 
                UI_LAYER_ANIMATION_DURATION, 0);
        }
        load_screen(to_screen);
    }

}


bool ui_is_hidden(){
    return g_ui_hidden;
}
// =========================================
// EEZ 回调 START
// =========================================

// 屏幕加载回调。更新“现在所显示的屏幕”变量。
// 对我们手动创建的widget，还需要把它们加入到group中。
void action_screen_loaded_cb(lv_event_t * e){
    g_cur_scr = (curr_screen_t)(lv_event_get_user_data(e));
    g_ui_hidden = (g_cur_scr == curr_screen_t_SCREEN_SPINNER);
    log_debug("action_screen_loaded_cb: cur_scr = %d", g_cur_scr);

    if(g_cur_scr == curr_screen_t_SCREEN_SETTINGS){
        app_theme_reload();
        ui_settings_load_ctrl_word();
    }
    else if(g_cur_scr == curr_screen_t_SCREEN_FILEMANAGER){
        add_filemanager_to_group();
    }
    else if(g_cur_scr == curr_screen_t_SCREEN_OPLIST){
        ui_oplist_refresh_catalog();
        add_oplist_btn_to_group();
        ui_oplist_focus_current_operator();
    }
    else if(g_cur_scr == curr_screen_t_SCREEN_DISPLAYIMG){
        ui_displayimg_request_refresh();
    }
    else if(g_cur_scr == curr_screen_t_SCREEN_APPLIST){
        ui_apps_refresh_catalog();
        add_applist_btn_to_group();
    }
    ui_shell_net_on_screen_loaded(g_cur_scr);
    return;
};


// 全局按钮回调。主要用于屏幕切换时放过渡。 
void screen_key_event_cb(uint32_t key){
    log_debug("screen_key_event_cb: g_cur_scr = %d, key = %d", g_cur_scr, key);

    if(key == LV_KEY_END){
        ui_confirm(UI_CONFIRM_TYPE_SHUTDOWN);
    }

    // 展示扩列图 界面，按下3/4按钮都可关闭
    if (g_cur_scr == curr_screen_t_SCREEN_DISPLAYIMG){
        if(key == LV_KEY_ESC || key == LV_KEY_ENTER){
            ui_clear_screen_history();
            ui_schedule_screen_transition(curr_screen_t_SCREEN_SPINNER);
        }
        else{
            ui_displayimg_key_event(key);
        }
        return;
    }

    // 主界面 和 干员列表
    if (g_cur_scr == curr_screen_t_SCREEN_MAINMENU || g_cur_scr == curr_screen_t_SCREEN_OPLIST){
        if(key == LV_KEY_ESC){
            ui_clear_screen_history();
            ui_schedule_screen_transition(curr_screen_t_SCREEN_SPINNER);
        }
        return;
    }

    // 警告页面 按任何按钮都复归
    if (g_cur_scr == curr_screen_t_SCREEN_WARNING){
        ui_clear_screen_history();
        ui_schedule_screen_transition(curr_screen_t_SCREEN_SPINNER);
        return;
    }

    if (g_cur_scr == curr_screen_t_SCREEN_SYSINFO2){
        if(key == LV_KEY_ESC){
            ui_pop_screen_transition(curr_screen_t_SCREEN_SYSINFO);
        }
        return;
    }

    if (g_cur_scr == curr_screen_t_SCREEN_SHELL || g_cur_scr == curr_screen_t_SCREEN_NET){
        if(key == LV_KEY_ESC){
            ui_pop_screen_transition(curr_screen_t_SCREEN_SYSINFO2);
        }
        return;
    }

    // 其他界面
    if (g_cur_scr != curr_screen_t_SCREEN_SPINNER){
        // 不在编辑状态的时候再按下esc 回到主界面
        bool is_editing = lv_group_get_editing(groups.op);
        if(key == LV_KEY_ESC){
            if(!is_editing)
                ui_schedule_screen_transition(curr_screen_t_SCREEN_MAINMENU);
            else
                lv_group_set_editing(groups.op, false);
        }
        return;
    }

    // spinner（空界面），处理ui出现

    switch(key){
        // go to oplist screen
        case LV_KEY_LEFT:
        case LV_KEY_RIGHT:
           ui_clear_screen_history();
           ui_push_screen_transition(curr_screen_t_SCREEN_OPLIST);
            break;
        case LV_KEY_ENTER:
            ui_clear_screen_history();
            ui_push_screen_transition(curr_screen_t_SCREEN_DISPLAYIMG);
            break;
        case LV_KEY_ESC:
            ui_clear_screen_history();
            ui_schedule_screen_transition(curr_screen_t_SCREEN_MAINMENU);
            break;
    }
}

curr_screen_t ui_get_current_screen(){
    return g_cur_scr;
}

bool screen_raw_key_event_cb(const key_enc_evdev_event_t *event)
{
    return ui_shell_net_handle_raw_key(event);
}
