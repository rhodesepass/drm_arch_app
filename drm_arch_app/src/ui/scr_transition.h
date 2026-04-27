// UI 屏幕过渡 相关处理方法
#pragma once
#include "vars.h"

// 自己添加的方法
curr_screen_t ui_get_current_screen();
void ui_schedule_screen_transition(curr_screen_t to_screen);
void ui_push_screen_transition(curr_screen_t to_screen);
void ui_pop_screen_transition(curr_screen_t fallback_screen);
void ui_clear_screen_history(void);
bool ui_is_hidden();
