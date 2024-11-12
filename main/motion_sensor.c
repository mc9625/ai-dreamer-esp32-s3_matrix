#include "motion_sensor.h"
#include "esp_log.h"
#include "driver/i2c.h"
#include "esp_timer.h"
#include <string.h>
#include <math.h>

static const char *TAG = "MOTION_SENSOR";
static TaskHandle_t monitor_task_handle = NULL;
static motion_callback_t motion_cb = NULL;

// Riduco le soglie per rendere più sensibile il rilevamento
#define SHAKE_THRESHOLD           1.2f    // Ridotto da 2.0f
#define MIN_SHAKE_THRESHOLD      0.8f    // Soglia minima per considerare un movimento
#define SHAKE_COUNT              8       // Ridotto da 10
#define SHAKE_WINDOW_MS          1500    // Aumentato da 1000
#define DEBOUNCE_TIME_MS         100     // Tempo minimo tra due shake

// Registri QMI8658
#define QMI8658_RESET_REG        0x60
#define QMI8658_CTRL1_REG        0x02  // Output data rate
#define QMI8658_CTRL2_REG        0x03  // Accelerometer config
#define QMI8658_CTRL3_REG        0x04  // Gyroscope config
#define QMI8658_CTRL7_REG        0x08  // Enable sensors
#define QMI8658_ACCEL_X_L        0x35
#define QMI8658_ACCEL_X_H        0x36
#define QMI8658_ACCEL_Y_L        0x37
#define QMI8658_ACCEL_Y_H        0x38
#define QMI8658_ACCEL_Z_L        0x39
#define QMI8658_ACCEL_Z_H        0x3A

static esp_err_t i2c_write_reg(uint8_t reg, uint8_t data) {
    uint8_t write_buf[2] = {reg, data};
    return i2c_master_write_to_device(I2C_MASTER_NUM, QMI8658_ADDR, write_buf, 
                                    sizeof(write_buf), pdMS_TO_TICKS(I2C_MASTER_TIMEOUT_MS));
}

static esp_err_t i2c_read_reg(uint8_t reg, uint8_t* data) {
    return i2c_master_write_read_device(I2C_MASTER_NUM, QMI8658_ADDR, 
                                      &reg, 1, data, 1, 
                                      pdMS_TO_TICKS(I2C_MASTER_TIMEOUT_MS));
}

static esp_err_t init_i2c(void) {
    ESP_LOGI(TAG, "Configuring I2C...");
    
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
        .clk_flags = 0
    };

    // Reset I2C pins
    gpio_reset_pin(I2C_MASTER_SDA_IO);
    gpio_reset_pin(I2C_MASTER_SCL_IO);
    
    esp_err_t err = i2c_param_config(I2C_MASTER_NUM, &conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure I2C parameters");
        return err;
    }

    err = i2c_driver_install(I2C_MASTER_NUM, conf.mode, 0, 0, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install I2C driver");
    }
    
    return err;
}


static esp_err_t init_qmi8658(void) {
    esp_err_t ret;
    uint8_t data;

    // Test connessione con WHO_AM_I
    ret = i2c_read_reg(QMI8658_WHO_AM_I_REG, &data);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read WHO_AM_I");
        return ret;
    }
    ESP_LOGI(TAG, "QMI8658 WHO_AM_I: 0x%02x", data);
    if (data != 0x05) {
        ESP_LOGE(TAG, "Unexpected WHO_AM_I value");
        return ESP_FAIL;
    }

    // Reset del sensore
    ret = i2c_write_reg(QMI8658_RESET_REG, 0xB0);
    if (ret != ESP_OK) return ret;
    vTaskDelay(pdMS_TO_TICKS(50));  // Attesa dopo il reset

    // Configurazione accelerometro:
    // CTRL1: Output data rate 100Hz (0x06)
    ret = i2c_write_reg(QMI8658_CTRL1_REG, 0x06);
    if (ret != ESP_OK) return ret;

    // CTRL2: ±8g range (0x02)
    ret = i2c_write_reg(QMI8658_CTRL2_REG, 0x02);
    if (ret != ESP_OK) return ret;

    // CTRL7: Enable accelerometer (0x40)
    ret = i2c_write_reg(QMI8658_CTRL7_REG, 0x40);
    if (ret != ESP_OK) return ret;

    // Verifica che la configurazione sia stata applicata
    ret = i2c_read_reg(QMI8658_CTRL7_REG, &data);
    if (ret != ESP_OK || !(data & 0x40)) {
        ESP_LOGE(TAG, "Failed to enable accelerometer");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "QMI8658 configured successfully");
    return ESP_OK;
}

