#include <stdlib.h>

#include "config.h"
#include "render/layer_animation.h"
#include "render/animation_driver.h"
#include "driver/drm_warpper.h"
#include "utils/log.h"
#include "lvgl.h"

int layer_animation_init(layer_animation_t *layer_animation,
                         drm_warpper_t *drm_warpper,
                         animation_driver_t *animation_driver){
    layer_animation->drm_warpper = drm_warpper;
    layer_animation->animation_driver = animation_driver;
    log_info("==> Layer Animation Initialized!");
    return 0;
}

static void layer_animation_move_data_free(void *userdata)
{
    layer_animation_move_data_t *move_data = (layer_animation_move_data_t *)userdata;

    if (!move_data) {
        return;
    }

    free(move_data->xarr);
    free(move_data->yarr);
    free(move_data);
}

static void layer_animation_fade_data_free(void *userdata)
{
    layer_animation_fade_data_t *fade_data = (layer_animation_fade_data_t *)userdata;

    if (!fade_data) {
        return;
    }

    free(fade_data->alphaarr);
    free(fade_data);
}

static void layer_animation_move_cb(void *userdata,bool is_last){
    layer_animation_move_data_t *move_data = (layer_animation_move_data_t *)userdata;

    // log_debug("layer_animation_move_cb: i=%d, step_count=%d", move_data->i, move_data->step_count);
    if(move_data->i >= move_data->step_count){
        log_error("i >= step_count??");
        move_data->i = move_data->step_count - 1;
    }
    drm_warpper_set_layer_coord(
        move_data->drm_warpper, 
        move_data->layer_id, 
        move_data->xarr[move_data->i], 
        move_data->yarr[move_data->i]
    );
    move_data->i++;

    if (is_last){
        log_debug("Layer Animation Move Finished!");
    }
}

int layer_animation_cubic_bezier_move_ex(layer_animation_t *layer_animation,int layer_id,int xs,int ys,int xe,int ye,int ctlx1,int ctly1,int ctx2,int cty2,int duration,int delay,animation_driver_handle_t *out_handle){
    int step_count = duration / LAYER_ANIMATION_STEP_TIME;
    if (step_count <= 0){
        log_error("step <= 0??");
        return -1;
    }
    layer_animation_move_data_t *move_data = malloc(sizeof(layer_animation_move_data_t));
    if (move_data == NULL){
        log_error("malloc failed??");
        return -1;
    }
    move_data->xarr = malloc(step_count * sizeof(int16_t));
    if (move_data->xarr == NULL){
        log_error("malloc failed??");
        free(move_data);
        return -1;
    }
    move_data->yarr = malloc(step_count * sizeof(int16_t));
    if (move_data->yarr == NULL){
        log_error("malloc failed??");
        free(move_data->xarr);
        free(move_data);
        return -1;
    }

    move_data->layer_id = layer_id;
    move_data->step_count = step_count;
    move_data->drm_warpper = layer_animation->drm_warpper;

    move_data->i = 0;
    for (int i = 0; i < step_count; i++){
        uint32_t t = lv_map(i, 0, step_count, 0, LV_BEZIER_VAL_MAX);
        int32_t step = lv_cubic_bezier(t, ctlx1, ctly1, ctx2, cty2);
        int32_t new_value;
        new_value = step * (xe - xs);
        new_value = new_value >> LV_BEZIER_VAL_SHIFT;
        new_value += xs;
        move_data->xarr[i] = new_value;

        new_value = step * (ye - ys);
        new_value = new_value >> LV_BEZIER_VAL_SHIFT;
        new_value += ys;
        move_data->yarr[i] = new_value;
    }

    // 确保最后一个点是目标点
    move_data->xarr[step_count - 1] = xe;
    move_data->yarr[step_count - 1] = ye;

    animation_driver_handle_t timer_handle;

    int ret = animation_driver_create_ex(
        layer_animation->animation_driver,
        &timer_handle, 
        delay, 
        step_count, 
        layer_animation_move_cb, 
        move_data,
        layer_animation_move_data_free
    );
    if (ret != 0){
        log_error("animation_driver_create failed??");
        free(move_data->xarr);
        free(move_data->yarr);
        free(move_data);
        return -1;
    }
    if(out_handle){
        *out_handle = timer_handle;
    }
    return 0;
}

// cubic-bezier can calculated in https://cubic-bezier.com/
int layer_animation_linear_move(layer_animation_t *layer_animation,int layer_id,int xs,int ys,int xe,int ye,int duration,int delay){
    return layer_animation_linear_move_ex(layer_animation,
        layer_id, xs, ys, xe, ye,
        duration, delay, NULL
    );
}

