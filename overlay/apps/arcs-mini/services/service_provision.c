/* Mini3 临时热点配网：STA 扫描 -> WPA2 SoftAP -> 本地网页 -> 切回 STA。 */

#include "service_provision.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "lisa_log.h"
#include "lwip/inet.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include "lwip/tcpip.h"
#include "net_al.h"
#include "net_ip.h"
#include "sys_network_manager.h"
#include "sys_wifi.h"
#include "voice_cloud.h"
#include "wifi_api.h"
#include "wifi_manager/wifi_manager.h"

#if CONFIG_LISA_SHELL
#include "cmd/cmd.h"
#include "shell.h"
#endif

#define TAG "provision"
#define PROV_SCAN_MAX       12
#define PROV_TIMEOUT_SEC    300
#define PROV_HTTP_PORT      80
#define PROV_STACK_WORDS    4096
#define PROV_PAGE_CAP       6144
#define PROV_REQ_CAP        1024

typedef enum {
    PROV_IDLE,
    PROV_SCANNING,
    PROV_STARTING,
    PROV_WAITING,
    PROV_CONNECTING,
    PROV_SUCCESS,
    PROV_FAILED,
} prov_state_t;

typedef struct {
    char request[PROV_REQ_CAP];
    size_t request_len;
    const char *response;
    size_t response_len;
    size_t response_sent;
} portal_conn_t;

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} page_builder_t;

static volatile prov_state_t s_state;
static volatile bool s_stop;
static volatile bool s_submitted;
static TaskHandle_t s_task;
static volatile int s_last_error;
static char s_ap_ssid[24] = "Mini3-Setup";
static char s_ap_pwd[16] = "mini3setup";
static char s_target_ssid[32];
static char s_target_pwd[64];
static char *s_form_page;
static size_t s_form_page_len;
static struct tcp_pcb *s_http_listener;
static struct tcp_pcb *s_http_client;
static volatile int s_raw_error;

static const char *const s_state_text[] = {
    "未启动", "扫描网络", "正在开热点", "等待配置", "正在连接", "已连接", "连接失败",
};

static const char s_connecting_page[] =
    "HTTP/1.0 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
    "Cache-Control: no-store\r\nConnection: close\r\n\r\n"
    "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<style>body{font:16px -apple-system,BlinkMacSystemFont,sans-serif;background:#eef7ff;"
    "color:#17202a;margin:0;padding:24px}.c{max-width:520px;margin:auto;background:#fff;"
    "padding:24px;border-radius:18px;box-shadow:0 8px 30px #87bde044}h1{font-size:24px}</style>"
    "<div class=c><h1>正在连接</h1><p>开发板将关闭临时热点并尝试连接，请回到开发板查看结果。</p></div>";

static const char s_invalid_page[] =
    "HTTP/1.0 400 Bad Request\r\nContent-Type: text/html; charset=utf-8\r\n"
    "Cache-Control: no-store\r\nConnection: close\r\n\r\n"
    "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<body style='font:16px -apple-system,sans-serif;background:#eef7ff;padding:24px'>"
    "<h1>信息不完整</h1><p>请返回并重新填写 Wi-Fi 名称。</p></body>";

static bool page_append(page_builder_t *b, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (!b || b->len >= b->cap) return false;
    va_start(ap, fmt);
    n = vsnprintf(b->data + b->len, b->cap - b->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= b->cap - b->len) {
        b->len = b->cap - 1;
        b->data[b->len] = '\0';
        return false;
    }
    b->len += (size_t)n;
    return true;
}

static bool page_escape(page_builder_t *b, const char *s)
{
    for (; s && *s; s++) {
        const char *escaped = NULL;
        switch (*s) {
        case '&': escaped = "&amp;"; break;
        case '<': escaped = "&lt;"; break;
        case '>': escaped = "&gt;"; break;
        case '\"': escaped = "&quot;"; break;
        case '\'': escaped = "&#39;"; break;
        default:
            if (!page_append(b, "%c", *s)) return false;
            continue;
        }
        if (!page_append(b, "%s", escaped)) return false;
    }
    return true;
}

