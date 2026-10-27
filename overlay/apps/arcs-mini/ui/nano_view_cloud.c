#include "nano_shell.h"
#include "nano_assets.h"
#include "nano_cam.h"
#include "nano_theme.h"

#include "service_cloud.h"
#include "service_photo.h"

#include "lisa_log.h"
#include "sysheap.h"

#include <stdio.h>
#include <string.h>

#define TAG "nano.cloud.view"
#define CLOUD_MENU_COUNT 2
#define BOOK_W 112
#define BOOK_H 84

typedef enum {
    CLOUD_VIEW_MENU = 0,
    CLOUD_VIEW_CAPTURE,
    CLOUD_VIEW_RESULT,
    CLOUD_VIEW_DETAIL,
    CLOUD_VIEW_BOOK,
} cloud_view_state_t;

static lv_obj_t *s_page;
static lv_obj_t *s_menu;
static lv_obj_t *s_menu_row[CLOUD_MENU_COUNT];
static lv_obj_t *s_menu_label[CLOUD_MENU_COUNT];
static lv_obj_t *s_capture;
static lv_obj_t *s_canvas;
static lv_obj_t *s_capture_notice;
static lv_obj_t *s_capture_hint;
static lv_obj_t *s_result;
static lv_obj_t *s_result_type;
static lv_obj_t *s_result_conf;
static lv_obj_t *s_result_place;
static lv_obj_t *s_result_weather;
static lv_obj_t *s_result_evidence;
static lv_obj_t *s_detail;
static lv_obj_t *s_detail_evidence;
static lv_obj_t *s_book;
static lv_obj_t *s_book_canvas;
static lv_obj_t *s_book_type;
static lv_obj_t *s_book_meta;
static lv_obj_t *s_book_detail;
static lv_obj_t *s_book_count;
static lv_obj_t *s_book_empty;
static lv_timer_t *s_timer;
static uint16_t *s_book_buf;
static cloud_record_t *s_records;
static int s_record_count;
static int s_record_index;
static int s_menu_sel;
static bool s_wait_save;
static bool s_wait_cloud;
static cloud_view_state_t s_view = CLOUD_VIEW_MENU;

