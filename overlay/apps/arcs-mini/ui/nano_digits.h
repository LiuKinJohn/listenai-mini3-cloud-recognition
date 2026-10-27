#ifndef NANO_DIGITS_H
#define NANO_DIGITS_H

#include <stdbool.h>
#include "lvgl.h"

#define NANO_DIGITS_MAX 4
#define NANO_SEG_COUNT  7

/* 七段段码数字，用矩形手工排——这个 build 里最大的字体是 rubik_bold_64（粗），
 * 拿不到 nano 那种细字重大时钟，所以自己画。 */
typedef struct {
    lv_obj_t *seg[NANO_DIGITS_MAX][NANO_SEG_COUNT];
    lv_obj_t *colon[2];
    lv_color_t on_color[NANO_DIGITS_MAX];
    uint8_t value[NANO_DIGITS_MAX];
    int count;
} nano_digits_t;

/* x/y 是整块数字左上角。冒号排在第 count/2 位数字之后。 */
void nano_digits_create(nano_digits_t *d, lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                        lv_coord_t digit_w, lv_coord_t digit_h, lv_coord_t thickness,
                        int digit_count, bool with_colon, lv_coord_t colon_gap);

void nano_digits_set(nano_digits_t *d, int index, int digit);
void nano_digits_set_value(nano_digits_t *d, int value);
void nano_digits_set_color(nano_digits_t *d, int index, lv_color_t on_color);
void nano_digits_set_colon(nano_digits_t *d, bool on);

#endif /* NANO_DIGITS_H */
