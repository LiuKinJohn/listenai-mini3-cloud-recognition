#include "nano_shell.h"
#include "nano_theme.h"
#include "nano_assets.h"
#include "nano_list.h"
#include "nano_settings.h"
#include "nano_cam.h"

#include "autoconf.h"
#include "lisa_log.h"
#include "listen_system.h"
#include "sys_wifi.h"
#include "netdev.h"
#include "multi_heap.h"
#include "project_version.h"
#include "service_brightness.h"
#include "service_camera.h"
#include "service_volume.h"
#include "service_sd_music.h"
#include "service_provision.h"
#include "button_factory_reset.h"
#include "power/power_manager.h"

#include "FreeRTOS.h"
#include "task.h"

#include "lsfs.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define TAG "nano.set.page"

#define SET_HEAD_H  18
#define SET_FOOT_H  26
#define SET_ROW_H   21
#define SET_ITEM_N  8

static const char *const s_items[SET_ITEM_N] = {
    "Wi-Fi", "显示", "相机", "声音", "时间", "关于", "系统", "诊断",
};

#define ROWS_N(x) ((int)(sizeof(x) / sizeof((x)[0])))

/* ==================== 只读数据源 ==================== */

static void net_ip_text(uint32_t v, char *buf, int size)
{
    if (v == 0) {
        snprintf(buf, size, "—");
        return;
    }
    /* 地址是小端存放、按字节序展开的，sys_wifi.c 打 DHCP 回调时用的同一套移位。 */
    snprintf(buf, size, "%u.%u.%u.%u", (unsigned)(v & 0xffU), (unsigned)((v >> 8) & 0xffU),
             (unsigned)((v >> 16) & 0xffU), (unsigned)((v >> 24) & 0xffU));
}

static void txt_wifi_state(char *buf, int size)
{
    if (sys_wifi_is_connected()) {
        snprintf(buf, size, "已连接");
    } else if (sys_wifi_has_ap()) {
        snprintf(buf, size, "有凭据");
    } else {
        snprintf(buf, size, "未配置");
    }
}

static void txt_wifi_rssi(char *buf, int size)
{
    int rssi = 0;

    if (sys_wifi_is_connected() && sys_wifi_get_signal_quality(&rssi)) {
        snprintf(buf, size, "%d dBm", rssi);
    } else {
        snprintf(buf, size, "—");
    }
}

static void txt_wifi_ip(char *buf, int size)
{
    struct netdev *nd = netdev_get_by_name("wifi0");

    net_ip_text(nd ? nd->ip_addr.addr : 0, buf, size);
}

static void txt_wifi_gw(char *buf, int size)
{
    struct netdev *nd = netdev_get_by_name("wifi0");

    net_ip_text(nd ? nd->gw.addr : 0, buf, size);
}

static void txt_time(char *buf, int size)
{
    struct tm t;

    if (ls_sys_get_localtime(&t) != 0) {
        snprintf(buf, size, "--:--:--");
        return;
    }
    snprintf(buf, size, "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
}

static void txt_date(char *buf, int size)
{
    static const char *const w[7] = {"日", "一", "二", "三", "四", "五", "六"};
    struct tm t;

    if (ls_sys_get_localtime(&t) != 0) {
        snprintf(buf, size, "等待校时");
        return;
    }
    snprintf(buf, size, "%04d.%02d.%02d 周%s", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
             w[t.tm_wday % 7]);
}

static void txt_time_src(char *buf, int size)
{
    snprintf(buf, size, ls_sys_time_is_valid() ? "RTC 有效" : "RTC 无效");
}

static void txt_tz(char *buf, int size) { snprintf(buf, size, "UTC+8"); }
static void txt_name(char *buf, int size) { snprintf(buf, size, "聆思 nano"); }
static void txt_model(char *buf, int size) { snprintf(buf, size, "arcs_mini3"); }
static void txt_fw(char *buf, int size) { snprintf(buf, size, "%s", PROJECT_VERSION_STR); }
static void txt_build(char *buf, int size) { snprintf(buf, size, "%s %s", __DATE__, __TIME__); }
static void txt_cam_state(char *buf, int size)
{
    if (nano_cam_running()) {
        snprintf(buf, size, "取景中");
    } else if (service_camera_is_inited()) {
        snprintf(buf, size, "已就绪");
    } else {
        snprintf(buf, size, "未初始化");
    }
}

/* 诊断页和取景屏互斥，跑着的时候看不到这里，所以取最近一次测到的值。 */
static void txt_cam_fps(char *buf, int size)
{
    uint32_t f10 = nano_cam_running() ? nano_cam_fps10() : nano_cam_fps10_last();

    if (f10 == 0) {
        snprintf(buf, size, "未测得");
        return;
    }
    snprintf(buf, size, "%u.%u fps", (unsigned)(f10 / 10U), (unsigned)(f10 % 10U));
}

static void txt_uptime(char *buf, int size)
{
    uint32_t s = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS / 1000U);

    snprintf(buf, size, "%uh%02um", (unsigned)(s / 3600U), (unsigned)((s / 60U) % 60U));
}

