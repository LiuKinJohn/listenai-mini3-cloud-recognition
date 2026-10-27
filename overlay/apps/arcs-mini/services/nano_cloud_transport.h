#pragma once
#include <stddef.h>
#include <stdint.h>
int nano_cloud_post(const uint8_t *image, size_t length, const char *gateway_token,
                    const char *device_token, char *body, size_t body_cap);
