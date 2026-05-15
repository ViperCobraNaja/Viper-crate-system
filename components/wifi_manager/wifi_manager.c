#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_wifi_remote.h"
#include "esp_eap_client.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "wifi_manager.h"
#include "http_server.h"

static const char *TAG = "wifi_mgr";

#define WIFI_NVS_NS         "wifi_cred"
#define WIFI_AP_SSID        "PetWhisperer-Setup"
#define WIFI_MAX_RETRY       3
#define WIFI_CONNECT_TIMEOUT_MS  15000

static volatile bool s_connected = false;
static char s_ip_str[16] = {0};
static int s_retry_count = 0;
static bool s_ntp_started = false;

static void ntp_sync_start(void)
{
    if (s_ntp_started) return;
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_setservername(1, "pool.ntp.org");
    esp_sntp_init();
    s_ntp_started = true;
    ESP_LOGI(TAG, "NTP sync started (ntp.aliyun.com, pool.ntp.org)");
}

static esp_err_t connect_to_wifi(const wifi_credential_t *cred);

/* ---- NVS 凭证存取 ---- */
static esp_err_t nvs_save_cred(const wifi_credential_t *cred)
{
    nvs_handle_t h;
    esp_err_t ret = nvs_open(WIFI_NVS_NS, NVS_READWRITE, &h);
    if (ret != ESP_OK) return ret;

    ret = nvs_set_blob(h, "cred", cred, sizeof(wifi_credential_t));
    if (ret == ESP_OK) nvs_commit(h);
    nvs_close(h);
    return ret;
}

static esp_err_t nvs_load_cred(wifi_credential_t *cred)
{
    nvs_handle_t h;
    esp_err_t ret = nvs_open(WIFI_NVS_NS, NVS_READONLY, &h);
    if (ret != ESP_OK) return ret;

    size_t size = sizeof(wifi_credential_t);
    ret = nvs_get_blob(h, "cred", cred, &size);
    nvs_close(h);
    return ret;
}

esp_err_t wifi_manager_reset(void)
{
    nvs_handle_t h;
    nvs_open(WIFI_NVS_NS, NVS_READWRITE, &h);
    nvs_erase_all(h);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Credentials erased, reboot to re-provision");
    esp_restart();
    return ESP_OK; /* never reaches */
}

/* ---- WiFi 事件回调 ---- */
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *ev = data;
            ESP_LOGW(TAG, "WiFi disconnected: reason=%d", ev->reason);
            s_connected = false;
            memset(s_ip_str, 0, sizeof(s_ip_str));
            if (s_retry_count < WIFI_MAX_RETRY) {
                s_retry_count++;
                esp_wifi_connect();
                ESP_LOGI(TAG, "Reconnecting (%d/%d)...", s_retry_count, WIFI_MAX_RETRY);
            } else {
                ESP_LOGW(TAG, "Max retries, staying in AP mode for re-provision");
            }
            break;
        }
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = data;
        snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&ev->ip_info.ip));
        ESP_LOGI(TAG, "WiFi connected: %s", s_ip_str);
        s_connected = true;
        s_retry_count = 0;
        ntp_sync_start();
    }
}

static esp_err_t connect_to_wifi(const wifi_credential_t *cred)
{
    wifi_config_t cfg = {0};
    memcpy(cfg.sta.ssid, cred->ssid, sizeof(cfg.sta.ssid));

    if (cred->auth_mode == WIFI_AUTH_WPA2_PSK || cred->auth_mode == WIFI_AUTH_WPA3_PSK
        || cred->auth_mode == WIFI_AUTH_WPA_WPA2_PSK || cred->auth_mode == WIFI_AUTH_WPA2_WPA3_PSK) {
        memcpy(cfg.sta.password, cred->password, sizeof(cfg.sta.password));
    } else if (cred->auth_mode == WIFI_AUTH_WPA2_ENTERPRISE) {
        cfg.sta.pmf_cfg.capable = true;
    }
    /* WIFI_AUTH_OPEN: no password */

    esp_wifi_set_config(WIFI_IF_STA, &cfg);

    if (cred->auth_mode == WIFI_AUTH_WPA2_ENTERPRISE) {
        esp_eap_client_set_identity((uint8_t *)cred->identity, strlen(cred->identity));
        esp_eap_client_set_username((uint8_t *)cred->identity, strlen(cred->identity));
        esp_eap_client_set_password((uint8_t *)cred->eap_password, strlen(cred->eap_password));
        esp_eap_client_set_disable_time_check(true);
        ESP_LOGI(TAG, "WPA2-Enterprise: identity=%s", cred->identity);
    }

    ESP_LOGI(TAG, "Connecting to %s...", cred->ssid);
    return esp_wifi_connect();
}

esp_err_t wifi_manager_provision(const wifi_credential_t *cred)
{
    esp_err_t ret = nvs_save_cred(cred);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save cred");
        return ret;
    }
    s_retry_count = 0;
    return connect_to_wifi(cred);
}

esp_err_t wifi_manager_init(void)
{
    /* NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* TCP/IP + netif */
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    /* WiFi 初始化 (esp_wifi_remote 自动路由) */
    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    /* 事件回调 */
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                wifi_event_handler, NULL));

    /* 启动 SoftAP */
    wifi_config_t ap_cfg = {
        .ap = {
            .ssid = WIFI_AP_SSID,
            .ssid_len = strlen(WIFI_AP_SSID),
            .authmode = WIFI_AUTH_OPEN,
            .max_connection = 4,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "SoftAP: %s", WIFI_AP_SSID);

    /* 启动配网 HTTP server */
    http_server_start();

    /* 尝试从 NVS 加载凭证并连接 */
    wifi_credential_t saved_cred;
    if (nvs_load_cred(&saved_cred) == ESP_OK) {
        ESP_LOGI(TAG, "Loaded saved cred for %s", saved_cred.ssid);
        connect_to_wifi(&saved_cred);
    } else {
        ESP_LOGI(TAG, "No saved credentials, waiting for provisioning");
    }

    return ESP_OK;
}

bool wifi_is_connected(void)
{
    return s_connected;
}

esp_err_t wifi_get_ip(char *ip_str, size_t len)
{
    if (!s_connected) return ESP_FAIL;
    strncpy(ip_str, s_ip_str, len);
    return ESP_OK;
}