static void set_visible(lv_obj_t *obj, bool visible)
{
    if (visible) lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

static void show_panel(cloud_view_state_t view)
{
    s_view = view;
    set_visible(s_menu, view == CLOUD_VIEW_MENU);
    set_visible(s_capture, view == CLOUD_VIEW_CAPTURE);
    set_visible(s_result, view == CLOUD_VIEW_RESULT);
    set_visible(s_detail, view == CLOUD_VIEW_DETAIL);
    set_visible(s_book, view == CLOUD_VIEW_BOOK);
}

static void menu_render(void)
{
    for (int i = 0; i < CLOUD_MENU_COUNT; ++i) {
        bool on = i == s_menu_sel;
        lv_obj_set_style_bg_color(s_menu_row[i], on ? NANO_COL_ACCENT : NANO_COL_GHOST, 0);
        lv_obj_set_style_text_color(s_menu_label[i], on ? NANO_COL_ON_SEL : NANO_COL_FG, 0);
    }
}

static void result_render(const cloud_record_t *record)
{
    char buf[64];
    bool ok = record && record->success;

    lv_label_set_text(s_result_type, ok ? (!strcmp(record->type, "unknown") ? "暂无法识别" : record->type) : "识别未完成");
    if (ok && record->confidence_description[0]) snprintf(buf, sizeof(buf), "模型自评 %s",
        !strcmp(record->confidence_description, "high") ? "高" : !strcmp(record->confidence_description, "medium") ? "中" : "低");
    else if (ok) snprintf(buf, sizeof(buf), "可信度 %u%%", (unsigned)record->confidence);
    else snprintf(buf, sizeof(buf), "%s", (record && record->error[0]) ? record->error : "未知错误");
    lv_label_set_text(s_result_conf, buf);
    lv_label_set_text(s_result_place, (record && record->location[0]) ? record->location : "位置待获取");
    lv_label_set_text(s_result_weather, (record && record->weather[0]) ? record->weather : "气象数据待获取");
    lv_label_set_text(s_result_evidence,
                      (record && record->evidence[0]) ? record->evidence : "完成云端绑定后显示辅助判别信息");
    lv_label_set_text(s_detail_evidence,
                      (record && record->evidence[0]) ? record->evidence : "暂无辅助判别信息");
}

static void book_render(void)
{
    cloud_record_t *r;
    char buf[64];
    bool image_ok = false;

    set_visible(s_book_empty, s_record_count <= 0);
    set_visible(s_book_canvas, s_record_count > 0);
    set_visible(s_book_type, s_record_count > 0);
    set_visible(s_book_meta, s_record_count > 0);
    set_visible(s_book_detail, s_record_count > 0);
    set_visible(s_book_count, s_record_count > 0);
    if (s_record_count <= 0) return;

    r = &s_records[s_record_index];
    if (!s_book_buf) {
        s_book_buf = psram_malloc((size_t)BOOK_W * BOOK_H * sizeof(uint16_t));
        if (s_book_buf) lv_canvas_set_buffer(s_book_canvas, s_book_buf, BOOK_W, BOOK_H, LV_IMG_CF_TRUE_COLOR);
    }
    if (s_book_buf && r->image[0])
        image_ok = service_photo_render(r->image, s_book_buf, BOOK_W, BOOK_H) == 0;
    if (s_book_buf && !image_ok)
        memset(s_book_buf, 0, (size_t)BOOK_W * BOOK_H * sizeof(uint16_t));
    if (s_book_buf) lv_obj_invalidate(s_book_canvas);

    lv_label_set_text(s_book_type, r->success ? (!strcmp(r->type, "unknown") ? "暂无法识别" : r->type) : "未完成");
    if (r->success && r->confidence_description[0]) snprintf(buf, sizeof(buf), "%s · %s",
        !strcmp(r->confidence_description, "high") ? "自评高" : !strcmp(r->confidence_description, "medium") ? "自评中" : "自评低", r->captured_at);
    else if (r->success) snprintf(buf, sizeof(buf), "%u%% · %s", (unsigned)r->confidence, r->captured_at);
    else snprintf(buf, sizeof(buf), "%s", r->error[0] ? r->error : "识别失败");
    lv_label_set_text(s_book_meta, buf);
    lv_label_set_text(s_book_detail, r->evidence[0] ? r->evidence : r->image);
    lv_label_set_text_fmt(s_book_count, "%d / %d", s_record_index + 1, s_record_count);
}

static void book_open(void)
{
    if (!s_records) {
        s_records = psram_malloc((size_t)CLOUD_RECORD_MAX * sizeof(*s_records));
        if (s_records) memset(s_records, 0, (size_t)CLOUD_RECORD_MAX * sizeof(*s_records));
    }
    if (!s_records) {
        s_record_count = 0;
        lv_label_set_text(s_book_empty, "云册内存不足");
        book_render();
        show_panel(CLOUD_VIEW_BOOK);
        return;
    }
    lv_label_set_text(s_book_empty, "云册还是空的");
    s_record_count = service_cloud_list(s_records, CLOUD_RECORD_MAX);
    s_record_index = 0;
    book_render();
    show_panel(CLOUD_VIEW_BOOK);
}

static void capture_open(void)
{
    show_panel(CLOUD_VIEW_CAPTURE);
    s_wait_save = false;
    s_wait_cloud = false;
    lv_label_set_text(s_capture_hint, service_cloud_endpoint_configured() ? "双击拍摄并识别" : "双击拍摄 · 云端待绑定");
    if (nano_cam_attach(s_canvas) != 0 || nano_cam_start_raw() != 0) {
        lv_label_set_text(s_capture_notice, "摄像头启动失败");
        set_visible(s_capture_notice, true);
    } else {
        set_visible(s_capture_notice, false);
    }
}

static void capture_close(void)
{
    nano_cam_stop();
    s_wait_save = false;
}

static void cloud_tick(lv_timer_t *timer)
{
    cloud_record_t latest;
    (void)timer;

    if (s_view == CLOUD_VIEW_CAPTURE && s_wait_save && !nano_cam_is_saving()) {
        s_wait_save = false;
        if (!nano_cam_last_save_ok()) {
            lv_label_set_text(s_capture_hint, "照片保存失败");
        } else if (service_cloud_start(nano_cam_last_saved_name()) == 0) {
            s_wait_cloud = true;
            lv_label_set_text(s_capture_hint, "正在识别...");
        } else {
            lv_label_set_text(s_capture_hint, "识别任务忙");
        }
    }
    if (s_view == CLOUD_VIEW_CAPTURE && s_wait_cloud && service_cloud_latest(&latest)) {
        s_wait_cloud = false;
        capture_close();
        result_render(&latest);
        show_panel(CLOUD_VIEW_RESULT);
    }
}

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color,
                            const char *text, int x, int y)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_label_set_text(label, text);
    lv_obj_set_pos(label, x, y);
    return label;
}

