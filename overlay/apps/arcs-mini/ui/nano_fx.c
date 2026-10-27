#include "nano_fx.h"
#include "nano_theme.h"

#include "lisa_log.h"
#include "sysheap.h"

#include <string.h>

#define TAG "nano.fx"

/* 8x8 有序抖动矩阵，0..63，用的时候右移 2 位取 0..15。 */
static const uint8_t s_bayer[64] = {
       0, 32,  8, 40,  2, 34, 10, 42,
      48, 16, 56, 24, 50, 18, 58, 26,
      12, 44,  4, 36, 14, 46,  6, 38,
      60, 28, 52, 20, 62, 30, 54, 22,
       3, 35, 11, 43,  1, 33,  9, 41,
      51, 19, 59, 27, 49, 17, 57, 25,
      15, 47,  7, 39, 13, 45,  5, 37,
      63, 31, 55, 23, 61, 29, 53, 21,
};

/* 强调色就是主题里那颗绿，特效不许引入第二种彩度。 */
#define ACCENT_R5  ((0x13 >> 3) & 0x1f)
#define ACCENT_G6  ((0xBD >> 2) & 0x3f)
#define ACCENT_B5  ((0x9B >> 3) & 0x1f)

#define SCAN_GAIN  42 /* 扫描线暗行的千分比，420/1000 */

static uint16_t *s_lut_even;
static uint16_t *s_lut_odd;
static nano_fx_t s_fx = NANO_FX_POSTER;
static bool s_dither;
static bool s_scanline;
static bool s_ready;

static inline uint16_t bswap16(uint16_t v)
{
    return (uint16_t)((v >> 8) | (v << 8));
}

static inline uint16_t pack565(int r5, int g6, int b5)
{
    return (uint16_t)(((r5 & 0x1f) << 11) | ((g6 & 0x3f) << 5) | (b5 & 0x1f));
}

static inline int lum15(int r4, int g4, int b4)
{
    return (3 * r4 + 6 * g4 + b4 + 5) / 10;
}

static uint16_t scale565(uint16_t p, int permille)
{
    int r = (int)((((p >> 11) & 0x1fU) * permille) / 1000);
    int g = (int)((((p >> 5) & 0x3fU) * permille) / 1000);
    int b = (int)(((p & 0x1fU) * permille) / 1000);

    return pack565(r, g, b);
}

static uint16_t map_plain(int r5, int g6, int b5)
{
    return pack565(r5, g6, b5);
}

static uint16_t map_poster(int r4, int g4, int b4)
{
    /* 每通道 4 档，档位端点钉到 0 和满量程，色块边界才够硬。 */
    int r = (r4 >> 2) * 31 / 3;
    int g = (g4 >> 2) * 63 / 3;
    int b = (b4 >> 2) * 31 / 3;

    return pack565(r, g, b);
}

static uint16_t map_mono(int r4, int g4, int b4)
{
    int y = lum15(r4, g4, b4);

    if (y >= 14) {
        return pack565(ACCENT_R5, ACCENT_G6, ACCENT_B5);
    }
    return pack565(y * 31 / 15, y * 63 / 15, y * 31 / 15);
}

static uint16_t map_crt(int r4, int g4, int b4)
{
    int y = lum15(r4, g4, b4);
    int s = (y * 8) / 16; /* 8 档荧光灰 */

    return pack565(s * 20 / 7, s * 56 / 7, s * 26 / 7);
}

static void lut_fill(uint16_t *lut, nano_fx_t fx, int permille)
{
    for (int i = 0; i < NANO_FX_LUT_N; i++) {
        int r4 = (i >> 8) & 15;
        int g4 = (i >> 4) & 15;
        int b4 = i & 15;
        uint16_t o;

        switch (fx) {
        case NANO_FX_POSTER: o = map_poster(r4, g4, b4); break;
        case NANO_FX_MONO:   o = map_mono(r4, g4, b4);   break;
        case NANO_FX_CRT:    o = map_crt(r4, g4, b4);    break;
        default:             o = map_plain(r4 * 2 + 1, g4 * 4 + 2, b4 * 2 + 1); break;
        }
        lut[i] = (permille == 1000) ? o : scale565(o, permille);
    }
}

