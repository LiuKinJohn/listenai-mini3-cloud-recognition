/* Temporary, credential-free TLS diagnostic for the Mini3 cloud relay. */
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include "FreeRTOS.h"
#include "task.h"
#include "listen_system.h"
#include "sys_wifi.h"
#include "ze_tls.h"
#include "lwip/netdb.h"
#include "cmd/cmd.h"
#include "shell.h"
#include "nano_root_ca.h"

#if !defined(MBEDTLS_HAVE_TIME) || !defined(MBEDTLS_HAVE_TIME_DATE)
#error "TLS diagnostic requires certificate date checks"
#endif

#define RELAY_HOST "mini-clognition-zcbovnxwvp.cn-beijing.fcapp.run"

/* Overrides the SDK's weak uptime-based clock only for mbedTLS. */
mbedtls_time_t ls_time(mbedtls_time_t *out)
{
    struct timeval tv = {0};
    mbedtls_time_t result = 0;
    if (ls_sys_time_is_valid() && ls_sys_get_time(&tv) == 0)
        result = (mbedtls_time_t)tv.tv_sec;
    if (out) *out = result;
    return result;
}

/* The SDK helper applies China time and uses a 1970 year base. mbedTLS needs
 * UTC and struct tm's 1900 year base. Keep this correction inside mbedTLS. */
struct tm *mbedtls_platform_gmtime_r(const mbedtls_time_t *clock, struct tm *out)
{
    long utc_adjusted;
    if (!clock || !out || *clock < 8 * 3600) return NULL;
    utc_adjusted = (long)*clock - 8 * 3600;
    memset(out, 0, sizeof(*out));
    if (!ls_sys_get_tmtime(&utc_adjusted, out)) return NULL;
    out->tm_year += 70;
    return out;
}

static volatile bool probe_running;
static volatile int probe_result = -999;
static volatile unsigned probe_flags;
static bool wrong_name;

static void probe_task(void *unused)
{
    struct addrinfo hints = {0}, *address = NULL;
    security_client cert = {0};
    mbedtls_context *ctx = NULL;
    mbedtls_sock sock;
    int rc = -1;
    (void)unused;
    sock.fd = -1;
    if (!sys_wifi_is_connected() || !ls_sys_time_is_valid()) { rc = -10; goto done; }
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(RELAY_HOST, "443", &hints, &address) != 0 || !address) { rc = -11; goto done; }
    sock.fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
    if (sock.fd < 0) { rc = -12; goto done; }
    ctx = mbedtls_init_context(MBEDTLS_SSL_IS_CLIENT);
    if (!ctx) { rc = -13; goto done; }
    cert.pCa = (char *)nano_root_ca;
    cert.nCa = sizeof(nano_root_ca);
    if (mbedtls_config_context(ctx, &cert, MBEDTLS_SSL_VERIFY_REQUIRED) != 0) { rc = -14; goto done; }
    if (mbedtls_connect(ctx, &sock, address->ai_addr, address->ai_addrlen,
                        wrong_name ? "wrong-host.example" : RELAY_HOST) != 0) { rc = -15; goto done; }
    if (mbedtls_handshake(ctx, &sock) != 0) {
        probe_flags = mbedtls_ssl_get_verify_result(&ctx->ssl);
        rc = -16;
        goto done;
    }
    probe_flags = mbedtls_ssl_get_verify_result(&ctx->ssl);
    rc = probe_flags ? -17 : 0;
done:
    if (ctx) mbedtls_deinit_context(ctx);
    if (sock.fd >= 0) closesocket(sock.fd);
    if (address) freeaddrinfo(address);
    probe_result = rc;
    probe_running = false;
    vTaskDelete(NULL);
}

static int tls_probe_cmd(int argc, char **argv)
{
    Shell *sh = shellGetCurrent();
    if (argc > 1 && !strcmp(argv[1], "status")) {
        shellPrint(sh, "tls probe running=%d result=%d flags=0x%x\n", probe_running, probe_result, probe_flags);
        return 0;
    }
    if (probe_running) { shellPrint(sh, "tls probe busy\n"); return 0; }
    wrong_name = argc > 1 && !strcmp(argv[1], "wrong-name");
    probe_result = -999;
    probe_flags = 0;
    probe_running = true;
    if (xTaskCreate(probe_task, "tls.probe", 8192, NULL, 4, NULL) != pdPASS) {
        probe_running = false;
        probe_result = -18;
    }
    shellPrint(sh, "tls probe started; use tlsprobe status\n");
    return 0;
}

SHELL_EXPORT_CMD(SHELL_CMD_PERMISSION(0) | SHELL_CMD_TYPE(SHELL_TYPE_CMD_MAIN) | SHELL_CMD_DISABLE_RETURN,
                 tlsprobe, tls_probe_cmd, verify relay TLS without credentials);
