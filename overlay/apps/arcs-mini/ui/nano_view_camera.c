#include "nano_shell.h"
#include "nano_theme.h"
#include "nano_assets.h"
#include "nano_cam.h"
#include "nano_fx.h"
#include "nano_settings.h"

#include "lisa_log.h"

#include <stdio.h>

#define TAG "nano.cam.view"

#define CAM_FOOT_H   38
#define CAM_LINE1_Y  (NANO_BODY_H - CAM_FOOT_H + 3)
#define CAM_LINE2_Y  (NANO_BODY_H - CAM_FOOT_H + 20)

static const char *const HINT_SHUTTER = "拍照";
static const char *const HINT_EFFECT = "切换特效";

static lv_obj_t *s_canvas;
static lv_obj_t *s_effect;
static lv_obj_t *s_notice;
static lv_obj_t *s_hint;
static lv_timer_t *s_timer;
static bool s_was_saving;
static int s_control;

static void camera_hint_save_result(void)
{
    char buf[40];

    if (nano_cam_last_save_ok()) {
        snprintf(buf, sizeof(buf), "%s  %u KB", nano_cam_last_saved_name(),
                 (unsigned)(nano_cam_last_saved_bytes() / 1024U));
    } else {
        snprintf(buf, sizeof(buf), "写入失败");
    }
    lv_label_set_text(s_hint, buf);
}

static void camera_tick(lv_timer_t *t)
{
    (void)t;
    if (nano_cam_is_saving()) {
        s_was_saving = true;
        return;
    }
    if (s_was_saving) {
        s_was_saving = false;
        camera_hint_save_result();
    }
}

static void render_effect(void)
{
    char buf[40];

    if (nano_settings()->fx_lock) {
        snprintf(buf, sizeof(buf), "%s · 锁", nano_fx_name(nano_cam_effect()));
    } else {
        snprintf(buf, sizeof(buf), "%s", nano_fx_name(nano_cam_effect()));
    }
    lv_label_set_text(s_effect, buf);
}

static void render_control(void)
{
    lv_label_set_text(s_hint, s_control == 0 ? HINT_SHUTTER : HINT_EFFECT);
    lv_obj_set_style_text_color(s_hint, NANO_COL_ACCENT, 0);
}

static lv_obj_t *camera_create(lv_obj_t *parent)
{
    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_t *line;
    lv_obj_t *card;
    lv_coord_t vf_h = NANO_BODY_H - CAM_FOOT_H;

    nano_style_flat(page, NANO_COL_BG);
    lv_obj_set_size(page, NANO_W, NANO_BODY_H);

    s_canvas = lv_canvas_create(page);
    lv_obj_set_pos(s_canvas, 0, 0);
    lv_obj_set_size(s_canvas, NANO_FX_IMG_W, NANO_FX_IMG_H);

    s_notice = lv_label_create(s_canvas);
    lv_obj_set_style_text_font(s_notice, &lv_font_chinese_16, 0);
    lv_obj_set_style_text_color(s_notice, NANO_COL_DIM, 0);
    lv_label_set_text(s_notice, "摄像头未就绪");
    lv_obj_align(s_notice, LV_ALIGN_CENTER, 0, 0);

    line = nano_hairline_create(page);
    lv_obj_set_size(line, NANO_W, 1);
    lv_obj_set_pos(line, 0, vf_h);

    s_effect = lv_label_create(page);
    lv_obj_set_style_text_font(s_effect, &lv_font_chinese_16, 0);
    lv_obj_set_style_text_color(s_effect, NANO_COL_ACCENT, 0);
    lv_label_set_text(s_effect, "—");
    lv_obj_set_pos(s_effect, 10, CAM_LINE1_Y);

    card = lv_img_create(page);
    lv_img_set_src(card, &icons_ic_status_tf_card_png);
    lv_obj_align(card, LV_ALIGN_TOP_RIGHT, -10, CAM_LINE1_Y - 1);

    s_hint = lv_label_create(page);
    lv_obj_set_style_text_font(s_hint, &lv_font_chinese_16, 0);
    lv_obj_set_style_text_color(s_hint, NANO_COL_DIM, 0);
    lv_label_set_text(s_hint, HINT_SHUTTER);
    lv_obj_align(s_hint, LV_ALIGN_TOP_MID, 0, CAM_LINE2_Y);

    return page;
}

static bool camera_action(nano_action_t act)
{
    if (act == NANO_ACT_NEXT) {
        s_control = (s_control + 1) % 2;
        render_control();
        return true;
    }
    if (act == NANO_ACT_SELECT && s_control == 1) {
        if (nano_cam_next_effect()) {
            render_effect();
            render_control();
        } else {
            lv_label_set_text(s_hint, "特效已锁定");
        }
        return true;
    }
    if (act == NANO_ACT_SELECT) {
        int rc;
        const char *msg;

        if (!nano_cam_running()) {
            lv_label_set_text(s_hint, "没有取景，拍不了");
            return true;
        }
        rc = nano_cam_shutter();
        switch (rc) {
        case 0:
            s_was_saving = true;
            msg = "正在写入...";
            break;
        case -1:
            msg = "没有卡，插 TF 卡才能拍";
            break;
        case -3:
            msg = "内存不够，拍不了";
            break;
        default:
            msg = "上一张还在写";
            break;
        }
        LISA_LOGI(TAG, "快门 rc=%d fx=%d", rc, (int)nano_cam_effect());
        lv_label_set_text(s_hint, msg);
        return true;
    }
    return false;
}

static void camera_show(void)
{
    if (nano_cam_attach(s_canvas) != 0) {
        lv_label_set_text(s_notice, "显存不够");
        lv_obj_clear_flag(s_notice, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    render_effect();

    if (nano_cam_start() != 0) {
        lv_label_set_text(s_notice, "取景启动失败");
        lv_obj_clear_flag(s_notice, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_add_flag(s_notice, LV_OBJ_FLAG_HIDDEN);
    s_control = 0;
    render_control();
    if (s_timer) {
        lv_timer_resume(s_timer);
    } else {
        s_timer = lv_timer_create(camera_tick, 200, NULL);
    }
}

static void camera_hide(void)
{
    if (s_timer) {
        lv_timer_pause(s_timer);
    }
    nano_cam_stop();
}

const nano_view_t nano_view_camera = {
    .create = camera_create,
    .action = camera_action,
    .show = camera_show,
    .hide = camera_hide,
};
