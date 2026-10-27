/*
 * 取帧链路实测：把「传感器/DVP → 软件拷贝 → LUT → SPI 上屏」拆开计时，
 * 用来判断"取景即效果"能不能实时跑。只走 ADB shell，不接 UI。
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "lisa_camera.h"
#include "lisa_device.h"
#include "lisa_display.h"
#include "lisa_log.h"
#include "sysheap.h"

#include "service_camera.h"

#if CONFIG_LISA_SHELL
#include "cmd/cmd.h"
#include "shell.h"

#define TAG "camfps"

/* shell 里的 printf 只走 UART，日志宏才会被 ADB 通道带出来。 */
#define CAMFPS_LOG(fmt, ...) LISA_LOGI(TAG, fmt, ##__VA_ARGS__)

#define CAMFPS_SCREEN_W 240
#define CAMFPS_SCREEN_H 240
#define CAMFPS_DROP_STALE 2
#define CAMFPS_DEFAULT_FRAMES 30
#define CAMFPS_MAX_FRAMES 300
#define CAMFPS_LUT_PASSES 32

struct camfps_stats {
    uint32_t frames;
    uint32_t elapsed_ms;
    uint32_t cap_ms;
    uint32_t lut_ms;
    uint32_t panel_ms;
};

static uint8_t *s_src;
static uint16_t *s_dst;
static uint16_t *s_lut;
static bool s_lut_ready;

extern void lisa_ui_handler_suspend(void);
extern void lisa_ui_handler_resume(void);

static uint32_t camfps_now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

/* 15-bit 索引：r5/g5/b5 → 量化到 4 级后回写 RGB565。 */
static void camfps_lut_build(uint16_t *lut)
{
    for (uint32_t p = 0; p < 65536U; p++) {
        uint32_t r = (p >> 11) & 0x1fU;
        uint32_t g = (p >> 6) & 0x1fU;
        uint32_t b = (p >> 1) & 0x1fU;

        r = (r * 3U + 16U) / 31U;
        g = (g * 3U + 16U) / 31U;
        b = (b * 3U + 16U) / 31U;

        uint32_t y = (r + g + b);

        if (y <= 1U) {
            r = 0U; g = 1U; b = 2U;
        } else if (y >= 8U) {
            r = 3U; g = 3U; b = 2U;
        }

        uint16_t out = (uint16_t)(((r * 31U / 3U) << 11) |
                                  ((g * 63U / 3U) << 5) |
                                  (b * 31U / 3U));
        lut[p] = out;
    }
}

static void camfps_apply_lut(const void *pixels, uint16_t w, uint16_t h, bool swap_bytes)
{
    const uint16_t *src = (const uint16_t *)pixels;
    const uint32_t x_off = (w > CAMFPS_SCREEN_W) ? (w - CAMFPS_SCREEN_W) / 2U : 0U;

    for (uint32_t y = 0; y < CAMFPS_SCREEN_H; y++) {
        const uint16_t *src_row = src + (size_t)(y * h / CAMFPS_SCREEN_H) * w + x_off;
        uint16_t *dst_row = s_dst + (size_t)y * CAMFPS_SCREEN_W;

        if (swap_bytes) {
            for (uint32_t x = 0; x < CAMFPS_SCREEN_W; x++) {
                uint16_t p = src_row[x];
                dst_row[x] = s_lut[(uint16_t)((p >> 8) | (p << 8))];
            }
        } else {
            for (uint32_t x = 0; x < CAMFPS_SCREEN_W; x++) {
                dst_row[x] = s_lut[src_row[x]];
            }
        }
    }
}

static int camfps_prepare(uint16_t *width, uint16_t *height)
{
    if (!service_camera_is_inited()) {
        if (service_camera_init() != 0) {
            CAMFPS_LOG("camfps: camera init failed\n");
            return -1;
        }
    }
    service_camera_resume();

    if (service_camera_get_framesize(width, height) != 0) {
        CAMFPS_LOG("camfps: framesize unavailable\n");
        return -2;
    }

    if (!s_src) {
        s_src = psram_malloc((size_t)(*width) * (*height) * 2U);
        s_dst = psram_malloc((size_t)CAMFPS_SCREEN_W * CAMFPS_SCREEN_H * 2U);
        s_lut = psram_malloc(65536U * sizeof(uint16_t));
    }
    if (!s_src || !s_dst || !s_lut) {
        CAMFPS_LOG("psram alloc failed src=%d dst=%d lut=%d",
                   s_src != NULL, s_dst != NULL, s_lut != NULL);
        return -3;
    }

    return 0;
}