static lv_obj_t *cloud_create(lv_obj_t *parent)
{
    static const char *const menu_names[CLOUD_MENU_COUNT] = {"开始识别", "云册"};
    s_page = lv_obj_create(parent);
    nano_style_flat(s_page, NANO_COL_BG);
    lv_obj_set_size(s_page, NANO_W, NANO_BODY_H);

    s_menu = lv_obj_create(s_page);
    nano_style_flat(s_menu, NANO_COL_BG);
    lv_obj_set_size(s_menu, NANO_W, NANO_BODY_H);
    make_label(s_menu, &lv_font_montserrat_14, NANO_COL_DIM, "CLOUD", 12, 5);
    for (int i = 0; i < CLOUD_MENU_COUNT; ++i) {
        s_menu_row[i] = lv_obj_create(s_menu);
        nano_style_flat(s_menu_row[i], NANO_COL_GHOST);
        lv_obj_set_size(s_menu_row[i], 216, 58);
        lv_obj_set_pos(s_menu_row[i], 12, 36 + i * 66);
        lv_obj_set_style_radius(s_menu_row[i], 12, 0);
        s_menu_label[i] = lv_label_create(s_menu_row[i]);
        lv_obj_set_style_text_font(s_menu_label[i], &lv_font_chinese_16, 0);
        lv_label_set_text(s_menu_label[i], menu_names[i]);
        lv_obj_align(s_menu_label[i], LV_ALIGN_LEFT_MID, 16, 0);
        lv_obj_t *chev = lv_label_create(s_menu_row[i]);
        lv_obj_set_style_text_font(chev, &lv_font_montserrat_14, 0);
        lv_label_set_text(chev, ">");
        lv_obj_align(chev, LV_ALIGN_RIGHT_MID, -16, 0);
    }
    make_label(s_menu, &lv_font_chinese_16, NANO_COL_DIM, "单击选择 · 双击打开", 38, 185);

    s_capture = lv_obj_create(s_page);
    nano_style_flat(s_capture, NANO_COL_BG);
    lv_obj_set_size(s_capture, NANO_W, NANO_BODY_H);
    s_canvas = lv_canvas_create(s_capture);
    lv_obj_set_size(s_canvas, 240, 180);
    lv_obj_set_pos(s_canvas, 0, 0);
    s_capture_notice = lv_label_create(s_canvas);
    lv_obj_set_style_text_font(s_capture_notice, &lv_font_chinese_16, 0);
    lv_obj_set_style_text_color(s_capture_notice, NANO_COL_DIM, 0);
    lv_label_set_text(s_capture_notice, "摄像头未就绪");
    lv_obj_align(s_capture_notice, LV_ALIGN_CENTER, 0, 0);
    s_capture_hint = make_label(s_capture, &lv_font_chinese_16, NANO_COL_ACCENT,
                                "双击拍摄并识别", 28, 190);

    s_result = lv_obj_create(s_page);
    nano_style_flat(s_result, NANO_COL_BG);
    lv_obj_set_size(s_result, NANO_W, NANO_BODY_H);
    make_label(s_result, &lv_font_montserrat_14, NANO_COL_DIM, "RESULT", 12, 5);
    s_result_type = make_label(s_result, &lv_font_chinese_16, NANO_COL_FG, "—", 12, 34);
    s_result_conf = make_label(s_result, &lv_font_chinese_16, NANO_COL_ACCENT, "—", 12, 69);
    s_result_place = make_label(s_result, &lv_font_chinese_16, NANO_COL_FG, "—", 12, 101);
    s_result_weather = make_label(s_result, &lv_font_chinese_16, NANO_COL_DIM, "—", 12, 128);
    s_result_evidence = make_label(s_result, &lv_font_chinese_16, NANO_COL_DIM, "—", 12, 157);
    lv_obj_set_size(s_result_evidence, 216, 32);
    lv_label_set_long_mode(s_result_evidence, LV_LABEL_LONG_DOT);
    make_label(s_result, &lv_font_chinese_16, NANO_COL_DIM, "单击详情 · 长按返回", 26, 198);

    s_detail = lv_obj_create(s_page);
    nano_style_flat(s_detail, NANO_COL_BG);
    lv_obj_set_size(s_detail, NANO_W, NANO_BODY_H);
    make_label(s_detail, &lv_font_chinese_16, NANO_COL_FG, "识别依据", 12, 8);
    s_detail_evidence = make_label(s_detail, &lv_font_chinese_16, NANO_COL_DIM, "—", 12, 38);
    lv_obj_set_size(s_detail_evidence, 216, 140);
    lv_label_set_long_mode(s_detail_evidence, LV_LABEL_LONG_DOT);
    make_label(s_detail, &lv_font_chinese_16, NANO_COL_DIM, "单击结果 · 长按返回", 26, 198);

    s_book = lv_obj_create(s_page);
    nano_style_flat(s_book, NANO_COL_BG);
    lv_obj_set_size(s_book, NANO_W, NANO_BODY_H);
    make_label(s_book, &lv_font_montserrat_14, NANO_COL_DIM, "CLOUD BOOK", 12, 5);
    s_book_canvas = lv_canvas_create(s_book);
    lv_obj_set_size(s_book_canvas, BOOK_W, BOOK_H);
    lv_obj_set_pos(s_book_canvas, 12, 31);
    s_book_type = make_label(s_book, &lv_font_chinese_16, NANO_COL_FG, "—", 134, 37);
    s_book_meta = make_label(s_book, &lv_font_chinese_16, NANO_COL_ACCENT, "—", 134, 67);
    lv_obj_set_width(s_book_meta, 94);
    lv_label_set_long_mode(s_book_meta, LV_LABEL_LONG_WRAP);
    s_book_detail = make_label(s_book, &lv_font_chinese_16, NANO_COL_DIM, "—", 12, 127);
    lv_obj_set_width(s_book_detail, 216);
    lv_label_set_long_mode(s_book_detail, LV_LABEL_LONG_WRAP);
    s_book_count = make_label(s_book, &lv_font_montserrat_14, NANO_COL_DIM, "", 12, 196);
    s_book_empty = make_label(s_book, &lv_font_chinese_16, NANO_COL_DIM,
                              "云册还是空的", 66, 92);

    menu_render();
    show_panel(CLOUD_VIEW_MENU);
    s_timer = lv_timer_create(cloud_tick, 200, NULL);
    lv_timer_pause(s_timer);
    return s_page;
}

