#include "service_cloud.h"

#include "FreeRTOS.h"
#include "task.h"

#include "cJSON.h"
#include "lisa_log.h"
#include "listen_system.h"
#include "lsfs.h"
#include "sysheap.h"
#include "lisa_kv.h"
#include "nano_cloud_transport.h"

#include <stdio.h>
#include <string.h>

#define TAG "nano.cloud"
#define CLOUD_DIR "/SD:/CLOUD"
#define CLOUD_ROOT CLOUD_DIR "/"
#define CLOUD_JSON_MAX 4096U
#define CLOUD_STACK 8192
#define CLOUD_PRIO 2

#define CLOUD_GATEWAY_KEY "nano.cloud.gateway"
#define CLOUD_DEVICE_KEY "nano.cloud.device"
static volatile cloud_state_t s_state = CLOUD_STATE_IDLE;
static TaskHandle_t s_task;
static cloud_record_t s_latest;
static char s_pending_photo[PHOTO_NAME_LEN];

static void copy_text(char *dst, size_t cap, const char *src)
{
    if (dst && cap) snprintf(dst, cap, "%s", src ? src : "");
}

static int record_index(const char *name)
{
    int value = 0;
    if (!name || strlen(name) != 13 || strncmp(name, "CLD_", 4) != 0 ||
        strcmp(name + 8, ".JSON") != 0) return -1;
    for (int i = 4; i < 8; ++i) {
        if (name[i] < '0' || name[i] > '9') return -1;
        value = value * 10 + name[i] - '0';
    }
    return value;
}

static int next_record_index(void)
{
    struct lsfs_dir_t dir;
    struct lsfs_dirent ent;
    int next = 1;

    (void)lsfs_mkdir(CLOUD_DIR);
    lsfs_dir_t_init(&dir);
    if (lsfs_opendir(&dir, CLOUD_ROOT) != 0) return next;
    while (lsfs_readdir(&dir, &ent) == 0 && ent.name[0] != '\0') {
        int idx;
        if (ent.type != LSFS_DIR_ENTRY_FILE) continue;
        idx = record_index(ent.name);
        if (idx >= next) next = idx + 1;
    }
    lsfs_closedir(&dir);
    return next;
}

static void record_time(char *out, size_t cap)
{
    struct tm t;
    if (ls_sys_get_localtime(&t) == 0) {
        snprintf(out, cap, "%04d-%02d-%02d %02d:%02d", t.tm_year + 1900, t.tm_mon + 1,
                 t.tm_mday, t.tm_hour, t.tm_min);
    } else {
        copy_text(out, cap, "时间待同步");
    }
}

static bool json_string(cJSON *root, const char *key, char *dst, size_t cap)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsString(item) || !item->valuestring) return false;
    copy_text(dst, cap, item->valuestring);
    return true;
}

static void parse_evidence(cJSON *root, cloud_record_t *r)
{
    cJSON *items = cJSON_GetObjectItemCaseSensitive(root, "evidence");
    cJSON *item;
    bool first = true;
    size_t used = 0;

    if (cJSON_IsString(items)) {
        copy_text(r->evidence, sizeof(r->evidence), items->valuestring);
        return;
    }
    if (!cJSON_IsArray(items)) return;
    cJSON_ArrayForEach(item, items) {
        if (cJSON_IsString(item) && item->valuestring) {
            int n = snprintf(r->evidence + used, sizeof(r->evidence) - used, "%s%s",
                             first ? "" : " · ", item->valuestring);
            if (n < 0 || (size_t)n >= sizeof(r->evidence) - used) break;
            used += (size_t)n;
            first = false;
        }
    }
}

