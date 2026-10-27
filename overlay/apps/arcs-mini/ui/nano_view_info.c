#include "nano_shell.h"
#include "nano_theme.h"
#include "nano_digits.h"
#include "nano_assets.h"
#include "nano_settings.h"

#include "listen_system.h"
#include "sysheap.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define CLOCK_DIGITS   4
#define CLOCK_DIGIT_W  42
#define CLOCK_DIGIT_H  76
#define CLOCK_STROKE   6
#define CLOCK_COLON_GAP 16

#define CLOCK_TOP 38
#define RUBIK_H   84

/* 点阵时钟：25 列 × 7 行，每格 9px 里画 7px 的方点，留 2px 缝。
 * 亮灭只有 1bit，所以抖动用在熄灭的点上——按 8x8 Bayer 阈值把整块面板
 * 的余辉画成稀疏暗点，这才是实体点阵屏没点亮时的样子。 */
#define MX_COLS      25
#define MX_ROWS      7
#define MX_CELL      9
#define MX_DOT       7
#define MX_W         (MX_COLS * MX_CELL)
#define MX_H         (MX_ROWS * MX_CELL)
#define MX_GHOST_TH  20
#define MX_COLON_COL 6

#define MX_RGB565(r8, g8, b8) \
    (uint16_t)((((r8) >> 3) << 11) | (((g8) >> 2) << 5) | ((b8) >> 3))
#define MX_COL_ON    MX_RGB565(0xF2, 0xF2, 0xF2)
#define MX_COL_GHOST MX_RGB565(0x12, 0x12, 0x12)

static const uint8_t s_mx_glyph[10][MX_ROWS] = {
    {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E},
    {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F},
    {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E},
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02},
    {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E},
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E},
    {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},
};

static const uint8_t s_mx_bayer[64] = {
     0, 32,  8, 40,  3, 35, 11, 43,
    48, 16, 56, 24, 51, 19, 59, 27,
    12, 44,  4, 36, 15, 47,  7, 39,
    60, 28, 52, 20, 63, 31, 55, 23,
     3, 35, 11, 43,  0, 32,  8, 40,
    51, 19, 59, 27, 48, 16, 56, 24,
    15, 47,  7, 39, 12, 44,  4, 36,
    63, 31, 55, 23, 60, 28, 52, 20,
};

/* 4 个数字各占 5 列，冒号占 1 列，值是各自左上角在点阵里的列号。 */
static const uint8_t s_mx_col_of_digit[4] = {0, 8, 14, 20};

static nano_digits_t s_clock;
static lv_obj_t *s_w_seg;
static lv_obj_t *s_w_rubik;
static lv_obj_t *s_rubik_label;
static lv_obj_t *s_w_mx;
static lv_obj_t *s_mx_canvas;
static uint16_t *s_mx_buf;
static bool s_mx_failed;
static int s_style_shown = -1;

static lv_obj_t *s_seconds;
static lv_obj_t *s_date;
static lv_obj_t *s_hint;
static lv_timer_t *s_timer;
static bool s_colon_on;

static const char *const s_week[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};

static void mx_cell(int col, int row, bool lit)
{
    uint16_t c = 0;
    int x0 = col * MX_CELL;
    int y0 = row * MX_CELL;

    if (lit) {
        c = MX_COL_ON;
    } else if (s_mx_bayer[((row & 7) << 3) + (col & 7)] < MX_GHOST_TH) {
        c = MX_COL_GHOST;
    }
    for (int y = y0; y < y0 + MX_DOT; y++) {
        uint16_t *p = s_mx_buf + (size_t)y * MX_W + x0;
        for (int x = 0; x < MX_DOT; x++) {
            p[x] = c;
        }
    }
}

