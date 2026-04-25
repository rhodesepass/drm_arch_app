#include "mediaplayer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <CdxParser.h>
#include <vdecoder.h>
#include <memoryAdapter.h>


#include "driver/drm_warpper.h"
#include "utils/log.h"
#include "cdx_config.h"
#include "config.h"
#include "driver/srgn_drm.h"
#include "utils/misc.h"

/* external cedarx plugin entry */
extern void AddVDPlugin(void);
extern buffer_object_t g_video_buf;

#define mp_get_now_us get_now_us

#define MP_PARSER_POLL_SLEEP_US 1000
#define MP_BUFFER_BACKPRESSURE_SLEEP_US (50 * 1000)
#define MP_FRAME_RATE_FALLBACK_MILLI_FPS 30000

static void mp_log_input_file_state(const char *file)
{
    struct stat st;

    if (file == NULL || file[0] == '\0') {
        log_error("mediaplayer:file path is empty");
        return;
    }

    if (stat(file, &st) != 0) {
        log_error("mediaplayer:file stat failed path=%s errno=%d (%s)",
                  file, errno, strerror(errno));
        return;
    }

    if (access(file, R_OK) != 0) {
        log_error("mediaplayer:file is not readable path=%s errno=%d (%s)",
                  file, errno, strerror(errno));
        return;
    }

    if (!S_ISREG(st.st_mode)) {
        log_warn("mediaplayer:file is not a regular file path=%s mode=%o",
                 file, st.st_mode & 07777);
    }

    log_info("mediaplayer:file path=%s size=%lld mode=%o",
             file,
             (long long)st.st_size,
             st.st_mode & 07777);
}

static int mp_validate_media_info(mediaplayer_t *mp, const char *file)
{
    struct CdxProgramS *program;

    if (mp->media_info.programNum <= 0) {
        log_error("mediaplayer: no program found for %s", file);
        return -1;
    }

    if (mp->media_info.programIndex < 0 ||
        mp->media_info.programIndex >= mp->media_info.programNum) {
        log_error("mediaplayer: invalid program index %d/%d for %s",
                  mp->media_info.programIndex,
                  mp->media_info.programNum,
                  file);
        return -1;
    }

    program = &mp->media_info.program[mp->media_info.programIndex];
    if (program->videoNum <= 0) {
        log_error("mediaplayer: no video stream found for %s (audio=%d)",
                  file, program->audioNum);
        return -1;
    }

    log_info("mediaplayer:media program_num=%d program_index=%d video_num=%d audio_num=%d",
             mp->media_info.programNum,
             mp->media_info.programIndex,
             program->videoNum,
             program->audioNum);
    return 0;
}

static void mp_log_decoder_config(const char *file,
                                  const VideoStreamInfo *vInfo,
                                  const VConfig *vConfig)
{
    log_info("mediaplayer:stream file=%s codec=%d width=%d height=%d frame_rate=%d frame_duration=%d aspect=%d csd_len=%d",
             file,
             vInfo->eCodecFormat,
             vInfo->nWidth,
             vInfo->nHeight,
             vInfo->nFrameRate,
             vInfo->nFrameDuration,
             vInfo->nAspectRatio,
             vInfo->nCodecSpecificDataLen);
    log_info("mediaplayer:decoder-config vbv=%d hold_di=%d hold_display=%d hold_rotate=%d hold_smooth=%d pixel_format=%d",
             vConfig->nVbvBufferSize,
             vConfig->nDeInterlaceHoldingFrameBufferNum,
             vConfig->nDisplayHoldingFrameBufferNum,
             vConfig->nRotateHoldingFrameBufferNum,
             vConfig->nDecodeSmoothFrameBufferNum,
             vConfig->eOutputPixelFormat);
}

static long long mp_get_frame_interval_us(int frame_rate_milli_fps)
{
    if (frame_rate_milli_fps <= 0) {
        log_warn("invalid frame rate: %d, fallback to %d mFPS",
                 frame_rate_milli_fps, MP_FRAME_RATE_FALLBACK_MILLI_FPS);
        frame_rate_milli_fps = MP_FRAME_RATE_FALLBACK_MILLI_FPS;
    }

    return (1000LL * 1000LL * 1000LL) / frame_rate_milli_fps;
}

