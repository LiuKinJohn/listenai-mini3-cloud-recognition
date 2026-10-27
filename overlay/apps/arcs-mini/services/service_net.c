/*
 * 有线配网。走 USB shell 而不是 BLE：这块板子的线本来就常驻，
 * 而 BLE 那套协议要手机按 START/SSID/PWD/DONE 分四包写，还得装专用工具。
 */

#include <stdbool.h>
#include <string.h>

#include "lisa_log.h"
#include "sys_network_manager.h"
#include "sys_wifi.h"
#include "FreeRTOS.h"
#include "task.h"
#include "API/HTTPClient.h"

#if CONFIG_LISA_SHELL

#include "cmd/cmd.h"
#include "shell.h"

#define TAG "net"

/* wifi_mgr_wifi_sta_config_t 是 ssid[32] / pwd[64]，超出的部分会被
 * strncpy 悄悄切掉，于是"密码明明对却连不上"。在门口就拦住。 */
#define NET_SSID_MAX 31
#define NET_PWD_MAX  63

/* Public health endpoint only; no photographs, credentials, or tokens are sent. */
static volatile bool s_probe_running;
static volatile int s_probe_rc = -999;
static volatile int s_probe_http;
static char s_probe_url[96];

static void net_probe_task(void *arg)
{
    HTTP_SESSION_HANDLE h;
    HTTP_CLIENT info;
    int rc;
    (void)arg;
    h = HTTPClientOpenRequest(0);
    if (!h) {
        s_probe_rc = -998;
        s_probe_running = false;
        vTaskDelete(NULL);
        return;
    }
    rc = HTTPClientSetVerb(h, VerbGet);
    if (rc == HTTP_CLIENT_SUCCESS)
        rc = HTTPClientSendRequest(h,
            s_probe_url,
            NULL, 0, FALSE, 15, 0, NULL, NULL);
    if (rc == HTTP_CLIENT_SUCCESS) rc = HTTPClientRecvResponse(h, 15);
    if (rc == HTTP_CLIENT_SUCCESS && HTTPClientGetInfo(h, &info) == HTTP_CLIENT_SUCCESS)
        s_probe_http = info.HTTPStatusCode;
    s_probe_rc = rc;
    HTTPClientCloseRequest(&h);
    s_probe_running = false;
    vTaskDelete(NULL);
}

static int net_probe(int argc, char **argv)
{
    Shell *sh = shellGetCurrent();
    if (s_probe_running) {
        shellPrint(sh, "probe running\n");
    } else if (!sys_wifi_is_connected()) {
        shellPrint(sh, "probe unavailable: wifi offline\n");
    } else {
        s_probe_running = true;
        snprintf(s_probe_url, sizeof(s_probe_url), "%s",
                 (argc > 0 && strcmp(argv[0], "ali") == 0)
                 ? "https://dashscope.aliyuncs.com/"
                 : "https://mini3-cloud-observer.mini3-cloud-observer.workers.dev/health");
        s_probe_rc = -999;
        s_probe_http = 0;
        if (xTaskCreate(net_probe_task, "net.probe", 8192, NULL, 4, NULL) != pdPASS) {
            s_probe_running = false;
            shellPrint(sh, "probe task failed\n");
        } else shellPrint(sh, "probe started; use net probe-status\n");
    }
    return 0;
}

static int net_probe_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    shellPrint(shellGetCurrent(), "probe running=%d rc=%d http=%d\n",
               s_probe_running, s_probe_rc, s_probe_http);
    return 0;
}

static const char *net_result_text(sys_wifi_connect_result_t r)
{
    switch (r) {
    case SYS_WIFI_CONNECT_RESULT_SUCCESS:
        return "连上了";
    case SYS_WIFI_CONNECT_RESULT_FAIL_NO_AP:
        return "扫不到这个 SSID";
    case SYS_WIFI_CONNECT_RESULT_FAIL_PASSWORD:
        return "密码不对或认证失败";
    case SYS_WIFI_CONNECT_RESULT_FAIL_IP:
        return "关联成功但没拿到 IP";
    default:
        return "其它原因";
    }
}

