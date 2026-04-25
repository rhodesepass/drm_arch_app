#include "utils/theme.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "screens.h"
#include "ui/shell_net.h"
#include "utils/cJSON.h"
#include "utils/log.h"
#include "utils/misc.h"

#define APP_THEME_ROOT_DIR "/root/themes"
#define APP_THEME_ACTIVE_FILE "/root/.epass_active_theme"
#define APP_THEME_CONFIG_JSON "theme.json"
#define APP_THEME_CONFIG_FILE "theme.conf"
#define APP_THEME_RES_DIR "res"
#define APP_THEME_ROOT_RES_PREFIX "/root/res/"
#define APP_THEME_MAX 16
#define APP_THEME_ID_MAX 64
#define APP_THEME_NAME_MAX 64
#define APP_THEME_PATH_MAX 256
#define APP_THEME_OPTIONS_MAX (APP_THEME_MAX * (APP_THEME_NAME_MAX + 1))

typedef struct {
    char id[APP_THEME_ID_MAX];
    char name[APP_THEME_NAME_MAX];
    bool dark;
    uint32_t primary;
    uint32_t secondary;
    uint32_t bg_color;
    uint32_t text_color;
    char dir_path[APP_THEME_PATH_MAX];
    char bg_image_path[APP_THEME_PATH_MAX];
} app_theme_entry_t;

typedef struct {
    app_theme_entry_t themes[APP_THEME_MAX];
    uint32_t count;
    uint32_t active_index;
    bool initialized;
    char options[APP_THEME_OPTIONS_MAX];
} app_theme_state_t;

static app_theme_state_t g_theme_state;

static void theme_set_defaults(app_theme_entry_t *theme, const char *id, const char *name)
{
    memset(theme, 0, sizeof(*theme));
    safe_strcpy(theme->id, sizeof(theme->id), id);
    safe_strcpy(theme->name, sizeof(theme->name), name);
    theme->dark = true;
    theme->primary = parse_rgbff("1793d1");
    theme->secondary = parse_rgbff("E05D44");
    theme->bg_color = parse_rgbff("15171A");
    theme->text_color = parse_rgbff("ffffff");
}

static char *trim_inplace(char *s)
{
    char *end;

    while (*s != '\0' && isspace((unsigned char)*s)) {
        s++;
    }

    end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) {
        end--;
    }

    *end = '\0';
    return s;
}

static app_theme_entry_t *theme_get_active(void)
{
    if (g_theme_state.count == 0) {
        return NULL;
    }

    if (g_theme_state.active_index >= g_theme_state.count) {
        g_theme_state.active_index = 0;
    }

    return &g_theme_state.themes[g_theme_state.active_index];
}

static void theme_rebuild_options(void)
{
    size_t used = 0;

    g_theme_state.options[0] = '\0';

    for (uint32_t i = 0; i < g_theme_state.count; i++) {
        int written;

        written = snprintf(
            g_theme_state.options + used,
            sizeof(g_theme_state.options) - used,
            "%s%s",
            i == 0 ? "" : "\n",
            g_theme_state.themes[i].name
        );

        if (written < 0 || (size_t)written >= sizeof(g_theme_state.options) - used) {
            break;
        }

        used += (size_t)written;
    }
}

static int theme_find_index_by_id(const char *id)
{
    if (id == NULL || id[0] == '\0') {
        return -1;
    }

    for (uint32_t i = 0; i < g_theme_state.count; i++) {
        if (strcmp(g_theme_state.themes[i].id, id) == 0) {
            return (int)i;
        }
    }

    return -1;
}

static int theme_save_active(void)
{
    FILE *fp;
    app_theme_entry_t *theme = theme_get_active();

    if (theme == NULL) {
        return -1;
    }

    fp = fopen(APP_THEME_ACTIVE_FILE, "w");
    if (fp == NULL) {
        log_warn("theme: failed to write %s: %s", APP_THEME_ACTIVE_FILE, strerror(errno));
        return -1;
    }

    fprintf(fp, "%s\n", theme->id);
    fclose(fp);
    return 0;
}