/* parser thread: read bitstream and feed decoder */
static void *mp_parser_thread(void *param)
{
    mediaplayer_t *mp = (mediaplayer_t *)param;
    CdxParserT *parser = mp->parser;
    VideoDecoder *decoder = mp->decoder;
    CdxPacketT packet;
    VideoStreamDataInfo dataInfo;
    int ret;
    int validSize;
    int requestSize;
    int streamNum;
    int trytime = 0;
    unsigned char *buf = NULL;

    buf = malloc(1024 * 1024);
    if (buf == NULL) {
        log_error("parser thread malloc err");
        goto parser_exit;
    }

    memset(&packet, 0, sizeof(packet));
    memset(&dataInfo, 0, sizeof(dataInfo));

    log_info("==> mp_parser Thread Started!");

    startagain:
    while (0 == CdxParserPrefetch(parser, &packet)) {
        usleep(MP_PARSER_POLL_SLEEP_US);

        pthread_rwlock_rdlock(&mp->thread.rwlock);
        int state = mp->thread.state;
        int requested_stop = mp->thread.requested_stop;
        pthread_rwlock_unlock(&mp->thread.rwlock);

        if (requested_stop || (state & (MEDIAPLAYER_PARSER_ERROR |
                                        MEDIAPLAYER_DECODER_ERROR |
                                        MEDIAPLAYER_DECODE_FINISH))) {
            // log_info("parser:get exit flag");
            break;
        }

        validSize = VideoStreamBufferSize(decoder, 0) - VideoStreamDataSize(decoder, 0);
        requestSize = packet.length;

        if (trytime >= 2000) {
            log_error("try time too much");
            pthread_rwlock_wrlock(&mp->thread.rwlock);
            mp->thread.state |= MEDIAPLAYER_PARSER_ERROR;
            pthread_rwlock_unlock(&mp->thread.rwlock);
            break;
        }

        if (packet.type == CDX_MEDIA_VIDEO && ((packet.flags & MINOR_STREAM) == 0)) {
            if (requestSize > validSize) {
                usleep(MP_BUFFER_BACKPRESSURE_SLEEP_US);
                trytime++;
                continue;
            }

            ret = RequestVideoStreamBuffer(decoder, requestSize,
                                           (char **)&packet.buf, &packet.buflen,
                                           (char **)&packet.ringBuf, &packet.ringBufLen, 0);
            if (ret != 0) {
                log_debug("RequestVideoStreamBuffer err, request=%d, valid=%d",
                          requestSize, validSize);
                usleep(MP_BUFFER_BACKPRESSURE_SLEEP_US);
                continue;
            }

            if (packet.buflen + packet.ringBufLen < requestSize) {
                log_error("RequestVideoStreamBuffer err, not enough space");
                pthread_rwlock_wrlock(&mp->thread.rwlock);
                mp->thread.state |= MEDIAPLAYER_PARSER_ERROR;
                pthread_rwlock_unlock(&mp->thread.rwlock);
                break;
            }
        } else {
            packet.buf = buf;
            packet.buflen = packet.length;
            CdxParserRead(parser, &packet);
            continue;
        }

        trytime = 0;
        streamNum = VideoStreamFrameNum(decoder, 0);
        if (streamNum > 1024) {
            usleep(MP_BUFFER_BACKPRESSURE_SLEEP_US);
        }

        ret = CdxParserRead(parser, &packet);
        if (ret != 0) {
            log_error("cdxparser read err");
            pthread_rwlock_wrlock(&mp->thread.rwlock);
            mp->thread.state |= MEDIAPLAYER_PARSER_ERROR;
            pthread_rwlock_unlock(&mp->thread.rwlock);
            break;
        }

        memset(&dataInfo, 0, sizeof(dataInfo));
        dataInfo.pData        = packet.buf;
        dataInfo.nLength      = packet.length;
        dataInfo.nPts         = packet.pts;
        dataInfo.nPcr         = packet.pcr;
        dataInfo.bIsFirstPart = !!(packet.flags & FIRST_PART);
        dataInfo.bIsLastPart  = !!(packet.flags & LAST_PART);

        ret = SubmitVideoStreamData(decoder, &dataInfo, 0);
        if (ret != 0) {
            log_error("SubmitVideoStreamData err");
            pthread_rwlock_wrlock(&mp->thread.rwlock);
            mp->thread.state |= MEDIAPLAYER_PARSER_ERROR;
            pthread_rwlock_unlock(&mp->thread.rwlock);
            break;
        }
    }
    
    if(CdxParserGetStatus(parser) == PSR_EOS){
        log_debug("eos, start again!");
        CdxParserSeekTo(parser, 0, AW_SEEK_CLOSEST);  
        goto startagain;
    }

    pthread_rwlock_wrlock(&mp->thread.rwlock);
    mp->thread.end_of_stream = 1;
    mp->thread.state |= MEDIAPLAYER_PARSER_EXIT;
    pthread_rwlock_unlock(&mp->thread.rwlock);

parser_exit:
    if (buf) {
        free(buf);
    }
    log_info("==> mp_parser Thread Ended!");
    pthread_exit(NULL);
    return NULL;
}

