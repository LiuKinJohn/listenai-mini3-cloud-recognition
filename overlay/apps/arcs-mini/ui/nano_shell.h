#ifndef NANO_SHELL_H
#define NANO_SHELL_H

#include <stdbool.h>
#include "lvgl.h"
#include "voice_msg.h"

typedef enum {
    NANO_SCREEN_HOME = 0,
    NANO_SCREEN_CLOCK,
    NANO_SCREEN_CLOUD,
    NANO_SCREEN_CAMERA,
    NANO_SCREEN_PLAYBACK,
    NANO_SCREEN_SETTINGS,
    NANO_SCREEN_COUNT
} nano_screen_t;

/* 全机统一：单击移到下一项，双击确认，中等长按松开返回。 */
typedef enum {
    NANO_ACT_NEXT = 0,
    NANO_ACT_SELECT,
    NANO_ACT_BACK,
} nano_action_t;

typedef struct {
    lv_obj_t *(*create)(lv_obj_t *parent);
    /* 返回 true 表示动作被页面内部消费；BACK=false 时由 shell 返回主页。 */
    bool (*action)(nano_action_t act);
    void (*show)(void);
    void (*hide)(void);
} nano_view_t;

extern const nano_view_t nano_view_info;
extern const nano_view_t nano_view_cloud;
extern const nano_view_t nano_view_camera;
extern const nano_view_t nano_view_playback;
extern const nano_view_t nano_view_settings;

/* 由 main.c 在 lisa_ui_init() 之后调用：把自己注册成默认屏，
 * 出厂 home 会被 nav 框架 close 掉，代码不删。 */
int nano_shell_install(void);

/* app_button.c 用它决定是否放行出厂分发。高阶连击由 shell 吞掉，
 * 不再保留隐藏的配网或恢复出厂手势；3 秒长按仍交给电源逻辑。 */
bool nano_shell_owns_keys(voice_msg_button_action_t action);

/* shell 已经建好并在前台。出厂的被动网络提示用它自己让路，
 * 主动 BLE 配网走的是另一条路，不受这里影响。 */
bool nano_shell_is_active(void);

nano_screen_t nano_shell_current(void);
void nano_shell_goto(nano_screen_t target);
const char *nano_shell_screen_name(nano_screen_t screen);

#endif /* NANO_SHELL_H */
