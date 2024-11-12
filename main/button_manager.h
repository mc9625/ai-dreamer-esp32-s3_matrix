#ifndef BUTTON_MANAGER_H
#define BUTTON_MANAGER_H

#include "esp_err.h"
#include "driver/gpio.h"

#define BOOT_BUTTON_PIN GPIO_NUM_0

typedef void (*button_callback_t)(void);

/**
 * @brief Inizializza il gestore dei pulsanti
 * @param button_pressed_cb Callback da chiamare quando il pulsante viene premuto
 * @return ESP_OK in caso di successo
 */
esp_err_t button_manager_init(button_callback_t button_pressed_cb);

/**
 * @brief Ferma il monitoraggio dei pulsanti
 * @return ESP_OK in caso di successo
 */
esp_err_t button_manager_stop(void);

#endif // BUTTON_MANAGER_H