/* decoder thread: decode and copy one frame to output buffer */
static void *mp_decoder_thread(void *param)
{
    mediaplayer_t *mp = (mediaplayer_t *)param;
    VideoDecoder *decoder = mp->decoder;
    int ret;
    int end_of_stream = 0;
    long long next_frame_time = 0;
    long long frame_interval_us;
    uint64_t stats_window_start_us = mp_get_now_us();
    uint32_t decoded_frames = 0;
    uint32_t late_frames = 0;
    uint32_t enqueue_failures = 0;


    frame_interval_us = mp_get_frame_interval_us(mp->framerate);
    next_frame_time = mp_get_now_us() + frame_interval_us;

    log_info("==> mp_decoder Thread Started!");
    log_info("==> target fps: %d", mp->framerate);

    while (1) {
        long long current_time = mp_get_now_us();
        if (current_time < next_frame_time) {
            usleep(next_frame_time - current_time);
            current_time = mp_get_now_us();
        }

        if (current_time > next_frame_time + 2 * frame_interval_us) {
            log_warn("can't keep up, delay: %lld us", current_time - next_frame_time);
            late_frames++;
            next_frame_time = current_time + frame_interval_us;
        }

        pthread_rwlock_rdlock(&mp->thread.rwlock);
        end_of_stream = mp->thread.end_of_stream;
        int state = mp->thread.state;
        int requested_stop = mp->thread.requested_stop;
        pthread_rwlock_unlock(&mp->thread.rwlock);

        if (requested_stop) {
            // log_info("req stop,exiting");
            break;
        }

        if (state & (MEDIAPLAYER_PARSER_ERROR | MEDIAPLAYER_DECODER_ERROR)) {
            log_error("err state,exiting");
            break;
        }

        // first try to dequeue free buffer from drm_warpper
        drm_warpper_queue_item_t* item;
        while(drm_warpper_try_dequeue_free_item(mp->drm_warpper, DRM_WARPPER_LAYER_VIDEO, &item) == 0){
            VideoPicture* pic = (VideoPicture*)item->userdata;
            // log_debug("dequeue");
            if(pic){
                ReturnPicture(decoder, pic);
            }
            free(item);
        }

        // long long start = mp_get_now_us();
        ret = DecodeVideoStream(decoder, end_of_stream, 0, 0, 0);
        // long long finish = mp_get_now_us();
        // log_debug("frame time: %lld us", finish - start);

        if (end_of_stream == 1 && ret == VDECODE_RESULT_NO_BITSTREAM) {
            log_info("data end!!!");
            break;
        }

        if (ret == VDECODE_RESULT_KEYFRAME_DECODED ||
            ret == VDECODE_RESULT_FRAME_DECODED) {
            int validNum = ValidPictureNum(decoder, 0);
            if (validNum > 0) {
                VideoPicture *picture = RequestPicture(decoder, 0);
                if (!picture) {
                    log_error("RequestPicture err");
                    continue;
                }

                if (picture->nWidth != MEDIAPLAYER_FRAME_WIDTH ||
                    picture->nHeight != MEDIAPLAYER_FRAME_HEIGHT) {
                    log_error("err size, expect %dx%d, actual %dx%d",
                              MEDIAPLAYER_FRAME_WIDTH, MEDIAPLAYER_FRAME_HEIGHT,
                              picture->nWidth, picture->nHeight);
                    ReturnPicture(decoder, picture);
                    pthread_rwlock_wrlock(&mp->thread.rwlock);
                    mp->thread.state |= MEDIAPLAYER_DECODER_ERROR;
                    pthread_rwlock_unlock(&mp->thread.rwlock);
                    break;
                }

                // int dataLen = picture->nWidth * picture->nHeight * 3 / 2;
                // memcpy(mp->output_buf, picture->pData0,
                //        picture->nWidth * picture->nHeight);
                // memcpy(mp->output_buf + picture->nWidth * picture->nHeight,
                //        picture->pData1,
                //        picture->nWidth * picture->nHeight / 2);

                // ReturnPicture(decoder, picture);

                // 把拿到的picture直接交给drm ioctl挂上去(Zero Copy!)
                drm_warpper_queue_item_t* item_to_display = malloc(sizeof(drm_warpper_queue_item_t));
                if(item_to_display == NULL){
                    log_error("malloc err");
                    ReturnPicture(decoder, picture);
                    continue;
                }

                item_to_display->mount.type = DRM_SRGN_ATOMIC_COMMIT_MOUNT_FB_YUV;
                item_to_display->mount.arg0 = (uint32_t)picture->pData0;
                item_to_display->mount.arg1 = (uint32_t)picture->pData1;
                item_to_display->mount.arg2 = 0;
                item_to_display->userdata = (void*)picture;
                // this "on_heap" means that the item_to_display 
                // will be free by the drm_warpper, not by the mediaplayer.
                item_to_display->on_heap = false;
                if (drm_warpper_enqueue_display_item(mp->drm_warpper, DRM_WARPPER_LAYER_VIDEO, item_to_display) != 0) {
                    log_error("failed to enqueue decoded frame");
                    enqueue_failures++;
                    ReturnPicture(decoder, picture);
                    free(item_to_display);
                    pthread_rwlock_wrlock(&mp->thread.rwlock);
                    mp->thread.state |= MEDIAPLAYER_DECODER_ERROR;
                    pthread_rwlock_unlock(&mp->thread.rwlock);
                    break;
                }
                decoded_frames++;
                next_frame_time = next_frame_time + frame_interval_us;
            }
        }

        if (ret < 0) {
            log_error("DecodeVideoStream err: %d", ret);
            pthread_rwlock_wrlock(&mp->thread.rwlock);
            mp->thread.state |= MEDIAPLAYER_DECODER_ERROR;
            pthread_rwlock_unlock(&mp->thread.rwlock);
            break;
        }

        {
            uint64_t stats_now_us = (uint64_t)mp_get_now_us();

            if (stats_now_us - stats_window_start_us >= 1000000ULL) {
                if (decoded_frames != 0 || late_frames != 0 || enqueue_failures != 0) {
                    log_info("mediaplayer:stats decoded_frames=%u late_frames=%u enqueue_failures=%u",
                             decoded_frames,
                             late_frames,
                             enqueue_failures);
                }
                stats_window_start_us = stats_now_us;
                decoded_frames = 0;
                late_frames = 0;
                enqueue_failures = 0;
            }
        }
    }

    pthread_rwlock_wrlock(&mp->thread.rwlock);
    mp->thread.state |= MEDIAPLAYER_DECODER_EXIT;
    pthread_rwlock_unlock(&mp->thread.rwlock);

    log_info("==> mp_decoder Thread Ended!");
    pthread_exit(NULL);
    return NULL;
}

