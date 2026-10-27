#include "nano_shell.h"
#include "nano_theme.h"
#include "nano_status_bar.h"
#include "nano_assets.h"
#include "nano_settings.h"
#include "nano_home.h"

#include "lisa_log.h"
#include "apps/llm/lisa_ui_nav_scr_ids.h"
#include "lisa_ui_nav_scr.h"
#include "lisa_ui_invoke.h"
#include "listen_system.h"
#include "battery/battery.h"
#include "power/power_manager.h"
#include "sys_wifi.h"
#include "service_timesync.h"
#include "service_power_policy.h"
#include "button_factory_reset.h"
#include "alarm_ring.h"
#ifdef CONFIG_OTA
#include "ota_manager.h"
#endif

#include <stdio.h>
#include <string.h>
#include <time.h>

#define TAG "nano.shell"
#define PAGE_W NANO_W

#define MENU_ITEM_N 5

static const char *const s_screen_names[NANO_SCREEN_COUNT] = {
    "主页", "时钟", "识云", "相机", "相册", "设置"
};
/* 状态栏用拉丁缩写：栏里其他文字都是 montserrat_14，中文 16 号压不住。 */
static const char *const s_screen_tags[NANO_SCREEN_COUNT] = {"HOME", "TIME", "CLOUD", "CAM", "ALBUM", "SET"};
static const char *const s_menu_names[MENU_ITEM_N] = {"熄屏待机", "重启", "清配网", "恢复出厂", "关机"};

static const nano_view_t *const s_views[NANO_SCREEN_COUNT] = {
    NULL, &nano_view_info, &nano_view_cloud, &nano_view_camera, &nano_view_playback, &nano_view_settings,
};

static lv_obj_t *s_root;
static lv_obj_t *s_bar;
static lv_obj_t *s_strip;
static lv_obj_t *s_menu;
static lv_obj_t *s_menu_row[MENU_ITEM_N];
static lv_obj_t *s_menu_hint;
static lv_timer_t *s_status_timer;
static nano_screen_t s_current = NANO_SCREEN_HOME;
static int s_menu_sel;
static bool s_menu_confirm;
static bool s_ready;

const char *nano_shell_screen_name(nano_screen_t screen)
{
    return (screen < NANO_SCREEN_COUNT) ? s_screen_names[screen] : "";
}

nano_screen_t nano_shell_current(void)
{
    return s_current;
}

bool nano_shell_is_active(void)
{
    return s_ready;
}

/* ==================== 系统菜单 ==================== */

static void menu_close(void)
{
    if (s_menu) {
        lv_obj_add_flag(s_menu, LV_OBJ_FLAG_HIDDEN);
    }
    s_menu_confirm = false;
}

static void menu_render(void)
{
    for (int i = 0; i < MENU_ITEM_N; i++) {
        bool on = (i == s_menu_sel);
        bool danger = (i >= 3);
        lv_obj_set_style_bg_color(s_menu_row[i], on ? (danger ? NANO_COL_WARN : NANO_COL_ACCENT) : NANO_COL_BG, 0);
        lv_obj_set_style_border_width(s_menu_row[i], on ? 0 : 1, 0);
        lv_obj_set_style_border_color(s_menu_row[i], NANO_COL_HAIR, 0);
    }
    if (s_menu_hint) {
        lv_label_set_text(s_menu_hint, s_menu_confirm ? "再次双击确认" : "单击选择 · 双击执行");
        lv_obj_set_style_text_color(s_menu_hint, s_menu_confirm ? NANO_COL_WARN : NANO_COL_DIM, 0);
    }
}

static void menu_exec(void)
{
    switch (s_menu_sel) {
    case 0:
        LISA_LOGI(TAG, "menu: hibernate");
        menu_close();
        (void)service_power_policy_request_hibernate();
        break;
    case 1:
        LISA_LOGI(TAG, "menu: soft reboot");
        power_reboot_soft();
        break;
    case 2:
        LISA_LOGI(TAG, "menu: clear saved Wi-Fi credentials");
        sys_wifi_clear_saved_aps();
        menu_close();
        break;
    case 3:
        LISA_LOGI(TAG, "menu: factory reset");
        button_factory_reset();
        break;
    case 4:
        LISA_LOGI(TAG, "menu: shutdown (只在拔掉 USB 后才真正断电)");
        power_shutdown();
        break;
    default:
        break;
    }
}

