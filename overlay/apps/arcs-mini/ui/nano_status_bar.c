#include "nano_status_bar.h"
#include "nano_assets.h"
#include "nano_theme.h"

#include <stdio.h>

static lv_obj_t *s_dot;
static lv_obj_t *s_wifi;
static lv_obj_t *s_time;
static lv_obj_t *s_name;
static lv_obj_t *s_battery;

static const lv_img_dsc_t *const s_power_icon[10] = {
    &icons_ic_status_power0_png, &icons_ic_status_power1_png, &icons_ic_status_power2_png,
    &icons_ic_status_power3_png, &icons_ic_status_power4_png, &icons_ic_status_power5_png,
    &icons_ic_status_power6_png, &icons_ic_status_power7_png, &icons_ic_status_power8_png,
    &icons_ic_status_power9_png,
};

static const lv_img_dsc_t *const s_charging_icon[10] = {
    &icons_ic_status_charging0_png, &icons_ic_status_charging1_png, &icons_ic_status_charging2_png,
    &icons_ic_status_charging3_png, &icons_ic_status_charging4_png, &icons_ic_status_charging5_png,
    &icons_ic_status_charging6_png, &icons_ic_status_charging7_png, &icons_ic_status_charging8_png,
    &icons_ic_status_charging9_png,
};

lv_obj_t *nano_status_bar_create(lv_obj_t *parent)
{
    lv_obj_t *bar = lv_obj_create(parent);
    nano_style_flat(bar, NANO_COL_BG);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_size(bar, NANO_W, NANO_BAR_H);

    s_dot = lv_obj_create(bar);
    nano_style_flat(s_dot, NANO_COL_BG);
    lv_obj_set_size(s_dot, 6, 6);
    lv_obj_set_style_radius(s_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_pos(s_dot, 4, (NANO_BAR_H - 6) / 2);

    s_wifi = lv_img_create(bar);
    lv_img_set_src(s_wifi, &icons_ic_status_wifi_no_connect_png);
    lv_obj_set_pos(s_wifi, 13, (NANO_BAR_H - 20) / 2);

    s_time = lv_label_create(bar);
    lv_obj_set_style_text_font(s_time, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_time, NANO_COL_FG, 0);
    lv_label_set_text(s_time, "--:--");
    lv_obj_set_pos(s_time, 36, (NANO_BAR_H - 14) / 2);

    s_battery = lv_img_create(bar);
    lv_img_set_src(s_battery, &icons_ic_status_power0_png);
    lv_obj_align(s_battery, LV_ALIGN_TOP_RIGHT, -4, (NANO_BAR_H - 20) / 2);

    s_name = lv_label_create(bar);
    lv_obj_set_style_text_font(s_name, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_name, NANO_COL_FG, 0);
    lv_label_set_text(s_name, "");
    lv_obj_align(s_name, LV_ALIGN_TOP_RIGHT, -32, (NANO_BAR_H - 14) / 2);

    return bar;
}

void nano_status_bar_set_screen(lv_obj_t *bar, const char *name)
{
    (void)bar;
    lv_label_set_text(s_name, name ? name : "");
    lv_obj_align(s_name, LV_ALIGN_TOP_RIGHT, -32, (NANO_BAR_H - 14) / 2);
}

void nano_status_bar_set_wifi(lv_obj_t *bar, nano_wifi_state_t state, uint8_t level)
{
    const lv_img_dsc_t *icon;

    (void)bar;
    switch (state) {
    case NANO_WIFI_CONNECTED:
        icon = (level >= 3) ? &icons_ic_status_wifi_lvl_3_png
             : (level == 2) ? &icons_ic_status_wifi_lvl_2_png
                            : &icons_ic_status_wifi_lvl_1_png;
        break;
    case NANO_WIFI_CONNECTING:
        icon = &icons_ic_status_wifi_no_network_png;
        break;
    default:
        icon = &icons_ic_status_wifi_no_connect_png;
        break;
    }
    lv_img_set_src(s_wifi, icon);
}

void nano_status_bar_set_battery(lv_obj_t *bar, uint8_t percent, bool charging)
{
    uint8_t idx = (percent > 99) ? 9 : (percent / 10);

    (void)bar;
    lv_img_set_src(s_battery, charging ? s_charging_icon[idx] : s_power_icon[idx]);
}

void nano_status_bar_set_time(lv_obj_t *bar, int hour, int minute)
{
    char buf[8];

    (void)bar;
    snprintf(buf, sizeof(buf), "%02d:%02d", hour % 24, minute % 60);
    lv_label_set_text(s_time, buf);
}

void nano_status_bar_set_syncing(lv_obj_t *bar, bool syncing)
{
    (void)bar;
    lv_obj_set_style_bg_color(s_dot, syncing ? NANO_COL_WARN : NANO_COL_BG, 0);
}