/* internal helper: release parser/decoder/memops */
static void mp_cleanup_internal(mediaplayer_t *mp)
{
    if (mp->parser) {
        CdxParserClose(mp->parser);
        mp->parser = NULL;
    }
    if (mp->decoder) {
        DestroyVideoDecoder(mp->decoder);
        mp->decoder = NULL;
    }
    if (mp->memops) {
        CdcMemClose(mp->memops);
        mp->memops = NULL;
    }
}

static void mp_reclaim_video_item(VideoDecoder *decoder, drm_warpper_queue_item_t *item)
{
    VideoPicture *picture;

    if (item == NULL) {
        return;
    }

    picture = (VideoPicture *)item->userdata;
    if (decoder != NULL && picture != NULL) {
        ReturnPicture(decoder, picture);
    }

    free(item);
}

static void mp_drain_video_layer_locked(mediaplayer_t *mp, VideoDecoder *decoder, bool include_curr_item)
{
    layer_t *layer = &mp->drm_warpper->layer[DRM_WARPPER_LAYER_VIDEO];
    drm_warpper_queue_item_t *item;

    while (spsc_bq_try_pop(&layer->display_queue, (void **)&item) == 0) {
        mp_reclaim_video_item(decoder, item);
    }

    while (spsc_bq_try_pop(&layer->free_queue, (void **)&item) == 0) {
        mp_reclaim_video_item(decoder, item);
    }

    if (include_curr_item && layer->curr_item != NULL) {
        mp_reclaim_video_item(decoder, layer->curr_item);
        layer->curr_item = NULL;
    }
}

static int mp_wait_for_item_to_be_current(mediaplayer_t *mp, drm_warpper_queue_item_t *expected_item, int timeout_ms)
{
    layer_t *layer = &mp->drm_warpper->layer[DRM_WARPPER_LAYER_VIDEO];
    struct timespec deadline;

    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec += 1;
        deadline.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&layer->lock);
    while (layer->curr_item != expected_item) {
        int ret = pthread_cond_timedwait(&layer->curr_item_cv, &layer->lock, &deadline);
        if (ret == ETIMEDOUT) {
            pthread_mutex_unlock(&layer->lock);
            return -1;
        }
    }
    pthread_mutex_unlock(&layer->lock);
    return 0;
}

