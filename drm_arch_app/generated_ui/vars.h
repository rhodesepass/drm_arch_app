#ifndef EEZ_LVGL_UI_VARS_H
#define EEZ_LVGL_UI_VARS_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// enum declarations

typedef enum {
    sw_mode_t_SW_MODE_SEQUENCE = 0,
    sw_mode_t_SW_MODE_RANDOM = 1,
    sw_mode_t_SW_RANDOM_MANUAL = 2
} sw_mode_t;

typedef enum {
    sw_interval_t_SW_INTERVAL_1MIN = 0,
    sw_interval_t_SW_INTERVAL_3MIN = 1,
    sw_interval_t_SW_INTERVAL_5MIN = 2,
    sw_interval_t_SW_INTERVAL_10MIN = 3,
    sw_interval_t_SW_INTERVAL_30MIN = 4
} sw_interval_t;

typedef enum {
    curr_screen_t_SCREEN_MAINMENU = 0,
    curr_screen_t_SCREEN_OPLIST = 1,
    curr_screen_t_SCREEN_SYSINFO = 2,
    curr_screen_t_SCREEN_SPINNER = 3,
    curr_screen_t_SCREEN_DISPLAYIMG = 4,
    curr_screen_t_SCREEN_FILEMANAGER = 5,
    curr_screen_t_SCREEN_SETTINGS = 6,
    curr_screen_t_SCREEN_WARNING = 7,
    curr_screen_t_SCREEN_CONFIRM = 8,
    curr_screen_t_SCREEN_APPLIST = 9,
    curr_screen_t_SCREEN_SHELL = 10,
    curr_screen_t_SCREEN_NET = 11
} curr_screen_t;

typedef enum {
    usb_mode_t_MTP = 0,
    usb_mode_t_SERIAL = 1,
    usb_mode_t_RNDIS = 2,
    usb_mode_t_NONE = 3
} usb_mode_t;

// Flow global variables

enum FlowGlobalVariables {
    FLOW_GLOBAL_VARIABLE_SD_LABEL = 0,
    FLOW_GLOBAL_VARIABLE_RAM_LABEL = 1,
    FLOW_GLOBAL_VARIABLE_SD_PERCENT = 2,
    FLOW_GLOBAL_VARIABLE_RAM_PERCENT = 3,
    FLOW_GLOBAL_VARIABLE_DISPLAYIMG_SIZE_LBL = 4,
    FLOW_GLOBAL_VARIABLE_USB_MODE = 5,
    FLOW_GLOBAL_VARIABLE_THEME_MODE = 6,
    FLOW_GLOBAL_VARIABLE_WARNING_TITLE = 7,
    FLOW_GLOBAL_VARIABLE_WARNING_DESC = 8,
    FLOW_GLOBAL_VARIABLE_DISPIMG_HIDE_WARNING = 9,
    FLOW_GLOBAL_VARIABLE_DISPIMG_PATH = 10,
    FLOW_GLOBAL_VARIABLE_CONFIRM_TITLE = 11,
    FLOW_GLOBAL_VARIABLE_WARNING_ICON = 12,
    FLOW_GLOBAL_VARIABLE_APPLIST_HIDE_WARNING = 13,
    FLOW_GLOBAL_VARIABLE_OS_DETAIL = 14,
    FLOW_GLOBAL_VARIABLE_KER_DETAIL = 15,
    FLOW_GLOBAL_VARIABLE_SOC_DETAIL = 16,
    FLOW_GLOBAL_VARIABLE_BOARD_DETAIL = 17,
    FLOW_GLOBAL_VARIABLE_RAM_DETAIL = 18,
    FLOW_GLOBAL_VARIABLE_UPTIME_DETAIL = 19,
    FLOW_GLOBAL_VARIABLE_IP_DETAIL = 20,
    FLOW_GLOBAL_VARIABLE_CODENAME_DETAIL = 21
};

// Native global variables

extern const char *get_var_epass_version();
extern void set_var_epass_version(const char *value);
extern const char *get_var_sysinfo();
extern void set_var_sysinfo(const char *value);
extern sw_mode_t get_var_sw_mode();
extern void set_var_sw_mode(sw_mode_t value);
extern sw_interval_t get_var_sw_interval();
extern void set_var_sw_interval(sw_interval_t value);
extern int32_t get_var_brightness();
extern void set_var_brightness(int32_t value);
extern const char *get_var_sd_label();
extern void set_var_sd_label(const char *value);
extern const char *get_var_ram_label();
extern void set_var_ram_label(const char *value);
extern int32_t get_var_sd_percent();
extern void set_var_sd_percent(int32_t value);
extern int32_t get_var_ram_percent();
extern void set_var_ram_percent(int32_t value);
extern const char *get_var_displayimg_size_lbl();
extern void set_var_displayimg_size_lbl(const char *value);
extern usb_mode_t get_var_usb_mode();
extern void set_var_usb_mode(usb_mode_t value);
extern int32_t get_var_theme_mode();
extern void set_var_theme_mode(int32_t value);
extern const char *get_var_warning_title();
extern void set_var_warning_title(const char *value);
extern const char *get_var_warning_desc();
extern void set_var_warning_desc(const char *value);
extern bool get_var_dispimg_hide_warning();
extern void set_var_dispimg_hide_warning(bool value);
extern const char *get_var_dispimg_path();
extern void set_var_dispimg_path(const char *value);
extern const char *get_var_confirm_title();
extern void set_var_confirm_title(const char *value);
extern const char *get_var_warning_icon();
extern void set_var_warning_icon(const char *value);
extern bool get_var_applist_hide_warning();
extern void set_var_applist_hide_warning(bool value);
extern const char *get_var_os_detail();
extern void set_var_os_detail(const char *value);
extern const char *get_var_ker_detail();
extern void set_var_ker_detail(const char *value);
extern const char *get_var_soc_detail();
extern void set_var_soc_detail(const char *value);
extern const char *get_var_board_detail();
extern void set_var_board_detail(const char *value);
extern const char *get_var_ram_detail();
extern void set_var_ram_detail(const char *value);
extern const char *get_var_uptime_detail();
extern void set_var_uptime_detail(const char *value);
extern const char *get_var_ip_detail();
extern void set_var_ip_detail(const char *value);
extern const char *get_var_codename_detail();
extern void set_var_codename_detail(const char *value);


#ifdef __cplusplus
}
#endif

#endif /*EEZ_LVGL_UI_VARS_H*/
