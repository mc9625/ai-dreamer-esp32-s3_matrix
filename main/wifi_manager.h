#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "esp_err.h"
#include "esp_netif.h"
#include "esp_wifi_types.h"
#include "esp_mac.h"
#include "esp_event.h"
#include <string.h>

/**
 * @brief Inizializza il WiFi in modalità AP con captive portal
 * @param wifi_netif Puntatore al netif che verrà inizializzato
 * @return ESP_OK in caso di successo
 */
esp_err_t wifi_manager_init(esp_netif_t **wifi_netif);

#endif