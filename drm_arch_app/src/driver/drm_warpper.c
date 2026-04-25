#include <drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <sys/mman.h>
#include <unistd.h>

#include "driver/drm_warpper.h"
#include "utils/log.h"
#include "driver/srgn_drm.h"
#include "config.h"
#include "utils/spsc_queue.h"

static inline int DRM_IOCTL(int fd, unsigned long cmd, void *arg) {
  int ret = drmIoctl(fd, cmd, arg);
  return ret < 0 ? -errno : ret;
}

static uint64_t drm_warpper_now_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)(ts.tv_nsec / 1000ULL);
}

static void drm_warpper_emit_boot_mark(const char *stage) {
    int fd;

    fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        return;
    }

    dprintf(fd, "<6>epass-boot: %s\n", stage);
    close(fd);
}

void drm_warpper_request_gui_ready(drm_warpper_t *drm_warpper) {
    atomic_store(&drm_warpper->ui_ready_request_pending, 1);
}

int drm_warpper_mark_gui_ready(drm_warpper_t *drm_warpper) {
    int fd;

    if (atomic_exchange(&drm_warpper->ui_ready_signal_sent, 1) != 0) {
        return 0;
    }

    if (mkdir("/run/epass", 0755) < 0 && errno != EEXIST) {
        log_error("failed to create /run/epass: %s", strerror(errno));
        atomic_store(&drm_warpper->ui_ready_signal_sent, 0);
        return -1;
    }

    fd = open("/run/epass/gui-alive", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        log_error("failed to create /run/epass/gui-alive: %s", strerror(errno));
        atomic_store(&drm_warpper->ui_ready_signal_sent, 0);
        return -1;
    }

    if (write(fd, "ready\n", 6) < 0) {
        log_error("failed to write /run/epass/gui-alive: %s", strerror(errno));
        close(fd);
        atomic_store(&drm_warpper->ui_ready_signal_sent, 0);
        return -1;
    }

    if (fsync(fd) < 0) {
        log_error("failed to fsync /run/epass/gui-alive: %s", strerror(errno));
    }

    close(fd);
    log_info("ui:ready-signal-created");
    drm_warpper_emit_boot_mark("ui:ready-signal-created");
    return 0;
}

static void drm_warpper_wait_for_vsync(drm_warpper_t *drm_warpper){
    drm_warpper->blank.request.type = DRM_VBLANK_RELATIVE;
    drm_warpper->blank.request.sequence = 1;
    // log_info("wait for vsync");
    if (drmWaitVBlank(drm_warpper->fd, (drmVBlankPtr) &drm_warpper->blank)) {
      log_error("drmWaitVBlank failed");
    }
    // log_info("vsync done");
}

static int drm_warpper_recycle_curr_item(drm_warpper_t *drm_warpper, int layer_id, drm_warpper_queue_item_t *item)
{
    layer_t *layer;
    int ret;

    if (drm_warpper == NULL || item == NULL) {
        return 0;
    }

    layer = &drm_warpper->layer[layer_id];
    while (atomic_load(&drm_warpper->thread_running)) {
        ret = spsc_bq_try_push(&layer->free_queue, item);
        if (ret == 0) {
            return 0;
        }
        if (ret == EPIPE) {
            return ret;
        }
        if (ret != EAGAIN) {
            log_warn("failed to recycle layer %d current item: %d", layer_id, ret);
            return ret;
        }
        usleep(1000);
    }

    log_warn("dropping layer %d recycled item during shutdown", layer_id);
    return EAGAIN;
}

// 内核那边会缓存用户地址到物理地址，以便快速挂。
// 每次启动前都需要reset cache，避免上次启动的残留。
void drm_warpper_reset_cache_ioctl(drm_warpper_t *drm_warpper){
    int ret;
    ret = drmIoctl(drm_warpper->fd, DRM_IOCTL_SRGN_RESET_FB_CACHE, NULL);
    if(ret < 0){
        log_error("drm_warpper_reset_cache_ioctl failed %s(%d)", strerror(errno), errno);
    }
}

