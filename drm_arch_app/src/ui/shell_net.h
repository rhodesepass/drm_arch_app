#pragma once

#include <stdbool.h>

#include "driver/key_enc_evdev.h"
#include "vars.h"

void ui_shell_net_init(void);
void ui_shell_net_on_screen_loaded(curr_screen_t screen);
bool ui_shell_net_handle_raw_key(const key_enc_evdev_event_t *event);
void ui_shell_net_refresh_theme(void);
void ui_shell_net_shutdown(void);
