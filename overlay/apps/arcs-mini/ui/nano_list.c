#include "nano_list.h"
#include "nano_theme.h"
#include "nano_assets.h"

#include <stdio.h>
#include <string.h>

#define LIST_HEAD_H  18
#define LIST_ROW_H   21
#define LIST_FOOT_H  26
#define LIST_MAX_ROW 8

/* 整机只有一处会用到下层列表页（设置），所以一个静态上下文够了；
 * 真要两处同时开，再把 ctx 变成句柄。 */
static lv_obj_t *s_root;
static lv_obj_t *s_row[LIST_MAX_ROW];
static lv_obj_t *s_name[LIST_MAX_ROW];
static lv_obj_t *s_val[LIST_MAX_ROW];
static lv_obj_t *s_title;
static lv_obj_t *s_hint;
static lv_timer_t *s_timer;
static const nano_page_t *s_page;
static int s_sel;
static int s_rows;
static bool s_confirm;

static void fill_value(const nano_row_t *r, char *buf, int size)
{
    buf[0] = '\0';
    switch (r->kind) {
    case NANO_ROW_ENUM:
        if (r->names && r->get) {
            int v = r->get();
            if (v < 0 || v >= r->name_count) {
                v = 0;
            }
            snprintf(buf, size, "%s", r->names[v]);
        }
        break;
    case NANO_ROW_FLAG:
        snprintf(buf, size, "%s", (r->get && r->get()) ? "开" : "关");
        break;
    case NANO_ROW_RO:
        if (r->text) {
            r->text(buf, size);
        }
        break;
    case NANO_ROW_ACTION:
        snprintf(buf, size, "%s", r->verb ? r->verb : "");
        break;
    }
}

