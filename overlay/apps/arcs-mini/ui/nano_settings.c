#include "nano_settings.h"
#include "nano_theme.h"
#include "service_brightness.h"

#include "lisa_kv.h"
#include "lisa_log.h"

#define TAG "nano.set"

/* KV 键刻意压到 8 字符内：出厂 kv shell 一行要显示键+值，长了会折行。 */
#define KV_CLK    "nano.clk"
#define KV_FX     "nano.fx"
#define KV_FXLOCK "nano.fxlk"
#define KV_SND    "nano.snd"
#define KV_TICK   "nano.tick"
#define KV_SLEEP  "nano.slp"
#define KV_WIFI   "nano.wifi"

static nano_settings_t s_cfg = {
    .clock_style = NANO_CLOCK_SEG,
    .fx_default  = NANO_FX_POSTER,
    .fx_lock     = false,
    .sound_on    = true,
    .tick_on     = true,
    .sleep_mode  = NANO_SLEEP_ALWAYS_ON,
    .wifi_on     = true,
};

nano_settings_t *nano_settings(void)
{
    return &s_cfg;
}

static int load_int(const char *key, int fallback, int max)
{
    int v = fallback;

    if (lisa_kv_get_int(key, &v) != 0 || v < 0 || v > max) {
        v = fallback;
    }
    return v;
}

static bool load_bool(const char *key, bool fallback)
{
    bool v = fallback;

    if (lisa_kv_get_bool(key, &v) != 0) {
        v = fallback;
    }
    return v;
}

void nano_settings_init(void)
{
    s_cfg.clock_style = load_int(KV_CLK, NANO_CLOCK_SEG, NANO_CLOCK_STYLE_COUNT - 1);
    s_cfg.fx_default  = load_int(KV_FX, NANO_FX_POSTER, NANO_FX_COUNT - 1);
    s_cfg.fx_lock     = load_bool(KV_FXLOCK, false);
    s_cfg.sound_on    = load_bool(KV_SND, true);
    s_cfg.tick_on     = load_bool(KV_TICK, true);
    s_cfg.sleep_mode  = load_int(KV_SLEEP, NANO_SLEEP_ALWAYS_ON, NANO_SLEEP_COUNT - 1);
    s_cfg.wifi_on     = load_bool(KV_WIFI, true);

    /* 亮度由 service_brightness 自己持久化，这里只把值取进缓存供 UI 显示。 */
    LISA_LOGI(TAG, "clk=%d fx=%d fxlk=%d snd=%d tick=%d slp=%d wifi=%d brt=%d",
              s_cfg.clock_style, s_cfg.fx_default, s_cfg.fx_lock, s_cfg.sound_on, s_cfg.tick_on,
              s_cfg.sleep_mode, s_cfg.wifi_on, service_brightness_get());
}

void nano_set_clock_style(int v)
{
    s_cfg.clock_style = v;
    lisa_kv_set_int(KV_CLK, v);
}

void nano_set_fx_default(int v)
{
    s_cfg.fx_default = v;
    lisa_kv_set_int(KV_FX, v);
}

void nano_set_fx_lock(bool v)
{
    s_cfg.fx_lock = v;
    lisa_kv_set_bool(KV_FXLOCK, v);
}

void nano_set_sound_on(bool v)
{
    s_cfg.sound_on = v;
    lisa_kv_set_bool(KV_SND, v);
}

void nano_set_tick_on(bool v)
{
    s_cfg.tick_on = v;
    lisa_kv_set_bool(KV_TICK, v);
}

void nano_set_brightness(int v)
{
    service_brightness_set(v);
}

int nano_get_brightness(void)
{
    return service_brightness_get();
}

void nano_set_sleep_mode(int v)
{
    s_cfg.sleep_mode = v;
    lisa_kv_set_int(KV_SLEEP, v);
}

void nano_set_wifi_on(bool v)
{
    s_cfg.wifi_on = v;
    lisa_kv_set_bool(KV_WIFI, v);
}