static void txt_tasks(char *buf, int size)
{
    snprintf(buf, size, "%u", (unsigned)uxTaskGetNumberOfTasks());
}

/* 一次遍历把 SRAM / PSRAM 的空闲与最大块分开累加，分界用 CONFIG_MEM_PSRAM_BASE。
 * 只读页每 2 秒取一次，代价是走一遍堆元数据，不在常态路径上。 */
struct heap_acc {
    uint32_t sram_free;
    uint32_t psram_free;
    uint32_t sram_max;
    uint32_t psram_max;
};

static struct heap_acc s_heap_acc;

static void heap_cb(void *start, void *end, multi_heap_info_t *info)
{
    bool psram = ((uintptr_t)start >= (uintptr_t)CONFIG_MEM_PSRAM_BASE);

    (void)end;
    if (psram) {
        s_heap_acc.psram_free += (uint32_t)info->total_free_bytes;
        if ((uint32_t)info->largest_free_block > s_heap_acc.psram_max) {
            s_heap_acc.psram_max = (uint32_t)info->largest_free_block;
        }
    } else {
        s_heap_acc.sram_free += (uint32_t)info->total_free_bytes;
        if ((uint32_t)info->largest_free_block > s_heap_acc.sram_max) {
            s_heap_acc.sram_max = (uint32_t)info->largest_free_block;
        }
    }
}

static void heap_collect(void)
{
    void heap_caps_travel(void (*callback)(void *, void *, multi_heap_info_t *));

    memset(&s_heap_acc, 0, sizeof(s_heap_acc));
    heap_caps_travel(heap_cb);
}

static void txt_heap(char *buf, int size)
{
    heap_collect();
    snprintf(buf, size, "%luK", (unsigned long)(s_heap_acc.sram_free / 1024U));
}

static void txt_heap_max(char *buf, int size)
{
    snprintf(buf, size, "%luK", (unsigned long)(s_heap_acc.sram_max / 1024U));
}

static void txt_psram(char *buf, int size)
{
    snprintf(buf, size, "%luK", (unsigned long)(s_heap_acc.psram_free / 1024U));
}

static void txt_psram_max(char *buf, int size)
{
    snprintf(buf, size, "%luK", (unsigned long)(s_heap_acc.psram_max / 1024U));
}

static int photo_count(void)
{
    struct lsfs_dir_t dir;
    struct lsfs_dirent ent;
    int n = 0;

    lsfs_dir_t_init(&dir);
    if (lsfs_opendir(&dir, "/SD:/") != 0) {
        return -1;
    }
    while (lsfs_readdir(&dir, &ent) == 0 && ent.name[0] != '\0') {
        const char *dot = strrchr(ent.name, '.');

        if (ent.type == LSFS_DIR_ENTRY_FILE && dot &&
            (strcmp(dot, ".JPG") == 0 || strcmp(dot, ".jpg") == 0)) {
            n++;
        }
    }
    lsfs_closedir(&dir);
    return n;
}

static void txt_photos(char *buf, int size)
{
    int n = photo_count();

    if (n < 0) {
        snprintf(buf, size, service_sd_music_is_card_ready() ? "读不到" : "无卡");
    } else {
        snprintf(buf, size, "%d 张", n);
    }
}

static void txt_card(char *buf, int size)
{
    snprintf(buf, size, service_sd_music_is_card_ready() ? "已挂载" : "未就绪");
}

/* ==================== 动作 ==================== */

static void act_clear_ap(void)
{
    sys_wifi_clear_saved_aps();
    LISA_LOGI(TAG, "已清除保存的 AP");
}

