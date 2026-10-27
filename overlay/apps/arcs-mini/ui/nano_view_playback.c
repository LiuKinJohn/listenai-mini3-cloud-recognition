#include "nano_shell.h"
#include "nano_theme.h"
#include "nano_assets.h"

#include "service_photo.h"

#include "lisa_log.h"
#include "sysheap.h"

#include <stdio.h>
#include <string.h>

#define TAG "nano.pb"

#define PB_FOOT_H   40
#define PB_AREA_H   (NANO_BODY_H - PB_FOOT_H)
#define PB_SLIDE_W  180
#define PB_SLIDE_H  135
#define PB_SLIDE_GAP 14
#define PB_PITCH    (PB_SLIDE_W + PB_SLIDE_GAP)
/* 中间那张左右各留 30px 黑边，邻图因此各露出 16px，剩下 14px 是真空隙。 */
#define PB_SIDE_PEEK ((NANO_W - PB_SLIDE_W) / 2)
#define PB_TRACK_X  (PB_SIDE_PEEK - PB_PITCH)
#define PB_SLIDE_PX ((size_t)PB_SLIDE_W * PB_SLIDE_H)
#define PB_DOT_MAX  8

static lv_obj_t *s_track;
static lv_obj_t *s_slide[3];
static lv_obj_t *s_pic[3];
static uint16_t *s_pic_buf[3];
static char s_pic_name[3][PHOTO_NAME_LEN];
static lv_obj_t *s_name;
static lv_obj_t *s_meta;
static lv_obj_t *s_dot[PB_DOT_MAX];
static lv_obj_t *s_empty;
static int s_index;
static int s_count;
static bool s_animating;
static photo_entry_t s_items[PHOTO_LIST_MAX];

static void pb_set_visible(lv_obj_t *obj, bool visible);
static void pb_render(void);

static int pb_wrap(int i)
{
    if (s_count <= 0) {
        return 0;
    }
    while (i < 0) {
        i += s_count;
    }
    return i % s_count;
}

/* 每次进这屏重扫一遍：卡是热插拔的，快门也可能刚往里塞了新文件。 */
static void pb_rescan(void)
{
    int n = service_photo_list(s_items, PHOTO_LIST_MAX);

    s_count = (n > 0) ? n : 0;
    if (s_index >= s_count) {
        s_index = 0;
    }
    for (int i = 0; i < 3; i++) {
        s_pic_name[i][0] = '\0';
    }
}

static void pb_slot_fill(int slot, int idx)
{
    const char *name = s_items[idx].name;

    if (s_pic_buf[slot] == NULL) {
        s_pic_buf[slot] = psram_malloc(PB_SLIDE_PX * sizeof(uint16_t));
        if (!s_pic_buf[slot]) {
            LISA_LOGE(TAG, "幻灯片缓冲分配失败");
            return;
        }
        lv_canvas_set_buffer(s_pic[slot], s_pic_buf[slot], PB_SLIDE_W, PB_SLIDE_H,
                             LV_IMG_CF_TRUE_COLOR);
        /* 没缓冲的 canvas 别丢给渲染器，先藏好。 */
        lv_obj_clear_flag(s_pic[slot], LV_OBJ_FLAG_HIDDEN);
        s_pic_name[slot][0] = '\0';
    }
    if (strcmp(s_pic_name[slot], name) == 0) {
        return;
    }
    strncpy(s_pic_name[slot], name, PHOTO_NAME_LEN - 1);
    s_pic_name[slot][PHOTO_NAME_LEN - 1] = '\0';

    if (service_photo_render(name, s_pic_buf[slot], PB_SLIDE_W, PB_SLIDE_H) != 0) {
        memset(s_pic_buf[slot], 0x08, PB_SLIDE_PX * sizeof(uint16_t));
        LISA_LOGW(TAG, "%s 解码失败", name);
    }
    lv_obj_invalidate(s_pic[slot]);
}

static void pb_render(void)
{
    for (int slot = 0; slot < 3; slot++) {
        int idx = pb_wrap(s_index + slot - 1);

        if (s_count > 0) {
            pb_slot_fill(slot, idx);
        }
        lv_obj_set_style_border_width(s_slide[slot], slot == 1 ? 1 : 0, 0);
        lv_obj_set_style_border_color(s_slide[slot], NANO_COL_FG, 0);
        lv_obj_set_style_border_opa(s_slide[slot], slot == 1 ? LV_OPA_30 : LV_OPA_TRANSP, 0);
        /* 邻图只做压暗，不做换色：一眼能看出哪张是当前张。 */
        lv_obj_set_style_opa(s_pic[slot], slot == 1 ? LV_OPA_COVER : LV_OPA_60, 0);
    }
    lv_obj_move_foreground(s_slide[1]);

    if (s_count > 0) {
        lv_label_set_text_fmt(s_name, "%s", s_items[pb_wrap(s_index)].name);
        lv_label_set_text_fmt(s_meta, "240x180  %u KB",
                              (unsigned)(s_items[pb_wrap(s_index)].size / 1024U));
    } else {
        lv_label_set_text(s_name, "");
        lv_label_set_text(s_meta, "");
    }

    for (int i = 0; i < PB_DOT_MAX; i++) {
        bool on = (i < s_count);
        bool active = on && (i == pb_wrap(s_index));

        pb_set_visible(s_dot[i], on);
        lv_obj_set_style_bg_color(s_dot[i], active ? NANO_COL_ACCENT : NANO_COL_HAIR, 0);
        lv_obj_set_width(s_dot[i], active ? 14 : 4);
    }
}