static int write_record(const cloud_record_t *r)
{
    struct lsfs_file_t fp;
    char path[48];
    char *text = NULL;
    cJSON *root = cJSON_CreateObject();
    cJSON *evidence;
    int rc = -1;

    if (!root) return -1;
    cJSON_AddNumberToObject(root, "index", r->index);
    cJSON_AddBoolToObject(root, "success", r->success);
    cJSON_AddStringToObject(root, "image", r->image);
    cJSON_AddStringToObject(root, "type", r->type);
    cJSON_AddNumberToObject(root, "confidence", r->confidence);
    cJSON_AddStringToObject(root, "confidence_description", r->confidence_description);
    cJSON_AddStringToObject(root, "captured_at", r->captured_at);
    cJSON_AddStringToObject(root, "location", r->location);
    evidence = cJSON_AddArrayToObject(root, "evidence");
    if (evidence && r->evidence[0]) cJSON_AddItemToArray(evidence, cJSON_CreateString(r->evidence));
    cJSON_AddStringToObject(root, "weather", r->weather);
    cJSON_AddStringToObject(root, "error", r->error);
    text = cJSON_PrintUnformatted(root);
    if (!text) goto done;

    (void)lsfs_mkdir(CLOUD_DIR);
    snprintf(path, sizeof(path), CLOUD_ROOT "CLD_%04d.JSON", r->index);
    lsfs_file_t_init(&fp);
    if (lsfs_open(&fp, path, LSFS_O_WRITE | LSFS_O_CREATE | LSFS_O_TRUNC) == 0) {
        size_t len = strlen(text);
        rc = ((size_t)lsfs_write(&fp, text, len) == len) ? 0 : -1;
        lsfs_close(&fp);
    }
done:
    if (text) cJSON_free(text);
    cJSON_Delete(root);
    return rc;
}

static int read_record(const char *name, cloud_record_t *record)
{
    struct lsfs_dirent st;
    struct lsfs_file_t fp;
    char path[48];
    char *text;
    cJSON *root = NULL;
    cJSON *item;
    int confidence = 0;
    int rc = -1;

    snprintf(path, sizeof(path), CLOUD_ROOT "%s", name);
    if (lsfs_stat(path, &st) != 0 || st.size <= 0 || (size_t)st.size > CLOUD_JSON_MAX) return -1;
    text = psram_malloc((size_t)st.size + 1U);
    if (!text) return -1;
    lsfs_file_t_init(&fp);
    if (lsfs_open(&fp, path, LSFS_O_READ) != 0) goto done;
    if ((size_t)lsfs_read(&fp, text, (size_t)st.size) != (size_t)st.size) {
        lsfs_close(&fp);
        goto done;
    }
    lsfs_close(&fp);
    text[st.size] = '\0';
    root = cJSON_Parse(text);
    if (!cJSON_IsObject(root)) goto done;
    memset(record, 0, sizeof(*record));
    record->index = record_index(name);
    item = cJSON_GetObjectItemCaseSensitive(root, "success");
    record->success = cJSON_IsTrue(item);
    (void)json_string(root, "image", record->image, sizeof(record->image));
    (void)json_string(root, "type", record->type, sizeof(record->type));
    item = cJSON_GetObjectItemCaseSensitive(root, "confidence");
    if (cJSON_IsNumber(item)) confidence = item->valueint;
    if (confidence < 0) confidence = 0;
    if (confidence > 100) confidence = 100;
    record->confidence = (uint8_t)confidence;
    (void)json_string(root, "confidence_description", record->confidence_description, sizeof(record->confidence_description));
    (void)json_string(root, "captured_at", record->captured_at, sizeof(record->captured_at));
    (void)json_string(root, "location", record->location, sizeof(record->location));
    parse_evidence(root, record);
    (void)json_string(root, "weather", record->weather, sizeof(record->weather));
    (void)json_string(root, "error", record->error, sizeof(record->error));
    rc = 0;
done:
    if (root) cJSON_Delete(root);
    psram_free(text);
    return rc;
}

