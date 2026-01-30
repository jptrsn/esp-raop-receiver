#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include "esp_http_server.h"

// Start the web server (for WiFi configuration)
httpd_handle_t web_server_start(void);

// Stop the web server
void web_server_stop(httpd_handle_t server);

#endif // WEB_SERVER_H