// static void drm_warpper_switch_buffer_ioctl(drm_warpper_t *drm_warpper,int layer_id,int type,uint8_t *ch0_addr,uint8_t *ch1_addr,uint8_t *ch2_addr){
//     int ret;
//     struct drm_srgn_mount_fb srgn_mount_fb;

//     srgn_mount_fb.layer_id = layer_id;
//     srgn_mount_fb.type = type;
//     srgn_mount_fb.ch0_addr = (uint32_t)ch0_addr;
//     srgn_mount_fb.ch1_addr = (uint32_t)ch1_addr;
//     srgn_mount_fb.ch2_addr = (uint32_t)ch2_addr;
    
//     ret = drmIoctl(drm_warpper->fd, DRM_IOCTL_SRGN_MOUNT_FB, &srgn_mount_fb);
//     if(ret < 0){
//         log_error("drm_warpper_switch_buffer_ioctl failed %s(%d)", strerror(errno), errno);
//     }
// }

static void* drm_warpper_display_thread(void *arg){
    drm_warpper_t *drm_warpper = (drm_warpper_t *)arg;
    struct drm_srgn_atomic_commit_data commits[12] = {0};
    struct drm_srgn_atomic_commit commit_req = {
        .size = 0,
        .data = (uint32_t)(uintptr_t)commits,
    };
    int ret;
    uint64_t stats_window_start_us = 0;
    uint32_t vsync_wakeups = 0;
    uint32_t atomic_commits = 0;
    uint32_t ui_commits = 0;
    uint32_t video_commits = 0;

    log_info("==> DRM_Warpper Display Thread Started!");
    stats_window_start_us = drm_warpper_now_us();

    while(atomic_load(&drm_warpper->thread_running)){
        bool ui_commit_present = false;
        bool video_commit_present = false;
        uint64_t now_us;

        drm_warpper_wait_for_vsync(drm_warpper);
        vsync_wakeups++;
        // log_info("vsync");
        commit_req.size = 0;
        for(int i = 0; i < 4; i++){
            layer_t* layer = &drm_warpper->layer[i];
            if(layer->used){
                pthread_mutex_lock(&layer->lock);
                drm_warpper_queue_item_t* item;
                while(spsc_bq_try_pop(&layer->display_queue, (void**)&item) == 0){
                    // somthing is wait to be displayed.
                    // so, switch buffer using ioctl,and put current item to free queue.
                    // log_info("switch buffer on layer %d type %d", i, item->mount.type);
                    commits[commit_req.size].layer_id = i;
                    commits[commit_req.size].type = item->mount.type;
                    commits[commit_req.size].arg0 = item->mount.arg0;
                    commits[commit_req.size].arg1 = item->mount.arg1;
                    commits[commit_req.size].arg2 = item->mount.arg2;
                    commit_req.size++;
                    if(item->mount.type == DRM_SRGN_ATOMIC_COMMIT_MOUNT_FB_NORMAL 
                        || item->mount.type == DRM_SRGN_ATOMIC_COMMIT_MOUNT_FB_YUV){
                        if(i == DRM_WARPPER_LAYER_UI){
                            ui_commit_present = true;
                        }
                        if(i == DRM_WARPPER_LAYER_VIDEO){
                            video_commit_present = true;
                        }
                        if(layer->curr_item){
                            ret = drm_warpper_recycle_curr_item(drm_warpper, i, layer->curr_item);
                            if(ret != 0 && layer->curr_item->on_heap){
                                free(layer->curr_item);
                            }
                        }
                        layer->curr_item = item;
                        pthread_cond_broadcast(&layer->curr_item_cv);
                    }
                    else{
                        if(item->on_heap){
                            free(item);
                        }
                    }
                }
                pthread_mutex_unlock(&layer->lock);
            }
        }
        if(commit_req.size > 0){
            ret = drmIoctl(drm_warpper->fd, DRM_IOCTL_SRGN_ATOMIC_COMMIT, &commit_req);
            if(ret < 0){
                log_error("DRM_IOCTL_SRGN_ATOMIC_COMMIT failed %s(%d)", strerror(errno), errno);
            }
            else{
                atomic_commits++;
                if(ui_commit_present){
                    ui_commits++;
                }
                if(video_commit_present){
                    video_commits++;
                }

                if(ui_commit_present){
                    bool ui_commit_can_signal_ready =
                        atomic_exchange(&drm_warpper->ui_initial_commit_done, 1) != 0;

                    if(ui_commit_can_signal_ready
                        && atomic_exchange(&drm_warpper->ui_ready_request_pending, 0) != 0){
                        if(drm_warpper_mark_gui_ready(drm_warpper) == 0){
                            log_info("ui:first-flush-ready");
                        }
                        else{
                            atomic_store(&drm_warpper->ui_ready_request_pending, 1);
                        }
                    }
                }
            }
        }

        now_us = drm_warpper_now_us();
        if (now_us - stats_window_start_us >= 1000000ULL) {
            if (vsync_wakeups != 0 || atomic_commits != 0 || ui_commits != 0 || video_commits != 0) {
                log_info("drm:stats vsync_wakeups=%u atomic_commits=%u ui_commits=%u video_commits=%u",
                         vsync_wakeups,
                         atomic_commits,
                         ui_commits,
                         video_commits);
            }
            stats_window_start_us = now_us;
            vsync_wakeups = 0;
            atomic_commits = 0;
            ui_commits = 0;
            video_commits = 0;
        }
    }

    log_info("==> DRM_Warpper Display Thread Ended!");
    return NULL;
}