static drm_warpper_queue_item_t *mp_create_black_frame_item(void)
{
    drm_warpper_queue_item_t* item;

    item = malloc(sizeof(*item));
    if(item == NULL){
        log_error("malloc err");
        return NULL;
    }

    item->mount.type = DRM_SRGN_ATOMIC_COMMIT_MOUNT_FB_YUV;
    item->mount.arg0 = (uint32_t)g_video_buf.vaddr;
    item->mount.arg1 = (uint32_t)g_video_buf.vaddr + g_video_buf.width * g_video_buf.height;
    item->mount.arg2 = 0;
    item->userdata = NULL;
    item->on_heap = false;
    return item;
}

static int mediaplayer_play_video_locked(mediaplayer_t *mp, const char *file);
static int mediaplayer_stop_locked(mediaplayer_t *mp);
static int mediaplayer_start_locked(mediaplayer_t *mp);

int mediaplayer_init(mediaplayer_t *mp,drm_warpper_t *drm_warpper)
{

    memset(mp, 0, sizeof(*mp));

    pthread_mutex_init(&mp->parser_mutex, NULL);
    pthread_mutex_init(&mp->state_mutex, NULL);
    pthread_rwlock_init(&mp->thread.rwlock, NULL);
    mp->thread.end_of_stream = 0;
    mp->thread.state = 0;
    mp->thread.requested_stop = 0;
    atomic_store(&mp->running, 0);
    memset(mp->video_path, 0, sizeof(mp->video_path));

    mp->drm_warpper = drm_warpper;

    AddVDPlugin();

    log_info("==> mp Initalized!");
    return 0;
}

int mediaplayer_destroy(mediaplayer_t *mp)
{
    layer_t *layer;

    if (!mp) {
        return -1;
    }

    pthread_mutex_lock(&mp->state_mutex);
    /* ensure stopped */
    mediaplayer_stop_locked(mp);

    layer = &mp->drm_warpper->layer[DRM_WARPPER_LAYER_VIDEO];
    if (layer->lock_inited) {
        pthread_mutex_lock(&layer->lock);
        mp_drain_video_layer_locked(mp, NULL, true);
        pthread_mutex_unlock(&layer->lock);
    }

    mp_cleanup_internal(mp);
    pthread_mutex_unlock(&mp->state_mutex);

    pthread_rwlock_destroy(&mp->thread.rwlock);
    pthread_mutex_destroy(&mp->state_mutex);
    pthread_mutex_destroy(&mp->parser_mutex);

    return 0;
}

