#pragma once


typedef enum {
    UI_CONFIRM_TYPE_FORMAT_SD_CARD,
    UI_CONFIRM_TYPE_SHUTDOWN
} ui_confirm_type_t;


void ui_confirm_init();
void ui_confirm_destroy();

void ui_confirm(ui_confirm_type_t type);