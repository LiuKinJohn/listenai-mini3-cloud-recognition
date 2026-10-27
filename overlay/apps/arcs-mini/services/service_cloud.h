#ifndef SERVICE_CLOUD_H
#define SERVICE_CLOUD_H

#include <stdbool.h>
#include <stdint.h>

#include "service_photo.h"

#define CLOUD_RECORD_MAX 32

typedef enum {
    CLOUD_STATE_IDLE = 0,
    CLOUD_STATE_UPLOADING,
    CLOUD_STATE_DONE,
    CLOUD_STATE_ERROR,
} cloud_state_t;

typedef struct {
    int index;
    bool success;
    char image[PHOTO_NAME_LEN];
    char type[32];
    uint8_t confidence;
    char confidence_description[12];
    char captured_at[32];
    char location[64];
    char evidence[128];
    char weather[96];
    char error[64];
} cloud_record_t;

/* 当前固件先完成本地云册链路；拿到唯一 Worker 地址后再启用上传。 */
bool service_cloud_endpoint_configured(void);
int service_cloud_start(const char *photo_name);
cloud_state_t service_cloud_state(void);
bool service_cloud_latest(cloud_record_t *out);
int service_cloud_list(cloud_record_t *items, int max);

#endif /* SERVICE_CLOUD_H */
