#include "styles.h"
#include "images.h"
#include "fonts.h"

#include "ui.h"
#include "screens.h"

//
// Style: op_btn
//

void init_style_op_btn_MAIN_DEFAULT(lv_style_t *style) {
    lv_style_set_pad_left(style, 8);
    lv_style_set_pad_bottom(style, 8);
    lv_style_set_pad_right(style, 8);
    lv_style_set_pad_top(style, 8);
    lv_style_set_margin_top(style, 0);
    lv_style_set_bg_color(style, lv_color_hex(0xff494947));
};

lv_style_t *get_style_op_btn_MAIN_DEFAULT() {
    static lv_style_t *style;
    if (!style) {
        style = (lv_style_t *)lv_malloc(sizeof(lv_style_t));
        lv_style_init(style);
        init_style_op_btn_MAIN_DEFAULT(style);
    }
    return style;
};

void init_style_op_btn_MAIN_FOCUSED(lv_style_t *style) {
    lv_style_set_bg_color(style, lv_color_hex(0xff67d9ec));
};

lv_style_t *get_style_op_btn_MAIN_FOCUSED() {
    static lv_style_t *style;
    if (!style) {
        style = (lv_style_t *)lv_malloc(sizeof(lv_style_t));
        lv_style_init(style);
        init_style_op_btn_MAIN_FOCUSED(style);
    }
    return style;
};

