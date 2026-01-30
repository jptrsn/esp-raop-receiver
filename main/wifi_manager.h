#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdbool.h>

// Initialize WiFi (either connects to saved network or starts AP)
void wifi_manager_init(void);

// Save WiFi credentials to NVS
bool wifi_manager_save_credentials(const char *ssid, const char *password);

// Get current WiFi status
bool wifi_manager_is_connected(void);

// Get current IP address (returns empty string if not connected)
const char* wifi_manager_get_ip(void);

bool wifi_manager_is_ap_mode(void);

#endif // WIFI_MANAGER_H