int nano_fx_init(void)
{
    if (s_ready) {
        return 0;
    }
    if (!s_lut_even) {
        s_lut_even = psram_malloc(NANO_FX_LUT_N * sizeof(uint16_t));
        s_lut_odd = psram_malloc(NANO_FX_LUT_N * sizeof(uint16_t));
    }
    if (!s_lut_even || !s_lut_odd) {
        LISA_LOGE(TAG, "LUT 分配失败");
        return -1;
    }
    s_ready = true;
    nano_fx_select(s_fx);
    return 0;
}

void nano_fx_select(nano_fx_t fx)
{
    if (!s_ready) {
        s_fx = fx;
        return;
    }
    s_fx = fx;
    s_dither = (fx == NANO_FX_POSTER || fx == NANO_FX_MONO || fx == NANO_FX_CRT);
    s_scanline = (fx == NANO_FX_CRT);

    lut_fill(s_lut_even, fx, 1000);
    if (s_scanline) {
        lut_fill(s_lut_odd, fx, SCAN_GAIN);
    }
    LISA_LOGI(TAG, "特效切到 %d，抖动=%d 扫描线=%d", (int)fx, s_dither, s_scanline);
}

nano_fx_t nano_fx_current(void)
{
    return s_fx;
}

const char *nano_fx_name(nano_fx_t fx)
{
    switch (fx) {
    case NANO_FX_POSTER: return "色块";
    case NANO_FX_MONO:   return "单色";
    case NANO_FX_CRT:    return "CRT";
    default:             return "原图";
    }
}

int nano_fx_save_quality(nano_fx_t fx)
{
    return (fx == NANO_FX_CRT) ? 3 : 2;
}

void nano_fx_apply(const uint16_t *src, uint16_t *dst, uint16_t src_w, uint16_t src_h,
                   uint16_t dst_w, uint16_t dst_h)
{
    const uint32_t x_off = (src_w > dst_w) ? ((uint32_t)src_w - dst_w) / 2U : 0U;
    const uint32_t y_off = (src_h > dst_h) ? ((uint32_t)src_h - dst_h) / 2U : 0U;

    if (!s_ready) {
        memset(dst, 0, (size_t)dst_w * dst_h * sizeof(uint16_t));
        return;
    }

    for (uint32_t y = 0; y < dst_h; y++) {
        const uint16_t *srow = src + (size_t)(y + y_off) * src_w + x_off;
        uint16_t *drow = dst + (size_t)y * dst_w;
        const uint16_t *lut = (s_scanline && (y & 1U)) ? s_lut_odd : s_lut_even;
        const uint32_t brow = (y & 7U) << 3;

        if (s_fx == NANO_FX_PLAIN) {
            /* 原图保留传感器完整 RGB565 色深，只进行字节序转换和中心裁切。 */
            for (uint32_t x = 0; x < dst_w; x++) drow[x] = bswap16(srow[x]);
        } else if (s_dither) {
            for (uint32_t x = 0; x < dst_w; x++) {
                /* fb 是大端 RGB565，先换成主机序再按位取，别在换序后的位图上做移位。 */
                uint16_t v = bswap16(srow[x]);
                uint32_t t = (uint32_t)(s_bayer[brow + (x & 7U)] >> 2);
                uint32_t r4 = (((v >> 11) & 0x1fU) * 16U + t) >> 5;
                uint32_t g4 = (((v >> 5) & 0x3fU) * 16U + t) >> 6;
                uint32_t b4 = ((v & 0x1fU) * 16U + t) >> 5;

                drow[x] = lut[(r4 << 8) | (g4 << 4) | b4];
            }
        } else {
            for (uint32_t x = 0; x < dst_w; x++) {
                uint16_t v = bswap16(srow[x]);

                drow[x] = lut[((((v >> 11) & 0x1fU) >> 1) << 8) |
                              ((((v >> 5) & 0x3fU) >> 2) << 4) |
                              (((v & 0x1fU) >> 1))];
            }
        }
    }
}