static void camfps_report(const char *mode, uint16_t w, uint16_t h, const struct camfps_stats *st)
{
    uint32_t per = st->frames ? (st->elapsed_ms / st->frames) : 0U;
    uint32_t fps10 = st->elapsed_ms ? (st->frames * 10000U / st->elapsed_ms) : 0U;

    CAMFPS_LOG("camfps %s: %ux%u frames=%u total=%ums per_frame=%ums fps=%u.%u\n", mode, w, h,
           st->frames, st->elapsed_ms, per, fps10 / 10U, fps10 % 10U);
    if (st->cap_ms) {
        CAMFPS_LOG("  cap=%ums lut=%ums panel=%ums (avg/frame %u/%u/%u)\n", st->cap_ms, st->lut_ms,
               st->panel_ms, st->frames ? st->cap_ms / st->frames : 0U,
               st->frames ? st->lut_ms / st->frames : 0U,
               st->frames ? st->panel_ms / st->frames : 0U);
    }
}

static void camfps_sink(uint16_t w, uint16_t h, uint32_t frames)
{
    lisa_device_t *cam = lisa_device_get("camera");
    lisa_camera_fb_t *fb = NULL;
    struct camfps_stats st = {0};
    uint32_t prev_ts = 0;
    uint32_t min_dt = 0xffffffffU;
    uint32_t max_dt = 0;
    uint32_t sum_dt = 0;
    uint32_t dt_cnt = 0;

    if (!cam || lisa_camera_start(cam) != LISA_DEVICE_OK) {
        CAMFPS_LOG("camfps sink: start stream failed\n");
        return;
    }

    for (uint32_t i = 0; i < CAMFPS_DROP_STALE; i++) {
        if (lisa_camera_capture(cam, &fb) == LISA_DEVICE_OK && fb) {
            lisa_camera_release_fb(cam, fb);
            fb = NULL;
        }
    }

    uint32_t t0 = camfps_now_ms();
    for (uint32_t i = 0; i < frames; i++) {
        if (lisa_camera_capture(cam, &fb) != LISA_DEVICE_OK || !fb) {
            break;
        }
        if (fb->width != w || fb->height != h) {
            CAMFPS_LOG("camfps sink: unexpected fb %ux%u\n", fb->width, fb->height);
        }
        if (fb->timestamp && prev_ts) {
            uint32_t dt = fb->timestamp - prev_ts;
            if (dt < min_dt) min_dt = dt;
            if (dt > max_dt) max_dt = dt;
            sum_dt += dt;
            dt_cnt++;
        }
        prev_ts = fb->timestamp;
        lisa_camera_release_fb(cam, fb);
        fb = NULL;
        st.frames++;
    }
    st.elapsed_ms = camfps_now_ms() - t0;

    lisa_camera_stop(cam);

    camfps_report("sink", w, h, &st);
    if (dt_cnt) {
        CAMFPS_LOG("  sensor interval: min=%ums max=%ums avg=%ums over=%u (driver timestamps)\n",
               min_dt, max_dt, sum_dt / dt_cnt, dt_cnt);
    }
}

static void camfps_loop(const char *mode, uint16_t w, uint16_t h, uint32_t frames, bool do_lut,
                        bool do_panel)
{
    lisa_device_t *disp = NULL;
    struct camfps_stats st = {0};
    int ret;

    if (do_panel) {
        disp = lisa_device_get("display");
        if (!disp) {
            CAMFPS_LOG("camfps panel: display device not found\n");
            return;
        }
        lisa_ui_handler_suspend();
    }

    uint32_t t0 = camfps_now_ms();
    for (uint32_t i = 0; i < frames; i++) {
        uint32_t t = camfps_now_ms();

        ret = service_camera_capture(s_src, (uint32_t)((size_t)w * h * 2U));
        if (ret != 0) {
            CAMFPS_LOG("camfps %s: capture failed at frame %u (ret=%d)\n", mode, i, ret);
            break;
        }
        st.cap_ms += camfps_now_ms() - t;

        if (do_lut) {
            t = camfps_now_ms();
            camfps_apply_lut(s_src, w, h, false);
            st.lut_ms += camfps_now_ms() - t;
        }

        if (do_panel) {
            lisa_display_buffer_desc_t desc = {
                .width = CAMFPS_SCREEN_W,
                .height = CAMFPS_SCREEN_H,
                .pitch = CAMFPS_SCREEN_W * sizeof(uint16_t),
                .buf_size = (uint32_t)((size_t)CAMFPS_SCREEN_W * CAMFPS_SCREEN_H * 2U),
            };
            t = camfps_now_ms();
            lisa_display_write(disp, 0, 0, &desc, s_dst);
            st.panel_ms += camfps_now_ms() - t;
        }

        st.frames++;
    }
    st.elapsed_ms = camfps_now_ms() - t0;

    if (do_panel) {
        lisa_ui_handler_resume();
    }

    camfps_report(mode, w, h, &st);

    if (do_lut && st.frames) {
        uint32_t t = camfps_now_ms();
        for (uint32_t p = 0; p < CAMFPS_LUT_PASSES; p++) {
            camfps_apply_lut(s_src, w, h, false);
        }
        uint32_t dt = camfps_now_ms() - t;
        CAMFPS_LOG("  lut only: %u passes in %ums -> %u.%ums/pass\n", CAMFPS_LUT_PASSES, dt,
               dt / CAMFPS_LUT_PASSES,
               (dt * 10U / CAMFPS_LUT_PASSES) % 10U);
    }
}