static void theme_load_active_from_disk(void)
{
    FILE *fp;
    char buf[APP_THEME_ID_MAX];
    int index;

    fp = fopen(APP_THEME_ACTIVE_FILE, "r");
    if (fp == NULL) {
        g_theme_state.active_index = 0;
        theme_save_active();
        return;
    }

    if (fgets(buf, sizeof(buf), fp) == NULL) {
        fclose(fp);
        g_theme_state.active_index = 0;
        theme_save_active();
        return;
    }
    fclose(fp);

    safe_strcpy(buf, sizeof(buf), trim_inplace(buf));
    index = theme_find_index_by_id(buf);
    if (index < 0) {
        g_theme_state.active_index = 0;
        theme_save_active();
        return;
    }

    g_theme_state.active_index = (uint32_t)index;
}

static int theme_load_one(const char *dir_name)
{
    app_theme_entry_t *theme;
    char dir_path[APP_THEME_PATH_MAX];
    char json_path[APP_THEME_PATH_MAX];
    char conf_path[APP_THEME_PATH_MAX];

    if (g_theme_state.count >= APP_THEME_MAX) {
        log_warn("theme: theme count exceeds max %d", APP_THEME_MAX);
        return -1;
    }

    join_path(dir_path, sizeof(dir_path), APP_THEME_ROOT_DIR, dir_name);
    join_path(json_path, sizeof(json_path), dir_path, APP_THEME_CONFIG_JSON);
    join_path(conf_path, sizeof(conf_path), dir_path, APP_THEME_CONFIG_FILE);
    if (!file_exists_readable(json_path) && !file_exists_readable(conf_path)) {
        return -1;
    }

    theme = &g_theme_state.themes[g_theme_state.count];
    theme_set_defaults(theme, dir_name, dir_name);
    safe_strcpy(theme->dir_path, sizeof(theme->dir_path), dir_path);

    if (file_exists_readable(json_path)) {
        size_t json_len = 0;
        char *buf = read_file_all(json_path, &json_len);
        cJSON *json;
        const char *value;

        if (buf == NULL) {
            log_warn("theme: failed to read %s", json_path);
            return -1;
        }

        json = cJSON_Parse(buf);
        free(buf);
        if (json == NULL) {
            log_warn("theme: failed to parse %s", json_path);
            return -1;
        }

        value = json_get_string(json, "id");
        if (value != NULL && value[0] != '\0') {
            safe_strcpy(theme->id, sizeof(theme->id), value);
        }

        value = json_get_string(json, "name");
        if (value != NULL && value[0] != '\0') {
            safe_strcpy(theme->name, sizeof(theme->name), value);
        }

        theme->dark = json_get_bool(json, "dark", theme->dark);

        value = json_get_string(json, "primary");
        if (value != NULL && value[0] != '\0') {
            theme->primary = parse_rgbff(value);
        }
        value = json_get_string(json, "secondary");
        if (value != NULL && value[0] != '\0') {
            theme->secondary = parse_rgbff(value);
        }
        value = json_get_string(json, "bg_color");
        if (value != NULL && value[0] != '\0') {
            theme->bg_color = parse_rgbff(value);
        }
        value = json_get_string(json, "text_color");
        if (value != NULL && value[0] != '\0') {
            theme->text_color = parse_rgbff(value);
        }
        value = json_get_string(json, "bg_image");
        if (value != NULL && value[0] != '\0') {
            char abs_path[APP_THEME_PATH_MAX];

            join_path(abs_path, sizeof(abs_path), dir_path, value);
            if (file_exists_readable(abs_path)) {
                set_lvgl_path(theme->bg_image_path, sizeof(theme->bg_image_path), abs_path);
            } else {
                log_warn("theme: missing bg image %s", abs_path);
            }
        }

        cJSON_Delete(json);
    } else {
        FILE *fp;
        char line[256];

        fp = fopen(conf_path, "r");
        if (fp == NULL) {
            log_warn("theme: failed to open %s: %s", conf_path, strerror(errno));
            return -1;
        }

        while (fgets(line, sizeof(line), fp) != NULL) {
            char *key;
            char *value;
            char *eq = strchr(line, '=');

            if (eq == NULL) {
                continue;
            }

            *eq = '\0';
            key = trim_inplace(line);
            value = trim_inplace(eq + 1);

            if (key[0] == '\0' || key[0] == '#') {
                continue;
            }

            if (strcmp(key, "id") == 0) {
                safe_strcpy(theme->id, sizeof(theme->id), value);
            } else if (strcmp(key, "name") == 0) {
                safe_strcpy(theme->name, sizeof(theme->name), value);
            } else if (strcmp(key, "dark") == 0) {
                theme->dark = (strcmp(value, "0") != 0);
            } else if (strcmp(key, "primary") == 0) {
                theme->primary = parse_rgbff(value);
            } else if (strcmp(key, "secondary") == 0) {
                theme->secondary = parse_rgbff(value);
            } else if (strcmp(key, "bg_color") == 0) {
                theme->bg_color = parse_rgbff(value);
            } else if (strcmp(key, "text_color") == 0) {
                theme->text_color = parse_rgbff(value);
            } else if (strcmp(key, "bg_image") == 0 && value[0] != '\0') {
                char abs_path[APP_THEME_PATH_MAX];

                join_path(abs_path, sizeof(abs_path), dir_path, value);
                if (file_exists_readable(abs_path)) {
                    set_lvgl_path(theme->bg_image_path, sizeof(theme->bg_image_path), abs_path);
                } else {
                    log_warn("theme: missing bg image %s", abs_path);
                }
            }
        }

        fclose(fp);
    }

    g_theme_state.count++;
    return 0;
}

