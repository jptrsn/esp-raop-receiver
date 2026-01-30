#include "web_server.h"
#include "wifi_manager.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include <string.h>
#include <ctype.h>

static const char *TAG = "web_server";

// HTML for the configuration page
static const char *config_html =
    "<!DOCTYPE html>"
    "<html>"
    "<head>"
    "<title>ESP AirPlay Config</title>"
    "<meta name='viewport' content='width=device-width, initial-scale=1'>"
    "<style>"
    "body { font-family: Arial, sans-serif; max-width: 600px; margin: 50px auto; padding: 20px; }"
    "h1 { color: #333; }"
    "form { background: #f4f4f4; padding: 20px; border-radius: 5px; }"
    "label { display: block; margin: 10px 0 5px; }"
    "input { width: 100%; padding: 8px; margin-bottom: 15px; border: 1px solid #ddd; border-radius: 4px; box-sizing: border-box; }"
    "button { background: #007bff; color: white; padding: 10px 20px; border: none; border-radius: 4px; cursor: pointer; font-size: 16px; }"
    "button:hover { background: #0056b3; }"
    ".success { color: green; margin-top: 10px; }"
    ".error { color: red; margin-top: 10px; }"
    "</style>"
    "</head>"
    "<body>"
    "<h1>ESP AirPlay Configuration</h1>"
    "<form action='/save' method='post'>"
    "<label for='ssid'>WiFi SSID:</label>"
    "<input type='text' id='ssid' name='ssid' required>"
    "<label for='password'>WiFi Password:</label>"
    "<input type='password' id='password' name='password' required>"
    "<label for='device_name'>Device Name (optional):</label>"
    "<input type='text' id='device_name' name='device_name' placeholder='ESP-AirPlay'>"
    "<button type='submit'>Save & Restart</button>"
    "</form>"
    "</body>"
    "</html>";

static const char *success_html =
    "<!DOCTYPE html>"
    "<html>"
    "<head>"
    "<title>Success</title>"
    "<meta http-equiv='refresh' content='5;url=/'>"
    "<style>"
    "body { font-family: Arial, sans-serif; max-width: 600px; margin: 50px auto; padding: 20px; text-align: center; }"
    "h1 { color: #28a745; }"
    "</style>"
    "</head>"
    "<body>"
    "<h1>Configuration Saved!</h1>"
    "<p>Device will restart and connect to your WiFi network...</p>"
    "<p>This page will redirect in 5 seconds.</p>"
    "</body>"
    "</html>";

// Handler for root page
static esp_err_t root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, config_html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// URL decode helper function
static void url_decode(char *dst, const char *src)
{
    char a, b;
    while (*src) {
        if ((*src == '%') && ((a = src[1]) && (b = src[2])) && (isxdigit(a) && isxdigit(b))) {
            if (a >= 'a') a -= 'a'-'A';
            if (a >= 'A') a -= ('A' - 10);
            else a -= '0';
            if (b >= 'a') b -= 'a'-'A';
            if (b >= 'A') b -= ('A' - 10);
            else b -= '0';
            *dst++ = 16*a + b;
            src += 3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

// Parse form data
static bool parse_form_data(const char *buf, char *ssid, char *password, char *device_name)
{
    char *token, *saveptr;
    char *buf_copy = strdup(buf);
    if (!buf_copy) return false;

    token = strtok_r(buf_copy, "&", &saveptr);
    while (token != NULL) {
        char *key = strtok(token, "=");
        char *value = strtok(NULL, "=");

        if (key && value) {
            if (strcmp(key, "ssid") == 0) {
                url_decode(ssid, value);
            } else if (strcmp(key, "password") == 0) {
                url_decode(password, value);
            } else if (strcmp(key, "device_name") == 0) {
                url_decode(device_name, value);
            }
        }

        token = strtok_r(NULL, "&", &saveptr);
    }

    free(buf_copy);
    return (strlen(ssid) > 0 && strlen(password) > 0);
}

// Handler for save endpoint
static esp_err_t save_handler(httpd_req_t *req)
{
    char buf[512];
    char ssid[32] = {0};
    char password[64] = {0};
    char device_name[32] = {0};
    int ret, remaining = req->content_len;

    if (remaining > sizeof(buf) - 1) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Content too long");
        return ESP_FAIL;
    }

    ret = httpd_req_recv(req, buf, remaining);
    if (ret <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    ESP_LOGI(TAG, "Received form data: %s", buf);

    if (!parse_form_data(buf, ssid, password, device_name)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid form data");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Parsed - SSID: %s, Device: %s", ssid,
             strlen(device_name) > 0 ? device_name : "default");

    // Save credentials
    if (wifi_manager_save_credentials(ssid, password)) {
        // TODO: Save device name to NVS as well
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, success_html, HTTPD_RESP_USE_STRLEN);

        // Schedule restart after response is sent
        ESP_LOGI(TAG, "Configuration saved. Restarting in 2 seconds...");

        // Delay to allow response to be sent, then restart
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();

        return ESP_OK;
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save");
        return ESP_FAIL;
    }
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
    config.max_uri_handlers = 8;
    config.stack_size = 8192;

    httpd_handle_t server = NULL;

    ESP_LOGI(TAG, "Starting web server on port %d", config.server_port);

    if (httpd_start(&server, &config) == ESP_OK) {
        // Register root and save handlers first
        httpd_uri_t root_uri = {
            .uri       = "/",
            .method    = HTTP_GET,
            .handler   = root_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &root_uri);

        httpd_uri_t save_uri = {
            .uri       = "/save",
            .method    = HTTP_POST,
            .handler   = save_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &save_uri);

        // Register captive portal routes
        httpd_uri_t generate_204 = {
            .uri = "/generate_204",
            .method = HTTP_GET,
            .handler = captive_portal_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &generate_204);

        httpd_uri_t gen_204 = {
            .uri = "/gen_204",
            .method = HTTP_GET,
            .handler = captive_portal_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &gen_204);

        httpd_uri_t hotspot_detect = {
            .uri = "/hotspot-detect.html",
            .method = HTTP_GET,
            .handler = captive_portal_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &hotspot_detect);

        httpd_uri_t connecttest = {
            .uri = "/connecttest.txt",
            .method = HTTP_GET,
            .handler = captive_portal_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &connecttest);

        ESP_LOGI(TAG, "Web server started successfully");
        return server;
    }

    ESP_LOGE(TAG, "Failed to start web server");
    return NULL;
}

void web_server_stop(httpd_handle_t server)
{
    if (server) {
        httpd_stop(server);
        ESP_LOGI(TAG, "Web server stopped");
    }
}