#ifndef NANO_SETTINGS_H
#define NANO_SETTINGS_H

#include <stdbool.h>

typedef enum {
    NANO_CLOCK_SEG = 0,
    NANO_CLOCK_RUBIK,
    NANO_CLOCK_DITHER,
    NANO_CLOCK_STYLE_COUNT
} nano_clock_style_t;

/* 同一条管线（量化 → 调色板 → 扫描线/抖动调制）的四个档位，
 * PLAIN 是恒等映射，让"原图"也能被拍下来。 */
typedef enum {
    NANO_FX_PLAIN = 0,
    NANO_FX_POSTER,
    NANO_FX_MONO,
    NANO_FX_CRT,
    NANO_FX_COUNT
} nano_fx_t;

typedef enum {
    NANO_SLEEP_ALWAYS_ON = 0,
    NANO_SLEEP_30S,
    NANO_SLEEP_60S,
    NANO_SLEEP_180S,
    NANO_SLEEP_COUNT
} nano_sleep_t;

typedef struct {
    int clock_style;
    int fx_default;
    bool fx_lock;
    bool sound_on;
    bool tick_on;
    int sleep_mode;
    bool wifi_on;
} nano_settings_t;

nano_settings_t *nano_settings(void);

/* 从 KV 读回全部字段并落到 RAM 缓存；调用方负责把生效动作（亮度等）做一遍。 */
void nano_settings_init(void);

void nano_set_clock_style(int v);
void nano_set_fx_default(int v);
void nano_set_fx_lock(bool v);
void nano_set_sound_on(bool v);
void nano_set_tick_on(bool v);
void nano_set_sleep_mode(int v);
void nano_set_wifi_on(bool v);

/* 亮度不进 nano_settings_t：service_brightness 自己已经持久化，这里只做透传，
 * 免得同一个值有两份真相。 */
void nano_set_brightness(int v);
int nano_get_brightness(void);

#endif /* NANO_SETTINGS_H */
