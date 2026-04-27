#ifndef EEZ_LVGL_UI_EVENTS_H
#define EEZ_LVGL_UI_EVENTS_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

extern void action_show_oplist(lv_event_t * e);
extern void action_show_menu(lv_event_t * e);
extern void action_show_sysinfo(lv_event_t * e);
extern void action_shutdown(lv_event_t * e);
extern void action_show_sys(lv_event_t * e);
extern void action_screen_loaded_cb(lv_event_t * e);
extern void action_displayimg_key(lv_event_t * e);
extern void action_settings_ctrl_changed(lv_event_t * e);
extern void action_restart_app(lv_event_t * e);
extern void action_show_settings(lv_event_t * e);
extern void action_show_files(lv_event_t * e);
extern void action_show_apps(lv_event_t * e);
extern void action_show_dispimg(lv_event_t * e);
extern void action_confirm_proceed(lv_event_t * e);
extern void action_confirm_cancel(lv_event_t * e);
extern void action_call_srgn_config(lv_event_t * e);
extern void action_show_net(lv_event_t * e);
extern void action_show_shell(lv_event_t * e);


#ifdef __cplusplus
}
#endif

#endif /*EEZ_LVGL_UI_EVENTS_H*/