static void menu_open(void)
{
    if (!s_menu) {
        return;
    }
    s_menu_sel = 0;
    s_menu_confirm = false;
    menu_render();
    lv_obj_clear_flag(s_menu, LV_OBJ_FLAG_HIDDEN);
}

static void menu_key(voice_msg_button_action_t action)
{
    switch (action) {
    case VOICE_MSG_BUTTON_ACTION_CLICK:
        s_menu_sel = (s_menu_sel + 1) % MENU_ITEM_N;
        s_menu_confirm = false;
        menu_render();
        break;
    case VOICE_MSG_BUTTON_ACTION_DOUBLE_CLICK:
        if (s_menu_sel >= 2 && !s_menu_confirm) {
            s_menu_confirm = true;
            menu_render();
        } else {
            menu_exec();
        }
        break;
    case VOICE_MSG_BUTTON_ACTION_LONG_UP:
        menu_close();
        break;
    default:
        break;
    }
}

static void menu_build(lv_obj_t *parent)
{
    lv_obj_t *mask = lv_obj_create(parent);

    nano_style_flat(mask, NANO_COL_BG);
    lv_obj_set_style_bg_opa(mask, LV_OPA_80, 0);
    lv_obj_set_size(mask, NANO_W, NANO_H);
    lv_obj_set_pos(mask, 0, 0);

    for (int i = 0; i < MENU_ITEM_N; i++) {
        lv_obj_t *row = lv_obj_create(mask);
        lv_obj_t *label;

        nano_style_flat(row, NANO_COL_BG);
        lv_obj_set_size(row, NANO_W - 28, 26);
        lv_obj_set_pos(row, 14, 46 + i * 29);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);

        label = lv_label_create(row);
        lv_obj_set_style_text_font(label, &lv_font_chinese_16, 0);
        lv_obj_set_style_text_color(label, NANO_COL_FG, 0);
        lv_label_set_text(label, s_menu_names[i]);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 10, 0);
        s_menu_row[i] = row;
    }

    s_menu_hint = lv_label_create(mask);
    lv_obj_set_style_text_font(s_menu_hint, &lv_font_chinese_16, 0);
    lv_obj_set_style_text_color(s_menu_hint, NANO_COL_DIM, 0);
    lv_label_set_text(s_menu_hint, "单击选择 · 双击执行");
    lv_obj_align(s_menu_hint, LV_ALIGN_TOP_MID, 0, 46 + MENU_ITEM_N * 29 + 6);

    s_menu = mask;
    lv_obj_add_flag(mask, LV_OBJ_FLAG_HIDDEN);
}

/* ==================== 切屏 ==================== */

static void strip_exec(void *obj, int32_t v)
{
    lv_obj_set_x((lv_obj_t *)obj, v);
}

static void screen_hide(nano_screen_t screen)
{
    if (screen != NANO_SCREEN_HOME && s_views[screen] && s_views[screen]->hide) {
        s_views[screen]->hide();
    }
}

static void screen_show(nano_screen_t screen)
{
    if (screen == NANO_SCREEN_HOME) {
        nano_home_show();
    } else if (s_views[screen] && s_views[screen]->show) {
        s_views[screen]->show();
    }
}

void nano_shell_goto(nano_screen_t target)
{
    nano_screen_t from = s_current;
    lv_anim_t anim;

    if (target >= NANO_SCREEN_COUNT || target == from || !s_ready) {
        return;
    }

    screen_hide(from);
    s_current = target;

    lv_anim_init(&anim);
    lv_anim_set_var(&anim, s_strip);
    lv_anim_set_values(&anim, -(int32_t)from * PAGE_W, -(int32_t)target * PAGE_W);
    lv_anim_set_time(&anim, 200);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&anim, strip_exec);
    lv_anim_start(&anim);

    screen_show(target);
    nano_status_bar_set_screen(s_bar, s_screen_tags[target]);
}

/* ==================== 按键 ==================== */

