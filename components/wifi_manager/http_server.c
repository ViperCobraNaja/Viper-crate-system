#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_wifi_remote.h"
#include "wifi_manager.h"
#include "http_server.h"

static const char *TAG = "wifi_http";

/* ---- 配网 HTML 页面 ---- */
static const char PROV_HTML[] =
"<!DOCTYPE html><html><head><meta charset=utf-8><meta name=viewport"
" content='width=device-width,initial-scale=1'><title>PetWhisperer WiFi</title>"
"<style>*{box-sizing:border-box;margin:0;padding:0}"
"body{font:16px sans-serif;background:#1a1a2e;color:#eee;min-height:100vh;display:flex;align-items:center;justify-content:center}"
".card{background:#16213e;border-radius:12px;padding:24px;width:100%;max-width:380px;margin:16px}"
"h1{font-size:20px;text-align:center;margin-bottom:20px;color:#e94560}"
"label{display:block;font-size:13px;color:#aaa;margin:10px 0 4px}"
"select,input{width:100%;padding:10px;border-radius:8px;border:1px solid #333;background:#0f3460;color:#eee;font-size:14px;margin-bottom:8px}"
"button{width:100%;padding:12px;border-radius:8px;border:none;background:#e94560;color:#fff;font-size:16px;font-weight:bold;cursor:pointer;margin-top:8px}"
"button:disabled{opacity:.5;cursor:not-allowed}"
".hidden{display:none}"
".status{padding:10px;border-radius:8px;text-align:center;margin-top:12px;font-size:14px}"
".ok{background:#1b4d1b;color:#7f7}"
".err{background:#4d1b1b;color:#f77}"
"</style></head><body>"
"<div class=card>"
"<h1>PetWhisperer WiFi</h1>"
"<label>WiFi 网络</label>"
"<select id=ssid onchange=onSSID()>"
"<option value=''>选择网络...</option></select>"
"<label>认证方式</label>"
"<select id=auth onchange=onAuth()>"
"<option value=0>WPA2 个人 (家庭/热点)</option>"
"<option value=1>WPA2 企业 (校园网)</option>"
"<option value=2>开放网络</option></select>"
"<div id=divpw><label>密码</label>"
"<input id=password type=password placeholder=WiFi密码></div>"
"<div id=diveap class=hidden><label>用户名</label>"
"<input id=identity type=text placeholder='user@univ.edu'>"
"<label>密码</label>"
"<input id=eap_password type=password placeholder=认证密码></div>"
"<button onclick=connect()>连接</button>"
"<div id=status></div>"
"</div>"
"<script>"
"async function scan(){"
"let r=await fetch('/scan');let d=await r.json();"
"let s=document.getElementById('ssid');"
"s.innerHTML=\"<option value=''>选择网络...</option>\";"
"d.forEach(function(n){s.innerHTML+=`<option value='${n.ssid}' data-auth='${n.auth}'>${n.ssid}</option>`})}"
"function onSSID(){"
"let s=document.getElementById('ssid');"
"let opt=s.options[s.selectedIndex];"
"document.getElementById('auth').value=(opt.dataset.auth||0)}"
"function onAuth(){"
"let v=parseInt(document.getElementById('auth').value);"
"document.getElementById('divpw').className=v==1?'hidden':'';"
"document.getElementById('diveap').className=v==1?'':'hidden';"
"if(v==2)document.getElementById('divpw').className='hidden'}"
"async function connect(){"
"let s=document.getElementById('status');s.className='status';s.textContent='Connecting...';"
"let auth=parseInt(document.getElementById('auth').value);"
"let body={auth_type:auth,ssid:document.getElementById('ssid').value,"
"password:document.getElementById('password').value,"
"identity:document.getElementById('identity').value,"
"eap_password:document.getElementById('eap_password').value};"
"try{"
"let r=await fetch('/connect',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});"
"let d=await r.json();"
"if(d.success){s.className='status ok';s.textContent='Connected: '+d.ip}"
"else{s.className='status err';s.textContent='Failed: '+d.error}}"
"catch(e){s.className='status err';s.textContent='Error: '+e.message}}"
"scan();onAuth()"
"</script></body></html>";

/* ---- JSON 辅助 ---- */
static char *json_escape(const char *src, char *dst, size_t dst_len)
{
    size_t j = 0;
    for (size_t i = 0; src[i] && j < dst_len - 1; i++) {
        if (src[i] == '"' && j < dst_len - 2) { dst[j++] = '\\'; dst[j++] = '"'; }
        else if (src[i] == '\\' && j < dst_len - 2) { dst[j++] = '\\'; dst[j++] = '\\'; }
        else dst[j++] = src[i];
    }
    dst[j] = '\0';
    return dst;
}

static int json_parse_int(const char *json, const char *key, int def)
{
    char search[48];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p) return def;
    p += strlen(search);
    while (*p == ' ' || *p == '\t') p++;
    return atoi(p);
}

static void json_parse_str(const char *json, const char *key, char *out, size_t out_len)
{
    char search[48];
    snprintf(search, sizeof(search), "\"%s\":\"", key);
    const char *p = strstr(json, search);
    if (!p) { out[0] = '\0'; return; }
    p += strlen(search);
    size_t i = 0;
    while (*p && *p != '"' && i < out_len - 1) {
        if (*p == '\\' && *(p+1) == '"') { p++; out[i++] = '"'; }
        else out[i++] = *p;
        p++;
    }
    out[i] = '\0';
}