static void camfps_live(uint16_t w, uint16_t h, uint32_t frames)
{
    lisa_display_buffer_desc_t desc = {
        .width = CAMFPS_SCREEN_W,
        .height = CAMFPS_SCREEN_H,
        .pitch = CAMFPS_SCREEN_W * sizeof(uint16_t),
        .buf_size = (uint32_t)((size_t)CAMFPS_SCREEN_W * CAMFPS_SCREEN_H * 2U),
    };
    lisa_device_t *cam = lisa_device_get("camera");
    lisa_device_t *disp = lisa_device_get("display");
    struct camfps_stats st = {0};
    lisa_camera_fb_t *fb = NULL;
    uint32_t t, t0;

    if (!cam || !disp) {
        CAMFPS_LOG("live: camera or display missing");
        return;
    }
    lisa_ui_handler_suspend();

    if (lisa_camera_start(cam) != LISA_DEVICE_OK) {
        CAMFPS_LOG("live: start stream failed");
        lisa_ui_handler_resume();
        return;
    }
    for (uint32_t i = 0; i < CAMFPS_DROP_STALE; i++) {
        if (lisa_camera_capture(cam, &fb) == LISA_DEVICE_OK && fb) {
            lisa_camera_release_fb(cam, fb);
            fb = NULL;
        }
    }

    t0 = camfps_now_ms();
    for (uint32_t i = 0; i < frames; i++) {
        t = camfps_now_ms();
        if (lisa_camera_capture(cam, &fb) != LISA_DEVICE_OK || !fb || !fb->buf) {
            CAMFPS_LOG("live: capture failed at frame %u", i);
            break;
        }
        st.cap_ms += camfps_now_ms() - t;

        t = camfps_now_ms();
        camfps_apply_lut(fb->buf, fb->width, fb->height, true);
        lisa_camera_release_fb(cam, fb);
        fb = NULL;
        st.lut_ms += camfps_now_ms() - t;

        t = camfps_now_ms();
        lisa_display_write(disp, 0, 0, &desc, s_dst);
        st.panel_ms += camfps_now_ms() - t;

        st.frames++;
    }
    st.elapsed_ms = camfps_now_ms() - t0;

    lisa_camera_stop(cam);
    lisa_ui_handler_resume();

    camfps_report("live", w, h, &st);
}

static int camfps_shell_cmd(int argc, char **argv)
{
    uint16_t w = 0, h = 0;
    uint32_t frames = CAMFPS_DEFAULT_FRAMES;
    const char *mode = argc > 1 ? argv[1] : "stock";

    if (argc > 2) {
        long n = strtol(argv[2], NULL, 10);
        if (n < 1 || n > CAMFPS_MAX_FRAMES) {
            CAMFPS_LOG("camfps: frames must be 1..%u\n", CAMFPS_MAX_FRAMES);
            return -1;
        }
        frames = (uint32_t)n;
    }

    if (camfps_prepare(&w, &h) != 0) {
        return -2;
    }

    if (!s_lut_ready) {
        camfps_lut_build(s_lut);
        s_lut_ready = true;
    }

    CAMFPS_LOG("camfps %s: frames=%u cam=%ux%u screen=%ux%u\n", mode, frames, w, h,
           CAMFPS_SCREEN_W, CAMFPS_SCREEN_H);

    if (strcmp(mode, "sink") == 0) {
        camfps_sink(w, h, frames);
    } else if (strcmp(mode, "live") == 0) {
        camfps_live(w, h, frames);
    } else if (strcmp(mode, "lut") == 0) {
        camfps_loop("lut", w, h, frames, true, false);
    } else if (strcmp(mode, "panel") == 0) {
        camfps_loop("panel", w, h, frames, true, true);
    } else if (strcmp(mode, "stock") == 0) {
        camfps_loop("stock", w, h, frames, false, false);
    } else {
        CAMFPS_LOG("usage: camfps [stock|sink|live|lut|panel] [frames]\n");
        return -1;
    }

    service_camera_stop();
    return 0;
}

SHELL_EXPORT_CMD(SHELL_CMD_PERMISSION(0) | SHELL_CMD_TYPE(SHELL_TYPE_CMD_MAIN) |
                     SHELL_CMD_DISABLE_RETURN,
                 camfps, camfps_shell_cmd, measure camera capture pipeline frame rate);

#endif