static void act_hotspot_cfg(void)
{
    if (service_provision_active()) {
        service_provision_stop();
        return;
    }
    if (!service_provision_start()) {
        LISA_LOGW(TAG, "热点配网启动失败或 Wi-Fi 尚未就绪");
    }
}

static void txt_prov_state(char *buf, int size) { snprintf(buf, size, "%s", service_provision_status()); }
static void txt_prov_ssid(char *buf, int size) { snprintf(buf, size, "%s", service_provision_ssid()); }
static void txt_prov_pwd(char *buf, int size) { snprintf(buf, size, "%s", service_provision_password()); }
static void txt_prov_url(char *buf, int size) { snprintf(buf, size, "%s", service_provision_url()); }

static void act_reboot(void) { power_reboot_soft(); }
static void act_factory_reset(void) { button_factory_reset(); }

/* ==================== 枚举表与读写口 ==================== */

static const char *const s_clk_names[NANO_CLOCK_STYLE_COUNT] = {"段码", "Rubik", "点阵"};
static const char *const s_fx_names[NANO_FX_COUNT] = {"原图", "色块", "单色", "CRT"};
static const char *const s_sleep_names[NANO_SLEEP_COUNT] = {"常亮", "30 秒", "60 秒", "3 分"};
static const char *const s_step_names[9] = {"20", "30", "40", "50", "60", "70", "80", "90", "100"};

static int get_clk(void) { return nano_settings()->clock_style; }
static void set_clk(int v) { nano_set_clock_style(v); }
static int get_fx(void) { return nano_settings()->fx_default; }
static void set_fx(int v) { nano_set_fx_default(v); }
static int get_fxlock(void) { return nano_settings()->fx_lock ? 1 : 0; }
static void set_fxlock(int v) { nano_set_fx_lock(v != 0); }
static int get_snd(void) { return nano_settings()->sound_on ? 1 : 0; }
static void set_snd(int v) { nano_set_sound_on(v != 0); }
static int get_tick(void) { return nano_settings()->tick_on ? 1 : 0; }
static void set_tick(int v) { nano_set_tick_on(v != 0); }
static int get_wifi(void) { return nano_settings()->wifi_on ? 1 : 0; }
static void set_wifi(int v) { nano_set_wifi_on(v != 0); }
static int get_sleep(void) { return nano_settings()->sleep_mode; }
static void set_sleep(int v) { nano_set_sleep_mode(v); }

static int step_index(int percent)
{
    int v = percent / 10 - 1;

    if (v < 0) v = 0;
    if (v > 8) v = 8;
    return v;
}

static int get_brt(void) { return step_index(service_brightness_get()); }
static void set_brt(int v) { service_brightness_set((v + 1) * 10); }
static int get_vol(void) { return step_index(service_volume_get()); }
static void set_vol(int v) { service_volume_set((v + 1) * 10); }

/* ==================== 8 个子页 ==================== */

static const nano_row_t s_rows_wifi[] = {
    { .label = "状态", .kind = NANO_ROW_RO, .text = txt_wifi_state },
    { .label = "信号", .kind = NANO_ROW_RO, .text = txt_wifi_rssi },
    { .label = "地址", .kind = NANO_ROW_RO, .text = txt_wifi_ip },
    { .label = "自动连接", .kind = NANO_ROW_FLAG, .get = get_wifi, .set = set_wifi },
    { .label = "热点配网", .kind = NANO_ROW_ACTION, .verb = "启停", .run = act_hotspot_cfg },
    { .label = "配网状态", .kind = NANO_ROW_RO, .text = txt_prov_state },
    { .label = "热点名称", .kind = NANO_ROW_RO, .text = txt_prov_ssid },
    { .label = "热点密码", .kind = NANO_ROW_RO, .text = txt_prov_pwd },
    { .label = "配置地址", .kind = NANO_ROW_RO, .text = txt_prov_url },
    { .label = "清除凭据", .kind = NANO_ROW_ACTION, .verb = "清除", .run = act_clear_ap,
      .confirm = true },
};

static const nano_row_t s_rows_display[] = {
    { .label = "时钟样式", .kind = NANO_ROW_ENUM, .names = s_clk_names,
      .name_count = NANO_CLOCK_STYLE_COUNT, .get = get_clk, .set = set_clk },
    { .label = "亮度", .kind = NANO_ROW_ENUM, .names = s_step_names, .name_count = 9,
      .get = get_brt, .set = set_brt },
    { .label = "息屏", .kind = NANO_ROW_ENUM, .names = s_sleep_names,
      .name_count = NANO_SLEEP_COUNT, .get = get_sleep, .set = set_sleep },
};