static bool cloud_action(nano_action_t action)
{
    if (action == NANO_ACT_BACK) {
        if (s_view == CLOUD_VIEW_MENU) return false;
        if (s_view == CLOUD_VIEW_CAPTURE) capture_close();
        show_panel(CLOUD_VIEW_MENU);
        return true;
    }
    if (s_view == CLOUD_VIEW_MENU) {
        if (action == NANO_ACT_NEXT) {
            s_menu_sel = (s_menu_sel + 1) % CLOUD_MENU_COUNT;
            menu_render();
        } else if (action == NANO_ACT_SELECT) {
            if (s_menu_sel == 0) capture_open();
            else book_open();
        }
        return true;
    }
    if (s_view == CLOUD_VIEW_CAPTURE && action == NANO_ACT_SELECT) {
        int rc = nano_cam_shutter();
        if (rc == 0) {
            s_wait_save = true;
            lv_label_set_text(s_capture_hint, "正在保存照片...");
        } else if (rc == -1) lv_label_set_text(s_capture_hint, "请插入 TF 卡");
        else lv_label_set_text(s_capture_hint, "相机忙，请稍后");
        LISA_LOGI(TAG, "识云快门 rc=%d", rc);
        return true;
    }
    if ((s_view == CLOUD_VIEW_RESULT || s_view == CLOUD_VIEW_DETAIL) && action == NANO_ACT_NEXT) {
        show_panel(s_view == CLOUD_VIEW_RESULT ? CLOUD_VIEW_DETAIL : CLOUD_VIEW_RESULT);
        return true;
    }
    if (s_view == CLOUD_VIEW_BOOK && action == NANO_ACT_NEXT) {
        if (s_record_count > 0) s_record_index = (s_record_index + 1) % s_record_count;
        book_render();
        return true;
    }
    return action == NANO_ACT_SELECT || action == NANO_ACT_NEXT;
}

static void cloud_show(void)
{
    s_menu_sel = 0;
    menu_render();
    show_panel(CLOUD_VIEW_MENU);
    if (s_timer) lv_timer_resume(s_timer);
}

static void cloud_hide(void)
{
    if (s_view == CLOUD_VIEW_CAPTURE) capture_close();
    if (s_timer) lv_timer_pause(s_timer);
    show_panel(CLOUD_VIEW_MENU);
}

const nano_view_t nano_view_cloud = {
    .create = cloud_create,
    .action = cloud_action,
    .show = cloud_show,
    .hide = cloud_hide,
};
