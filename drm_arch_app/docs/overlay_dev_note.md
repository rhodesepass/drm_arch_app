# Overlay Layer Development Guide

## Basic Constraints

The overlay layer is used to draw overlay effects such as "transition animations" and "operator info (opinfo)" above VIDEO and below UI. The core constraint is: **the PRTS timer callback thread must return as quickly as possible**, so any potentially time-consuming per-frame drawing must run in the overlay worker thread.

### 1) Threading and timing model

- `overlay_t` internally maintains:
  - overlay layer double buffers (`overlay_buf_1/2`) and their corresponding display queue items
  - a dedicated thread `overlay_worker_thread`, with tasks submitted through `overlay_worker_schedule()`
  - `overlay->request_abort`: requests early termination (for example, stop immediately before the effect completes)
  - `overlay->overlay_timer_handle`: **a synchronization signal indicating that worker-side cleanup has completed**
- `overlay_worker_schedule(overlay, func, userdata)`:
  - if the worker is idle: submit the task (`func(userdata, skipped_frames)`)
  - if the worker is busy: drop this task and accumulate `skipped_frames` (to be deducted on the next execution)
- `overlay_abort(overlay)`:
  - sets `overlay->request_abort = 1`
  - polls until `overlay->overlay_timer_handle == 0`
  - semantically equivalent to: "request termination of the overlay effect and wait for the worker to finish resource cleanup / timer unregister"

> Conclusion: **as long as your effect creates `overlay->overlay_timer_handle` (that is, it is a type that requires the worker), you must ensure that resources are fully cleaned up in the worker and that `overlay->overlay_timer_handle` is reset to zero**, otherwise `overlay_abort()` will wait forever.

### 2) How to add a new Transition

#### 2.1 Implementation and configuration locations

- Implementation files:
  - `src/overlay/transitions.h`: add types / parameters / function declarations
  - `src/overlay/transitions.c`: implement drawing and animation driving
- Configuration mapping:
  - `src/prts/operators.c`: map the strings in `transition_in/transition_loop` to `transition_type_t`, and validate/fill `oltr_params_t`

#### 2.2 Transition that does not require a worker (one-time drawing + `layer_animation` driving)

Applicable when the preparation phase can finish all drawing in one pass, and the runtime phase only needs `layer_animation_*` to control alpha/coordinates without per-frame redraw. Existing references: `overlay_transition_fade()` and `overlay_transition_move()`.

Implementation steps:

- In the entry function, first set `overlay->request_abort = 0`
- Set the initial layer state (for example alpha/coordinates)
- Use `drm_warpper_dequeue_free_item()` to get a free buffer
- Complete one-time drawing on that buffer (background color, optional image, etc.)
- Submit it with `drm_warpper_enqueue_display_item()`
- Start the animation with `layer_animation_*`
- If you need a callback at a "midpoint" (for example, mount video after the screen is covered), use a one-shot `prts_timer_create(..., count=1, middle_cb)`

Notes:

- This model usually does **not use** `overlay->overlay_timer_handle` (for example, the middle timer in fade/move uses a local handle), so it does not depend on the wait logic in `overlay_abort()`.

#### 2.3 Transition that requires a worker (timer only schedules, worker performs per-frame drawing)

Applicable when per-frame drawing is required. Existing reference: `overlay_transition_swipe()`.

Key rules:

- **Resource allocation timing**: allocate or prepare worker resources in the "effect entry function" (not the timer callback), such as `malloc`, Bezier table precomputation, double-buffer clear/mount. Do not do time-consuming preparation in the timer callback.
- **Driving model**:
  - `prts_timer_create(&overlay->overlay_timer_handle, ..., cb=timer_cb, userdata=data)`
  - inside `timer_cb`, only call `overlay_worker_schedule(overlay, worker_func, data)`
  - actual drawing and state machine advancement happen inside `worker_func`
- **Where to handle `request_abort`**: you must check `overlay->request_abort` at the beginning of `worker_func`; if true, clean up and return immediately.
- **Where to clean up resources**: cleanup must happen inside the worker (see the reason below), and after cleanup `overlay->overlay_timer_handle` must be reset to `0`.

### 3) How to add a new OpInfo

#### 3.1 Implementation and configuration locations

- Implementation files:
  - `src/overlay/opinfo.h`: add types / parameters / function declarations
  - `src/overlay/opinfo.c`: implement drawing and animation driving
- Configuration mapping:
  - `src/prts/operators.c`: map the string in `overlay.type` to `opinfo_type_t`, and validate/fill `olopinfo_params_t`

#### 3.2 OpInfo that does not require a worker (one-time drawing + `layer_animation` driving)

Existing reference: `overlay_opinfo_show_image()`.

Suggested implementation order:

- In the entry function, first set `overlay->request_abort = 0`
- Use `drm_warpper_dequeue_free_item()` to get a free buffer
- Draw once (for example bitmap blit / clear screen)
- Submit with `drm_warpper_enqueue_display_item()`
- Use `layer_animation_*` for enter/exit animations (for example move)

#### 3.3 OpInfo that requires a worker (per-frame / partial updates)

Existing reference: `overlay_opinfo_show_arknights()`.

Suggested implementation order:

- **Prepare resources in the entry function (not the timer callback)**
  - initialize double-buffer templates (for example, draw the static background into both buffers first)
  - initialize worker data (state machine parameters, precomputed tables, etc.; if heap memory is needed, `malloc` here)
  - set `overlay->request_abort = 0`
- Create the timer: `prts_timer_create(&overlay->overlay_timer_handle, ..., cb=timer_cb, userdata=&data)`
  - inside `timer_cb`, only schedule
- `worker_func`:
  - check `overlay->request_abort` at the beginning; if true, clean up and return
  - advance the state machine (a practical order is "state transition -> draw -> swap buffer -> enqueue")
  - perform cleanup when the normal completion condition is met
- **Cleanup (inside the worker)**
  - `prts_timer_cancel(overlay->overlay_timer_handle)`
  - free heap resources used by the worker, if any
  - `overlay->overlay_timer_handle = 0`

### 4) Design considerations of the current Overlay programming model

#### 4.1 Why must resources be cleaned up in the worker?

Although the timer provides an `is_last` field to help with release handling, the timer callback thread and the overlay worker thread run concurrently. If you free data in the timer callback (or another thread) while the worker is still accessing it, you will get **UAF (Use-After-Free)**.

#### 4.2 You must ensure `overlay_timer_handle` can return to zero (otherwise stop will hang)

As long as your effect creates `overlay->overlay_timer_handle`:

- **Normal completion path**: when the final frame or end condition is reached, cancel the timer, clean up resources, and set `overlay_timer_handle = 0` inside the worker
- **Abort path**: when `request_abort` is detected, cancel the timer, clean up resources, and set `overlay_timer_handle = 0` inside the worker

Otherwise `overlay_abort()` will keep blocking and waiting by polling `overlay_timer_handle`.

## Development example

`arknights_overlay_worker` in [opinfo.c](../src/overlay/opinfo.c) is a development example.
