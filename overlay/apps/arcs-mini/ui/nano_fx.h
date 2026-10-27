#ifndef NANO_FX_H
#define NANO_FX_H

#include <stdint.h>

#include "nano_settings.h"

/* 一条管线三个档位：量化 → 调色板 → 扫描线/抖动调制。
 * 量化落在 4bit/通道，所以调色板只有 4096 项、8KB，随机访问不炸缓存；
 * 抖动在截断之前加阈值，等于把丢掉的那 1bit 用位置噪声换回来。 */
#define NANO_FX_LUT_N   4096
/* 取景与照片同一个尺寸：320x240 的中心 240x180，正好 4:3，不缩放、不抽行。
 * 这样"所见即所得"是字面意义上的。 */
#define NANO_FX_IMG_W   240
#define NANO_FX_IMG_H   180

/* 建好表并把当前档位设为 fx；重复调用是幂等的。返回 0 表示可用。 */
int nano_fx_init(void);
void nano_fx_select(nano_fx_t fx);
nano_fx_t nano_fx_current(void);
const char *nano_fx_name(nano_fx_t fx);
/* 落到 JPEG 时的 tiny_jpeg 质量档（3 最好 / 1 最省）。
 * CRT 的扫描线是高频细节，压狠了会糊成摩尔纹，所以它独占最高档。 */
int nano_fx_save_quality(nano_fx_t fx);

/* src 是摄像头 fb 的原始大端 RGB565。dst 取 src 的中心 dst_w x dst_h，
 * 要求 dst 不大于 src。 */
void nano_fx_apply(const uint16_t *src, uint16_t *dst, uint16_t src_w, uint16_t src_h,
                   uint16_t dst_w, uint16_t dst_h);

#endif /* NANO_FX_H */
