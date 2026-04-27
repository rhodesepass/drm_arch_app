#if defined(EEZ_FOR_LVGL)
#include <eez/core/vars.h>
#endif

#include "ui.h"
#include "screens.h"
#include "images.h"
#include "actions.h"
#include "vars.h"







#if defined(EEZ_FOR_LVGL)

void ui_init() {
    eez_flow_init(assets, sizeof(assets), (lv_obj_t **)&objects, sizeof(objects), images, sizeof(images), actions);
}

void ui_tick() {
    eez_flow_tick();
    tick_screen(g_currentScreen);
}

#else

#include <string.h>

static enum ScreensEnum currentScreen = 0;

static lv_obj_t *get_screen_object(enum ScreensEnum screenId) {
    switch (screenId) {
        case SCREEN_ID_MAINMENU:
            return objects.mainmenu;
        case SCREEN_ID_DISPLAYIMG:
            return objects.displayimg;
        case SCREEN_ID_OPLIST:
            return objects.oplist;
        case SCREEN_ID_SYSINFO:
            return objects.sysinfo;
        case SCREEN_ID_SYSINFO2:
            return objects.sysinfo2;
        case SCREEN_ID_SPINNER:
            return objects.spinner;
        case SCREEN_ID_FILEMANAGER:
            return objects.filemanager;
        case SCREEN_ID_SETTINGS:
            return objects.settings;
        case SCREEN_ID_WARNING:
            return objects.warning;
        case SCREEN_ID_CONFIRM:
            return objects.confirm;
        case SCREEN_ID_APPLIST:
            return objects.applist;
        case SCREEN_ID_SHELL:
            return objects.shell;
        case SCREEN_ID_NET:
            return objects.net;
        default:
            return 0;
    }
}

void loadScreen(enum ScreensEnum screenId) {
    lv_obj_t *screen = get_screen_object(screenId);
    if (screen == 0) {
        return;
    }
    currentScreen = screenId;
    lv_scr_load_anim(screen, LV_SCR_LOAD_ANIM_FADE_IN, 200, 0, false);
}

void ui_init() {
    create_screens();
    loadScreen(SCREEN_ID_MAINMENU);

}

void ui_tick() {
    if (currentScreen != 0) {
        tick_screen_by_id(currentScreen);
    }
}

#endif
