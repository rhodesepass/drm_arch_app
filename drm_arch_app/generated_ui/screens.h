#ifndef EEZ_LVGL_UI_SCREENS_H
#define EEZ_LVGL_UI_SCREENS_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _groups_t {
    lv_group_t *op;
} groups_t;

extern groups_t groups;

void ui_create_groups();

typedef struct _objects_t {
    lv_obj_t *mainmenu;
    lv_obj_t *displayimg;
    lv_obj_t *oplist;
    lv_obj_t *sysinfo;
    lv_obj_t *sysinfo2;
    lv_obj_t *spinner;
    lv_obj_t *filemanager;
    lv_obj_t *settings;
    lv_obj_t *warning;
    lv_obj_t *applist;
    lv_obj_t *shell;
    lv_obj_t *net;
    lv_obj_t *mainmenu_version_label;
    lv_obj_t *oplst_btn;
    lv_obj_t *dispimg_btn;
    lv_obj_t *apps_btn;
    lv_obj_t *file_btn;
    lv_obj_t *sett_btn;
    lv_obj_t *dev_btn;
    lv_obj_t *brightness_scroller;
    lv_obj_t *restart_app_btn;
    lv_obj_t *shutdown_btn;
    lv_obj_t *displayimg_index_label;
    lv_obj_t *dispimg_no_pic_label;
    lv_obj_t *dispimg_lbl_path;
    lv_obj_t *dispimg_container;
    lv_obj_t *oplst_container;
    lv_obj_t *obj0;
    lv_obj_t *obj0__opbtn;
    lv_obj_t *obj0__oplogo;
    lv_obj_t *obj0__opdesc;
    lv_obj_t *obj0__opname;
    lv_obj_t *obj0__sd_flag_1;
    lv_obj_t *obj1;
    lv_obj_t *obj1__opbtn;
    lv_obj_t *obj1__oplogo;
    lv_obj_t *obj1__opdesc;
    lv_obj_t *obj1__opname;
    lv_obj_t *obj1__sd_flag_1;
    lv_obj_t *mainmenu_btn;
    lv_obj_t *obj2;
    lv_obj_t *obj3;
    lv_obj_t *back_btn;
    lv_obj_t *sys_btn;
    lv_obj_t *obj4;
    lv_obj_t *obj5;
    lv_obj_t *obj6;
    lv_obj_t *sysinfo_os_value_label;
    lv_obj_t *sysinfo_kernel_value_label;
    lv_obj_t *sysinfo_board_value_label;
    lv_obj_t *sysinfo_soc_value_label;
    lv_obj_t *sysinfo_ram_value_label;
    lv_obj_t *sysinfo_ip_value_label;
    lv_obj_t *uptime;
    lv_obj_t *net_btn;
    lv_obj_t *shell_btn;
    lv_obj_t *back_btn_3;
    lv_obj_t *sysinfo_version_value_label;
    lv_obj_t *sysinfo_codename_value_label;
    lv_obj_t *obj7;
    lv_obj_t *obj8;
    lv_obj_t *file_container;
    lv_obj_t *dummybtn;
    lv_obj_t *lowbat_trip;
    lv_obj_t *no_intro_block;
    lv_obj_t *no_overlay_block;
    lv_obj_t *swmode_dropdown;
    lv_obj_t *swint_dropdown;
    lv_obj_t *usbmode_dropdown;
    lv_obj_t *thememode_dropdown;
    lv_obj_t *obj9;
    lv_obj_t *back_btn_1;
    lv_obj_t *warning_icon_label;
    lv_obj_t *warning_title_label;
    lv_obj_t *warning_desc_label;
    lv_obj_t *app_container;
    lv_obj_t *obj10;
    lv_obj_t *obj10__appbtn;
    lv_obj_t *obj10__applogo;
    lv_obj_t *obj10__appdesc;
    lv_obj_t *obj10__appname;
    lv_obj_t *obj10__bgfg_flag;
    lv_obj_t *obj10__sd_flag;
    lv_obj_t *obj11;
    lv_obj_t *obj11__appbtn;
    lv_obj_t *obj11__applogo;
    lv_obj_t *obj11__appdesc;
    lv_obj_t *obj11__appname;
    lv_obj_t *obj11__bgfg_flag;
    lv_obj_t *obj11__sd_flag;
    lv_obj_t *applist_back_btn;
    lv_obj_t *applist_no_app_label;
    lv_obj_t *shell_header_panel;
    lv_obj_t *shell_status_label;
    lv_obj_t *shell_keyboard_label;
    lv_obj_t *shell_terminal_panel;
    lv_obj_t *shell_terminal_label;
    lv_obj_t *shell_quick_panel;
    lv_obj_t *shell_ip_btn;
    lv_obj_t *shell_route_btn;
    lv_obj_t *shell_usb_rndis_btn;
    lv_obj_t *shell_usb_mtp_btn;
    lv_obj_t *shell_usb_report_btn;
    lv_obj_t *shell_service_log_btn;
    lv_obj_t *shell_mem_btn;
    lv_obj_t *shell_disk_btn;
    lv_obj_t *shell_footer_panel;
    lv_obj_t *shell_clear_btn;
    lv_obj_t *shell_restart_btn;
    lv_obj_t *shell_nav_back_btn;
    lv_obj_t *net_header_panel;
    lv_obj_t *net_mode_card;
    lv_obj_t *net_mode_label;
    lv_obj_t *net_detail_panel;
    lv_obj_t *net_detail_label;
    lv_obj_t *net_status_card;
    lv_obj_t *net_status_label;
    lv_obj_t *net_action_panel;
    lv_obj_t *net_refresh_btn;
    lv_obj_t *net_rndis_btn;
    lv_obj_t *net_mtp_btn;
    lv_obj_t *net_shell_btn;
    lv_obj_t *net_nav_back_btn;
    lv_obj_t *confirm;
    lv_obj_t *confirm_title_label;
    lv_obj_t *confirm_cancel_btn;
    lv_obj_t *confirm_proceed_btn;
} objects_t;