int drm_warpper_enqueue_display_item(drm_warpper_t *drm_warpper,int layer_id,drm_warpper_queue_item_t* item){
    layer_t* layer = &drm_warpper->layer[layer_id];
    return spsc_bq_push(&layer->display_queue, item);
}

int drm_warpper_dequeue_free_item(drm_warpper_t *drm_warpper,int layer_id,drm_warpper_queue_item_t** out_item){
    layer_t* layer = &drm_warpper->layer[layer_id];
    return spsc_bq_pop(&layer->free_queue, (void**)out_item);
}

int drm_warpper_try_dequeue_free_item(drm_warpper_t *drm_warpper,int layer_id,drm_warpper_queue_item_t** out_item){
    layer_t* layer = &drm_warpper->layer[layer_id];
    return spsc_bq_try_pop(&layer->free_queue, (void**)out_item);
}

int drm_warpper_set_layer_coord(drm_warpper_t *drm_warpper,int layer_id,int x,int y){
    drm_warpper_queue_item_t *item = malloc(sizeof(drm_warpper_queue_item_t));
    if(item == NULL){
        log_error("failed to allocate memory");
        return -1;
    }
    item->mount.layer_id = layer_id;
    item->mount.type = DRM_SRGN_ATOMIC_COMMIT_MOUNT_SET_COORD;
    item->mount.arg0 = (int16_t)y << 16 | (int16_t)x;
    item->mount.arg1 = 0;
    item->mount.arg2 = 0;
    item->userdata = NULL;
    item->on_heap = true;
#ifndef APP_RELEASE
    log_trace("drm coord y:%d,x:%d,reg:%x",y,x,item->mount.arg0);
#endif // APP_RELEASE
    return drm_warpper_enqueue_display_item(drm_warpper, layer_id, item);
}

int drm_warpper_set_layer_alpha(drm_warpper_t *drm_warpper,int layer_id,int alpha){
    drm_warpper_queue_item_t *item = malloc(sizeof(drm_warpper_queue_item_t));
    if(item == NULL){
        log_error("failed to allocate memory");
        return -1;
    }
    item->mount.layer_id = layer_id;
    item->mount.type = DRM_SRGN_ATOMIC_COMMIT_MOUNT_SET_ALPHA;
    item->mount.arg0 = alpha;
    item->mount.arg1 = 0;
    item->mount.arg2 = 0;
    item->userdata = NULL;
    item->on_heap = true;
    return drm_warpper_enqueue_display_item(drm_warpper, layer_id, item);
}

