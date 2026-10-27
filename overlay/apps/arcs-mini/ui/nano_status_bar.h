#ifndef NANO_STATUS_BAR_H
#define NANO_STATUS_BAR_H

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

typedef enum {
    NANO_WIFI_DISCONNECTED = 0,   /* 未配网 / 未连接 */
    NANO_WIFI_CONNECTING,         /* 连接中 */
    NANO_WIFI_CONNECTED           /* 已连接，level 给信号格 */
} nano_wifi_state_t;

lv_obj_t *nano_status_bar_create(lv_obj_t *parent);
void nano_status_bar_set_screen(lv_obj_t *bar, const char *name);
void nano_status_bar_set_wifi(lv_obj_t *bar, nano_wifi_state_t state, uint8_t level);
void nano_status_bar_set_battery(lv_obj_t *bar, uint8_t percent, bool charging);
void nano_status_bar_set_time(lv_obj_t *bar, int hour, int minute);
void nano_status_bar_set_syncing(lv_obj_t *bar, bool syncing);

#endif /* NANO_STATUS_BAR_H */
