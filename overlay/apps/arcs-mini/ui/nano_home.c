#include "nano_home.h"
#include "nano_icons.h"
#include "nano_theme.h"
#include "nano_assets.h"

#define HOME_APP_COUNT 5
#define HOME_TILE_W    96
#define HOME_TILE_H    76

static const char *const s_names[HOME_APP_COUNT] = {"时钟", "识云", "相机", "相册", "设置"};
static const lv_img_dsc_t *const s_icons[HOME_APP_COUNT] = {
    &nano_icon_clock, &nano_icon_cloud, &nano_icon_camera, &nano_icon_album, &nano_icon_settings,
};

static lv_obj_t *s_tile[HOME_APP_COUNT];
static lv_obj_t *s_label[HOME_APP_COUNT];
static lv_obj_t *s_icon[HOME_APP_COUNT];
static lv_obj_t *s_name;
static lv_obj_t *s_dot[2];
static int s_selected;

static void tile_place(int index, int page)
{
    int local = index - page * 4;

    lv_obj_set_pos(s_tile[index], 12 + (local % 2) * 120, 22 + (local / 2) * 82);
}

static void home_render(void)
{
    int page = s_selected / 4;

    for (int i = 0; i < HOME_APP_COUNT; i++) {
        bool visible = i / 4 == page;
        bool selected = i == s_selected;

        if (visible) {
            tile_place(i, page);
            lv_obj_clear_flag(s_tile[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_tile[i], LV_OBJ_FLAG_HIDDEN);
        }
        /* 焦点只用浅蓝描边表达；图标卡本身始终保持深色，避免整块蓝色抢画面。 */
        lv_obj_set_style_bg_color(s_tile[i], NANO_COL_GHOST, 0);
        lv_obj_set_style_bg_grad_color(s_tile[i], NANO_COL_GHOST, 0);
        lv_obj_set_style_bg_grad_dir(s_tile[i], LV_GRAD_DIR_NONE, 0);
        lv_obj_set_style_border_color(s_tile[i], selected ? NANO_COL_HILITE : NANO_COL_HAIR, 0);
        lv_obj_set_style_border_width(s_tile[i], selected ? 3 : 1, 0);
        lv_obj_set_style_text_color(s_label[i], NANO_COL_FG, 0);
        lv_obj_set_style_img_recolor(s_icon[i], NANO_COL_FG, 0);
        lv_obj_set_style_img_recolor_opa(s_icon[i], LV_OPA_COVER, 0);
    }

    for (int i = 0; i < 2; i++) {
        lv_obj_set_style_bg_color(s_dot[i], i == page ? NANO_COL_ACCENT : NANO_COL_HAIR, 0);
    }
    lv_label_set_text(s_name, s_names[s_selected]);
}

lv_obj_t *nano_home_create(lv_obj_t *parent)
{
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_t *title;

    nano_style_flat(root, NANO_COL_BG);
    lv_obj_set_size(root, NANO_W, NANO_BODY_H);

    title = lv_label_create(root);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(title, NANO_COL_DIM, 0);
    lv_label_set_text(title, "APPS");
    lv_obj_set_pos(title, 12, 3);

    for (int i = 0; i < HOME_APP_COUNT; i++) {
        s_tile[i] = lv_obj_create(root);
        nano_style_flat(s_tile[i], NANO_COL_GHOST);
        lv_obj_set_size(s_tile[i], HOME_TILE_W, HOME_TILE_H);
        lv_obj_set_style_radius(s_tile[i], 12, 0);

        s_icon[i] = lv_img_create(s_tile[i]);
        lv_img_set_src(s_icon[i], s_icons[i]);
        lv_obj_align(s_icon[i], LV_ALIGN_TOP_MID, 0, 9);

        s_label[i] = lv_label_create(s_tile[i]);
        lv_obj_set_style_text_font(s_label[i], &lv_font_chinese_16, 0);
        lv_label_set_text(s_label[i], s_names[i]);
        lv_obj_align(s_label[i], LV_ALIGN_BOTTOM_MID, 0, -7);
    }

    s_name = lv_label_create(root);
    lv_obj_set_style_text_font(s_name, &lv_font_chinese_16, 0);
    lv_obj_set_style_text_color(s_name, NANO_COL_FG, 0);
    lv_obj_align(s_name, LV_ALIGN_BOTTOM_MID, 0, -5);

    for (int i = 0; i < 2; i++) {
        s_dot[i] = lv_obj_create(root);
        nano_style_flat(s_dot[i], NANO_COL_HAIR);
        lv_obj_set_size(s_dot[i], 5, 5);
        lv_obj_set_style_radius(s_dot[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_align(s_dot[i], LV_ALIGN_BOTTOM_MID, (i == 0) ? -7 : 7, -25);
    }

    home_render();
    return root;
}

void nano_home_next(void)
{
    s_selected = (s_selected + 1) % HOME_APP_COUNT;
    home_render();
}

nano_screen_t nano_home_selected_screen(void)
{
    return (nano_screen_t)(NANO_SCREEN_CLOCK + s_selected);
}

void nano_home_show(void)
{
    home_render();
}
