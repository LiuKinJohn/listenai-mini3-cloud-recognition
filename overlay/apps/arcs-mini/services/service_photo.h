#ifndef SERVICE_PHOTO_H
#define SERVICE_PHOTO_H

#include <stdbool.h>
#include <stdint.h>

#define PHOTO_ROOT      "/SD:/"
#define PHOTO_NAME_LEN  13 /* "IMG_0001.JPG" + NUL */
#define PHOTO_LIST_MAX  32

typedef struct {
    char name[PHOTO_NAME_LEN];
    uint32_t size;
} photo_entry_t;

/* 文件系统层能不能写。热插拔由 sd_music 服务的监测循环负责重挂。 */
bool service_photo_card_ready(void);

/* RGB565（主机字节序，与 LVGL canvas 同一布局）→ JPEG 直接写进文件。
 * quality 走 tiny_jpeg 的约定：3 最好、1 最省。失败时不留半个文件。 */
int service_photo_save(const uint16_t *rgb565, uint16_t w, uint16_t h, int quality,
                       char *name_out, int name_out_len, uint32_t *out_bytes);

/* 卡上的 JPG，新→旧。返回条数。 */
int service_photo_list(photo_entry_t *items, int max);

/* 解码 JPEG 并按比例放进 dst（四周留黑）。dst 是 w*h 的 RGB565 缓冲。 */
int service_photo_render(const char *name, uint16_t *dst, uint16_t dst_w, uint16_t dst_h);

/* 读取原始 JPEG 到 PSRAM；成功后调用方用 psram_free() 释放。 */
int service_photo_read_jpeg(const char *name, uint8_t **data_out, uint32_t *size_out);

#endif /* SERVICE_PHOTO_H */
