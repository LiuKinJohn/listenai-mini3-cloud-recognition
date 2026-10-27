#ifndef NANO_ASSETS_H
#define NANO_ASSETS_H

#include "lvgl.h"

/* 直接引用出厂状态栏图标与已编进的字体。自己声明而不去 include
 * lisa_ui_assets.h / lisa_ui_fonts.h，是为了不给新库再挂两条 apps-ui 的 include 路径。
 * 符号由 apps-ui 的 assets 目标提供，链接期同一个可执行文件。 */

LV_IMG_DECLARE(icons_ic_status_wifi_lvl_1_png);
LV_IMG_DECLARE(icons_ic_status_wifi_lvl_2_png);
LV_IMG_DECLARE(icons_ic_status_wifi_lvl_3_png);
LV_IMG_DECLARE(icons_ic_status_wifi_no_connect_png);
LV_IMG_DECLARE(icons_ic_status_wifi_no_network_png);
LV_IMG_DECLARE(icons_ic_status_power0_png);
LV_IMG_DECLARE(icons_ic_status_power1_png);
LV_IMG_DECLARE(icons_ic_status_power2_png);
LV_IMG_DECLARE(icons_ic_status_power3_png);
LV_IMG_DECLARE(icons_ic_status_power4_png);
LV_IMG_DECLARE(icons_ic_status_power5_png);
LV_IMG_DECLARE(icons_ic_status_power6_png);
LV_IMG_DECLARE(icons_ic_status_power7_png);
LV_IMG_DECLARE(icons_ic_status_power8_png);
LV_IMG_DECLARE(icons_ic_status_power9_png);
LV_IMG_DECLARE(icons_ic_status_charging0_png);
LV_IMG_DECLARE(icons_ic_status_charging1_png);
LV_IMG_DECLARE(icons_ic_status_charging2_png);
LV_IMG_DECLARE(icons_ic_status_charging3_png);
LV_IMG_DECLARE(icons_ic_status_charging4_png);
LV_IMG_DECLARE(icons_ic_status_charging5_png);
LV_IMG_DECLARE(icons_ic_status_charging6_png);
LV_IMG_DECLARE(icons_ic_status_charging7_png);
LV_IMG_DECLARE(icons_ic_status_charging8_png);
LV_IMG_DECLARE(icons_ic_status_charging9_png);
LV_IMG_DECLARE(icons_ic_status_tf_card_png);
LV_IMG_DECLARE(icons_ic_status_usb_png);

LV_FONT_DECLARE(lv_font_chinese_16);
LV_FONT_DECLARE(lv_font_rubik_bold_64);

#endif /* NANO_ASSETS_H */