static void cloud_task(void *arg)
{
    cloud_record_t record = {0};
    char *gateway = NULL, *device = NULL;
    uint8_t *photo = NULL;
    uint32_t photo_size = 0;
    char *response = NULL;
    cJSON *root = NULL;
    (void)arg;

    record.index = next_record_index();
    copy_text(record.image, sizeof(record.image), s_pending_photo);
    record_time(record.captured_at, sizeof(record.captured_at));
    copy_text(record.error, sizeof(record.error), "识别失败，请重试");
    if (lisa_kv_get_string(CLOUD_GATEWAY_KEY, &gateway) != 0 ||
        lisa_kv_get_string(CLOUD_DEVICE_KEY, &device) != 0 || !gateway || !device) {
        copy_text(record.error, sizeof(record.error), "云端服务待绑定");
        goto finish;
    }
    if (service_photo_read_jpeg(record.image, &photo, &photo_size) != 0) {
        copy_text(record.error, sizeof(record.error), "照片读取失败");
        goto finish;
    }
    response = psram_malloc(CLOUD_JSON_MAX);
    if (!response) { copy_text(record.error, sizeof(record.error), "内存不足"); goto finish; }
    if (nano_cloud_post(photo, photo_size, gateway, device, response, CLOUD_JSON_MAX) != 0) {
        copy_text(record.error, sizeof(record.error), "网络或安全连接失败");
        goto finish;
    }
    root = cJSON_Parse(response);
    if (!cJSON_IsObject(root) || !json_string(root, "type", record.type, sizeof(record.type)) ||
        !json_string(root, "confidence_description", record.confidence_description, sizeof(record.confidence_description)) ||
        (strcmp(record.confidence_description, "high") && strcmp(record.confidence_description, "medium") &&
         strcmp(record.confidence_description, "low"))) {
        copy_text(record.error, sizeof(record.error), "识别结果无效");
        goto finish;
    }
    parse_evidence(root, &record);
    record.success = true;
    record.error[0] = '\0';
finish:
    if (root) cJSON_Delete(root);
    if (response) psram_free(response);
    if (photo) psram_free(photo);
    if (gateway) lisa_kv_free(gateway);
    if (device) lisa_kv_free(device);
    if (write_record(&record) != 0) { record.success = false; copy_text(record.error, sizeof(record.error), "记录保存失败"); }
    s_latest = record;
    s_state = record.success ? CLOUD_STATE_DONE : CLOUD_STATE_ERROR;
    s_task = NULL;
    LISA_LOGI(TAG, "识云记录已处理");
    vTaskDelete(NULL);
}

bool service_cloud_endpoint_configured(void)
{
    char *gateway = NULL, *device = NULL;
    bool ok = lisa_kv_get_string(CLOUD_GATEWAY_KEY, &gateway) == 0 &&
              lisa_kv_get_string(CLOUD_DEVICE_KEY, &device) == 0 &&
              gateway && *gateway && device && *device;
    if (gateway) lisa_kv_free(gateway);
    if (device) lisa_kv_free(device);
    return ok;
}

int service_cloud_start(const char *photo_name)
{
    if (!photo_name || !photo_name[0] || s_task || s_state == CLOUD_STATE_UPLOADING) return -1;
    copy_text(s_pending_photo, sizeof(s_pending_photo), photo_name);
    memset(&s_latest, 0, sizeof(s_latest));
    s_state = CLOUD_STATE_UPLOADING;
    if (xTaskCreate(cloud_task, "nano.cloud", CLOUD_STACK, NULL, CLOUD_PRIO, &s_task) != pdPASS) {
        s_task = NULL;
        s_state = CLOUD_STATE_ERROR;
        copy_text(s_latest.error, sizeof(s_latest.error), "任务创建失败");
        return -1;
    }
    return 0;
}

cloud_state_t service_cloud_state(void) { return s_state; }

bool service_cloud_latest(cloud_record_t *out)
{
    if (!out || (s_state != CLOUD_STATE_DONE && s_state != CLOUD_STATE_ERROR)) return false;
    *out = s_latest;
    return true;
}

static void list_insert(cloud_record_t *items, int *count, int max, const cloud_record_t *r)
{
    int at = *count;
    while (at > 0 && items[at - 1].index < r->index) --at;
    if (*count >= max) {
        if (at >= *count) return;
        --*count;
    }
    for (int i = *count; i > at; --i) items[i] = items[i - 1];
    items[at] = *r;
    ++*count;
}

int service_cloud_list(cloud_record_t *items, int max)
{
    struct lsfs_dir_t dir;
    struct lsfs_dirent ent;
    int count = 0;

    if (!items || max <= 0) return 0;
    lsfs_dir_t_init(&dir);
    if (lsfs_opendir(&dir, CLOUD_ROOT) != 0) return 0;
    while (lsfs_readdir(&dir, &ent) == 0 && ent.name[0] != '\0') {
        cloud_record_t record;
        if (ent.type != LSFS_DIR_ENTRY_FILE || record_index(ent.name) < 0) continue;
        if (read_record(ent.name, &record) == 0) list_insert(items, &count, max, &record);
    }
    lsfs_closedir(&dir);
    return count;
}
