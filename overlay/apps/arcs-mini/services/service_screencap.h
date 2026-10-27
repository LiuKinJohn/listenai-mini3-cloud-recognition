#ifndef SERVICE_SCREENCAP_H
#define SERVICE_SCREENCAP_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* 在 UI 线程上重画当前屏幕 → RGB565 → BMP，阻塞到完成或超时。 */
bool service_screencap_capture(void);
const uint8_t *service_screencap_bmp(size_t *size);

#endif
