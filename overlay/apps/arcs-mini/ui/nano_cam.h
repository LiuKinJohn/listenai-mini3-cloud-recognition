#ifndef NANO_CAM_H
#define NANO_CAM_H

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"
#include "nano_settings.h"

/* 取流线程把 fx 处理好的 240x180 帧写进 canvas 自己的缓冲，
 * 只把 invalidate 甩回 UI 线程——LVGL 只能在 UI 线程碰。
 * 撕裂在这条链路上不是缺陷：传感器本身就在一直变。 */
/* 分配帧缓冲并绑到 canvas 上。在 UI 线程调用，可以重复调。 */
int nano_cam_attach(lv_obj_t *canvas);
int nano_cam_start(void);
/* 识云独立使用原片，不改变相机保存的默认特效。 */
int nano_cam_start_raw(void);
void nano_cam_stop(void);
bool nano_cam_running(void);

nano_fx_t nano_cam_effect(void);
/* 返回 false 表示被"锁定特效"挡住了。 */
bool nano_cam_next_effect(void);
uint32_t nano_cam_fps10(void);
/* 停止取景后 fps10 归零，诊断页要看的"上一次跑出来的帧率"留在这里。 */
uint32_t nano_cam_fps10_last(void);

/* 快门：把当前帧涂白一拍（约定的"咔哒"），并在取流线程的帧边界把这一帧交出去落盘。
 * 必须在 UI 线程调用。返回值只说"这次按下去怎么样"，不是最终写盘结果：
 *   0  已排入，之后 nano_cam_is_saving() 为真
 *  -1  没有卡
 *  -2  没在取景，或上一张还在写
 *  -3  快照缓冲分不出来   */
int nano_cam_shutter(void);
bool nano_cam_is_saving(void);
/* 最近一次落盘的收尾。写成功时 name 是文件名、bytes 是 JPEG 字节数；
 * 失败或还没写过则 name 为空串。 */
bool nano_cam_last_save_ok(void);
const char *nano_cam_last_saved_name(void);
uint32_t nano_cam_last_saved_bytes(void);

#endif /* NANO_CAM_H */
