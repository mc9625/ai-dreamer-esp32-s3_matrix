#ifndef MOTION_SENSOR_H
#define MOTION_SENSOR_H

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_timer.h"

// QMI8658 specifiche
#define QMI8658_ADDR               0x6B
#define QMI8658_WHO_AM_I_REG      0x00
#define QMI8658_CTRL7_REG         0x15
#define QMI8658_CTRL8_REG         0x16
#define QMI8658_CTRL9_REG         0x17
#define QMI8658_ACCEL_DATA_REG    0x35

// Configurazione I2C
#define I2C_MASTER_SCL_IO         12
#define I2C_MASTER_SDA_IO         11
#define I2C_MASTER_NUM            0
#define I2C_MASTER_FREQ_HZ        400000
#define I2C_MASTER_TIMEOUT_MS     100

// Nel motion_sensor.h
#define SHAKE_THRESHOLD           0.8f    // Ridotto ulteriormente
#define MIN_SHAKE_THRESHOLD      0.4f    // Ridotto ulteriormente
#define SHAKE_COUNT              6       // Ridotto il numero di shake necessari
#define SHAKE_WINDOW_MS          2000    // Aumentato la finestra temporale
#define DEBOUNCE_TIME_MS         100     // Mantenuto il debounce

typedef void (*motion_callback_t)(void);

typedef struct {
    float x;
    float y;
    float z;
} motion_sensor_data_t;

/**
 * @brief Inizializza il sensore di movimento
 * @param motion_detected_cb Callback da chiamare quando viene rilevato movimento
 * @return ESP_OK in caso di successo
 */
esp_err_t motion_sensor_init(motion_callback_t motion_detected_cb);

/**
 * @brief Ferma il monitoraggio del movimento
 * @return ESP_OK in caso di successo
 */
esp_err_t motion_sensor_stop(void);

/**
 * @brief Legge i dati correnti del sensore
 * @param data Puntatore alla struttura dove salvare i dati
 * @return ESP_OK in caso di successo
 */
esp_err_t motion_sensor_read(motion_sensor_data_t *data);

#endif // MOTION_SENSOR_H