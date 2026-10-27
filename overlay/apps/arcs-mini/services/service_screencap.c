/*
 * 屏幕截图回传：面板没有可读回的帧缓冲，所以让 LVGL 把当前活动屏重画进
 * 我们自己的 RGB565 缓冲，再打包成 24 位 BMP，由 miniapp 的 ADB pull 送出。
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "lvgl.h"

#include "lisa_log.h"
#include "sysheap.h"

#include "lisa_ui_invoke.h"

#include "service_screencap.h"

#define TAG "screencap"

#define SC_W 240
#define SC_H 240
#define SC_STRIDE (SC_W * 3)
#define SC_RGB565_BYTES (SC_W * SC_H * 2U)
#define SC_HEADER_BYTES 54U
#define SC_BMP_BYTES (SC_HEADER_BYTES + (SC_H * SC_STRIDE))
#define SC_UI_WAIT_TICKS 100U

static uint8_t *s_rgb565;
static uint8_t *s_bmp;
static size_t s_bmp_size;
static lv_img_dsc_t s_dsc;
static volatile bool s_ui_done;
static volatile lv_res_t s_ui_res;

static void sc_put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void sc_put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static size_t sc_build_bmp(void)
{
    const uint16_t *px = (const uint16_t *)s_rgb565;

    memset(s_bmp, 0, SC_HEADER_BYTES);
    s_bmp[0] = 'B';
    s_bmp[1] = 'M';
    sc_put32(s_bmp + 2, SC_BMP_BYTES);
    sc_put32(s_bmp + 10, SC_HEADER_BYTES);
    sc_put32(s_bmp + 14, 40U);
    sc_put32(s_bmp + 18, SC_W);
    sc_put32(s_bmp + 22, SC_H);
    sc_put16(s_bmp + 26, 1U);
    sc_put16(s_bmp + 28, 24U);
    sc_put32(s_bmp + 30, 0U);
    sc_put32(s_bmp + 34, (uint32_t)(SC_H * SC_STRIDE));
    sc_put32(s_bmp + 38, 2835U);
    sc_put32(s_bmp + 42, 2835U);

    for (uint32_t y = 0; y < SC_H; y++) {
        uint8_t *row = s_bmp + SC_HEADER_BYTES + (SC_H - 1U - y) * SC_STRIDE;
        const uint16_t *src = px + (size_t)y * SC_W;

        for (uint32_t x = 0; x < SC_W; x++) {
            uint32_t p = src[x];
            row[x * 3U + 0U] = (uint8_t)(((p & 0x1fU) * 255U + 15U) / 31U);
            row[x * 3U + 1U] = (uint8_t)((((p >> 5) & 0x3fU) * 255U + 31U) / 63U);
            row[x * 3U + 2U] = (uint8_t)((((p >> 11) & 0x1fU) * 255U + 15U) / 31U);
        }
    }

    return SC_BMP_BYTES;
}

static void sc_worker(void *arg, uint32_t len)
{
    lv_obj_t *scr;

    (void)arg;
    (void)len;

    scr = lv_scr_act();
    if (!scr) {
        s_ui_res = LV_RES_INV;
    } else {
        s_ui_res = lv_snapshot_take_to_buf(scr, LV_IMG_CF_TRUE_COLOR, &s_dsc, s_rgb565,
                                           SC_RGB565_BYTES);
    }
    s_bmp_size = (s_ui_res == LV_RES_OK) ? sc_build_bmp() : 0U;
    s_ui_done = true;
}

bool service_screencap_capture(void)
{
    if (!s_rgb565) {
        s_rgb565 = psram_malloc(SC_RGB565_BYTES);
        s_bmp = psram_malloc(SC_BMP_BYTES);
    }
    if (!s_rgb565 || !s_bmp) {
        LISA_LOGE(TAG, "psram alloc failed");
        return false;
    }

    s_ui_done = false;
    s_ui_res = LV_RES_INV;
    LISA_UI_INVOKE_UI(sc_worker, NULL, 0);

    for (uint32_t i = 0; i < SC_UI_WAIT_TICKS && !s_ui_done; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (!s_ui_done) {
        LISA_LOGE(TAG, "UI thread did not answer");
        return false;
    }
    if (s_ui_res != LV_RES_OK || !s_bmp_size) {
        LISA_LOGE(TAG, "snapshot failed (res=%d)", (int)s_ui_res);
        return false;
    }

    LISA_LOGI(TAG, "captured %ux%u bmp=%u bytes", SC_W, SC_H, (unsigned)s_bmp_size);
    return true;
}

const uint8_t *service_screencap_bmp(size_t *size)
{
    if (!s_bmp_size) {
        return NULL;
    }
    if (size) {
        *size = s_bmp_size;
    }
    return s_bmp;
}