extern objects_t objects;

enum ScreensEnum {
    SCREEN_ID_MAINMENU = 1,
    SCREEN_ID_DISPLAYIMG = 2,
    SCREEN_ID_OPLIST = 3,
    SCREEN_ID_SYSINFO = 4,
    SCREEN_ID_SYSINFO2 = 5,
    SCREEN_ID_SPINNER = 6,
    SCREEN_ID_FILEMANAGER = 7,
    SCREEN_ID_SETTINGS = 8,
    SCREEN_ID_WARNING = 9,
    SCREEN_ID_CONFIRM = 10,
    SCREEN_ID_APPLIST = 11,
    SCREEN_ID_SHELL = 12,
    SCREEN_ID_NET = 13,
};

void create_screen_mainmenu();
void tick_screen_mainmenu();

void create_screen_displayimg();
void tick_screen_displayimg();

void create_screen_oplist();
void tick_screen_oplist();

void create_screen_sysinfo();
void tick_screen_sysinfo();

void create_screen_sysinfo2();
void tick_screen_sysinfo2();

void create_screen_spinner();
void tick_screen_spinner();

void create_screen_filemanager();
void tick_screen_filemanager();

void create_screen_settings();
void tick_screen_settings();

void create_screen_warning();
void tick_screen_warning();

void create_screen_confirm();
void tick_screen_confirm();

void create_screen_applist();
void tick_screen_applist();

void create_screen_shell();
void tick_screen_shell();

void create_screen_net();
void tick_screen_net();

void create_user_widget_operator_entry(lv_obj_t *parent_obj, int startWidgetIndex);
void tick_user_widget_operator_entry(int startWidgetIndex);

void create_user_widget_app_entry(lv_obj_t *parent_obj, int startWidgetIndex);
void tick_user_widget_app_entry(int startWidgetIndex);

void tick_screen_by_id(enum ScreensEnum screenId);
void tick_screen(int screen_index);

void create_screens();


#ifdef __cplusplus
}
#endif

#endif /*EEZ_LVGL_UI_SCREENS_H*/
