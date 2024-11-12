#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "esp_err.h"
#include "esp_netif.h"
#include "esp_wifi_types.h"
#include "esp_mac.h"
#include "esp_event.h"
#include <string.h>

typedef enum {
    WIFI_STATE_OFF,
    WIFI_STATE_ON,
    WIFI_STATE_CLIENT_CONNECTED
} wifi_state_t;

/**
 * @brief Inizializza il WiFi manager
 * @return ESP_OK in caso di successo
 */
esp_err_t wifi_manager_init(void);

/**
 * @brief Avvia l'access point WiFi
 * @return ESP_OK in caso di successo
 */
esp_err_t wifi_manager_start(void);

/**
 * @brief Ferma l'access point WiFi
 * @return ESP_OK in caso di successo
 */
esp_err_t wifi_manager_stop(void);

/**
 * @brief Ottiene lo stato corrente del WiFi
 * @return Stato corrente del WiFi
 */
wifi_state_t wifi_manager_get_state(void);

/**
 * @brief Ottiene il netif corrente
 * @return Puntatore al netif corrente
 */
esp_netif_t* wifi_manager_get_netif(void);

#endif // WIFI_MANAGER_H