static int mediaplayer_play_video_locked(mediaplayer_t *mp, const char *file)
{
    if (!mp || !file) {
        log_error("invalid params");
        return -1;
    }

    if (atomic_load(&mp->running)) {
        log_error("mediaplayer is running");
        return -1;
    }

    // 每次开始之前都需要reset cache
    drm_warpper_reset_cache_ioctl(mp->drm_warpper);

    memset(mp->input_uri, 0, sizeof(mp->input_uri));
    snprintf(mp->input_uri, sizeof(mp->input_uri), "file://%s", file);
    mp_log_input_file_state(file);
    log_info("mediaplayer:start path=%s uri=%s", file, mp->input_uri);

    mp->memops = MemAdapterGetOpsS();
    if (!mp->memops) {
        log_error("MemAdapterGetOpsS err");
        return -1;
    }
    CdcMemOpen(mp->memops);

    memset(&mp->source, 0, sizeof(CdxDataSourceT));
    memset(&mp->media_info, 0, sizeof(CdxMediaInfoT));

    mp->source.uri = mp->input_uri;

    int force_exit = 0;
    CdxStreamT *stream = NULL;
    int ret = CdxParserPrepare(&mp->source, 0, &mp->parser_mutex,
                               &force_exit, &mp->parser, &stream, NULL, NULL);
    if (ret < 0 || !mp->parser) {
        log_error("CdxParserPrepare err");
        mp_cleanup_internal(mp);
        return -1;
    }

    ret = CdxParserGetMediaInfo(mp->parser, &mp->media_info);
    if (ret != 0) {
        log_error("CdxParserGetMediaInfo err");
        mp_cleanup_internal(mp);
        return -1;
    }
    if (mp_validate_media_info(mp, file) != 0) {
        mp_cleanup_internal(mp);
        return -1;
    }

    mp->decoder = CreateVideoDecoder();
    if (!mp->decoder) {
        log_error("CreateVideoDecoder err");
        mp_cleanup_internal(mp);
        return -1;
    }

    VConfig vConfig;
    VideoStreamInfo vInfo;
    memset(&vConfig, 0, sizeof(VConfig));
    memset(&vInfo, 0, sizeof(VideoStreamInfo));

    struct CdxProgramS *program =
        &mp->media_info.program[mp->media_info.programIndex];

    /* only use first video stream */
    vInfo.eCodecFormat          = program->video[0].eCodecFormat;
    vInfo.nWidth                = program->video[0].nWidth;
    vInfo.nHeight               = program->video[0].nHeight;
    vInfo.nFrameRate            = program->video[0].nFrameRate;
    vInfo.nFrameDuration        = program->video[0].nFrameDuration;
    vInfo.nAspectRatio          = program->video[0].nAspectRatio;
    vInfo.bIs3DStream           = program->video[0].bIs3DStream;
    vInfo.nCodecSpecificDataLen = program->video[0].nCodecSpecificDataLen;
    vInfo.pCodecSpecificData    = program->video[0].pCodecSpecificData;

    mp->framerate = vInfo.nFrameRate;

    vConfig.eOutputPixelFormat  = PIXEL_FORMAT_YUV_MB32_420;
    vConfig.nDeInterlaceHoldingFrameBufferNum = BUF_CNT_4_DI;
    vConfig.nDisplayHoldingFrameBufferNum = BUF_CNT_4_LIST;
    vConfig.nRotateHoldingFrameBufferNum = BUF_CNT_4_ROTATE;
    vConfig.nDecodeSmoothFrameBufferNum = BUF_CNT_4_SMOOTH;
    vConfig.memops = mp->memops;
    vConfig.nVbvBufferSize = VBVBUFFERSIZE;
    mp_log_decoder_config(file, &vInfo, &vConfig);

    ret = InitializeVideoDecoder(mp->decoder, &vInfo, &vConfig);
    if (ret != 0) {
        log_error("InitializeVideoDecoder err");
        mp_cleanup_internal(mp);
        return -1;
    }

    /* reset thread flags */
    pthread_rwlock_wrlock(&mp->thread.rwlock);
    mp->thread.end_of_stream = 0;
    mp->thread.state = 0;
    mp->thread.requested_stop = 0;
    pthread_rwlock_unlock(&mp->thread.rwlock);

    atomic_store(&mp->running, 1);

    if (pthread_create(&mp->parser_thread, NULL, mp_parser_thread, mp) != 0) {
        log_error("parser create err");
        atomic_store(&mp->running, 0);
        mp_cleanup_internal(mp);
        return -1;
    }

    if (pthread_create(&mp->decoder_thread, NULL, mp_decoder_thread, mp) != 0) {
        log_error("decoder create err");
        pthread_rwlock_wrlock(&mp->thread.rwlock);
        mp->thread.requested_stop = 1;
        pthread_rwlock_unlock(&mp->thread.rwlock);
        pthread_join(mp->parser_thread, NULL);
        atomic_store(&mp->running, 0);
        mp_cleanup_internal(mp);
        return -1;
    }

    // /* wait for both threads to finish */
    // pthread_join(mp->parser_thread, NULL);
    // pthread_join(mp->decoder_thread, NULL);
    // mp->running = 0;

    // /* check final state */
    // pthread_rwlock_rdlock(&mp->thread.rwlock);
    // int final_state = mp->thread.state;
    // pthread_rwlock_unlock(&mp->thread.rwlock);

    // mp_cleanup_internal(mp);

    // if (final_state & (MEDIAPLAYER_PARSER_ERROR | MEDIAPLAYER_DECODER_ERROR)) {
    //     log_error("play failed, err state");
    //     return -1;
    // }
    // if (!(final_state & MEDIAPLAYER_DECODE_FINISH)) {
    //     log_error("decode failed, no frame");
    //     return -1;
    // }

    return 0;
}

int mediaplayer_play_video(mediaplayer_t *mp, const char *file)
{
    int ret;

    if (!mp) {
        return -1;
    }

    pthread_mutex_lock(&mp->state_mutex);
    ret = mediaplayer_play_video_locked(mp, file);
    pthread_mutex_unlock(&mp->state_mutex);
    return ret;
}