static const nano_row_t s_rows_camera[] = {
    { .label = "默认特效", .kind = NANO_ROW_ENUM, .names = s_fx_names, .name_count = NANO_FX_COUNT,
      .get = get_fx, .set = set_fx },
    { .label = "锁定特效", .kind = NANO_ROW_FLAG, .get = get_fxlock, .set = set_fxlock },
    { .label = "存储卡", .kind = NANO_ROW_RO, .text = txt_card },
    { .label = "照片", .kind = NANO_ROW_RO, .text = txt_photos },
};

static const nano_row_t s_rows_sound[] = {
    { .label = "总开关", .kind = NANO_ROW_FLAG, .get = get_snd, .set = set_snd },
    { .label = "按键音", .kind = NANO_ROW_FLAG, .get = get_tick, .set = set_tick },
    { .label = "音量", .kind = NANO_ROW_ENUM, .names = s_step_names, .name_count = 9,
      .get = get_vol, .set = set_vol },
};

static const nano_row_t s_rows_time[] = {
    { .label = "时间", .kind = NANO_ROW_RO, .text = txt_time },
    { .label = "日期", .kind = NANO_ROW_RO, .text = txt_date },
    { .label = "时间源", .kind = NANO_ROW_RO, .text = txt_time_src },
    { .label = "时区", .kind = NANO_ROW_RO, .text = txt_tz },
};

static const nano_row_t s_rows_about[] = {
    { .label = "名称", .kind = NANO_ROW_RO, .text = txt_name },
    { .label = "型号", .kind = NANO_ROW_RO, .text = txt_model },
    { .label = "固件", .kind = NANO_ROW_RO, .text = txt_fw },
    { .label = "构建", .kind = NANO_ROW_RO, .text = txt_build },
};

static const nano_row_t s_rows_system[] = {
    { .label = "运行", .kind = NANO_ROW_RO, .text = txt_uptime },
    { .label = "任务数", .kind = NANO_ROW_RO, .text = txt_tasks },
    { .label = "内存空闲", .kind = NANO_ROW_RO, .text = txt_heap },
    { .label = "最大块", .kind = NANO_ROW_RO, .text = txt_heap_max },
    { .label = "PSRAM 空闲", .kind = NANO_ROW_RO, .text = txt_psram },
    { .label = "PSRAM 大块", .kind = NANO_ROW_RO, .text = txt_psram_max },
    { .label = "重新启动", .kind = NANO_ROW_ACTION, .verb = "重启", .run = act_reboot,
      .confirm = true },
    { .label = "恢复出厂", .kind = NANO_ROW_ACTION, .verb = "抹除", .run = act_factory_reset,
      .confirm = true },
};

static const nano_row_t s_rows_diag[] = {
    { .label = "Wi-Fi", .kind = NANO_ROW_RO, .text = txt_wifi_state },
    { .label = "RSSI", .kind = NANO_ROW_RO, .text = txt_wifi_rssi },
    { .label = "IP", .kind = NANO_ROW_RO, .text = txt_wifi_ip },
    { .label = "网关", .kind = NANO_ROW_RO, .text = txt_wifi_gw },
    { .label = "存储卡", .kind = NANO_ROW_RO, .text = txt_card },
    { .label = "照片", .kind = NANO_ROW_RO, .text = txt_photos },
    { .label = "摄像头", .kind = NANO_ROW_RO, .text = txt_cam_state },
    { .label = "帧率", .kind = NANO_ROW_RO, .text = txt_cam_fps },
};

static const nano_page_t s_page_wifi    = { "WIFI",    "单击选择 · 双击执行", s_rows_wifi,   ROWS_N(s_rows_wifi),   1000 };
static const nano_page_t s_page_display = { "DISPLAY", "单击选择 · 双击更改", s_rows_display, ROWS_N(s_rows_display), 0 };
static const nano_page_t s_page_camera  = { "CAMERA",  "单击选择 · 双击更改", s_rows_camera, ROWS_N(s_rows_camera), 1000 };
static const nano_page_t s_page_sound   = { "SOUND",   "单击选择 · 双击更改", s_rows_sound,  ROWS_N(s_rows_sound),  0 };
static const nano_page_t s_page_time    = { "TIME",    "长按返回",             s_rows_time,   ROWS_N(s_rows_time),   1000 };
static const nano_page_t s_page_about   = { "ABOUT",   "长按返回",             s_rows_about,  ROWS_N(s_rows_about),  0 };
static const nano_page_t s_page_system  = { "SYSTEM",  "长按返回",             s_rows_system, ROWS_N(s_rows_system), 2000 };
static const nano_page_t s_page_diag    = { "DIAG",    "长按返回",             s_rows_diag,   ROWS_N(s_rows_diag),   1000 };

