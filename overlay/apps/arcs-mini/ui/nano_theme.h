#ifndef NANO_THEME_H
#define NANO_THEME_H

#include "lvgl.h"

#define NANO_W        240
#define NANO_H        240
#define NANO_BAR_H    22
#define NANO_BODY_H   (NANO_H - NANO_BAR_H)

/* 早期 iOS 的清透浅蓝：主选中态、高光和按下态共用同一蓝色家族。 */
#define NANO_COL_ACCENT  lv_color_hex(0x5AAAF5)
#define NANO_COL_HILITE  lv_color_hex(0x8DCEFF)
#define NANO_COL_PRESS    lv_color_hex(0x2D7FC5)
#define NANO_COL_WARN    lv_color_hex(0xFF9F0A)
#define NANO_COL_BG      lv_color_hex(0x000000)
#define NANO_COL_FG      lv_color_hex(0xF2F2F2)
#define NANO_COL_DIM     lv_color_hex(0x6A6A6A)
#define NANO_COL_GHOST   lv_color_hex(0x121212)
#define NANO_COL_HAIR    lv_color_hex(0x1E1E1E)
#define NANO_COL_ON_SEL  lv_color_hex(0x06121C)

static inline void nano_style_flat(lv_obj_t *obj, lv_color_t bg)
{
    lv_obj_set_style_bg_color(obj, bg, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_outline_width(obj, 0, 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

static inline lv_obj_t *nano_hairline_create(lv_obj_t *parent)
{
    lv_obj_t *line = lv_obj_create(parent);
    nano_style_flat(line, NANO_COL_HAIR);
    return line;
}

#endif /* NANO_THEME_H */
