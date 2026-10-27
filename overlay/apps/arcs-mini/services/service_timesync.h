#ifndef SERVICE_TIMESYNC_H
#define SERVICE_TIMESYNC_H

#include <stdbool.h>

/* 开机跑一次 SNTP。RTC 不联网也是准的，这一步治的是长期漂移，
 * 顺带给状态栏那颗橙灯一个真实的"在飞"窗口。 */
int service_timesync_start(void);
bool service_timesync_is_running(void);

#endif /* SERVICE_TIMESYNC_H */