static int net_show_status(void)
{
    Shell *sh = shellGetCurrent();
    int rssi = 0;

    shellPrint(sh, "wifi      : %s\n", sys_wifi_is_connected() ? "CONNECTED" : "offline");
    shellPrint(sh, "radio     : ready=%d started=%d\n", sys_wifi_is_ready(), sys_wifi_is_started());
    shellPrint(sh, "saved     : %s\n", sys_wifi_has_ap() ? "yes" : "no");

    if (sys_wifi_is_connected() && sys_wifi_get_signal_quality(&rssi)) {
        shellPrint(sh, "rssi      : %d dBm\n", rssi);
    }

    return 0;
}

static int net_connect(const char *ssid, const char *pwd)
{
    Shell *sh = shellGetCurrent();
    bool has_pwd = (pwd != NULL && pwd[0] != '\0');
    int rc;

    if (strlen(ssid) > NET_SSID_MAX) {
        shellPrint(sh, "SSID 太长：%u 字节，最多 %u\n", (unsigned)strlen(ssid), NET_SSID_MAX);
        return -1;
    }
    if (has_pwd && strlen(pwd) > NET_PWD_MAX) {
        shellPrint(sh, "密码太长：%u 字节，最多 %u\n", (unsigned)strlen(pwd), NET_PWD_MAX);
        return -1;
    }

    shellPrint(sh, "connecting \"%s\"%s\n", ssid, has_pwd ? "" : " (open)");

    rc = sys_network_connect_wifi(ssid, has_pwd ? pwd : NULL, NULL);
    if (rc != 0) {
        shellPrint(sh, "connect failed rc=%d\n", rc);
    }

    if (!sys_wifi_is_connected()) {
        shellPrint(sh, "result    : %s\n", net_result_text(sys_wifi_get_connect_result()));
        return -1;
    }

    /* 驱动在连接成功事件里已经存过一次，这里再存一遍是为了让"重启后还在"
     * 由我们这句话保证，而不是指望别人的回调。失败时不存，免得把错密码写进去。 */
    if (sys_wifi_save_ap(ssid, has_pwd ? pwd : NULL, NULL) != 0) {
        shellPrint(sh, "connected but save failed\n");
        return -1;
    }

    shellPrint(sh, "saved     : yes\n");

    return 0;
}

static int net_forget(int argc, char **argv)
{
    Shell *sh = shellGetCurrent();
    int rc;

    (void)argc;
    (void)argv;

    rc = sys_wifi_clear_saved_aps();
    shellPrint(sh, "clear saved aps: %d\n", rc);
    LISA_LOGI(TAG, "凭据已由 net forget 清除");

    return rc;
}

static const struct listen_cmd_t g_net_cmds[] = {
    {"probe", net_probe, "test public Worker health"},
    {"probe-status", net_probe_status, "show Worker test result"},
    {"forget", net_forget, "clear stored wifi credentials"},
    {"help", NULL, "net | net <ssid> [pwd] | net forget"},
};

static int net_cmd_handler(int argc, char **argv)
{
    Shell *sh = shellGetCurrent();

    if (argc < 2) {
        return net_show_status();
    }

    for (unsigned i = 0; i < sizeof(g_net_cmds) / sizeof(g_net_cmds[0]); i++) {
        if (g_net_cmds[i].exec && strcmp(g_net_cmds[i].name, argv[1]) == 0) {
            return g_net_cmds[i].exec(argc - 2, argv + 2);
        }
    }

    if (strcmp(argv[1], "help") == 0) {
        for (unsigned i = 0; i < sizeof(g_net_cmds) / sizeof(g_net_cmds[0]); i++) {
            shellPrint(sh, "%-8s : %s\n", g_net_cmds[i].name, g_net_cmds[i].help);
        }
        shellPrint(sh, "(none)   : show status\n");
        return 0;
    }

    return net_connect(argv[1], (argc >= 3) ? argv[2] : NULL);
}

SHELL_EXPORT_CMD(SHELL_CMD_PERMISSION(0) | SHELL_CMD_TYPE(SHELL_TYPE_CMD_MAIN) | SHELL_CMD_DISABLE_RETURN,
                 net, net_cmd_handler, provision wifi over usb);

#endif /* CONFIG_LISA_SHELL */
