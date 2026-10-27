#include "service_timesync.h"

#include <sys/time.h>

#include "FreeRTOS.h"
#include "task.h"

#include "lisa_log.h"
#include "lisa_sntp.h"
#include "listen_system.h"
#include "sys_wifi.h"

#if CONFIG_LISA_SHELL
#include "cmd/cmd.h"
#include "shell.h"
#endif

#define TAG "nano.timesync"

/* DNS + socket 都在这条线程里跑，SDK 自己的 probe 线程给的是 8192，照抄。 */
#define TS_STACK          8192
#define TS_PRIO           4
#define TS_QUERY_TIMEOUT  5000
#define TS_TRIES          2
#define TS_POLL_MS        5000
#define TS_RETRY_MS       60000
#define TS_REFRESH_MS     (6U * 60U * 60U * 1000U)

static const char *s_servers[] = {
    "ntp.aliyun.com",
    "ntp.tencent.com",
    "ntp.ntsc.ac.cn",
};

static volatile bool s_running;
static volatile int s_last_rc = -999;
static volatile unsigned s_attempts;
static bool s_started;

static void ts_task(void *arg)
{
    struct lisa_sntp_time t;
    uint32_t remaining_ms = 0;
    bool was_connected = false;

    (void)arg;
    for (;;) {
        bool connected = sys_wifi_is_connected();
        int rc = -1;
        if (!connected) {
            was_connected = false;
            remaining_ms = 0;
            vTaskDelay(pdMS_TO_TICKS(TS_POLL_MS));
            continue;
        }
        if (!was_connected) {
            was_connected = true;
            remaining_ms = 0;
        }
        if (remaining_ms > 0) {
            remaining_ms -= TS_POLL_MS;
            vTaskDelay(pdMS_TO_TICKS(TS_POLL_MS));
            continue;
        }
        s_running = true;
        for (int i = 0; i < TS_TRIES && rc != 0 && sys_wifi_is_connected(); i++) {
            rc = lisa_sntp_query(s_servers, (int)(sizeof(s_servers) / sizeof(s_servers[0])),
                                 TS_QUERY_TIMEOUT, &t);
            s_last_rc = rc;
            s_attempts++;
            if (rc != 0) LISA_LOGW(TAG, "SNTP 失败 rc=%d（第 %d 次）", rc, i + 1);
        }
        if (rc == 0) {
            struct timeval tv = {
                .tv_sec = (time_t)t.sec,
                .tv_usec = (suseconds_t)(t.nsec / 1000U),
            };
            settimeofday(&tv, NULL);
            LISA_LOGI(TAG, "校时完成 sec=%u", (unsigned)t.sec);
            remaining_ms = TS_REFRESH_MS;
        } else {
            LISA_LOGW(TAG, "校时失败，将在一分钟后重试");
            remaining_ms = TS_RETRY_MS;
        }
        s_running = false;
    }
}

int service_timesync_start(void)
{
    if (s_started) {
        return 0;
    }
    if (xTaskCreate(ts_task, "nano.ts", TS_STACK, NULL, TS_PRIO, NULL) != pdPASS) {
        LISA_LOGE(TAG, "线程创建失败");
        return -1;
    }
    s_started = true;
    return 0;
}

bool service_timesync_is_running(void)
{
    return s_running;
}

#if CONFIG_LISA_SHELL
static int timesync_cmd_handler(int argc, char **argv)
{
    struct tm t;
    Shell *sh = shellGetCurrent();
    (void)argc;
    (void)argv;
    shellPrint(sh, "wifi=%d running=%d attempts=%u last_rc=%d valid=%d\n",
               sys_wifi_is_connected(), s_running, s_attempts, s_last_rc, ls_sys_time_is_valid());
    if (ls_sys_get_localtime(&t) == 0)
        shellPrint(sh, "local=%04d-%02d-%02d %02d:%02d:%02d\n", t.tm_year + 1900,
                   t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
    return 0;
}
SHELL_EXPORT_CMD(SHELL_CMD_PERMISSION(0) | SHELL_CMD_TYPE(SHELL_TYPE_CMD_MAIN) |
                     SHELL_CMD_DISABLE_RETURN,
                 timesync, timesync_cmd_handler, time synchronization status);
#endif