int layer_animation_linear_move_ex(layer_animation_t *layer_animation,int layer_id,int xs,int ys,int xe,int ye,int duration,int delay,animation_driver_handle_t *out_handle){
    return layer_animation_cubic_bezier_move_ex(layer_animation, 
        layer_id, xs, ys, xe, ye, 
        LV_BEZIER_VAL_FLOAT(0), 
        LV_BEZIER_VAL_FLOAT(0), 
        LV_BEZIER_VAL_FLOAT(1), 
        LV_BEZIER_VAL_FLOAT(1), 
        duration, delay,
        out_handle
    );
}

int layer_animation_ease_in_out_move(layer_animation_t *layer_animation,int layer_id,int xs,int ys,int xe,int ye,int duration,int delay){
    return layer_animation_ease_in_out_move_ex(layer_animation,
        layer_id, xs, ys, xe, ye,
        duration, delay, NULL
    );
}

int layer_animation_ease_in_out_move_ex(layer_animation_t *layer_animation,int layer_id,int xs,int ys,int xe,int ye,int duration,int delay,animation_driver_handle_t *out_handle){
    return layer_animation_cubic_bezier_move_ex(layer_animation, 
        layer_id, xs, ys, xe, ye, 
        LV_BEZIER_VAL_FLOAT(0.42), 
        LV_BEZIER_VAL_FLOAT(0), 
        LV_BEZIER_VAL_FLOAT(0.58), 
        LV_BEZIER_VAL_FLOAT(1), 
        duration, delay,
        out_handle
    );
}

int layer_animation_ease_out_move(layer_animation_t *layer_animation,int layer_id,int xs,int ys,int xe,int ye,int duration,int delay){
    return layer_animation_ease_out_move_ex(layer_animation,
        layer_id, xs, ys, xe, ye,
        duration, delay, NULL
    );
}

int layer_animation_ease_out_move_ex(layer_animation_t *layer_animation,int layer_id,int xs,int ys,int xe,int ye,int duration,int delay,animation_driver_handle_t *out_handle){
    return layer_animation_cubic_bezier_move_ex(layer_animation, 
        layer_id, xs, ys, xe, ye, 
        LV_BEZIER_VAL_FLOAT(0), 
        LV_BEZIER_VAL_FLOAT(0), 
        LV_BEZIER_VAL_FLOAT(0.58), 
        LV_BEZIER_VAL_FLOAT(1), 
        duration, delay,
        out_handle
    );
}
int layer_animation_ease_in_move(layer_animation_t *layer_animation,int layer_id,int xs,int ys,int xe,int ye,int duration,int delay){
    return layer_animation_ease_in_move_ex(layer_animation,
        layer_id, xs, ys, xe, ye,
        duration, delay, NULL
    );
}

int layer_animation_ease_in_move_ex(layer_animation_t *layer_animation,int layer_id,int xs,int ys,int xe,int ye,int duration,int delay,animation_driver_handle_t *out_handle){
    return layer_animation_cubic_bezier_move_ex(layer_animation, 
        layer_id, xs, ys, xe, ye, 
        LV_BEZIER_VAL_FLOAT(0.42), 
        LV_BEZIER_VAL_FLOAT(0), 
        LV_BEZIER_VAL_FLOAT(1), 
        LV_BEZIER_VAL_FLOAT(1), 
        duration, delay,
        out_handle
    );
}

//https://cubic-bezier.com/#.17,.67,.83,.67
int layer_animation_ease_move(layer_animation_t *layer_animation,int layer_id,int xs,int ys,int xe,int ye,int duration,int delay){
    return layer_animation_ease_move_ex(layer_animation,
        layer_id, xs, ys, xe, ye,
        duration, delay, NULL
    );
}

int layer_animation_ease_move_ex(layer_animation_t *layer_animation,int layer_id,int xs,int ys,int xe,int ye,int duration,int delay,animation_driver_handle_t *out_handle){
    return layer_animation_cubic_bezier_move_ex(layer_animation, 
        layer_id, xs, ys, xe, ye, 
        LV_BEZIER_VAL_FLOAT(0.17), 
        LV_BEZIER_VAL_FLOAT(0.67), 
        LV_BEZIER_VAL_FLOAT(0.83), 
        LV_BEZIER_VAL_FLOAT(0.67), 
        duration, delay,
        out_handle
    );
}