static void theme_add_fallback(void)
{
    app_theme_entry_t *theme = &g_theme_state.themes[0];

    theme_set_defaults(theme, "arch-blue", "Arch Blue");
    g_theme_state.count = 1;
    g_theme_state.active_index = 0;
}

static void theme_apply_screens(const app_theme_entry_t *theme)
{
    lv_obj_t *screens[] = {
        objects.mainmenu,
        objects.displayimg,
        objects.oplist,
        objects.sysinfo,
        objects.sysinfo2,
        objects.spinner,
        objects.filemanager,
        objects.settings,
        objects.warning,
        objects.confirm,
        objects.applist,
        objects.shell,
        objects.net,
    };

    for (size_t i = 0; i < sizeof(screens) / sizeof(screens[0]); i++) {
        lv_obj_t *screen = screens[i];

        if (screen == NULL) {
            continue;
        }

        lv_obj_set_style_bg_color(screen, lv_color_hex(theme->bg_color & 0x00FFFFFFu), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(screen, lv_color_hex(theme->text_color & 0x00FFFFFFu), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_image_src(
            screen,
            theme->bg_image_path[0] != '\0' ? theme->bg_image_path : NULL,
            LV_PART_MAIN | LV_STATE_DEFAULT
        );
        lv_obj_set_style_bg_image_opa(
            screen,
            theme->bg_image_path[0] != '\0' ? LV_OPA_COVER : LV_OPA_TRANSP,
            LV_PART_MAIN | LV_STATE_DEFAULT
        );
    }
}

int app_theme_init(void)
{
    DIR *dir;
    struct dirent *entry;

    if (g_theme_state.initialized) {
        return 0;
    }

    memset(&g_theme_state, 0, sizeof(g_theme_state));

    dir = opendir(APP_THEME_ROOT_DIR);
    if (dir != NULL) {
        while ((entry = readdir(dir)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }

            if (entry->d_type != DT_DIR && entry->d_type != DT_UNKNOWN) {
                continue;
            }

            (void)theme_load_one(entry->d_name);
        }
        closedir(dir);
    }

    if (g_theme_state.count == 0) {
        theme_add_fallback();
    }

    theme_rebuild_options();
    theme_load_active_from_disk();
    g_theme_state.initialized = true;

    log_info(
        "theme: loaded %u theme(s), active=%s",
        g_theme_state.count,
        theme_get_active()->id
    );
    return 0;
}

int32_t app_theme_get_active_index(void)
{
    app_theme_init();
    return (int32_t)g_theme_state.active_index;
}

const char *app_theme_get_options(void)
{
    app_theme_init();
    return g_theme_state.options;
}

int app_theme_set_active_index(uint32_t index)
{
    uint32_t prev_index;

    app_theme_init();

    if (index >= g_theme_state.count) {
        return -1;
    }

    if (g_theme_state.active_index == index) {
        return 0;
    }

    prev_index = g_theme_state.active_index;
    g_theme_state.active_index = index;
    if (theme_save_active() != 0) {
        g_theme_state.active_index = prev_index;
        return -1;
    }
    log_info("theme: switched to %s", theme_get_active()->id);
    app_theme_refresh_ui();
    return 0;
}

void app_theme_refresh_ui(void)
{
    app_theme_entry_t *theme;
    lv_display_t *disp;

    app_theme_init();
    theme = theme_get_active();
    if (theme == NULL) {
        return;
    }

    disp = lv_display_get_default();
    if (disp != NULL) {
        lv_theme_t *lv_theme = lv_theme_default_init(
            disp,
            lv_color_hex(theme->primary & 0x00FFFFFFu),
            lv_color_hex(theme->secondary & 0x00FFFFFFu),
            theme->dark,
            LV_FONT_DEFAULT
        );

        lv_display_set_theme(disp, lv_theme);
        theme_apply_screens(theme);
        lv_obj_report_style_change(NULL);
    }

    if (objects.thememode_dropdown != NULL) {
        lv_dropdown_set_options(objects.thememode_dropdown, g_theme_state.options);
        if (lv_dropdown_get_selected(objects.thememode_dropdown) != g_theme_state.active_index) {
            lv_dropdown_set_selected(objects.thememode_dropdown, g_theme_state.active_index);
        }
    }

    ui_shell_net_refresh_theme();
}

uint32_t app_theme_get_primary_color(void)
{
    app_theme_entry_t *theme;

    app_theme_init();
    theme = theme_get_active();
    return theme != NULL ? theme->primary : parse_rgbff("1793d1");
}

uint32_t app_theme_get_secondary_color(void)
{
    app_theme_entry_t *theme;

    app_theme_init();
    theme = theme_get_active();
    return theme != NULL ? theme->secondary : parse_rgbff("E05D44");
}

uint32_t app_theme_get_bg_color(void)
{
    app_theme_entry_t *theme;

    app_theme_init();
    theme = theme_get_active();
    return theme != NULL ? theme->bg_color : parse_rgbff("15171A");
}

uint32_t app_theme_get_text_color(void)
{
    app_theme_entry_t *theme;

    app_theme_init();
    theme = theme_get_active();
    return theme != NULL ? theme->text_color : parse_rgbff("ffffff");
}

bool app_theme_is_dark(void)
{
    app_theme_entry_t *theme;

    app_theme_init();
    theme = theme_get_active();
    return theme != NULL ? theme->dark : true;
}

const char *app_theme_resolve_path(const char *default_path, char *dst, size_t dst_sz)
{
    app_theme_entry_t *theme;
    char themed_res_dir[APP_THEME_PATH_MAX];
    const char *relative_path;

    if (dst == NULL || dst_sz == 0) {
        return default_path;
    }

    if (default_path == NULL || default_path[0] == '\0') {
        dst[0] = '\0';
        return dst;
    }

    app_theme_init();
    safe_strcpy(dst, dst_sz, default_path);

    theme = theme_get_active();
    if (theme == NULL || theme->dir_path[0] == '\0') {
        return dst;
    }

    if (strncmp(default_path, APP_THEME_ROOT_RES_PREFIX, strlen(APP_THEME_ROOT_RES_PREFIX)) != 0) {
        return dst;
    }

    relative_path = default_path + strlen(APP_THEME_ROOT_RES_PREFIX);
    if (relative_path[0] == '\0') {
        return dst;
    }

    join_path(themed_res_dir, sizeof(themed_res_dir), theme->dir_path, APP_THEME_RES_DIR);
    join_path(dst, dst_sz, themed_res_dir, relative_path);
    if (!file_exists_readable(dst)) {
        safe_strcpy(dst, dst_sz, default_path);
    }

    return dst;
}

void app_theme_resolve_lvgl_path(char *dst, size_t dst_sz, const char *default_path)
{
    char resolved[APP_THEME_PATH_MAX];
    const char *abs_path = default_path;

    if (default_path != NULL &&
        isalpha((unsigned char)default_path[0]) &&
        default_path[1] == ':') {
        abs_path = default_path + 2;
    }

    app_theme_resolve_path(abs_path, resolved, sizeof(resolved));
    set_lvgl_path(dst, dst_sz, resolved);
}