static bool build_form_page(const wifi_mgr_scan_info_t *aps, int count)
{
    page_builder_t b;

    s_form_page = malloc(PROV_PAGE_CAP);
    if (!s_form_page) return false;
    b.data = s_form_page;
    b.len = 0;
    b.cap = PROV_PAGE_CAP;

    if (!page_append(&b,
        "HTTP/1.0 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
        "Cache-Control: no-store\r\nConnection: close\r\n\r\n"
        "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
        "<style>body{font:16px -apple-system,BlinkMacSystemFont,sans-serif;background:#eef7ff;"
        "color:#17202a;margin:0;padding:24px}.c{max-width:520px;margin:auto;background:#fff;"
        "padding:24px;border-radius:18px;box-shadow:0 8px 30px #87bde044}h1{font-size:24px}"
        "label{display:block;margin:16px 0 6px}input{box-sizing:border-box;width:100%%;padding:13px;"
        "border:2px solid #8dceff;border-radius:10px;font-size:16px}button{width:100%%;margin-top:22px;"
        "padding:14px;border:0;border-radius:10px;background:#8dceff;color:#06121c;font-weight:700}"
        "small{color:#647789}</style><div class=c><h1>Mini3 Wi-Fi 配置</h1>"
        "<p>选择附近的 2.4 GHz Wi-Fi，密码只会发送到当前开发板。</p>"
        "<form method=post action=/save><label>Wi-Fi 名称</label>"
        "<input name=ssid list=nets maxlength=31 required autocomplete=off><datalist id=nets>")) {
        free(s_form_page);
        s_form_page = NULL;
        return false;
    }

    for (int i = 0; i < count; i++) {
        if (!aps[i].ssid[0]) continue;
        if (!page_append(&b, "<option value=\"") || !page_escape(&b, aps[i].ssid) ||
            !page_append(&b, "\">")) {
            free(s_form_page);
            s_form_page = NULL;
            return false;
        }
    }
    if (!page_append(&b,
        "</datalist><label>Wi-Fi 密码</label>"
        "<input name=pwd type=password maxlength=63 autocomplete=current-password>"
        "<button type=submit>连接 Wi-Fi</button></form>"
        "<small>隐藏网络可直接输入名称；开放网络请留空密码。</small></div>")) {
        free(s_form_page);
        s_form_page = NULL;
        return false;
    }
    s_form_page_len = b.len;
    return true;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = (char)tolower((unsigned char)c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static void url_decode(char *dst, size_t cap, const char *src, size_t n)
{
    size_t o = 0;
    while (n-- && *src && o + 1 < cap) {
        if (*src == '+') {
            dst[o++] = ' ';
            src++;
        } else if (*src == '%' && n >= 2 && hex_value(src[1]) >= 0 && hex_value(src[2]) >= 0) {
            dst[o++] = (char)((hex_value(src[1]) << 4) | hex_value(src[2]));
            src += 3;
            n -= 2;
        } else {
            dst[o++] = *src++;
        }
    }
    dst[o] = '\0';
}

static bool form_value(const char *body, const char *key, char *out, size_t cap)
{
    size_t key_len = strlen(key);
    const char *p = body;
    while (*p) {
        const char *eq = strchr(p, '=');
        const char *end = strchr(p, '&');
        if (!end) end = p + strlen(p);
        if (eq && eq < end && (size_t)(eq - p) == key_len && memcmp(p, key, key_len) == 0) {
            url_decode(out, cap, eq + 1, (size_t)(end - eq - 1));
            return true;
        }
        p = *end ? end + 1 : end;
    }
    return false;
}

static bool request_complete(const portal_conn_t *conn)
{
    const char *header_end = strstr(conn->request, "\r\n\r\n");
    int content_len = 0;
    if (!header_end) return false;
    if (strncmp(conn->request, "POST ", 5) != 0) return true;
    const char *cl = strstr(conn->request, "Content-Length:");
    if (cl && cl < header_end) content_len = atoi(cl + 15);
    return conn->request_len >= (size_t)(header_end + 4 - conn->request) + (size_t)content_len;
}

static void portal_conn_free(struct tcp_pcb *pcb, portal_conn_t *conn)
{
    if (s_http_client == pcb) s_http_client = NULL;
    free(conn);
}

static void portal_conn_abort(struct tcp_pcb *pcb, portal_conn_t *conn)
{
    tcp_arg(pcb, NULL);
    tcp_recv(pcb, NULL);
    tcp_sent(pcb, NULL);
    tcp_err(pcb, NULL);
    portal_conn_free(pcb, conn);
    tcp_abort(pcb);
}

static void portal_conn_close(struct tcp_pcb *pcb, portal_conn_t *conn)
{
    err_t rc;
    tcp_arg(pcb, NULL);
    tcp_recv(pcb, NULL);
    tcp_sent(pcb, NULL);
    tcp_err(pcb, NULL);
    rc = tcp_close(pcb);
    portal_conn_free(pcb, conn);
    if (rc != ERR_OK) tcp_abort(pcb);
}

static err_t portal_send_more(struct tcp_pcb *pcb, portal_conn_t *conn)
{
    while (conn->response_sent < conn->response_len) {
        size_t remaining = conn->response_len - conn->response_sent;
        u16_t available = tcp_sndbuf(pcb);
        u16_t chunk;
        err_t rc;
        if (available == 0) return ERR_OK;
        chunk = (u16_t)(remaining < available ? remaining : available);
        rc = tcp_write(pcb, conn->response + conn->response_sent, chunk, TCP_WRITE_FLAG_COPY);
        if (rc == ERR_MEM) return ERR_OK;
        if (rc != ERR_OK) {
            portal_conn_abort(pcb, conn);
            return ERR_ABRT;
        }
        conn->response_sent += chunk;
    }
    (void)tcp_output(pcb);
    portal_conn_close(pcb, conn);
    return ERR_OK;
}

static err_t portal_sent(void *arg, struct tcp_pcb *pcb, u16_t len)
{
    (void)len;
    return portal_send_more(pcb, (portal_conn_t *)arg);
}

static void portal_error(void *arg, err_t err)
{
    (void)err;
    s_http_client = NULL;
    free(arg);
}

static err_t portal_handle_request(struct tcp_pcb *pcb, portal_conn_t *conn)
{
    if (strncmp(conn->request, "POST /save ", 11) == 0) {
        char *body = strstr(conn->request, "\r\n\r\n");
        memset(s_target_ssid, 0, sizeof(s_target_ssid));
        memset(s_target_pwd, 0, sizeof(s_target_pwd));
        if (body && form_value(body + 4, "ssid", s_target_ssid, sizeof(s_target_ssid)) &&
            s_target_ssid[0] && form_value(body + 4, "pwd", s_target_pwd, sizeof(s_target_pwd))) {
            conn->response = s_connecting_page;
            conn->response_len = strlen(s_connecting_page);
            s_submitted = true;
        } else {
            conn->response = s_invalid_page;
            conn->response_len = strlen(s_invalid_page);
        }
    } else {
        conn->response = s_form_page;
        conn->response_len = s_form_page_len;
    }
    conn->response_sent = 0;
    return portal_send_more(pcb, conn);
}

static err_t portal_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    portal_conn_t *conn = arg;
    size_t room;
    u16_t copy_len;

    if (!p) {
        portal_conn_close(pcb, conn);
        return ERR_OK;
    }
    if (err != ERR_OK) {
        pbuf_free(p);
        portal_conn_abort(pcb, conn);
        return ERR_ABRT;
    }

    room = sizeof(conn->request) - conn->request_len - 1;
    copy_len = (u16_t)(p->tot_len < room ? p->tot_len : room);
    if (copy_len > 0) {
        pbuf_copy_partial(p, conn->request + conn->request_len, copy_len, 0);
        conn->request_len += copy_len;
        conn->request[conn->request_len] = '\0';
    }
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);

    if (request_complete(conn)) return portal_handle_request(pcb, conn);
    if (room == 0 || copy_len == 0) {
        portal_conn_abort(pcb, conn);
        return ERR_ABRT;
    }
    return ERR_OK;
}