/* 将 ESP-IDF authmode 映射为前端简化类型: 0=Personal, 1=Enterprise, 2=Open */
static int authmode_to_simple(wifi_auth_mode_t mode)
{
    if (mode == WIFI_AUTH_OPEN) return 2;
    if (mode == WIFI_AUTH_WPA2_ENTERPRISE ||
        mode == WIFI_AUTH_WPA3_ENTERPRISE ||
        mode == WIFI_AUTH_WPA_ENTERPRISE ||
        mode == WIFI_AUTH_WPA2_WPA3_ENTERPRISE ||
        mode == WIFI_AUTH_WPA3_ENT_192) return 1;
    return 0; /* WPA2-PSK, WPA3-PSK, WEP 等都当 Personal */
}

/* 将前端简化类型映射为 ESP-IDF authmode */
static wifi_auth_mode_t simple_to_authmode(int stype)
{
    if (stype == 2) return WIFI_AUTH_OPEN;
    if (stype == 1) return WIFI_AUTH_WPA2_ENTERPRISE;
    return WIFI_AUTH_WPA2_PSK;
}

/* ---- HTTP 处理器 ---- */
static esp_err_t handler_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, PROV_HTML, sizeof(PROV_HTML) - 1);
    return ESP_OK;
}

static esp_err_t handler_scan(httpd_req_t *req)
{
    wifi_scan_config_t scan_cfg = {
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };
    esp_wifi_scan_start(&scan_cfg, true);

    uint16_t num = 0;
    esp_wifi_scan_get_ap_num(&num);
    if (num == 0) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "[]");
        return ESP_OK;
    }

    wifi_ap_record_t *aps = calloc(num, sizeof(wifi_ap_record_t));
    esp_wifi_scan_get_ap_records(&num, aps);

    char *json = malloc(num * 128 + 8);
    if (!json) { free(aps); return ESP_ERR_NO_MEM; }

    size_t pos = 1; /* skip initial '[' */
    json[0] = '[';
    for (int i = 0; i < num; i++) {
        int atype = authmode_to_simple(aps[i].authmode);

        char safe_ssid[36];
        json_escape((char *)aps[i].ssid, safe_ssid, sizeof(safe_ssid));

        int n = snprintf(json + pos, (num * 128 + 8) - pos,
                         "%s{\"ssid\":\"%s\",\"auth\":%d,\"rssi\":%d}",
                         i > 0 ? "," : "", safe_ssid, atype, aps[i].rssi);
        if (n < 0) break;
        pos += n;
    }
    pos += snprintf(json + pos, 4, "]");

    free(aps);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, pos);
    free(json);
    return ESP_OK;
}

static esp_err_t handler_connect(httpd_req_t *req)
{
    char buf[512] = {0};
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"empty body\"}");
        return ESP_OK;
    }
    buf[received] = '\0';

    int simple_auth = json_parse_int(buf, "auth_type", 0);

    wifi_credential_t cred = {0};
    cred.auth_mode = simple_to_authmode(simple_auth);
    json_parse_str(buf, "ssid", cred.ssid, sizeof(cred.ssid));
    json_parse_str(buf, "password", cred.password, sizeof(cred.password));
    json_parse_str(buf, "identity", cred.identity, sizeof(cred.identity));
    json_parse_str(buf, "eap_password", cred.eap_password, sizeof(cred.eap_password));

    ESP_LOGI(TAG, "Provision: ssid=%s auth=%d", cred.ssid, cred.auth_mode);

    esp_err_t ret = wifi_manager_provision(&cred);

    char resp[128];
    if (ret == ESP_OK) {
        char ip[16] = "pending";
        wifi_get_ip(ip, sizeof(ip));
        snprintf(resp, sizeof(resp), "{\"success\":true,\"ip\":\"%s\"}", ip);
    } else {
        snprintf(resp, sizeof(resp), "{\"success\":false,\"error\":\"err 0x%x\"}", ret);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

static esp_err_t handler_status(httpd_req_t *req)
{
    char resp[256];
    bool connected = wifi_is_connected();
    char ip[16] = "0.0.0.0";
    if (connected) wifi_get_ip(ip, sizeof(ip));

    snprintf(resp, sizeof(resp),
             "{\"connected\":%s,\"ssid\":\"%s\",\"ip\":\"%s\"}",
             connected ? "true" : "false",
             connected ? "(checking...)" : "",
             ip);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

esp_err_t http_server_start(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 8;

    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return ESP_FAIL;
    }

    httpd_uri_t handlers[] = {
        {.uri = "/",        .method = HTTP_GET,  .handler = handler_root},
        {.uri = "/scan",    .method = HTTP_GET,  .handler = handler_scan},
        {.uri = "/connect", .method = HTTP_POST, .handler = handler_connect},
        {.uri = "/status",  .method = HTTP_GET,  .handler = handler_status},
    };

    for (int i = 0; i < sizeof(handlers) / sizeof(handlers[0]); i++) {
        httpd_register_uri_handler(server, &handlers[i]);
    }

    ESP_LOGI(TAG, "Provisioning HTTP server started on port 80");
    return ESP_OK;
}