static void render(void)
{
    int first = (s_sel / LIST_MAX_ROW) * LIST_MAX_ROW;

    for (int i = 0; i < LIST_MAX_ROW; i++) {
        int index = first + i;
        bool on = (index == s_sel);
        char buf[40];

        if (index >= s_rows) {
            lv_obj_add_flag(s_row[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(s_row[i], LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_name[i], s_page->rows[index].label);
        lv_obj_set_style_bg_color(s_row[i], on ? NANO_COL_ACCENT : NANO_COL_BG, 0);
        lv_obj_set_style_text_color(s_name[i], on ? NANO_COL_ON_SEL : NANO_COL_FG, 0);
        lv_obj_set_style_text_color(s_val[i], on ? NANO_COL_ON_SEL : NANO_COL_DIM, 0);

        if (s_confirm && on) snprintf(buf, sizeof(buf), "再次确认");
        else fill_value(&s_page->rows[index], buf, sizeof(buf));
        lv_label_set_text(s_val[i], buf);
    }
}

static void refresh_tick(lv_timer_t *timer)
{
    (void)timer;
    if (s_page) {
        render();
    }
}

static void stop_timer(void)
{
    if (s_timer) {
        lv_timer_del(s_timer);
        s_timer = NULL;
    }
}

bool nano_list_is_open(lv_obj_t *list)
{
    (void)list;
    return s_root && s_page && !lv_obj_has_flag(s_root, LV_OBJ_FLAG_HIDDEN);
}

void nano_list_close(lv_obj_t *list)
{
    (void)list;
    if (!s_root) {
        return;
    }
    stop_timer();
    s_page = NULL;
    s_confirm = false;
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);
}

void nano_list_open(lv_obj_t *list, const nano_page_t *page)
{
    int n;

    (void)list;
    if (!s_root || !page || page->row_count <= 0) {
        return;
    }
    n = page->row_count;
    stop_timer();

    s_page = page;
    s_rows = n;
    s_sel = 0;
    s_confirm = false;

    lv_label_set_text(s_title, page->title);
    lv_label_set_text(s_hint, page->hint ? page->hint : "");
    lv_obj_set_style_text_color(s_hint, NANO_COL_DIM, 0);

    for (int i = 0; i < LIST_MAX_ROW; i++) {
        lv_obj_set_y(s_row[i], LIST_HEAD_H + i * LIST_ROW_H);
    }

    if (page->on_open) {
        page->on_open();
    }
    render();

    if (page->refresh_ms > 0) {
        s_timer = lv_timer_create(refresh_tick, page->refresh_ms, NULL);
    }
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_HIDDEN);
}

static void act_row(int index)
{
    const nano_row_t *r = &s_page->rows[index];
    int v;

    if (r->page) {
        nano_list_open(NULL, r->page);
        return;
    }
    switch (r->kind) {
    case NANO_ROW_ENUM:
        if (r->get && r->set && r->name_count > 0) {
            v = r->get();
            v = (v + 1 >= r->name_count) ? 0 : (v + 1);
            r->set(v);
        }
        break;
    case NANO_ROW_FLAG:
        if (r->get && r->set) {
            r->set(!r->get());
        }
        break;
    case NANO_ROW_ACTION:
        if (r->run) {
            r->run();
        }
        break;
    default:
        return;
    }
    render();
}

void nano_list_act(lv_obj_t *list, nano_action_t act)
{
    (void)list;
    if (!s_page) {
        return;
    }
    if (act == NANO_ACT_BACK) {
        if (s_confirm) {
            s_confirm = false;
            lv_label_set_text(s_hint, s_page->hint ? s_page->hint : "");
            lv_obj_set_style_text_color(s_hint, NANO_COL_DIM, 0);
            render();
        } else {
            nano_list_close(NULL);
        }
    } else if (act == NANO_ACT_NEXT) {
        s_confirm = false;
        lv_label_set_text(s_hint, s_page->hint ? s_page->hint : "");
        lv_obj_set_style_text_color(s_hint, NANO_COL_DIM, 0);
        s_sel = (s_sel + 1) % s_rows;
        render();
    } else if (act == NANO_ACT_SELECT) {
        if (s_page->rows[s_sel].confirm && !s_confirm) {
            s_confirm = true;
            lv_label_set_text(s_hint, "再次双击确认");
            lv_obj_set_style_text_color(s_hint, NANO_COL_WARN, 0);
            render();
            return;
        }
        s_confirm = false;
        lv_label_set_text(s_hint, s_page->hint ? s_page->hint : "");
        lv_obj_set_style_text_color(s_hint, NANO_COL_DIM, 0);
        act_row(s_sel);
    }
}

lv_obj_t *nano_list_create(lv_obj_t *parent)
{
    lv_obj_t *root = lv_obj_create(parent);

    nano_style_flat(root, NANO_COL_BG);
    lv_obj_set_size(root, NANO_W, NANO_BODY_H);
    lv_obj_set_pos(root, 0, 0);

    s_title = lv_label_create(root);
    lv_obj_set_style_text_font(s_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_title, NANO_COL_DIM, 0);
    lv_label_set_text(s_title, "");
    lv_obj_set_pos(s_title, 12, 3);

    for (int i = 0; i < LIST_MAX_ROW; i++) {
        lv_obj_t *row = lv_obj_create(root);

        nano_style_flat(row, NANO_COL_BG);
        lv_obj_set_size(row, NANO_W, LIST_ROW_H);
        lv_obj_set_pos(row, 0, LIST_HEAD_H + i * LIST_ROW_H);

        s_name[i] = lv_label_create(row);
        lv_obj_set_style_text_font(s_name[i], &lv_font_chinese_16, 0);
        lv_obj_set_style_text_color(s_name[i], NANO_COL_FG, 0);
        lv_label_set_text(s_name[i], "");
        lv_obj_align(s_name[i], LV_ALIGN_LEFT_MID, 12, 0);

        s_val[i] = lv_label_create(row);
        lv_obj_set_style_text_font(s_val[i], &lv_font_chinese_16, 0);
        lv_obj_set_style_text_color(s_val[i], NANO_COL_DIM, 0);
        lv_label_set_text(s_val[i], "");
        lv_obj_align(s_val[i], LV_ALIGN_RIGHT_MID, -12, 0);

        s_row[i] = row;
        lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_t *line = nano_hairline_create(root);
    lv_obj_set_size(line, NANO_W - 24, 1);
    lv_obj_set_pos(line, 12, NANO_BODY_H - LIST_FOOT_H);

    s_hint = lv_label_create(root);
    lv_obj_set_style_text_font(s_hint, &lv_font_chinese_16, 0);
    lv_obj_set_style_text_color(s_hint, NANO_COL_DIM, 0);
    lv_label_set_text(s_hint, "");
    lv_obj_align(s_hint, LV_ALIGN_TOP_MID, 0, NANO_BODY_H - LIST_FOOT_H + 4);

    s_root = root;
    lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
    return root;
}
