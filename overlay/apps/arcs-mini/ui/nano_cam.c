#include "nano_cam.h"
#include "nano_fx.h"
#include "nano_theme.h"
#include "nano_settings.h"

#include "FreeRTOS.h"
#include "task.h"

#include "lisa_camera.h"
#include "lisa_device.h"
#include "lisa_log.h"
#include "lisa_ui_invoke.h"
#include "sysheap.h"

#include "service_camera.h"
#include "service_photo.h"

#include <string.h>

#define TAG "nano.cam"

#define CAM_DROP_STALE 2
#define CAM_STACK      4096
#define CAM_PRIO       4
/* tiny_jpeg 的 TJEState 是栈上的（两张 huff 表 + qt + 1KB 输出缓冲，约 5KB），
 * 4096 的取流栈装不下，所以写盘另开一条一次性线程。 */
#define SAVE_STACK     8192
#define SAVE_PRIO      4
#define SNAP_BYTES     ((size_t)NANO_FX_IMG_W * NANO_FX_IMG_H * sizeof(uint16_t))

static lv_obj_t *s_canvas;
static lisa_device_t *s_dev;
static uint16_t *s_frame;
static uint16_t *s_snap;
static TaskHandle_t s_task;
static volatile bool s_stop_req;
static volatile bool s_exited = true;
static volatile bool s_snap_req;
static volatile bool s_saving;
static volatile int s_saved_result;
static volatile uint32_t s_saved_bytes;
static char s_saved_name[PHOTO_NAME_LEN];
static volatile uint32_t s_fps10;
static volatile uint32_t s_fps10_last;
static nano_fx_t s_fx = NANO_FX_POSTER;