static void layer_animation_fade_cb(void *userdata,bool is_last){
    layer_animation_fade_data_t *fade_data = (layer_animation_fade_data_t *)userdata;
    if(fade_data->i >= fade_data->step_count){
        log_error("i >= step_count??");
        fade_data->i = fade_data->step_count - 1;
    }
    drm_warpper_set_layer_alpha(
        fade_data->drm_warpper, 
        fade_data->layer_id, 
        fade_data->alphaarr[fade_data->i]
    );
    fade_data->i++;

    if (is_last){
        drm_warpper_set_layer_alpha(
            fade_data->drm_warpper, 
            fade_data->layer_id, 
            fade_data->alphaarr[fade_data->step_count - 1]
        );
        log_debug("Layer Animation Fade Finished!");
    }
}

int layer_animation_fade_in_ex(layer_animation_t *layer_animation,int layer_id,int duration,int delay,animation_driver_handle_t *out_handle){
    int step_count = duration / LAYER_ANIMATION_STEP_TIME;
    if (step_count <= 0){
        log_error("step <= 0??");
        return -1;
    }
    layer_animation_fade_data_t *fade_data = malloc(sizeof(layer_animation_fade_data_t));
    if (fade_data == NULL){
        log_error("malloc failed??");
        return -1;
    }
    fade_data->alphaarr = malloc(step_count * sizeof(int16_t));
    if (fade_data->alphaarr == NULL){
        log_error("malloc failed??");
        free(fade_data);
        return -1;
    }
    fade_data->layer_id = layer_id;
    fade_data->step_count = step_count;
    fade_data->drm_warpper = layer_animation->drm_warpper;
    fade_data->i = 0;
    for (int i = 0; i < step_count; i++){
        fade_data->alphaarr[i] = 0 + (255 - 0) * i / step_count;
    }
    // 确保最后一个点是目标点
    fade_data->alphaarr[step_count - 1] = 255;
    animation_driver_handle_t timer_handle;
    int ret = animation_driver_create_ex(
        layer_animation->animation_driver,
        &timer_handle, 
        delay, 
        step_count, 
        layer_animation_fade_cb, 
        fade_data,
        layer_animation_fade_data_free
    );
    if (ret != 0){
        log_error("animation_driver_create failed??");
        free(fade_data->alphaarr);
        free(fade_data);
        return -1;
    }
    if(out_handle){
        *out_handle = timer_handle;
    }
    return 0;
}

int layer_animation_fade_in(layer_animation_t *layer_animation,int layer_id,int duration,int delay){
    return layer_animation_fade_in_ex(layer_animation, layer_id, duration, delay, NULL);
}

int layer_animation_fade_out_ex(layer_animation_t *layer_animation,int layer_id,int duration,int delay,animation_driver_handle_t *out_handle){
    int step_count = duration / LAYER_ANIMATION_STEP_TIME;
    if (step_count <= 0){
        log_error("step <= 0??");
        return -1;
    }
    layer_animation_fade_data_t *fade_data = malloc(sizeof(layer_animation_fade_data_t));
    if (fade_data == NULL){
        log_error("malloc failed??");
        return -1;
    }
    fade_data->alphaarr = malloc(step_count * sizeof(int16_t));
    if (fade_data->alphaarr == NULL){
        log_error("malloc failed??");
        free(fade_data);
        return -1;
    }
    fade_data->layer_id = layer_id;
    fade_data->step_count = step_count;
    fade_data->drm_warpper = layer_animation->drm_warpper;
    fade_data->i = 0;
    for (int i = 0; i < step_count; i++){
        fade_data->alphaarr[i] = 255 - (255 - 0) * i / step_count;
    }
    // 确保最后一个点是目标点
    fade_data->alphaarr[step_count - 1] = 0;
    animation_driver_handle_t timer_handle;
    int ret = animation_driver_create_ex(
        layer_animation->animation_driver,
        &timer_handle, 
        delay, 
        step_count, 
        layer_animation_fade_cb, 
        fade_data,
        layer_animation_fade_data_free
    );
    if (ret != 0){
        log_error("animation_driver_create failed??");
        free(fade_data->alphaarr);
        free(fade_data);
        return -1;
    }
    if(out_handle){
        *out_handle = timer_handle;
    }
    return 0;
}

int layer_animation_fade_out(layer_animation_t *layer_animation,int layer_id,int duration,int delay){
    return layer_animation_fade_out_ex(layer_animation, layer_id, duration, delay, NULL);
}

int layer_animation_cubic_bezier_move(layer_animation_t *layer_animation,int layer_id,int xs,int ys,int xe,int ye,int ctlx1,int ctly1,int ctx2,int cty2,int duration,int delay){
    return layer_animation_cubic_bezier_move_ex(
        layer_animation,
        layer_id,
        xs,
        ys,
        xe,
        ye,
        ctlx1,
        ctly1,
        ctx2,
        cty2,
        duration,
        delay,
        NULL
    );
}