static err_t portal_accept(void *arg, struct tcp_pcb *pcb, err_t err)
{
    portal_conn_t *conn;
    (void)arg;

    tcp_backlog_accepted(s_http_listener);
    if (err != ERR_OK || !pcb || s_http_client) {
        if (pcb) tcp_abort(pcb);
        return ERR_ABRT;
    }
    conn = calloc(1, sizeof(*conn));
    if (!conn) {
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    s_http_client = pcb;
    tcp_arg(pcb, conn);
    tcp_recv(pcb, portal_recv);
    tcp_sent(pcb, portal_sent);
    tcp_err(pcb, portal_error);
    return ERR_OK;
}

static void raw_http_start_cb(void *arg)
{
    struct tcp_pcb *pcb;
    err_t rc = ERR_OK;
    (void)arg;

    s_raw_error = 0;
    pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (!pcb) {
        s_raw_error = 1;
        return;
    }
    rc = tcp_bind(pcb, IP_ADDR_ANY, PROV_HTTP_PORT);
    if (rc != ERR_OK) {
        s_raw_error = 100 - rc;
        tcp_abort(pcb);
        return;
    }
    s_http_listener = tcp_listen_with_backlog_and_err(pcb, 1, &rc);
    if (!s_http_listener) {
        s_raw_error = 200 - rc;
        tcp_abort(pcb);
        return;
    }
    tcp_accept(s_http_listener, portal_accept);
}

static bool raw_http_start(void)
{
    s_raw_error = -1;
    if (tcpip_callback_wait(raw_http_start_cb, NULL) != ERR_OK) return false;
    return s_raw_error == 0 && s_http_listener != NULL;
}

static void raw_http_stop_cb(void *arg)
{
    (void)arg;
    if (s_http_client) {
        portal_conn_t *conn = s_http_client->callback_arg;
        portal_conn_abort(s_http_client, conn);
    }
    if (s_http_listener) {
        tcp_accept(s_http_listener, NULL);
        if (tcp_close(s_http_listener) != ERR_OK) tcp_abort(s_http_listener);
        s_http_listener = NULL;
    }
}

static void raw_http_stop(void)
{
    (void)tcpip_callback_wait(raw_http_stop_cb, NULL);
}

static bool start_ap(void)
{
    wifi_ap_cfg_params_t cfg = {0};
    struct ip_addr_cfg ip = {0};
    net_if_t *netif;
    int state = 0;
    int rc;
    wifi_mode_e mode = WIFI_MODE_STA;

    strncpy((char *)cfg.ssid, s_ap_ssid, sizeof(cfg.ssid) - 1);
    strncpy((char *)cfg.pwd, s_ap_pwd, sizeof(cfg.pwd) - 1);
    cfg.sec = WIFI_SEC_WPA2_PSK;
    cfg.channel = 6;
    cfg.bw = WIFI_BW_20;
    cfg.ap_max_inactivity = 120;

    s_last_error = 0;
    /* Mini3 只有一个虚拟 Wi-Fi 接口；先完整退出 STA 再启动 AP。 */
    (void)voice_cloud_disconnect();
    vTaskDelay(pdMS_TO_TICKS(500));
    rc = sys_wifi_stop();
    if (rc != 0) LISA_LOGW(TAG, "sys_wifi_stop rc=%d", rc);
    (void)wifi_mgr_auto_connect_stop();
    vTaskDelay(pdMS_TO_TICKS(500));
    (void)wifi_mgr_sta_disconnect(false);
    rc = wifi_mgr_sta_disable();
    if (rc != 0) {
        s_last_error = 1000 + rc;
        LISA_LOGE(TAG, "sta disable rc=%d", rc);
        return false;
    }
    for (int i = 0; i < 20; i++) {
        if (wifi_get_mode(WIFI_VIF_DEFAULT_IDX, &mode) == LS_OK && mode == WIFI_MODE_NULL) break;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (mode != WIFI_MODE_NULL) {
        s_last_error = 2000 + (int)mode;
        LISA_LOGE(TAG, "sta did not reach NULL mode, mode=%d", (int)mode);
        return false;
    }
    rc = wifi_ap_start(&cfg);
    if (rc != LS_OK) {
        s_last_error = 3000 + rc;
        LISA_LOGE(TAG, "wifi_ap_start rc=%d", rc);
        return false;
    }
    for (int i = 0; i < 30; i++) {
        if (wifi_get_ap_state(&state) == LS_OK && state) break;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!state) {
        s_last_error = 4000;
        return false;
    }

    netif = net_if_get(WIFI_VIF_AP_IDX);
    if (!netif) {
        s_last_error = 5000;
        return false;
    }
    net_if_up(netif);
    ip.mode = IP_ADDR_STATIC_IPV4;
    ip.default_output = true;
    ip.ipv4.addr = inet_addr("192.168.4.1");
    ip.ipv4.mask = inet_addr("255.255.255.0");
    ip.ipv4.gw = inet_addr("192.168.4.1");
    ip.ipv4.dns = inet_addr("192.168.4.1");
    rc = ls_set_static_ip(WIFI_VIF_AP_IDX, &ip);
    if (rc != LS_OK) {
        s_last_error = 6000 + rc;
        return false;
    }
    rc = ls_dhcps_start(WIFI_VIF_AP_IDX);
    if (rc != LS_OK) s_last_error = 7000 + rc;
    return rc == LS_OK;
}

static void stop_ap_restore_sta(bool autoconnect)
{
    (void)ls_dhcps_stop();
    (void)wifi_ap_stop();
    if (net_if_get(WIFI_VIF_AP_IDX)) net_if_down(net_if_get(WIFI_VIF_AP_IDX));
    vTaskDelay(pdMS_TO_TICKS(250));
    (void)wifi_mgr_sta_enable();
    (void)sys_wifi_start(autoconnect);
}

static void provision_task(void *arg)
{
    wifi_mgr_scan_info_t *aps = NULL;
    int ap_count = 0;
    bool ap_started = false;
    bool http_started = false;
    TickType_t deadline;
    (void)arg;

    s_state = PROV_SCANNING;
    s_last_error = 0;
    s_submitted = false;
    memset(s_target_ssid, 0, sizeof(s_target_ssid));
    memset(s_target_pwd, 0, sizeof(s_target_pwd));
    aps = calloc(PROV_SCAN_MAX, sizeof(*aps));
    if (aps) {
        ap_count = wifi_mgr_scan_ap(aps, PROV_SCAN_MAX, false);
        if (ap_count < 0) ap_count = 0;
    }
    if (!build_form_page(aps, ap_count)) {
        s_last_error = 7500;
        s_state = PROV_FAILED;
        goto done;
    }

    s_state = PROV_STARTING;
    ap_started = start_ap();
    if (!ap_started) {
        LISA_LOGE(TAG, "SoftAP start failed");
        s_state = PROV_FAILED;
        goto done;
    }
    http_started = raw_http_start();
    if (!http_started) {
        s_last_error = 8000 + s_raw_error;
        LISA_LOGE(TAG, "raw HTTP start failed rc=%d", (int)s_raw_error);
        s_state = PROV_FAILED;
        goto done;
    }

    s_state = PROV_WAITING;
    LISA_LOGI(TAG, "portal ready ssid=%s url=http://192.168.4.1 timeout=%ds", s_ap_ssid,
              PROV_TIMEOUT_SEC);
    deadline = xTaskGetTickCount() + pdMS_TO_TICKS(PROV_TIMEOUT_SEC * 1000U);
    while (!s_stop && !s_submitted && (int32_t)(deadline - xTaskGetTickCount()) > 0) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (s_submitted) vTaskDelay(pdMS_TO_TICKS(750));

done:
    if (http_started) raw_http_stop();
    free(s_form_page);
    s_form_page = NULL;
    s_form_page_len = 0;
    free(aps);
    /* start_ap 可能在已关闭 STA 后的任一步失败；无论 AP 是否完全启动都恢复 STA。 */
    if (ap_started || s_last_error != 0) stop_ap_restore_sta(!s_submitted);

    if (s_submitted) {
        int rc;
        s_state = PROV_CONNECTING;
        rc = sys_network_connect_wifi(s_target_ssid, s_target_pwd[0] ? s_target_pwd : NULL, NULL);
        if (rc == 0 && sys_wifi_is_connected() &&
            sys_wifi_save_ap(s_target_ssid, s_target_pwd[0] ? s_target_pwd : NULL, NULL) == 0) {
            s_state = PROV_SUCCESS;
            LISA_LOGI(TAG, "provision connected and saved ssid=%s", s_target_ssid);
        } else {
            s_state = PROV_FAILED;
            LISA_LOGW(TAG, "provision connect failed ssid=%s rc=%d result=%d", s_target_ssid, rc,
                      (int)sys_wifi_get_connect_result());
            (void)sys_wifi_start(true);
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    } else if (s_state != PROV_FAILED) {
        s_state = PROV_IDLE;
    }
    s_stop = false;
    s_submitted = false;
    s_task = NULL;
    vTaskDelete(NULL);
}

bool service_provision_start(void)
{
    uint8_t mac[6] = {0};
    if (s_task || !sys_wifi_is_ready()) return false;
    if (wifi_get_ap_mac(mac) == LS_OK) {
        snprintf(s_ap_ssid, sizeof(s_ap_ssid), "Mini3-Setup-%02X%02X", mac[4], mac[5]);
        snprintf(s_ap_pwd, sizeof(s_ap_pwd), "M3%02X%02X%02X!", mac[3], mac[4], mac[5]);
    }
    s_stop = false;
    return xTaskCreate(provision_task, "wifi_portal", PROV_STACK_WORDS, NULL, 4, &s_task) == pdPASS;
}

void service_provision_stop(void) { s_stop = true; }
bool service_provision_active(void) { return s_task != NULL; }
const char *service_provision_status(void) { return s_state_text[(unsigned)s_state < 7 ? s_state : 0]; }
const char *service_provision_ssid(void) { return s_ap_ssid; }
const char *service_provision_password(void) { return s_ap_pwd; }
const char *service_provision_url(void) { return "192.168.4.1"; }

#if CONFIG_LISA_SHELL
static int portal_cmd_handler(int argc, char **argv)
{
    Shell *sh = shellGetCurrent();

    if (argc >= 2 && strcmp(argv[1], "start") == 0) {
        shellPrint(sh, "portal start: %s\n", service_provision_start() ? "ok" : "failed");
    } else if (argc >= 2 && strcmp(argv[1], "stop") == 0) {
        service_provision_stop();
        shellPrint(sh, "portal stop requested\n");
    } else {
        shellPrint(sh, "state=%s active=%d error=%d ssid=%s pwd=%s url=http://%s\n",
                   service_provision_status(), service_provision_active(), (int)s_last_error,
                   service_provision_ssid(), service_provision_password(), service_provision_url());
    }
    return 0;
}

SHELL_EXPORT_CMD(SHELL_CMD_PERMISSION(0) | SHELL_CMD_TYPE(SHELL_TYPE_CMD_MAIN) |
                     SHELL_CMD_DISABLE_RETURN,
                 portal, portal_cmd_handler, temporary WiFi setup portal);
#endif