static int mediaplayer_stop_locked(mediaplayer_t *mp)
{
    layer_t *layer;
    drm_warpper_queue_item_t *black_item;

    if (!mp) {
        return -1;
    }

    if (!atomic_load(&mp->running)) {
        return 0;
    }

    pthread_rwlock_wrlock(&mp->thread.rwlock);
    mp->thread.requested_stop = 1;
    pthread_rwlock_unlock(&mp->thread.rwlock);

    pthread_join(mp->parser_thread, NULL);
    pthread_join(mp->decoder_thread, NULL);

    layer = &mp->drm_warpper->layer[DRM_WARPPER_LAYER_VIDEO];
    black_item = mp_create_black_frame_item();
    if (black_item == NULL) {
        mp_cleanup_internal(mp);
        atomic_store(&mp->running, 0);
        return -1;
    }

    pthread_mutex_lock(&layer->lock);
    mp_drain_video_layer_locked(mp, mp->decoder, false);
    spsc_bq_push(&layer->display_queue, black_item);
    pthread_mutex_unlock(&layer->lock);

    if (mp_wait_for_item_to_be_current(mp, black_item, 500) != 0) {
        log_warn("mediaplayer_stop: timeout waiting for black frame commit, fallback to direct mount, path=%s",
                 mp->video_path);
        drm_warpper_mount_layer(mp->drm_warpper, DRM_WARPPER_LAYER_VIDEO, 0, 0, &g_video_buf);
        pthread_mutex_lock(&layer->lock);
        mp_drain_video_layer_locked(mp, mp->decoder, true);
        pthread_mutex_unlock(&layer->lock);
    } else {
        pthread_mutex_lock(&layer->lock);
        mp_drain_video_layer_locked(mp, mp->decoder, false);
        pthread_mutex_unlock(&layer->lock);
    }

    mp_cleanup_internal(mp);
    atomic_store(&mp->running, 0);

    return 0;
}

int mediaplayer_stop(mediaplayer_t *mp)
{
    int ret;

    if (!mp) {
        return -1;
    }

    pthread_mutex_lock(&mp->state_mutex);
    ret = mediaplayer_stop_locked(mp);
    pthread_mutex_unlock(&mp->state_mutex);
    return ret;
}

int mediaplayer_switch_video(mediaplayer_t *mp, const char *file)
{
    int ret;

    if (!mp || !file) {
        log_error("invalid params");
        return -1;
    }

    pthread_mutex_lock(&mp->state_mutex);
    ret = mediaplayer_stop_locked(mp);
    if (ret == 0) {
        ret = mediaplayer_play_video_locked(mp, file);
    }
    pthread_mutex_unlock(&mp->state_mutex);
    return ret;
}

int mediaplayer_set_video(mediaplayer_t *mp, const char *path)
{
    if (!mp || !path) {
        log_error("invalid params");
        return -1;
    }

    if (atomic_load(&mp->running)) {
        log_error("cannot set video while playing, stop first");
        return -1;
    }

    memset(mp->video_path, 0, sizeof(mp->video_path));
    snprintf(mp->video_path, sizeof(mp->video_path), "%s", path);
    log_info("video path set to: %s", mp->video_path);

    return 0;
}

