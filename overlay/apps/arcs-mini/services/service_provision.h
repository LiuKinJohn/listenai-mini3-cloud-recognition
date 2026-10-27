#ifndef SERVICE_PROVISION_H
#define SERVICE_PROVISION_H

#include <stdbool.h>
#include <stddef.h>

bool service_provision_start(void);
void service_provision_stop(void);
bool service_provision_active(void);
const char *service_provision_status(void);
const char *service_provision_ssid(void);
const char *service_provision_password(void);
const char *service_provision_url(void);

#endif /* SERVICE_PROVISION_H */