void add_style_op_btn(lv_obj_t *obj) {
    (void)obj;
    lv_obj_add_style(obj, get_style_op_btn_MAIN_DEFAULT(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_style(obj, get_style_op_btn_MAIN_FOCUSED(), LV_PART_MAIN | LV_STATE_FOCUSED);
};

void remove_style_op_btn(lv_obj_t *obj) {
    (void)obj;
    lv_obj_remove_style(obj, get_style_op_btn_MAIN_DEFAULT(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_remove_style(obj, get_style_op_btn_MAIN_FOCUSED(), LV_PART_MAIN | LV_STATE_FOCUSED);
};

//
// Style: label_large
//

void init_style_label_large_MAIN_DEFAULT(lv_style_t *style) {
    lv_style_set_text_font(style, &ui_font_sourceselif_heavy_24);
};

lv_style_t *get_style_label_large_MAIN_DEFAULT() {
    static lv_style_t *style;
    if (!style) {
        style = (lv_style_t *)lv_malloc(sizeof(lv_style_t));
        lv_style_init(style);
        init_style_label_large_MAIN_DEFAULT(style);
    }
    return style;
};

void add_style_label_large(lv_obj_t *obj) {
    (void)obj;
    lv_obj_add_style(obj, get_style_label_large_MAIN_DEFAULT(), LV_PART_MAIN | LV_STATE_DEFAULT);
};

void remove_style_label_large(lv_obj_t *obj) {
    (void)obj;
    lv_obj_remove_style(obj, get_style_label_large_MAIN_DEFAULT(), LV_PART_MAIN | LV_STATE_DEFAULT);
};

//
// Style: label_small
//

void init_style_label_small_MAIN_DEFAULT(lv_style_t *style) {
    lv_style_set_text_font(style, &ui_font_sourcesans_reg_14);
};

lv_style_t *get_style_label_small_MAIN_DEFAULT() {
    static lv_style_t *style;
    if (!style) {
        style = (lv_style_t *)lv_malloc(sizeof(lv_style_t));
        lv_style_init(style);
        init_style_label_small_MAIN_DEFAULT(style);
    }
    return style;
};

void add_style_label_small(lv_obj_t *obj) {
    (void)obj;
    lv_obj_add_style(obj, get_style_label_small_MAIN_DEFAULT(), LV_PART_MAIN | LV_STATE_DEFAULT);
};

void remove_style_label_small(lv_obj_t *obj) {
    (void)obj;
    lv_obj_remove_style(obj, get_style_label_small_MAIN_DEFAULT(), LV_PART_MAIN | LV_STATE_DEFAULT);
};

//
// Style: main_btn
//

void init_style_main_btn_MAIN_DEFAULT(lv_style_t *style) {
    lv_style_set_text_align(style, LV_TEXT_ALIGN_CENTER);
    lv_style_set_bg_color(style, lv_color_hex(0xff20679f));
};

lv_style_t *get_style_main_btn_MAIN_DEFAULT() {
    static lv_style_t *style;
    if (!style) {
        style = (lv_style_t *)lv_malloc(sizeof(lv_style_t));
        lv_style_init(style);
        init_style_main_btn_MAIN_DEFAULT(style);
    }
    return style;
};

void init_style_main_btn_MAIN_FOCUSED(lv_style_t *style) {
    lv_style_set_bg_color(style, lv_color_hex(0xff398ed0));
};

lv_style_t *get_style_main_btn_MAIN_FOCUSED() {
    static lv_style_t *style;
    if (!style) {
        style = (lv_style_t *)lv_malloc(sizeof(lv_style_t));
        lv_style_init(style);
        init_style_main_btn_MAIN_FOCUSED(style);
    }
    return style;
};

void add_style_main_btn(lv_obj_t *obj) {
    (void)obj;
    lv_obj_add_style(obj, get_style_main_btn_MAIN_DEFAULT(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_style(obj, get_style_main_btn_MAIN_FOCUSED(), LV_PART_MAIN | LV_STATE_FOCUSED);
};

void remove_style_main_btn(lv_obj_t *obj) {
    (void)obj;
    lv_obj_remove_style(obj, get_style_main_btn_MAIN_DEFAULT(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_remove_style(obj, get_style_main_btn_MAIN_FOCUSED(), LV_PART_MAIN | LV_STATE_FOCUSED);
};

//
// Style: main_small_btn
//

void init_style_main_small_btn_MAIN_DEFAULT(lv_style_t *style) {
    lv_style_set_bg_color(style, lv_color_hex(0xff5f1010));
};

lv_style_t *get_style_main_small_btn_MAIN_DEFAULT() {
    static lv_style_t *style;
    if (!style) {
        style = (lv_style_t *)lv_malloc(sizeof(lv_style_t));
        lv_style_init(style);
        init_style_main_small_btn_MAIN_DEFAULT(style);
    }
    return style;
};

void init_style_main_small_btn_MAIN_FOCUSED(lv_style_t *style) {
    lv_style_set_bg_color(style, lv_color_hex(0xffa63737));
};

lv_style_t *get_style_main_small_btn_MAIN_FOCUSED() {
    static lv_style_t *style;
    if (!style) {
        style = (lv_style_t *)lv_malloc(sizeof(lv_style_t));
        lv_style_init(style);
        init_style_main_small_btn_MAIN_FOCUSED(style);
    }
    return style;
};

void add_style_main_small_btn(lv_obj_t *obj) {
    (void)obj;
    lv_obj_add_style(obj, get_style_main_small_btn_MAIN_DEFAULT(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_style(obj, get_style_main_small_btn_MAIN_FOCUSED(), LV_PART_MAIN | LV_STATE_FOCUSED);
};

void remove_style_main_small_btn(lv_obj_t *obj) {
    (void)obj;
    lv_obj_remove_style(obj, get_style_main_small_btn_MAIN_DEFAULT(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_remove_style(obj, get_style_main_small_btn_MAIN_FOCUSED(), LV_PART_MAIN | LV_STATE_FOCUSED);
};

//
// Style: fa_label
//

void init_style_fa_label_MAIN_DEFAULT(lv_style_t *style) {
    lv_style_set_text_font(style, &ui_font_fontawesome);
};

lv_style_t *get_style_fa_label_MAIN_DEFAULT() {
    static lv_style_t *style;
    if (!style) {
        style = (lv_style_t *)lv_malloc(sizeof(lv_style_t));
        lv_style_init(style);
        init_style_fa_label_MAIN_DEFAULT(style);
    }
    return style;
};

void add_style_fa_label(lv_obj_t *obj) {
    (void)obj;
    lv_obj_add_style(obj, get_style_fa_label_MAIN_DEFAULT(), LV_PART_MAIN | LV_STATE_DEFAULT);
};

void remove_style_fa_label(lv_obj_t *obj) {
    (void)obj;
    lv_obj_remove_style(obj, get_style_fa_label_MAIN_DEFAULT(), LV_PART_MAIN | LV_STATE_DEFAULT);
};

//
// Style: op_entry
//

void init_style_op_entry_MAIN_DEFAULT(lv_style_t *style) {
    lv_style_set_margin_top(style, 5);
};

lv_style_t *get_style_op_entry_MAIN_DEFAULT() {
    static lv_style_t *style;
    if (!style) {
        style = (lv_style_t *)lv_malloc(sizeof(lv_style_t));
        lv_style_init(style);
        init_style_op_entry_MAIN_DEFAULT(style);
    }
    return style;
};

void add_style_op_entry(lv_obj_t *obj) {
    (void)obj;
    lv_obj_add_style(obj, get_style_op_entry_MAIN_DEFAULT(), LV_PART_MAIN | LV_STATE_DEFAULT);
};

void remove_style_op_entry(lv_obj_t *obj) {
    (void)obj;
    lv_obj_remove_style(obj, get_style_op_entry_MAIN_DEFAULT(), LV_PART_MAIN | LV_STATE_DEFAULT);
};

//
// Style: sd_flag
//

void init_style_sd_flag_MAIN_DEFAULT(lv_style_t *style) {
    lv_style_set_bg_opa(style, 255);
    lv_style_set_bg_color(style, lv_color_hex(0xff2c3cbd));
    lv_style_set_pad_top(style, 0);
    lv_style_set_pad_bottom(style, 0);
    lv_style_set_pad_left(style, 2);
    lv_style_set_pad_right(style, 2);
    lv_style_set_radius(style, 15);
    lv_style_set_text_font(style, &ui_font_sourcesans_reg_14);
};

lv_style_t *get_style_sd_flag_MAIN_DEFAULT() {
    static lv_style_t *style;
    if (!style) {
        style = (lv_style_t *)lv_malloc(sizeof(lv_style_t));
        lv_style_init(style);
        init_style_sd_flag_MAIN_DEFAULT(style);
    }
    return style;
};

void add_style_sd_flag(lv_obj_t *obj) {
    (void)obj;
    lv_obj_add_style(obj, get_style_sd_flag_MAIN_DEFAULT(), LV_PART_MAIN | LV_STATE_DEFAULT);
};

void remove_style_sd_flag(lv_obj_t *obj) {
    (void)obj;
    lv_obj_remove_style(obj, get_style_sd_flag_MAIN_DEFAULT(), LV_PART_MAIN | LV_STATE_DEFAULT);
};

//
// Style: app_bg_running
//

void init_style_app_bg_running_MAIN_DEFAULT(lv_style_t *style) {
    init_style_sd_flag_MAIN_DEFAULT(style);
    
    lv_style_set_bg_color(style, lv_color_hex(0xff23910a));
};

lv_style_t *get_style_app_bg_running_MAIN_DEFAULT() {
    static lv_style_t *style;
    if (!style) {
        style = (lv_style_t *)lv_malloc(sizeof(lv_style_t));
        lv_style_init(style);
        init_style_app_bg_running_MAIN_DEFAULT(style);
    }
    return style;
};

void add_style_app_bg_running(lv_obj_t *obj) {
    (void)obj;
    lv_obj_add_style(obj, get_style_app_bg_running_MAIN_DEFAULT(), LV_PART_MAIN | LV_STATE_DEFAULT);
};

void remove_style_app_bg_running(lv_obj_t *obj) {
    (void)obj;
    lv_obj_remove_style(obj, get_style_app_bg_running_MAIN_DEFAULT(), LV_PART_MAIN | LV_STATE_DEFAULT);
};

//
// Style: app_fg
//

void init_style_app_fg_MAIN_DEFAULT(lv_style_t *style) {
    init_style_sd_flag_MAIN_DEFAULT(style);
    
    lv_style_set_bg_color(style, lv_color_hex(0xffb3550c));
};

lv_style_t *get_style_app_fg_MAIN_DEFAULT() {
    static lv_style_t *style;
    if (!style) {
        style = (lv_style_t *)lv_malloc(sizeof(lv_style_t));
        lv_style_init(style);
        init_style_app_fg_MAIN_DEFAULT(style);
    }
    return style;
};

void add_style_app_fg(lv_obj_t *obj) {
    (void)obj;
    lv_obj_add_style(obj, get_style_app_fg_MAIN_DEFAULT(), LV_PART_MAIN | LV_STATE_DEFAULT);
};

void remove_style_app_fg(lv_obj_t *obj) {
    (void)obj;
    lv_obj_remove_style(obj, get_style_app_fg_MAIN_DEFAULT(), LV_PART_MAIN | LV_STATE_DEFAULT);
};

//
// Style: app_bg_notrunning
//

void init_style_app_bg_notrunning_MAIN_DEFAULT(lv_style_t *style) {
    init_style_sd_flag_MAIN_DEFAULT(style);
    
    lv_style_set_bg_color(style, lv_color_hex(0xff919197));
};

lv_style_t *get_style_app_bg_notrunning_MAIN_DEFAULT() {
    static lv_style_t *style;
    if (!style) {
        style = (lv_style_t *)lv_malloc(sizeof(lv_style_t));
        lv_style_init(style);
        init_style_app_bg_notrunning_MAIN_DEFAULT(style);
    }
    return style;
};

void add_style_app_bg_notrunning(lv_obj_t *obj) {
    (void)obj;
    lv_obj_add_style(obj, get_style_app_bg_notrunning_MAIN_DEFAULT(), LV_PART_MAIN | LV_STATE_DEFAULT);
};

void remove_style_app_bg_notrunning(lv_obj_t *obj) {
    (void)obj;
    lv_obj_remove_style(obj, get_style_app_bg_notrunning_MAIN_DEFAULT(), LV_PART_MAIN | LV_STATE_DEFAULT);
};

//
//
//

void add_style(lv_obj_t *obj, int32_t styleIndex) {
    typedef void (*AddStyleFunc)(lv_obj_t *obj);
    static const AddStyleFunc add_style_funcs[] = {
        add_style_op_btn,
        add_style_label_large,
        add_style_label_small,
        add_style_main_btn,
        add_style_main_small_btn,
        add_style_fa_label,
        add_style_op_entry,
        add_style_sd_flag,
        add_style_app_bg_running,
        add_style_app_fg,
        add_style_app_bg_notrunning,
    };
    add_style_funcs[styleIndex](obj);
}

void remove_style(lv_obj_t *obj, int32_t styleIndex) {
    typedef void (*RemoveStyleFunc)(lv_obj_t *obj);
    static const RemoveStyleFunc remove_style_funcs[] = {
        remove_style_op_btn,
        remove_style_label_large,
        remove_style_label_small,
        remove_style_main_btn,
        remove_style_main_small_btn,
        remove_style_fa_label,
        remove_style_op_entry,
        remove_style_sd_flag,
        remove_style_app_bg_running,
        remove_style_app_fg,
        remove_style_app_bg_notrunning,
    };
    remove_style_funcs[styleIndex](obj);
}

