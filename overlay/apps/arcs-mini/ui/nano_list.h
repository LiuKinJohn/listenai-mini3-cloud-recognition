#ifndef NANO_LIST_H
#define NANO_LIST_H

#include <stdbool.h>

#include "lvgl.h"
#include "nano_shell.h"

typedef enum {
    NANO_ROW_ENUM = 0, /* 右对齐当前档位，双击循环到下一档 */
    NANO_ROW_FLAG,     /* 右对齐 开/关，双击翻转 */
    NANO_ROW_RO,       /* 只读，值由 text() 现取 */
    NANO_ROW_ACTION,   /* 右对齐动词，双击执行 run() */
} nano_row_kind_t;

typedef struct nano_page nano_page_t;

typedef struct {
    const char *label;
    nano_row_kind_t kind;
    const char *const *names;
    int name_count;
    int (*get)(void);
    void (*set)(int v);
    void (*text)(char *buf, int size);
    const char *verb;
    void (*run)(void);
    bool confirm; /* 危险动作第一次双击只进入确认态 */
    const nano_page_t *page; /* 非空则双击进入下层页 */
} nano_row_t;

struct nano_page {
    const char *title; /* 拉丁小标题，和 SETTINGS 那行同字体 */
    const char *hint;
    const nano_row_t *rows;
    int row_count;
    int refresh_ms; /* 非 0 则打开期间定时重取只读值 */
    void (*on_open)(void);
};

lv_obj_t *nano_list_create(lv_obj_t *parent);
void nano_list_open(lv_obj_t *list, const nano_page_t *page);
void nano_list_close(lv_obj_t *list);
bool nano_list_is_open(lv_obj_t *list);
void nano_list_act(lv_obj_t *list, nano_action_t act);

#endif /* NANO_LIST_H */
