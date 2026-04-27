#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"

int app_theme_init(void);
int app_theme_reload(void);
int32_t app_theme_get_active_index(void);
int app_theme_set_active_index(uint32_t index);
const char *app_theme_get_options(void);
void app_theme_refresh_ui(void);
uint32_t app_theme_get_primary_color(void);
uint32_t app_theme_get_secondary_color(void);
uint32_t app_theme_get_bg_color(void);
uint32_t app_theme_get_text_color(void);
bool app_theme_is_dark(void);
const char *app_theme_resolve_path(const char *default_path, char *dst, size_t dst_sz);
void app_theme_resolve_lvgl_path(char *dst, size_t dst_sz, const char *default_path);