static uint32_t cam_now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static void cam_inval(void *arg, uint32_t len)
{
    (void)arg;
    (void)len;
    if (s_canvas && !lv_obj_has_flag(s_canvas, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_invalidate(s_canvas);
    }
}

static void cam_save_task(void *arg)
{
    char name[PHOTO_NAME_LEN];
    uint32_t bytes = 0;
    int rc;

    (void)arg;
    name[0] = '\0';
    rc = service_photo_save(s_snap, NANO_FX_IMG_W, NANO_FX_IMG_H, nano_fx_save_quality(s_fx),
                            name, (int)sizeof(name), &bytes);
    s_saved_result = rc;
    s_saved_bytes = (rc == 0) ? bytes : 0;
    if (rc == 0) {
        strncpy(s_saved_name, name, sizeof(s_saved_name) - 1);
        s_saved_name[sizeof(s_saved_name) - 1] = '\0';
    } else {
        s_saved_name[0] = '\0';
    }
    LISA_LOGI(TAG, "落盘 rc=%d name=%s bytes=%u", rc, name, (unsigned)bytes);
    s_saving = false;
    vTaskDelete(NULL);
}

static void cam_task(void *arg)
{
    lisa_camera_fb_t *fb = NULL;
    uint32_t frames = 0;
    uint32_t t0;

    (void)arg;
    for (uint32_t i = 0; i < CAM_DROP_STALE; i++) {
        if (lisa_camera_capture(s_dev, &fb) == LISA_DEVICE_OK && fb) {
            lisa_camera_release_fb(s_dev, fb);
            fb = NULL;
        }
    }

    t0 = cam_now_ms();
    while (!s_stop_req) {
        if (lisa_camera_capture(s_dev, &fb) != LISA_DEVICE_OK || !fb || !fb->buf) {
            break;
        }
        nano_fx_apply((const uint16_t *)fb->buf, s_frame, fb->width, fb->height, NANO_FX_IMG_W,
                      NANO_FX_IMG_H);
        lisa_camera_release_fb(s_dev, fb);
        fb = NULL;

        /* 快照只在帧边界取，于是 UI 线程永远不必碰编码，
         * 写盘线程也不会读到半新半旧的 s_frame。 */
        if (s_snap_req && s_snap) {
            s_snap_req = false;
            memcpy(s_snap, s_frame, SNAP_BYTES);
            s_saving = true;
            if (xTaskCreate(cam_save_task, "nano.save", SAVE_STACK, NULL, SAVE_PRIO, NULL) !=
                pdPASS) {
                s_saving = false;
                s_saved_result = -1;
                LISA_LOGE(TAG, "写盘线程创建失败");
            }
        }

        frames++;
        lisa_ui_invoke_ui_delayed(cam_inval, NULL, 0, 0);

        uint32_t dt = cam_now_ms() - t0;
        if (dt >= 1000U) {
            s_fps10 = frames * 10000U / dt;
            s_fps10_last = s_fps10;
            frames = 0;
            t0 = cam_now_ms();
        }
    }

    if (fb) {
        lisa_camera_release_fb(s_dev, fb);
    }
    /* 停止时还悬着的快门就当没按过，别让 is_saving 卡在真。 */
    s_snap_req = false;
    lisa_camera_stop(s_dev);
    s_fps10 = 0;
    s_task = NULL;
    s_exited = true;
    vTaskDelete(NULL);
}

int nano_cam_attach(lv_obj_t *canvas)
{
    if (!s_frame) {
        s_frame = psram_malloc((size_t)NANO_FX_IMG_W * NANO_FX_IMG_H * sizeof(uint16_t));
    }
    if (!s_frame) {
        LISA_LOGE(TAG, "帧缓冲分配失败");
        return -1;
    }
    s_canvas = canvas;
    lv_canvas_set_buffer(canvas, s_frame, NANO_FX_IMG_W, NANO_FX_IMG_H, LV_IMG_CF_TRUE_COLOR);
    return 0;
}

static int cam_start(bool raw)
{
    if (s_task || !s_canvas) {
        return s_task ? 0 : -1;
    }
    if (nano_fx_init() != 0) {
        return -1;
    }
    if (!service_camera_is_inited() && service_camera_init() != 0) {
        LISA_LOGE(TAG, "摄像头初始化失败");
        return -1;
    }
    service_camera_resume();

    s_dev = lisa_device_get("camera");
    if (!s_dev || lisa_camera_start(s_dev) != LISA_DEVICE_OK) {
        LISA_LOGE(TAG, "取流启动失败");
        s_dev = NULL;
        return -1;
    }

    s_fx = raw ? NANO_FX_PLAIN : (nano_fx_t)nano_settings()->fx_default;
    nano_fx_select(s_fx);

    s_stop_req = false;
    s_exited = false;
    if (xTaskCreate(cam_task, "nano.cam", CAM_STACK, NULL, CAM_PRIO, &s_task) != pdPASS) {
        s_task = NULL;
        s_exited = true;
        lisa_camera_stop(s_dev);
        s_dev = NULL;
        LISA_LOGE(TAG, "取流线程创建失败");
        return -1;
    }
    LISA_LOGI(TAG, "取景启动，特效=%d", (int)s_fx);
    return 0;
}

int nano_cam_start(void) { return cam_start(false); }
int nano_cam_start_raw(void) { return cam_start(true); }

void nano_cam_stop(void)
{
    if (!s_task) {
        return;
    }
    s_stop_req = true;
    /* 线程最坏阻塞在一帧的 53ms 上，给它一秒退出；退出前不能碰设备。 */
    for (int i = 0; i < 100 && !s_exited; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    service_camera_stop();
    s_dev = NULL;
    LISA_LOGI(TAG, "取景停止");
}

bool nano_cam_running(void)
{
    return s_task != NULL;
}

nano_fx_t nano_cam_effect(void)
{
    return s_fx;
}

bool nano_cam_next_effect(void)
{
    if (nano_settings()->fx_lock) {
        return false;
    }
    s_fx = (nano_fx_t)(((int)s_fx + 1) % NANO_FX_COUNT);
    nano_fx_select(s_fx);
    return true;
}

uint32_t nano_cam_fps10(void)
{
    return s_fps10;
}

uint32_t nano_cam_fps10_last(void)
{
    return s_fps10_last;
}

int nano_cam_shutter(void)
{
    if (!s_frame || !s_canvas || !s_task) {
        return -2;
    }
    if (s_saving || s_snap_req) {
        return -2;
    }
    if (!service_photo_card_ready()) {
        return -1;
    }
    if (!s_snap) {
        s_snap = psram_malloc(SNAP_BYTES);
        if (!s_snap) {
            LISA_LOGE(TAG, "快照缓冲分配失败");
            return -3;
        }
    }

    /* 涂白一拍就是"咔哒"：这一帧马上会被取流线程覆盖，
     * 而 s_snap 拿到的是覆盖之后的真实画面。 */
    memset(s_frame, 0xff, SNAP_BYTES);
    cam_inval(NULL, 0);
    s_saved_result = 0;
    s_saved_bytes = 0;
    s_snap_req = true;
    return 0;
}

bool nano_cam_is_saving(void)
{
    return s_saving || s_snap_req;
}

bool nano_cam_last_save_ok(void)
{
    return !nano_cam_is_saving() && s_saved_result == 0 && s_saved_name[0] != '\0';
}

const char *nano_cam_last_saved_name(void)
{
    return s_saved_name;
}

uint32_t nano_cam_last_saved_bytes(void)
{
    return s_saved_bytes;
}