esp_err_t motion_sensor_read(motion_sensor_data_t *data) {
    if (!data) return ESP_ERR_INVALID_ARG;

    uint8_t raw_data[6];
    esp_err_t ret;
    
    // Leggi i dati asse per asse
    ret = i2c_read_reg(QMI8658_ACCEL_X_L, &raw_data[0]);
    if (ret != ESP_OK) return ret;
    ret = i2c_read_reg(QMI8658_ACCEL_X_H, &raw_data[1]);
    if (ret != ESP_OK) return ret;
    
    ret = i2c_read_reg(QMI8658_ACCEL_Y_L, &raw_data[2]);
    if (ret != ESP_OK) return ret;
    ret = i2c_read_reg(QMI8658_ACCEL_Y_H, &raw_data[3]);
    if (ret != ESP_OK) return ret;
    
    ret = i2c_read_reg(QMI8658_ACCEL_Z_L, &raw_data[4]);
    if (ret != ESP_OK) return ret;
    ret = i2c_read_reg(QMI8658_ACCEL_Z_H, &raw_data[5]);
    if (ret != ESP_OK) return ret;

    // Converti i dati raw (±8g range)
    int16_t x = (raw_data[1] << 8) | raw_data[0];
    int16_t y = (raw_data[3] << 8) | raw_data[2];
    int16_t z = (raw_data[5] << 8) | raw_data[4];

    data->x = x * 8.0f / 32768.0f;  // Converti a g
    data->y = y * 8.0f / 32768.0f;
    data->z = z * 8.0f / 32768.0f;

    // Debug log dei valori raw
    static int count = 0;
    if (++count % 50 == 0) {  // Log ogni 50 letture
        ESP_LOGI(TAG, "Raw values - X: %d, Y: %d, Z: %d", x, y, z);
    }

    return ESP_OK;
}

static void motion_monitor_task(void *pvParameters) {
    int shake_count = 0;
    int64_t shake_start_time = 0;
    int64_t last_shake_time = 0;
    motion_sensor_data_t last_data = {0};
    bool first_reading = true;
    int64_t last_print_time = 0;
    
    // Attendiamo che il sensore si stabilizzi
    vTaskDelay(pdMS_TO_TICKS(500));  // Aumentato a 500ms
    
    ESP_LOGI(TAG, "Motion monitoring started");
    
    while(1) {
        motion_sensor_data_t current_data;
        if (motion_sensor_read(&current_data) == ESP_OK) {
            // Stampa i valori ogni secondo
            int64_t current_time = esp_timer_get_time() / 1000;
            if (current_time - last_print_time > 1000) {
                ESP_LOGI(TAG, "ACC: X=%.2f Y=%.2f Z=%.2f", 
                        current_data.x, current_data.y, current_data.z);
                last_print_time = current_time;
            }

            if (first_reading) {
                first_reading = false;
                memcpy(&last_data, &current_data, sizeof(motion_sensor_data_t));
                continue;
            }

            float delta_x = fabsf(current_data.x - last_data.x);
            float delta_y = fabsf(current_data.y - last_data.y);
            float delta_z = fabsf(current_data.z - last_data.z);
            
            float total_delta = sqrtf(delta_x * delta_x + delta_y * delta_y + delta_z * delta_z);

            // Se c'è un movimento significativo, logghiamo sempre
            if (total_delta > MIN_SHAKE_THRESHOLD) {
                ESP_LOGI(TAG, "Movement detected! Delta=%.2f (x:%.2f y:%.2f z:%.2f)", 
                        total_delta, delta_x, delta_y, delta_z);
            }

            if (total_delta > SHAKE_THRESHOLD) {
                if (current_time - last_shake_time > DEBOUNCE_TIME_MS) {
                    if (shake_count == 0) {
                        shake_start_time = current_time;
                        ESP_LOGI(TAG, "Starting shake detection, delta=%.2f", total_delta);
                    }
                    shake_count++;
                    last_shake_time = current_time;
                    
                    ESP_LOGI(TAG, "Shake detected! Count: %d, Delta: %.2f", shake_count, total_delta);
                }
            }
            
            // Verifica se abbiamo raggiunto il numero necessario di shake
            if (shake_count >= SHAKE_COUNT && 
                (current_time - shake_start_time) <= SHAKE_WINDOW_MS) {
                ESP_LOGI(TAG, "*** SHAKE SEQUENCE COMPLETED! Count: %d, Time: %lld ms ***", 
                         shake_count, (current_time - shake_start_time));
                if (motion_cb) {
                    motion_cb();
                }
                shake_count = 0;
                shake_start_time = 0;
                vTaskDelay(pdMS_TO_TICKS(500));
            } else if ((current_time - shake_start_time) > SHAKE_WINDOW_MS && shake_count > 0) {
                ESP_LOGW(TAG, "Shake sequence timeout. Count: %d, Time: %lld ms", 
                         shake_count, (current_time - shake_start_time));
                shake_count = 0;
                shake_start_time = 0;
            }
            
            memcpy(&last_data, &current_data, sizeof(motion_sensor_data_t));
        }
        
        vTaskDelay(pdMS_TO_TICKS(20));  // Ridotto il polling a 50Hz
    }
}

esp_err_t motion_sensor_init(motion_callback_t motion_detected_cb) {
    if (monitor_task_handle != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Initializing motion sensor...");
    
    esp_err_t err = init_i2c();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize I2C");
        return err;
    }

    err = init_qmi8658();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize QMI8658");
        return err;
    }

    motion_cb = motion_detected_cb;
    
    BaseType_t ret = xTaskCreate(motion_monitor_task, "motion_monitor", 4096,
                                NULL, tskIDLE_PRIORITY + 3, &monitor_task_handle);
    
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create motion monitor task");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Motion sensor initialized successfully");
    return ESP_OK;
}


esp_err_t motion_sensor_stop(void) {
    if (monitor_task_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    vTaskDelete(monitor_task_handle);
    monitor_task_handle = NULL;
    return i2c_driver_delete(I2C_MASTER_NUM);
}