int drm_warpper_init(drm_warpper_t *drm_warpper){
    int ret;

    memset(drm_warpper, 0, sizeof(drm_warpper_t));
    drm_warpper->fd = -1;

    drm_warpper->fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (drm_warpper->fd < 0) {
        log_error("open /dev/dri/card0 failed");
        return -1;
    }

    ret = drmSetClientCap(drm_warpper->fd, DRM_CLIENT_CAP_ATOMIC, 1);
    if(ret) {
        log_error("No atomic modesetting support: %s", strerror(errno));
        close(drm_warpper->fd);
        return -1;
    }

    drm_warpper->res = drmModeGetResources(drm_warpper->fd);
    if (!drm_warpper->res || drm_warpper->res->count_crtcs == 0 || drm_warpper->res->count_connectors == 0) {
        log_error("drmModeGetResources failed or no CRTCs/connectors");
        close(drm_warpper->fd);
        return -1;
    }
    drm_warpper->crtc_id = drm_warpper->res->crtcs[0];
    drm_warpper->conn_id = drm_warpper->res->connectors[0];

    ret = drmSetClientCap(drm_warpper->fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
    if (ret) {
      log_error("failed to set client cap\n");
      drmModeFreeResources(drm_warpper->res);
      close(drm_warpper->fd);
      return -1;
    }
    drm_warpper->plane_res = drmModeGetPlaneResources(drm_warpper->fd);
    if (!drm_warpper->plane_res) {
        log_error("drmModeGetPlaneResources failed");
        drmModeFreeResources(drm_warpper->res);
        close(drm_warpper->fd);
        return -1;
    }
    log_info("Available Plane Count: %d", drm_warpper->plane_res->count_planes);

    drm_warpper->conn = drmModeGetConnector(drm_warpper->fd, drm_warpper->conn_id);
    if (!drm_warpper->conn) {
        log_error("drmModeGetConnector failed");
        drmModeFreePlaneResources(drm_warpper->plane_res);
        drmModeFreeResources(drm_warpper->res);
        close(drm_warpper->fd);
        return -1;
    }

    log_info("Connector Name: %s, %dx%d, Refresh Rate: %d",
        drm_warpper->conn->modes[0].name, drm_warpper->conn->modes[0].vdisplay, drm_warpper->conn->modes[0].hdisplay,
        drm_warpper->conn->modes[0].vrefresh);

    drm_warpper->blank.request.type = DRM_VBLANK_RELATIVE;
    drm_warpper->blank.request.sequence = 1;

    drm_warpper_reset_cache_ioctl(drm_warpper);

    atomic_store(&drm_warpper->thread_running, 1);
    if (pthread_create(&drm_warpper->display_thread, NULL, drm_warpper_display_thread, drm_warpper) != 0) {
        log_error("Failed to create display thread");
        drmModeFreeConnector(drm_warpper->conn);
        drmModeFreePlaneResources(drm_warpper->plane_res);
        drmModeFreeResources(drm_warpper->res);
        close(drm_warpper->fd);
        return -1;
    }
    drm_warpper->display_thread_started = true;
    return 0;
}

int drm_warpper_stop_display_thread(drm_warpper_t *drm_warpper){
    if(!drm_warpper->display_thread_started){
        atomic_store(&drm_warpper->thread_running, 0);
        return 0;
    }

    atomic_store(&drm_warpper->thread_running, 0);
    log_info("wait for display thread to finish");
    pthread_join(drm_warpper->display_thread, NULL);
    log_info("display thread finished");
    drm_warpper->display_thread_started = false;
    return 0;
}

int drm_warpper_destroy(drm_warpper_t *drm_warpper){
    drm_warpper_stop_display_thread(drm_warpper);

    for(int i = 0; i < 4; i++){
        drm_warpper_destroy_layer(drm_warpper, i);
    }

    if(drm_warpper->conn){
        drmModeFreeConnector(drm_warpper->conn);
        drm_warpper->conn = NULL;
    }
    if(drm_warpper->plane_res){
        drmModeFreePlaneResources(drm_warpper->plane_res);
        drm_warpper->plane_res = NULL;
    }
    if(drm_warpper->res){
        drmModeFreeResources(drm_warpper->res);
        drm_warpper->res = NULL;
    }
    if(drm_warpper->fd >= 0){
        close(drm_warpper->fd);
        drm_warpper->fd = -1;
    }
    return 0;
}

static int drm_warpper_create_buffer_object(int fd,buffer_object_t* bo,int width,int height,drm_warpper_layer_mode_t mode){
    struct drm_mode_create_dumb creq;
    struct drm_mode_map_dumb mreq;
    uint32_t handles[4], pitches[4], offsets[4];
    uint64_t modifiers[4];
    int ret;
 
    memset(&creq, 0, sizeof(struct drm_mode_create_dumb));
    if(mode == DRM_WARPPER_LAYER_MODE_MB32_NV12){
        creq.width = width;
        creq.height = height * 3 / 2;
        creq.bpp = 8;
    }
    else if(mode == DRM_WARPPER_LAYER_MODE_RGB565){
        creq.width = width;
        creq.height = height;
        creq.bpp = 16;
    }
    else if(mode == DRM_WARPPER_LAYER_MODE_ARGB8888){
        creq.width = width;
        creq.height = height;
        creq.bpp = 32;
    }
    else{
        log_error("invalid layer mode");
        return -1;
    }


    ret = drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq);
    if (ret < 0) {
      log_error("cannot create dumb buffer (%d): %m", errno);
      return -errno;
    }
  
    memset(&offsets, 0, sizeof(offsets));
    memset(&handles, 0, sizeof(handles));
    memset(&pitches, 0, sizeof(pitches));
    memset(&modifiers, 0, sizeof(modifiers));

    if(mode == DRM_WARPPER_LAYER_MODE_MB32_NV12){
        offsets[0] = 0;
        handles[0] = creq.handle;
        pitches[0] = creq.pitch;
        modifiers[0] = DRM_FORMAT_MOD_ALLWINNER_TILED;
      
        offsets[1] = creq.pitch * height;
        handles[1] = creq.handle;
        pitches[1] = creq.pitch;
        modifiers[1] = DRM_FORMAT_MOD_ALLWINNER_TILED;
    }
    else{
        offsets[0] = 0;
        handles[0] = creq.handle;
        pitches[0] = creq.pitch;
        modifiers[0] = 0;
    }

    if(mode == DRM_WARPPER_LAYER_MODE_MB32_NV12){
        ret = drmModeAddFB2WithModifiers(fd, width, height, DRM_FORMAT_NV12, handles,
                                     pitches, offsets, modifiers, &bo->fb_id,
                                     DRM_MODE_FB_MODIFIERS);
    }
    else if(mode == DRM_WARPPER_LAYER_MODE_RGB565){
        ret = drmModeAddFB2(fd, width, height, DRM_FORMAT_RGB565, handles, pitches, offsets,&bo->fb_id, 0);
    }
    else if(mode == DRM_WARPPER_LAYER_MODE_ARGB8888){
        ret = drmModeAddFB2(fd, width, height, DRM_FORMAT_ARGB8888, handles, pitches, offsets,&bo->fb_id, 0);
    }
  
    if (ret) {
      log_error("drmModeAddFB2 return err %d", ret);
      return -1;
    }
    
    /* prepare buffer for memory mapping */
    memset(&mreq, 0, sizeof(mreq));
    mreq.handle = creq.handle;
    ret = drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq);
    if (ret) {
      log_error("1st cannot map dumb buffer (%d): %m\n", errno);
      return -1;
    }
    /* perform actual memory mapping */
    bo->vaddr = mmap(0, creq.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, mreq.offset);   

    if (bo->vaddr == MAP_FAILED) {
        log_error("2nd cannot mmap dumb buffer (%d): %m\n", errno);
      return -1;
    }

    bo->pitch = creq.pitch;
    bo->handle = creq.handle;
    bo->size = creq.size;

    return 0;
}


