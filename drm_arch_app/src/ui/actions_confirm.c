// 二次确认 专用
#include "ui.h"
#include "ui/actions_confirm.h"
#include "utils/log.h"
#include "ui/scr_transition.h"
#include "config.h"
#include "utils/spsc_queue.h"
#include "lvgl.h"

static spsc_bq_t g_confirm_queue;
static ui_confirm_type_t g_confirm_type;
static lv_timer_t * g_confirm_timer = NULL;

extern int g_running;
extern int g_exitcode;
// =========================================
// 自己添加的方法 START
// =========================================

inline static const char *get_confirm_prompt(ui_confirm_type_t type){
    switch(type){
        case UI_CONFIRM_TYPE_FORMAT_SD_CARD:
            return "确定格式化SD卡吗？";
        case UI_CONFIRM_TYPE_SHUTDOWN:
            return "确定要关机吗？";
        default:
            return "未知的确认。";
    }
}
// 由外部任意线程请求
void ui_confirm(ui_confirm_type_t type){
    spsc_bq_push(&g_confirm_queue, (void *)type);
}

// 由LVGL timer回调触发。
// 别忘记：LVGL不是线程安全的。UI的读写只能由LVGL线程完成。
static void ui_confirm_timer_cb(lv_timer_t * timer){
    ui_confirm_type_t type;
    if(spsc_bq_try_pop(&g_confirm_queue, (void **)&type) == 0){
        log_debug("ui_confirm_timer_cb: type = %d", type);
        g_confirm_type = type;
        ui_schedule_screen_transition(curr_screen_t_SCREEN_CONFIRM);
    }
}

void ui_confirm_init(){
    log_info("==> UI Confirm Initializing...");
    spsc_bq_init(&g_confirm_queue, 10);
    g_confirm_timer = lv_timer_create(ui_confirm_timer_cb, UI_WARNING_TIMER_TICK_PERIOD / 1000, NULL);
    log_info("==> UI Confirm Initialized!");
}
void ui_confirm_destroy(){
    lv_timer_delete(g_confirm_timer);
    g_confirm_timer = NULL;
    spsc_bq_close(&g_confirm_queue);
    spsc_bq_destroy(&g_confirm_queue);
}


// =========================================
// EEZ 回调 START
// =========================================

const char *get_var_confirm_title(){
    return get_confirm_prompt(g_confirm_type);
}

void set_var_confirm_title(const char *value){
    return;
}

void action_confirm_proceed(lv_event_t * e){
    if(g_confirm_type == UI_CONFIRM_TYPE_FORMAT_SD_CARD){
        g_running = 0;
        g_exitcode = EXITCODE_FORMAT_SD_CARD;
    }
    else if(g_confirm_type == UI_CONFIRM_TYPE_SHUTDOWN){
        g_running = 0;
        g_exitcode = EXITCODE_SHUTDOWN;
    }
}

void action_confirm_cancel(lv_event_t * e){
    log_debug("action_confirm_cancel");
    ui_schedule_screen_transition(curr_screen_t_SCREEN_SPINNER);
}
