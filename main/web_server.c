#include "web_server.h"
#include "wifi_manager.h"
#include "config_manager.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include <string.h>
#include <ctype.h>
#include <stdio.h>

static const char *TAG = "web_server";

extern const char web_config_html_start[] asm("_binary_web_config_html_start");
extern const char web_config_html_end[]   asm("_binary_web_config_html_end");

static void str_replace(const char *src, const char *placeholder,
                        const char *value, char *dst, size_t dst_len)
{
    const char *pos = strstr(src, placeholder);
    if (!pos) {
        snprintf(dst, dst_len, "%s", src);
        return;
    }
    size_t prefix_len = pos - src;
    snprintf(dst, dst_len, "%.*s%s%s",
             (int)prefix_len, src,
             value,
             pos + strlen(placeholder));
}

// Handler for root page
static esp_err_t root_handler(httpd_req_t *req)
{
    app_config_t config = {0};
    config_manager_load(&config);

    const char *saved_ssid = wifi_manager_get_saved_ssid();

    // Work through each placeholder one at a time, alternating buffers
    size_t html_len = web_config_html_end - web_config_html_start;
    size_t buf_len = html_len + 256;

    char *buf_a = malloc(buf_len);
    char *buf_b = malloc(buf_len);
    if (!buf_a || !buf_b) {
        free(buf_a);
        free(buf_b);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    str_replace(web_config_html_start, "{{CURRENT_SSID}}",        saved_ssid,          buf_a, buf_len);
    str_replace(buf_a,                 "{{CURRENT_DEVICE_NAME}}", config.device_name,  buf_b, buf_len);
    str_replace(buf_b,                 "{{DEFAULT_DEVICE_NAME}}", config.device_name,  buf_a, buf_len);

    char pin_str[8];
    snprintf(pin_str, sizeof(pin_str), "%d", config.bck_pin);
    str_replace(buf_a, "{{BCK_PIN}}", pin_str, buf_b, buf_len);

    snprintf(pin_str, sizeof(pin_str), "%d", config.ws_pin);
    str_replace(buf_b, "{{WS_PIN}}", pin_str, buf_a, buf_len);

    snprintf(pin_str, sizeof(pin_str), "%d", config.dout_pin);
    str_replace(buf_a, "{{DOUT_PIN}}", pin_str, buf_b, buf_len);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, buf_b, HTTPD_RESP_USE_STRLEN);

    free(buf_a);
    free(buf_b);
    return ESP_OK;
}

static void url_decode(char *dst, const char *src, size_t dst_len)
{
    char a, b;
    size_t i = 0;
    while (*src && i < dst_len - 1) {
        if ((*src == '%') && ((a = src[1]) && (b = src[2])) && (isxdigit(a) && isxdigit(b))) {
            if (a >= 'a') a -= 'a' - 'A';
            if (a >= 'A') a -= ('A' - 10);
            else a -= '0';
            if (b >= 'a') b -= 'a' - 'A';
            if (b >= 'A') b -= ('A' - 10);
            else b -= '0';
            dst[i++] = 16 * a + b;
            src += 3;
        } else if (*src == '+') {
            dst[i++] = ' ';
            src++;
        } else {
            dst[i++] = *src++;
        }
    }
    dst[i] = '\0';
}

static bool get_form_field(const char *body, const char *key, char *dst, size_t dst_len)
{
    char search[64];
    snprintf(search, sizeof(search), "%s=", key);
    const char *pos = strstr(body, search);
    if (!pos) {
        dst[0] = '\0';
        return false;
    }
    pos += strlen(search);
    const char *end = strchrnul(pos, '&');
    size_t len = end - pos;
    char *encoded = strndup(pos, len);
    if (!encoded) {
        dst[0] = '\0';
        return false;
    }
    url_decode(dst, encoded, dst_len);
    free(encoded);
    return strlen(dst) > 0;
}

static esp_err_t read_request_body(httpd_req_t *req, char *buf, size_t buf_len)
{
    if (req->content_len >= buf_len) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Request too large");
        return ESP_FAIL;
    }
    int ret = httpd_req_recv(req, buf, req->content_len);
    if (ret <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) httpd_resp_send_408(req);
        return ESP_FAIL;
    }
    buf[ret] = '\0';
    return ESP_OK;
}