static void pb_anim_exec(void *obj, int32_t v)
{
    lv_obj_set_x((lv_obj_t *)obj, v);
}

static void pb_anim_done(lv_anim_t *a)
{
    (void)a;
    s_animating = false;
    lv_obj_set_x(s_track, PB_TRACK_X);
    pb_render();
}

static void pb_slide_to(int direction)
{
    int32_t to;

    if (s_animating || s_count <= 0) {
        return;
    }
    s_index = pb_wrap(s_index + direction);
    s_animating = true;
    to = PB_TRACK_X - direction * PB_PITCH;

    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, s_track);
    lv_anim_set_values(&anim, PB_TRACK_X, to);
    lv_anim_set_time(&anim, 180);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&anim, pb_anim_exec);
    lv_anim_set_ready_cb(&anim, pb_anim_done);
    lv_anim_start(&anim);
}

static lv_obj_t *playback_create(lv_obj_t *parent)
{
    lv_obj_t *page = lv_obj_create(parent);
    lv_coord_t slide_y = (PB_AREA_H - PB_SLIDE_H) / 2;

    nano_style_flat(page, NANO_COL_BG);
    lv_obj_set_size(page, NANO_W, NANO_BODY_H);

    s_track = lv_obj_create(page);
    nano_style_flat(s_track, NANO_COL_BG);
    lv_obj_set_style_bg_opa(s_track, LV_OPA_TRANSP, 0);
    lv_obj_set_size(s_track, PB_PITCH * 2 + PB_SLIDE_W, PB_SLIDE_H);
    lv_obj_set_pos(s_track, PB_TRACK_X, slide_y);

    for (int i = 0; i < 3; i++) {
        s_slide[i] = lv_obj_create(s_track);
        nano_style_flat(s_slide[i], NANO_COL_BG);
        lv_obj_set_size(s_slide[i], PB_SLIDE_W, PB_SLIDE_H);
        lv_obj_set_pos(s_slide[i], i * PB_PITCH, 0);
        lv_obj_set_style_pad_all(s_slide[i], 0, 0);

        s_pic[i] = lv_canvas_create(s_slide[i]);
        lv_obj_set_size(s_pic[i], PB_SLIDE_W, PB_SLIDE_H);
        lv_obj_set_pos(s_pic[i], 0, 0);
        lv_obj_add_flag(s_pic[i], LV_OBJ_FLAG_HIDDEN);
        s_pic_name[i][0] = '\0';
    }

    s_empty = lv_label_create(page);
    lv_obj_set_style_text_font(s_empty, &lv_font_chinese_16, 0);
    lv_obj_set_style_text_color(s_empty, NANO_COL_DIM, 0);
    lv_label_set_text(s_empty, "请插入 TF 卡");
    lv_obj_align(s_empty, LV_ALIGN_CENTER, 0, -20);

    lv_obj_t *line = nano_hairline_create(page);
    lv_obj_set_size(line, NANO_W - 24, 1);
    lv_obj_set_pos(line, 12, PB_AREA_H);

    s_name = lv_label_create(page);
    lv_obj_set_style_text_font(s_name, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_name, NANO_COL_FG, 0);
    lv_label_set_text(s_name, "");
    lv_obj_align(s_name, LV_ALIGN_TOP_MID, 0, PB_AREA_H + 6);

    s_meta = lv_label_create(page);
    lv_obj_set_style_text_font(s_meta, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_meta, NANO_COL_DIM, 0);
    lv_label_set_text(s_meta, "");
    lv_obj_align(s_meta, LV_ALIGN_TOP_MID, 0, PB_AREA_H + 24);

    for (int i = 0; i < PB_DOT_MAX; i++) {
        s_dot[i] = lv_obj_create(page);
        nano_style_flat(s_dot[i], NANO_COL_HAIR);
        lv_obj_set_size(s_dot[i], 4, 4);
        lv_obj_set_style_radius(s_dot[i], 2, 0);
        lv_obj_align(s_dot[i], LV_ALIGN_TOP_LEFT, 12 + i * 8, PB_AREA_H + 12);
    }

    return page;
}

static bool playback_action(nano_action_t act)
{
    if (act == NANO_ACT_NEXT) {
        pb_slide_to(1);
        return true;
    }
    if (act == NANO_ACT_SELECT) {
        return true;
    }
    return false;
}

static void pb_set_visible(lv_obj_t *obj, bool visible)
{
    if (visible) {
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

static void playback_show(void)
{
    pb_rescan();
    pb_set_visible(s_empty, s_count <= 0);
    pb_set_visible(s_track, s_count > 0);
    if (s_count <= 0) {
        lv_label_set_text(s_empty, service_photo_card_ready() ? "卡上还没有照片" : "请插入 TF 卡");
    }
    pb_render();
}

static void playback_hide(void)
{
}

const nano_view_t nano_view_playback = {
    .create = playback_create,
    .action = playback_action,
    .show = playback_show,
    .hide = playback_hide,
};