bool nano_shell_owns_keys(voice_msg_button_action_t action)
{
    if (!s_ready || alarm_ring_is_active()) {
        return false;
    }
#ifdef CONFIG_OTA
    if (ota_manager_get_state() == OTA_STATE_PACKAGE_INFO ||
        ota_manager_get_state() == OTA_STATE_CHECKING ||
        ota_manager_get_state() == OTA_STATE_UPDATING) {
        return false;
    }
#endif
    switch (action) {
    case VOICE_MSG_BUTTON_ACTION_CLICK:
    case VOICE_MSG_BUTTON_ACTION_DOUBLE_CLICK:
    case VOICE_MSG_BUTTON_ACTION_TRIPLE_CLICK:
    case VOICE_MSG_BUTTON_ACTION_QUADRUPLE_CLICK:
    case VOICE_MSG_BUTTON_ACTION_QUINTUPLE_CLICK:
    case VOICE_MSG_BUTTON_ACTION_SEXTUPLE_CLICK:
    case VOICE_MSG_BUTTON_ACTION_SEPTUPLE_CLICK:
    case VOICE_MSG_BUTTON_ACTION_REPEAT_CLICK:
    case VOICE_MSG_BUTTON_ACTION_LONG_UP:
        return true;
    default:
        /* LONG_HOLD（3 秒）刻意放行给设备关机。 */
        return false;
    }
}

static void key_worker(void *arg, uint32_t len)
{
    voice_msg_button_action_t action = *(const voice_msg_button_action_t *)arg;

    (void)len;
    if (action == VOICE_MSG_BUTTON_ACTION_CLICK &&
        service_power_policy_handle_function_click()) {
        return;
    }
    if (lv_obj_has_flag(s_menu, LV_OBJ_FLAG_HIDDEN)) {
        switch (action) {
        case VOICE_MSG_BUTTON_ACTION_CLICK:
            if (s_current == NANO_SCREEN_HOME) {
                nano_home_next();
            } else {
                (void)s_views[s_current]->action(NANO_ACT_NEXT);
            }
            break;
        case VOICE_MSG_BUTTON_ACTION_DOUBLE_CLICK:
            if (s_current == NANO_SCREEN_HOME) {
                nano_shell_goto(nano_home_selected_screen());
            } else {
                (void)s_views[s_current]->action(NANO_ACT_SELECT);
            }
            break;
        case VOICE_MSG_BUTTON_ACTION_LONG_UP:
            if (s_current == NANO_SCREEN_HOME) {
                menu_open();
            } else if (!s_views[s_current]->action(NANO_ACT_BACK)) {
                nano_shell_goto(NANO_SCREEN_HOME);
            }
            break;
        default:
            break;
        }
    } else {
        menu_key(action);
    }
}

static void key_msg_cb(void *unused, uint32_t msg_id, void *data, uint32_t len, void *user_data)
{
    const voice_msg_button_evt_t *evt = (const voice_msg_button_evt_t *)data;
    voice_msg_button_action_t action;

    (void)unused;
    (void)msg_id;
    (void)user_data;

    if (!evt || len < sizeof(*evt) || evt->button_id != 0 || !nano_shell_owns_keys(evt->action)) {
        return;
    }
    action = evt->action;
    lisa_ui_invoke_ui_delayed(key_worker, &action, sizeof(action), 0);
}

/* ==================== 状态栏心跳 ==================== */

static void wifi_render(void)
{
    nano_wifi_state_t state;
    uint8_t level = 0;
    int rssi = 0;

    if (sys_wifi_is_connected()) {
        state = NANO_WIFI_CONNECTED;
        if (!sys_wifi_get_signal_quality(&rssi)) {
            rssi = -70;
        }
        level = (rssi >= -55) ? 3 : ((rssi >= -67) ? 2 : 1);
    } else {
        /* 存过凭据却没连上才是"连接中"；压根没配过的卡不该给这个暗示。 */
        state = sys_wifi_has_ap() ? NANO_WIFI_CONNECTING : NANO_WIFI_DISCONNECTED;
    }
    nano_status_bar_set_wifi(s_bar, state, level);
}

