#pragma once
#include <apps/apps_types.h>
#include "prts/prts.h"
#include "lvgl.h"

// 虚拟滚动：可见区域 + 上下缓冲
// 容器高度 280px，每项 80px，可见约 4 项，加上缓冲共 8 项
// 对 这个是对着oplist抄的，谢谢你 伊卡洛斯sama

typedef struct {
    lv_obj_t *container;  // 外层容器对象
    // 请注意：以下顺序和EEZ保持一致！
    lv_obj_t *appbtn;
    lv_obj_t *applogo;
    lv_obj_t *appdesc;
    lv_obj_t *appname;
    lv_obj_t *bgfg_flag;
    lv_obj_t *sd_flag;
    int app_index;   // 该槽位当前显示的应用索引，-1 表示未使用
} ui_apps_entry_objs_t;

typedef struct {
    apps_t* apps;
    ui_apps_entry_objs_t slots[UI_APP_VISIBLE_SLOTS];  // 固定槽位
    int total_count;        // 
    int visible_start;      // 当前可见区域起始索引
} ui_apps_t;


// UI层就先全局变量漫天飞吧....
extern ui_apps_t g_ui_apps;

// 自己添加的方法
void ui_apps_init(apps_t *apps);
void ui_apps_refresh_catalog(void);
void add_applist_btn_to_group();
void ui_apps_destroy();
// EEZ回调不需要添加。
