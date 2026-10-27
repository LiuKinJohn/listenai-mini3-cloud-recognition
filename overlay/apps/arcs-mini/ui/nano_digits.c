#include "nano_digits.h"
#include "nano_theme.h"

#define SEG_A 0
#define SEG_B 1
#define SEG_C 2
#define SEG_D 3
#define SEG_E 4
#define SEG_F 5
#define SEG_G 6

static const uint8_t s_digit_map[10] = {
    /* 0 */ (1u << SEG_A) | (1u << SEG_B) | (1u << SEG_C) | (1u << SEG_D) | (1u << SEG_E) | (1u << SEG_F),
    /* 1 */ (1u << SEG_B) | (1u << SEG_C),
    /* 2 */ (1u << SEG_A) | (1u << SEG_B) | (1u << SEG_G) | (1u << SEG_E) | (1u << SEG_D),
    /* 3 */ (1u << SEG_A) | (1u << SEG_B) | (1u << SEG_G) | (1u << SEG_C) | (1u << SEG_D),
    /* 4 */ (1u << SEG_F) | (1u << SEG_G) | (1u << SEG_B) | (1u << SEG_C),
    /* 5 */ (1u << SEG_A) | (1u << SEG_F) | (1u << SEG_G) | (1u << SEG_C) | (1u << SEG_D),
    /* 6 */ (1u << SEG_A) | (1u << SEG_F) | (1u << SEG_G) | (1u << SEG_E) | (1u << SEG_C) | (1u << SEG_D),
    /* 7 */ (1u << SEG_A) | (1u << SEG_B) | (1u << SEG_C),
    /* 8 */ 0x7F,
    /* 9 */ (1u << SEG_A) | (1u << SEG_B) | (1u << SEG_C) | (1u << SEG_D) | (1u << SEG_F) | (1u << SEG_G),
};

static lv_obj_t *seg_create(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h)
{
    lv_obj_t *seg = lv_obj_create(parent);
    nano_style_flat(seg, NANO_COL_GHOST);
    lv_obj_set_pos(seg, x, y);
    lv_obj_set_size(seg, w, h);
    return seg;
}

static void digit_build(nano_digits_t *d, int index, lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                        lv_coord_t dw, lv_coord_t dh, lv_coord_t t)
{
    lv_coord_t vlen = (dh - 3 * t) / 2;
    lv_coord_t hlen = dw - 2 * t;
    lv_coord_t mid_y = y + (dh - t) / 2;

    d->seg[index][SEG_A] = seg_create(parent, x + t, y, hlen, t);
    d->seg[index][SEG_B] = seg_create(parent, x + dw - t, y + t, t, vlen);
    d->seg[index][SEG_C] = seg_create(parent, x + dw - t, mid_y + t, t, vlen);
    d->seg[index][SEG_D] = seg_create(parent, x + t, y + dh - t, hlen, t);
    d->seg[index][SEG_E] = seg_create(parent, x, mid_y + t, t, vlen);
    d->seg[index][SEG_F] = seg_create(parent, x, y + t, t, vlen);
    d->seg[index][SEG_G] = seg_create(parent, x + t, mid_y, hlen, t);
}

void nano_digits_create(nano_digits_t *d, lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                        lv_coord_t digit_w, lv_coord_t digit_h, lv_coord_t thickness,
                        int digit_count, bool with_colon, lv_coord_t colon_gap)
{
    lv_coord_t gap = thickness * 2;
    lv_coord_t cursor = x;

    if (digit_count > NANO_DIGITS_MAX) {
        digit_count = NANO_DIGITS_MAX;
    }
    d->count = digit_count;
    d->colon[0] = NULL;
    d->colon[1] = NULL;

    for (int i = 0; i < digit_count; i++) {
        /* 0xFF = 从未画过。set() 会跳过没变化的位，若初值就是 0，
         * 真实的 "0" 永远不会被刷出来，开机只剩一片暗 8。 */
        d->value[i] = 0xFF;
        d->on_color[i] = NANO_COL_FG;
        digit_build(d, i, parent, cursor, y, digit_w, digit_h, thickness);
        cursor += digit_w + gap;

        if (with_colon && i == digit_count / 2 - 1) {
            lv_coord_t cx = cursor + (colon_gap - thickness) / 2 - gap / 2;
            cursor += colon_gap;
            d->colon[0] = seg_create(parent, cx, y + digit_h / 4, thickness, thickness);
            d->colon[1] = seg_create(parent, cx, y + 3 * digit_h / 4 - thickness, thickness, thickness);
        }
    }

    nano_digits_set_value(d, 0);
}

static void digit_repaint(nano_digits_t *d, int index)
{
    uint8_t mask = s_digit_map[d->value[index]];
    for (int s = 0; s < NANO_SEG_COUNT; s++) {
        bool on = (mask & (1u << s)) != 0;
        lv_obj_set_style_bg_color(d->seg[index][s], on ? d->on_color[index] : NANO_COL_GHOST, 0);
    }
}

void nano_digits_set(nano_digits_t *d, int index, int digit)
{
    if (index < 0 || index >= d->count || digit < 0 || digit > 9) {
        return;
    }
    if (d->value[index] == (uint8_t)digit) {
        return;
    }
    d->value[index] = (uint8_t)digit;
    digit_repaint(d, index);
}

void nano_digits_set_value(nano_digits_t *d, int value)
{
    int divisor = 1;
    for (int i = d->count - 1; i >= 0; i--) {
        nano_digits_set(d, i, (value / divisor) % 10);
        divisor *= 10;
    }
}

void nano_digits_set_color(nano_digits_t *d, int index, lv_color_t on_color)
{
    if (index < 0 || index >= d->count) {
        return;
    }
    d->on_color[index] = on_color;
    digit_repaint(d, index);
}

void nano_digits_set_colon(nano_digits_t *d, bool on)
{
    lv_color_t color = on ? NANO_COL_ACCENT : NANO_COL_GHOST;
    for (int i = 0; i < 2; i++) {
        if (d->colon[i]) {
            lv_obj_set_style_bg_color(d->colon[i], color, 0);
        }
    }
}