static int mediaplayer_start_locked(mediaplayer_t *mp)
{
    if (!mp) {
        log_error("invalid paramst");
        return -1;
    }

    if (atomic_load(&mp->running)) {
        log_warn("mediaplayer already running");
        return 0;
    }

    if (strlen(mp->video_path) == 0) {
        log_error("no video path set");
        return -1;
    }

    memset(mp->input_uri, 0, sizeof(mp->input_uri));
    int written = snprintf(mp->input_uri, sizeof(mp->input_uri), "file://%s", mp->video_path);
    if ((size_t)written >= sizeof(mp->input_uri)) {
        log_error("snprintf err");
        return -1;
    }
    mp_log_input_file_state(mp->video_path);
    log_info("mediaplayer:start path=%s uri=%s", mp->video_path, mp->input_uri);

    mp->memops = MemAdapterGetOpsS();
    if (!mp->memops) {
        log_error("MemAdapterGetOpsS err");
        return -1;
    }
    CdcMemOpen(mp->memops);

    memset(&mp->source, 0, sizeof(CdxDataSourceT));
    memset(&mp->media_info, 0, sizeof(CdxMediaInfoT));

    mp->source.uri = mp->input_uri;

    int force_exit = 0;
    CdxStreamT *stream = NULL;
    int ret = CdxParserPrepare(&mp->source, 0, &mp->parser_mutex,
                               &force_exit, &mp->parser, &stream, NULL, NULL);
    if (ret < 0 || !mp->parser) {
        log_error("CdxParserPrepare err");
        mp_cleanup_internal(mp);
        return -1;
    }

    ret = CdxParserGetMediaInfo(mp->parser, &mp->media_info);
    if (ret != 0) {
        log_error("CdxParserGetMediaInfo err");
        mp_cleanup_internal(mp);
        return -1;
    }
    if (mp_validate_media_info(mp, mp->video_path) != 0) {
        mp_cleanup_internal(mp);
        return -1;
    }

    mp->decoder = CreateVideoDecoder();
    if (!mp->decoder) {
        log_error("CreateVideoDecoder err");
        mp_cleanup_internal(mp);
        return -1;
    }

    VConfig vConfig;
    VideoStreamInfo vInfo;
    memset(&vConfig, 0, sizeof(VConfig));
    memset(&vInfo, 0, sizeof(VideoStreamInfo));

    struct CdxProgramS *program =
        &mp->media_info.program[mp->media_info.programIndex];

    /* only use first video stream */
    vInfo.eCodecFormat          = program->video[0].eCodecFormat;
    vInfo.nWidth                = program->video[0].nWidth;
    vInfo.nHeight               = program->video[0].nHeight;
    vInfo.nFrameRate            = program->video[0].nFrameRate;
    vInfo.nFrameDuration        = program->video[0].nFrameDuration;
    vInfo.nAspectRatio          = program->video[0].nAspectRatio;
    vInfo.bIs3DStream           = program->video[0].bIs3DStream;
    vInfo.nCodecSpecificDataLen = program->video[0].nCodecSpecificDataLen;
    vInfo.pCodecSpecificData    = program->video[0].pCodecSpecificData;

    mp->framerate = vInfo.nFrameRate;

    vConfig.eOutputPixelFormat  = PIXEL_FORMAT_YUV_MB32_420;
    vConfig.nDeInterlaceHoldingFrameBufferNum = BUF_CNT_4_DI;
    vConfig.nDisplayHoldingFrameBufferNum = BUF_CNT_4_LIST;
    vConfig.nRotateHoldingFrameBufferNum = BUF_CNT_4_ROTATE;
    vConfig.nDecodeSmoothFrameBufferNum = BUF_CNT_4_SMOOTH;
    vConfig.memops = mp->memops;
    vConfig.nVbvBufferSize = VBVBUFFERSIZE;
    mp_log_decoder_config(mp->video_path, &vInfo, &vConfig);

    ret = InitializeVideoDecoder(mp->decoder, &vInfo, &vConfig);
    if (ret != 0) {
        log_error("InitializeVideoDecoder err");
        mp_cleanup_internal(mp);
        return -1;
    }

    /* reset thread flags */
    pthread_rwlock_wrlock(&mp->thread.rwlock);
    mp->thread.end_of_stream = 0;
    mp->thread.state = 0;
    mp->thread.requested_stop = 0;
    pthread_rwlock_unlock(&mp->thread.rwlock);

    atomic_store(&mp->running, 1);

    if (pthread_create(&mp->parser_thread, NULL, mp_parser_thread, mp) != 0) {
        log_error("parser create err");
        atomic_store(&mp->running, 0);
        mp_cleanup_internal(mp);
        return -1;
    }

    if (pthread_create(&mp->decoder_thread, NULL, mp_decoder_thread, mp) != 0) {
        log_error("decoder create err");
        pthread_rwlock_wrlock(&mp->thread.rwlock);
        mp->thread.requested_stop = 1;
        pthread_rwlock_unlock(&mp->thread.rwlock);
        pthread_join(mp->parser_thread, NULL);
        atomic_store(&mp->running, 0);
        mp_cleanup_internal(mp);
        return -1;
    }

    log_info("playback started");
    return 0;
}

int mediaplayer_start(mediaplayer_t *mp)
{
    int ret;

    if (!mp) {
        return -1;
    }

    pthread_mutex_lock(&mp->state_mutex);
    ret = mediaplayer_start_locked(mp);
    pthread_mutex_unlock(&mp->state_mutex);
    return ret;
}

mp_status_t mediaplayer_get_status(mediaplayer_t *mp)
{
    int state;

    if (!mp) {
        return MP_STATUS_ERROR;
    }

    if (!atomic_load(&mp->running)) {
        return MP_STATUS_STOPPED;
    }

    pthread_rwlock_rdlock(&mp->thread.rwlock);
    state = mp->thread.state;
    pthread_rwlock_unlock(&mp->thread.rwlock);

    if (state & (MEDIAPLAYER_PARSER_ERROR | MEDIAPLAYER_DECODER_ERROR)) {
        return MP_STATUS_ERROR;
    }

    return MP_STATUS_PLAYING;
}