int drm_warpper_init_layer(drm_warpper_t *drm_warpper,int layer_id,int width,int height,drm_warpper_layer_mode_t mode){

    layer_t* layer = &drm_warpper->layer[layer_id];
    int ret;

    ret = pthread_mutex_init(&layer->lock, NULL);
    if(ret != 0){
        log_error("failed to initialize layer lock");
        return -1;
    }
    layer->lock_inited = true;
    ret = pthread_cond_init(&layer->curr_item_cv, NULL);
    if(ret != 0){
        log_error("failed to initialize current item condition");
        pthread_mutex_destroy(&layer->lock);
        layer->lock_inited = false;
        return -1;
    }
    layer->curr_item_cv_inited = true;

    ret = spsc_bq_init(&layer->display_queue, 16);
    if(ret < 0){
        log_error("failed to initialize display queue");
        pthread_cond_destroy(&layer->curr_item_cv);
        layer->curr_item_cv_inited = false;
        pthread_mutex_destroy(&layer->lock);
        layer->lock_inited = false;
        return -1;
    }
    ret = spsc_bq_init(&layer->free_queue, 2);
    if(ret < 0){
        log_error("failed to initialize free queue");
        spsc_bq_destroy(&layer->display_queue);
        pthread_cond_destroy(&layer->curr_item_cv);
        layer->curr_item_cv_inited = false;
        pthread_mutex_destroy(&layer->lock);
        layer->lock_inited = false;
        return -1;
    }

    layer->mode = mode;
    layer->used = true;
    layer->width = width;
    layer->height = height;

    layer->curr_item = NULL;

    return 0;
}