static const nano_page_t *const s_pages[SET_ITEM_N] = {
    &s_page_wifi, &s_page_display, &s_page_camera, &s_page_sound,
    &s_page_time, &s_page_about, &s_page_system, &s_page_diag,
};

/* ==================== 顶层列表 ==================== */

static lv_obj_t *s_row[SET_ITEM_N];
static lv_obj_t *s_label[SET_ITEM_N];
static lv_obj_t *s_status;
static lv_obj_t *s_list;
static int s_sel;

static void set_render(void)
{
    for (int i = 0; i < SET_ITEM_N; i++) {
        bool on = (i == s_sel);

        lv_obj_set_style_bg_color(s_row[i], on ? NANO_COL_ACCENT : NANO_COL_BG, 0);
        lv_obj_set_style_text_color(s_label[i], on ? NANO_COL_ON_SEL : NANO_COL_FG, 0);
    }
}

static lv_obj_t *settings_create(lv_obj_t *parent)
{
    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_t *head;
    lv_obj_t *line;

    nano_style_flat(page, NANO_COL_BG);
    lv_obj_set_size(page, NANO_W, NANO_BODY_H);

    head = lv_label_create(page);
    lv_obj_set_style_text_font(head, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(head, NANO_COL_DIM, 0);
    lv_label_set_text(head, "SETTINGS");
    lv_obj_set_pos(head, 12, 3);

    for (int i = 0; i < SET_ITEM_N; i++) {
        lv_obj_t *row = lv_obj_create(page);
        lv_obj_t *label;
        lv_obj_t *chev;

        nano_style_flat(row, NANO_COL_BG);
        lv_obj_set_size(row, NANO_W, SET_ROW_H);
        lv_obj_set_pos(row, 0, SET_HEAD_H + i * SET_ROW_H);

        label = lv_label_create(row);
        lv_obj_set_style_text_font(label, &lv_font_chinese_16, 0);
        lv_obj_set_style_text_color(label, NANO_COL_FG, 0);
        lv_label_set_text(label, s_items[i]);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 12, 0);

        chev = lv_label_create(row);
        lv_obj_set_style_text_font(chev, &lv_font_montserrat_14, 0);
        lv_label_set_text(chev, ">");
        lv_obj_align(chev, LV_ALIGN_RIGHT_MID, -12, 0);

        s_row[i] = row;
        s_label[i] = label;
    }

    line = nano_hairline_create(page);
    lv_obj_set_size(line, NANO_W - 24, 1);
    lv_obj_set_pos(line, 12, NANO_BODY_H - SET_FOOT_H);

    s_status = lv_label_create(page);
    lv_obj_set_style_text_font(s_status, &lv_font_chinese_16, 0);
    lv_obj_set_style_text_color(s_status, NANO_COL_DIM, 0);
    lv_label_set_text(s_status, "单击选择 · 双击打开");
    lv_obj_align(s_status, LV_ALIGN_TOP_MID, 0, NANO_BODY_H - SET_FOOT_H + 4);

    s_list = nano_list_create(page);
    set_render();
    return page;
}

static bool settings_action(nano_action_t act)
{
    if (nano_list_is_open(s_list)) {
        nano_list_act(s_list, act);
        return true;
    }
    if (act == NANO_ACT_NEXT) {
        s_sel = (s_sel + 1) % SET_ITEM_N;
        set_render();
        return true;
    } else if (act == NANO_ACT_SELECT) {
        nano_list_open(s_list, s_pages[s_sel]);
        return true;
    }
    return false;
}

static void settings_show(void)
{
    set_render();
}

static void settings_hide(void)
{
    nano_list_close(s_list);
}

const nano_view_t nano_view_settings = {
    .create = settings_create,
    .action = settings_action,
    .show = settings_show,
    .hide = settings_hide,
};