static esp_err_t save_wifi_handler(httpd_req_t *req)
{
    char body[256];
    if (read_request_body(req, body, sizeof(body)) != ESP_OK) return ESP_FAIL;

    char ssid[32] = {0};
    char password[64] = {0};

    get_form_field(body, "ssid", ssid, sizeof(ssid));
    get_form_field(body, "password", password, sizeof(password));

    if (strlen(ssid) == 0 || strlen(password) == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing fields");
        return ESP_FAIL;
    }

    if (!wifi_manager_save_credentials(ssid, password)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save");
        return ESP_FAIL;
    }

    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t save_device_handler(httpd_req_t *req)
{
    char body[128];
    if (read_request_body(req, body, sizeof(body)) != ESP_OK) return ESP_FAIL;

    char device_name[32] = {0};
    get_form_field(body, "device_name", device_name, sizeof(device_name));

    if (config_manager_save_device(device_name) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid device name");
        return ESP_FAIL;
    }

    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t save_audio_handler(httpd_req_t *req)
{
    char body[128];
    if (read_request_body(req, body, sizeof(body)) != ESP_OK) return ESP_FAIL;

    char val[8];
    int bck_pin, ws_pin, dout_pin;

    if (!get_form_field(body, "bck_pin", val, sizeof(val)))  { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing bck_pin");  return ESP_FAIL; }
    bck_pin = atoi(val);

    if (!get_form_field(body, "ws_pin", val, sizeof(val)))   { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing ws_pin");   return ESP_FAIL; }
    ws_pin = atoi(val);

    if (!get_form_field(body, "dout_pin", val, sizeof(val))) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing dout_pin"); return ESP_FAIL; }
    dout_pin = atoi(val);

    if (config_manager_save_audio(bck_pin, ws_pin, dout_pin) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save");
        return ESP_FAIL;
    }

    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t restart_handler(httpd_req_t *req)
{
    httpd_resp_send(req, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return ESP_OK;
}

static esp_err_t captive_portal_handler(httpd_req_t *req)
{
    // Redirect to our config page
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

httpd_handle_t web_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 12;
    config.stack_size = 8192;

    httpd_handle_t server = NULL;

    ESP_LOGI(TAG, "Starting web server on port %d", config.server_port);

    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start web server");
        return NULL;
    }

    httpd_uri_t root_uri = {
        .uri = "/", .method = HTTP_GET,
        .handler = root_handler, .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &root_uri);

    httpd_uri_t save_wifi_uri = {
        .uri = "/save/wifi", .method = HTTP_POST,
        .handler = save_wifi_handler, .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &save_wifi_uri);

    httpd_uri_t save_device_uri = {
        .uri = "/save/device", .method = HTTP_POST,
        .handler = save_device_handler, .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &save_device_uri);

    httpd_uri_t save_audio_uri = {
        .uri = "/save/audio", .method = HTTP_POST,
        .handler = save_audio_handler, .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &save_audio_uri);

    httpd_uri_t restart_uri = {
        .uri = "/restart", .method = HTTP_POST,
        .handler = restart_handler, .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &restart_uri);

    httpd_uri_t generate_204 = {
        .uri = "/generate_204", .method = HTTP_GET,
        .handler = captive_portal_handler, .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &generate_204);

    httpd_uri_t gen_204 = {
        .uri = "/gen_204", .method = HTTP_GET,
        .handler = captive_portal_handler, .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &gen_204);

    httpd_uri_t hotspot_detect = {
        .uri = "/hotspot-detect.html", .method = HTTP_GET,
        .handler = captive_portal_handler, .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &hotspot_detect);

    httpd_uri_t connecttest = {
        .uri = "/connecttest.txt", .method = HTTP_GET,
        .handler = captive_portal_handler, .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &connecttest);

    ESP_LOGI(TAG, "Web server started successfully");
    return server;
}

void web_server_stop(httpd_handle_t server)
{
    if (server) {
        httpd_stop(server);
        ESP_LOGI(TAG, "Web server stopped");
    }
}