int drm_warpper_destroy_layer(drm_warpper_t *drm_warpper,int layer_id){
    layer_t* layer = &drm_warpper->layer[layer_id];
    drm_warpper_queue_item_t* item;
    if(!layer->used){
        return 0;
    }
    pthread_mutex_lock(&layer->lock);
    while(spsc_bq_try_pop(&layer->display_queue, (void**)&item) == 0){
        if(item && item->on_heap){
            free(item);
        }
    }
    while(spsc_bq_try_pop(&layer->free_queue, (void**)&item) == 0){
        if(item && item->on_heap){
            free(item);
        }
    }
    if(layer->curr_item && layer->curr_item->on_heap){
        free(layer->curr_item);
    }
    layer->curr_item = NULL;
    pthread_cond_broadcast(&layer->curr_item_cv);
    pthread_mutex_unlock(&layer->lock);
    spsc_bq_destroy(&layer->display_queue);
    spsc_bq_destroy(&layer->free_queue);
    pthread_cond_destroy(&layer->curr_item_cv);
    layer->curr_item_cv_inited = false;
    pthread_mutex_destroy(&layer->lock);
    layer->lock_inited = false;
    layer->used = false;
    return 0;
}

int drm_warpper_allocate_buffer(drm_warpper_t *drm_warpper,int layer_id,buffer_object_t *buf){
    int ret;
    layer_t* layer = &drm_warpper->layer[layer_id];
    buf->width = layer->width;
    buf->height = layer->height;
    ret = drm_warpper_create_buffer_object(drm_warpper->fd, buf, layer->width, layer->height, layer->mode);
    if(ret < 0){
        log_error("failed to allocate buffer");
        return -1;
    }
    return 0;
}

int drm_warpper_free_buffer(drm_warpper_t *drm_warpper,int layer_id,buffer_object_t *buf){
    struct drm_mode_destroy_dumb destroy;

    (void)layer_id;

    if(buf == NULL || buf->vaddr == NULL){
        return 0;
    }

    memset(&destroy, 0, sizeof(struct drm_mode_destroy_dumb));

    if(drm_warpper->fd >= 0 && buf->fb_id != 0){
        drmModeRmFB(drm_warpper->fd, buf->fb_id);
    }
    if(buf->size != 0){
        munmap(buf->vaddr, buf->size);
    }

    destroy.handle = buf->handle;
    if(drm_warpper->fd >= 0 && destroy.handle != 0){
        drmIoctl(drm_warpper->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
    }

    memset(buf, 0, sizeof(*buf));

    return 0;
}



int drm_warpper_mount_layer(drm_warpper_t *drm_warpper,int layer_id,int x,int y,buffer_object_t *buf){
    int ret;
    ret = drmModeSetPlane(drm_warpper->fd, 
        drm_warpper->plane_res->planes[layer_id], 
        drm_warpper->crtc_id, 
        buf->fb_id, 
        0,
        x, y, 
        buf->width, buf->height, 
        0, 0,
        (buf->width) << 16, (buf->height) << 16
    );
    if (ret < 0){
        log_error("drmModeSetPlane err %d", ret);
        return -1;
    }
    return 0;
}
