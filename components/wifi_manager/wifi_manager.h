#pragma once
#include "esp_err.h"
#include "esp_wifi_types.h"
#include <stdbool.h>

typedef struct {
    wifi_auth_mode_t auth_mode;
    char ssid[33];
    char password[65];
    char identity[65];
    char eap_password[65];
} wifi_credential_t;

esp_err_t wifi_manager_init(void);
bool wifi_is_connected(void);
esp_err_t wifi_get_ip(char *ip_str, size_t len);
esp_err_t wifi_manager_reset(void);

/* HTTP server calls this to save cred + connect */
esp_err_t wifi_manager_provision(const wifi_credential_t *cred);