static void mx_draw(int hour, int min, bool colon)
{
    const int v[4] = {hour / 10, hour % 10, min / 10, min % 10};
    uint32_t rows[MX_ROWS] = {0};

    for (int d = 0; d < 4; d++) {
        const int base = s_mx_col_of_digit[d];

        for (int r = 0; r < MX_ROWS; r++) {
            const uint8_t bits = s_mx_glyph[v[d]][r];

            for (int c = 0; c < 5; c++) {
                if ((bits >> (4 - c)) & 1u) {
                    rows[r] |= 1u << (base + c);
                }
            }
        }
    }
    if (colon) {
        rows[1] |= 1u << MX_COLON_COL;
        rows[5] |= 1u << MX_COLON_COL;
    }
    /* 连缝里的列一起画：暗场要铺满整块面板，否则字与字之间会出现纯黑竖带。 */
    for (int r = 0; r < MX_ROWS; r++) {
        for (int c = 0; c < MX_COLS; c++) {
            mx_cell(c, r, (rows[r] & (1u << c)) != 0);
        }
    }
}

static void style_apply(void)
{
    int st = nano_settings()->clock_style;

    if (st == NANO_CLOCK_DITHER && !s_mx_buf && !s_mx_failed) {
        s_mx_buf = psram_malloc((size_t)MX_W * MX_H * sizeof(uint16_t));
        if (!s_mx_buf) {
            s_mx_failed = true;
        } else {
            memset(s_mx_buf, 0, (size_t)MX_W * MX_H * sizeof(uint16_t));
            lv_canvas_set_buffer(s_mx_canvas, s_mx_buf, MX_W, MX_H, LV_IMG_CF_TRUE_COLOR);
        }
    }
    if (st == NANO_CLOCK_DITHER && s_mx_failed) {
        st = NANO_CLOCK_SEG;
    }
    if (st == s_style_shown) {
        return;
    }
    s_style_shown = st;
    lv_obj_add_flag(s_w_seg, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_w_rubik, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_w_mx, LV_OBJ_FLAG_HIDDEN);
    if (st == NANO_CLOCK_RUBIK) {
        lv_obj_clear_flag(s_w_rubik, LV_OBJ_FLAG_HIDDEN);
    } else if (st == NANO_CLOCK_DITHER) {
        lv_obj_clear_flag(s_w_mx, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(s_w_seg, LV_OBJ_FLAG_HIDDEN);
    }
}

static void clock_hide_all(void)
{
    lv_obj_add_flag(s_w_seg, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_w_rubik, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_w_mx, LV_OBJ_FLAG_HIDDEN);
    s_style_shown = -1;
}

static void info_tick(lv_timer_t *timer)
{
    struct tm tm_buf;

    (void)timer;
    if (ls_sys_get_localtime(&tm_buf) != 0) {
        /* RTC 里还没有有效时间，等 NTP 那一轮点亮；先挂提示牌而不是显示 00:00。 */
        clock_hide_all();
        lv_label_set_text(s_seconds, "--");
        lv_label_set_text(s_date, "等待校时");
        return;
    }

    style_apply();
    s_colon_on = !s_colon_on;

    switch (s_style_shown) {
    case NANO_CLOCK_RUBIK:
        lv_label_set_text_fmt(s_rubik_label, "%02d:%02d", tm_buf.tm_hour, tm_buf.tm_min);
        break;
    case NANO_CLOCK_DITHER:
        mx_draw(tm_buf.tm_hour, tm_buf.tm_min, s_colon_on);
        lv_obj_invalidate(s_mx_canvas);
        break;
    default:
        nano_digits_set_value(&s_clock, tm_buf.tm_hour * 100 + tm_buf.tm_min);
        nano_digits_set_colon(&s_clock, s_colon_on);
        break;
    }

    lv_label_set_text_fmt(s_seconds, "%02d", tm_buf.tm_sec);
    lv_label_set_text_fmt(s_date, "%04d.%02d.%02d %s", tm_buf.tm_year + 1900, tm_buf.tm_mon + 1,
                          tm_buf.tm_mday, s_week[tm_buf.tm_wday % 7]);
}

static lv_obj_t *clock_box(lv_obj_t *parent, lv_coord_t y, lv_coord_t h)
{
    lv_obj_t *box = lv_obj_create(parent);

    nano_style_flat(box, NANO_COL_BG);
    lv_obj_set_size(box, NANO_W, h);
    lv_obj_set_pos(box, 0, y);
    lv_obj_add_flag(box, LV_OBJ_FLAG_HIDDEN);
    return box;
}

static lv_obj_t *info_create(lv_obj_t *parent)
{
    lv_obj_t *page = lv_obj_create(parent);
    lv_coord_t clock_w = CLOCK_DIGITS * CLOCK_DIGIT_W + (CLOCK_DIGITS - 1) * CLOCK_STROKE * 2 + CLOCK_COLON_GAP;

    nano_style_flat(page, NANO_COL_BG);
    lv_obj_set_size(page, NANO_W, NANO_BODY_H);

    s_w_seg = clock_box(page, CLOCK_TOP, CLOCK_DIGIT_H);
    nano_digits_create(&s_clock, s_w_seg, (NANO_W - clock_w) / 2, 0, CLOCK_DIGIT_W, CLOCK_DIGIT_H,
                       CLOCK_STROKE, CLOCK_DIGITS, true, CLOCK_COLON_GAP);

    s_w_rubik = clock_box(page, CLOCK_TOP - 2, RUBIK_H);
    s_rubik_label = lv_label_create(s_w_rubik);
    lv_obj_set_style_text_font(s_rubik_label, &lv_font_rubik_bold_64, 0);
    lv_obj_set_style_text_color(s_rubik_label, NANO_COL_FG, 0);
    lv_label_set_text(s_rubik_label, "00:00");
    lv_obj_align(s_rubik_label, LV_ALIGN_TOP_MID, 0, 0);

    s_w_mx = clock_box(page, CLOCK_TOP + 6, MX_H);
    s_mx_canvas = lv_canvas_create(s_w_mx);
    lv_obj_set_pos(s_mx_canvas, (NANO_W - MX_W) / 2, 0);

    s_seconds = lv_label_create(page);
    lv_obj_set_style_text_font(s_seconds, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_seconds, NANO_COL_ACCENT, 0);
    lv_label_set_text(s_seconds, "00");
    lv_obj_align(s_seconds, LV_ALIGN_TOP_MID, 0, 122);

    s_date = lv_label_create(page);
    lv_obj_set_style_text_font(s_date, &lv_font_chinese_16, 0);
    lv_obj_set_style_text_color(s_date, NANO_COL_DIM, 0);
    lv_label_set_text(s_date, "----.--.-- --");
    lv_obj_align(s_date, LV_ALIGN_TOP_MID, 0, 148);

    lv_obj_t *line = nano_hairline_create(page);
    lv_obj_set_size(line, NANO_W - 24, 1);
    lv_obj_set_pos(line, 12, 184);

    s_hint = lv_label_create(page);
    lv_obj_set_style_text_font(s_hint, &lv_font_chinese_16, 0);
    lv_obj_set_style_text_color(s_hint, NANO_COL_DIM, 0);
    lv_label_set_text(s_hint, "");
    lv_obj_align(s_hint, LV_ALIGN_TOP_MID, 0, 194);

    return page;
}

static bool info_action(nano_action_t act)
{
    if (act == NANO_ACT_NEXT) {
        int next = (nano_settings()->clock_style + 1) % NANO_CLOCK_STYLE_COUNT;
        nano_set_clock_style(next);
        style_apply();
        return true;
    }
    return false;
}

static void info_show(void)
{
    lv_label_set_text(s_hint, "单击换表盘");

    info_tick(NULL);
    if (s_timer) {
        lv_timer_resume(s_timer);
    } else {
        s_timer = lv_timer_create(info_tick, 1000, NULL);
    }
}

static void info_hide(void)
{
    if (s_timer) {
        lv_timer_pause(s_timer);
    }
}

const nano_view_t nano_view_info = {
    .create = info_create,
    .action = info_action,
    .show = info_show,
    .hide = info_hide,
};
