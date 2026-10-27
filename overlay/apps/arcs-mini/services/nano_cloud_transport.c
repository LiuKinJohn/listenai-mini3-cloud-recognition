/* A bounded HTTPS client. Credentials are sent only after verified TLS. */
#include "nano_cloud_transport.h"
#include "nano_root_ca.h"
#include "listen_system.h"
#include "sys_wifi.h"
#include "ze_tls.h"
#include "lwip/netdb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#if !defined(MBEDTLS_HAVE_TIME) || !defined(MBEDTLS_HAVE_TIME_DATE)
#error "Cloud requests require certificate time checks"
#endif
#define CLOUD_HOST "mini-clognition-zcbovnxwvp.cn-beijing.fcapp.run"
#define RESPONSE_LIMIT 8192U
#define IMAGE_LIMIT (350U * 1024U)

static int write_all(mbedtls_context *ctx, const void *data, size_t length)
{
    const unsigned char *p = data;
    while (length) {
        int n = mbedtls_send(ctx, (char *)p, length > 2048 ? 2048 : (int)length);
        if (n <= 0) return -1;
        p += n; length -= (size_t)n;
    }
    return 0;
}

/* Expect a bounded, content-length-delimited response. Fail closed for
 * malformed or unsupported framing instead of interpreting partial JSON. */
static int response_body(char *data, size_t length, char *out, size_t capacity)
{
    char *end = strstr(data, "\r\n\r\n"), *p, *line;
    unsigned long wanted = 0;
    bool found = false;
    if (!end || strncmp(data, "HTTP/1.", 7) || strlen(data) < 12 ||
        strncmp(data + 9, "200", 3)) return -1;
    line = strstr(data, "\r\n");
    if (!line) return -1;
    line += 2;
    while (line < end) {
        p = strstr(line, "\r\n");
        if (!p || p > end) return -1;
        if (!strncasecmp(line, "Transfer-Encoding:", 18)) return -1;
        if (!strncasecmp(line, "Content-Length:", 15)) {
            char *number_end;
            if (found) return -1;
            wanted = strtoul(line + 15, &number_end, 10);
            while (number_end < p && (*number_end == ' ' || *number_end == '\t')) number_end++;
            if (number_end != p) return -1;
            found = true;
        }
        line = p + 2;
    }
    p = end + 4;
    if (!found || wanted >= capacity || wanted != length - (size_t)(p - data)) return -1;
    memcpy(out, p, wanted); out[wanted] = '\0';
    return 0;
}

int nano_cloud_post(const uint8_t *image, size_t length, const char *gateway_token,
                    const char *device_token, char *body, size_t body_cap)
{
    struct addrinfo hints = {0}, *address = NULL;
    security_client cert = {0};
    mbedtls_context *ctx = NULL;
    mbedtls_sock sock;
    char header[512], response[RESPONSE_LIMIT];
    size_t used = 0;
    int rc = -1, n;
    sock.fd = -1;
    if (!image || !length || length > IMAGE_LIMIT || !gateway_token || !device_token ||
        !*gateway_token || !*device_token || !body || body_cap < 2 ||
        strpbrk(gateway_token, "\r\n") || strpbrk(device_token, "\r\n") ||
        !sys_wifi_is_connected() || !ls_sys_time_is_valid()) return -1;
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(CLOUD_HOST, "443", &hints, &address) != 0 || !address) goto done;
    sock.fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
    if (sock.fd < 0) goto done;
    ctx = mbedtls_init_context(MBEDTLS_SSL_IS_CLIENT);
    if (!ctx) goto done;
    cert.pCa = (char *)nano_root_ca; cert.nCa = sizeof(nano_root_ca);
    if (mbedtls_config_context(ctx, &cert, MBEDTLS_SSL_VERIFY_REQUIRED) != 0) goto done;
    mbedtls_set_recv_timeout(ctx, 60000);
    if (mbedtls_connect(ctx, &sock, address->ai_addr, address->ai_addrlen, CLOUD_HOST) != 0 ||
        mbedtls_handshake(ctx, &sock) != 0 || mbedtls_ssl_get_verify_result(&ctx->ssl) != 0) goto done;
    n = snprintf(header, sizeof(header),
        "POST /recognize HTTP/1.1\r\nHost: " CLOUD_HOST "\r\nAuthorization: Bearer %s\r\n"
        "X-Device-Token: %s\r\nContent-Type: image/jpeg\r\nContent-Length: %lu\r\nConnection: close\r\n\r\n",
        gateway_token, device_token, (unsigned long)length);
    if (n <= 0 || (size_t)n >= sizeof(header) || write_all(ctx, header, (size_t)n) ||
        write_all(ctx, image, length)) goto done;
    while (used + 1 < sizeof(response)) {
        n = mbedtls_recv(ctx, response + used, (int)(sizeof(response) - used - 1));
        if (n < 0) goto done;
        if (!n) break;
        used += (size_t)n;
    }
    response[used] = '\0';
    rc = response_body(response, used, body, body_cap);
done:
    memset(header, 0, sizeof(header));
    if (ctx) mbedtls_deinit_context(ctx);
    if (sock.fd >= 0) closesocket(sock.fd);
    if (address) freeaddrinfo(address);
    return rc;
}