static void status_tick(lv_timer_t *timer)
{
    struct tm tm_buf;

    (void)timer;
    if (ls_sys_get_localtime(&tm_buf) == 0) {
        nano_status_bar_set_time(s_bar, tm_buf.tm_hour, tm_buf.tm_min);
    }
    nano_status_bar_set_battery(s_bar, battery_get_pct_raw(), battery_usb_plugged_stable_get());
    wifi_render();
    /* 灯跟着校时线程走，不跟消息走：开机时 Wi-Fi 往往比 shell 起得还早，
     * 靠订阅那个瞬态事件会稳定地错过它。 */
    nano_status_bar_set_syncing(s_bar, service_timesync_is_running());
}

/* ==================== 装载 ==================== */

static int shell_open(const struct lisa_ui_nav_scr *scr, void **data)
{
    lv_obj_t *screen = lv_scr_act();
    lv_obj_t *host;

    (void)scr;
    s_root = lv_obj_create(screen);
    nano_style_flat(s_root, NANO_COL_BG);
    lv_obj_set_size(s_root, NANO_W, NANO_H);
    lv_obj_set_pos(s_root, 0, 0);

    host = lv_obj_create(s_root);
    nano_style_flat(host, NANO_COL_BG);
    lv_obj_set_size(host, NANO_W, NANO_BODY_H);
    lv_obj_set_pos(host, 0, NANO_BAR_H);

    s_strip = lv_obj_create(host);
    nano_style_flat(s_strip, NANO_COL_BG);
    lv_obj_set_style_bg_opa(s_strip, LV_OPA_TRANSP, 0);
    lv_obj_set_size(s_strip, NANO_W * NANO_SCREEN_COUNT, NANO_BODY_H);
    lv_obj_set_pos(s_strip, 0, 0);

    for (int i = 0; i < NANO_SCREEN_COUNT; i++) {
        lv_obj_t *page = (i == NANO_SCREEN_HOME) ? nano_home_create(s_strip)
                                                 : s_views[i]->create(s_strip);
        lv_obj_set_x(page, i * NANO_W);
    }

    s_bar = nano_status_bar_create(s_root);
    menu_build(s_root);
    s_ready = true;

    *data = NULL;
    return 0;
}

static int shell_show(const struct lisa_ui_nav_scr *scr, void *data)
{
    (void)scr;
    (void)data;
    if (s_root) {
        lv_obj_clear_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    }
    nano_status_bar_set_screen(s_bar, s_screen_tags[s_current]);
    status_tick(NULL);
    if (!s_status_timer) {
        s_status_timer = lv_timer_create(status_tick, 1000, NULL);
    }
    screen_show(s_current);
    return 0;
}

static int shell_pause(const struct lisa_ui_nav_scr *scr, void *data)
{
    (void)scr;
    (void)data;
    if (s_status_timer) {
        lv_timer_pause(s_status_timer);
    }
    screen_hide(s_current);
    return 0;
}

static int shell_close(const struct lisa_ui_nav_scr *scr, void *data)
{
    (void)scr;
    (void)data;
    s_ready = false;
    if (s_status_timer) {
        lv_timer_del(s_status_timer);
        s_status_timer = NULL;
    }
    if (s_root) {
        lv_obj_del(s_root);
        s_root = NULL;
        s_bar = NULL;
        s_strip = NULL;
        s_menu = NULL;
    }
    return 0;
}

static const struct lisa_ui_nav_scr s_shell_scr = {
    .unique_id = LISA_UI_NAV_SCR_ID_HOME,
    .open = shell_open,
    .show = shell_show,
    .pause = shell_pause,
    .resume = shell_show,
    .close = shell_close,
};

static void install_worker(void *arg, uint32_t len)
{
    (void)arg;
    (void)len;
    nano_settings_init();
    service_timesync_start();
    lisa_ui_nav_scr_add(&s_shell_scr);
    lisa_ui_nav_scr_default_set(&s_shell_scr);
    LISA_LOGI(TAG, "nano shell 已接管屏幕");
}

int nano_shell_install(void)
{
    int ret = voice_msg_sub(VOICE_MSG_BUTTON_CHANGE, key_msg_cb, NULL);

    if (ret != 0) {
        LISA_LOGE(TAG, "按键订阅失败 ret=%d", ret);
        return ret;
    }
    return lisa_ui_invoke_ui_delayed(install_worker, NULL, 0, 0);
}
