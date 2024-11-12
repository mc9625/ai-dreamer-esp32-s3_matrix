#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "driver/gpio.h"
#include "driver/rmt.h"
#include "driver/i2c.h"

#include "llm.h"
#include "ws_matrix.h"
#include "wifi_manager.h"
#include "captive_portal.h"

#include <math.h>
#include "esp_timer.h"

// I2C e MPU6050 definizioni
#define I2C_MASTER_SCL_IO           8      // SCL pin 
#define I2C_MASTER_SDA_IO           18     // SDA pin
#define I2C_MASTER_NUM              0      // I2C master number
#define I2C_MASTER_FREQ_HZ          400000 // I2C master clock frequency
#define I2C_MASTER_TIMEOUT_MS       1000   // I2C master timeout
#define MPU6050_ADDR               0x68    // MPU6050 device address
#define SHAKE_THRESHOLD            16000   // Soglia per rilevare scuotimento forte
#define SHAKE_DURATION_MS          2000    // Durata minima dello scuotimento
#define SHAKE_SAMPLES_THRESHOLD    20      // Numero minimo di campioni sopra soglia

// Altri define esistenti
#define MAIN_TASK_STACK_SIZE 16384
#define LLM_TASK_PRIORITY    5
#define BOOT_BUTTON_PIN GPIO_NUM_0
#define LLM_COMPLETE_BIT BIT0

static esp_netif_t *wifi_netif = NULL;
static const char *TAG = "MAIN";
static bool wifi_requested = false;
static bool movement_detected = false;
static EventGroupHandle_t system_events;

typedef struct {
    int16_t accel_x;
    int16_t accel_y;
    int16_t accel_z;
} mpu6050_accel_t;

// I2C Master Initialization
static esp_err_t init_i2c_master(void) {
    ESP_LOGI(TAG, "Initializing I2C master...");
    
    // Reset pins
    gpio_reset_pin(I2C_MASTER_SCL_IO);
    gpio_reset_pin(I2C_MASTER_SDA_IO);
    
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
        .clk_flags = 0
    };
    
    esp_err_t err = i2c_param_config(I2C_MASTER_NUM, &conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C parameter configuration failed: %s", esp_err_to_name(err));
        return err;
    }

    err = i2c_driver_install(I2C_MASTER_NUM, conf.mode, 0, 0, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C driver installation failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "I2C master initialized successfully");
    return ESP_OK;
}

static esp_err_t init_mpu6050(void) {
    ESP_LOGI(TAG, "Initializing MPU6050...");
    
    // Test the connection first
    uint8_t test_reg = 0x75; // WHO_AM_I register
    uint8_t test_data = 0;
    
    esp_err_t err = i2c_master_write_read_device(I2C_MASTER_NUM, MPU6050_ADDR,
                                                &test_reg, 1, &test_data, 1,
                                                pdMS_TO_TICKS(I2C_MASTER_TIMEOUT_MS));
    
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to communicate with MPU6050: %s", esp_err_to_name(err));
        return err;
    }
    
    if (test_data != 0x68) {
        ESP_LOGE(TAG, "Unexpected WHO_AM_I value: 0x%02X (expected 0x68)", test_data);
        return ESP_FAIL;
    }
    
    // Wake up MPU6050
    uint8_t wake_cmd[] = {0x6B, 0x00};
    err = i2c_master_write_to_device(I2C_MASTER_NUM, MPU6050_ADDR,
                                    wake_cmd, sizeof(wake_cmd),
                                    pdMS_TO_TICKS(I2C_MASTER_TIMEOUT_MS));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to wake up MPU6050: %s", esp_err_to_name(err));
        return err;
    }
    
    // Configure accelerometer for ±16g range
    uint8_t accel_config[] = {0x1C, 0x18};
    err = i2c_master_write_to_device(I2C_MASTER_NUM, MPU6050_ADDR,
                                    accel_config, sizeof(accel_config),
                                    pdMS_TO_TICKS(I2C_MASTER_TIMEOUT_MS));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure MPU6050 accelerometer: %s", esp_err_to_name(err));
        return err;
    }
    
    ESP_LOGI(TAG, "MPU6050 initialized successfully");
    return ESP_OK;
}

static esp_err_t read_mpu6050_accel(mpu6050_accel_t *accel_data) {
    if (accel_data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t data[6];
    uint8_t reg = 0x3B;  // ACCEL_XOUT_H register
    
    esp_err_t ret = i2c_master_write_read_device(I2C_MASTER_NUM, MPU6050_ADDR,
                                                &reg, 1, data, sizeof(data),
                                                pdMS_TO_TICKS(I2C_MASTER_TIMEOUT_MS));
    if (ret != ESP_OK) {
        return ret;
    }

    accel_data->accel_x = (data[0] << 8) | data[1];
    accel_data->accel_y = (data[2] << 8) | data[3];
    accel_data->accel_z = (data[4] << 8) | data[5];
    
    return ESP_OK;
}

static float calculate_magnitude(int16_t x, int16_t y, int16_t z) {
    float x_sq = (float)x * (float)x;
    float y_sq = (float)y * (float)y;
    float z_sq = (float)z * (float)z;
    return sqrtf(x_sq + y_sq + z_sq);
}

// Motion monitor task
static void motion_monitor_task(void *pvParameters) {
    ESP_LOGI(TAG, "Motion monitor task started");
    
    int shake_count = 0;
    int64_t shake_start_time = 0;
    
    while(1) {
        mpu6050_accel_t accel_data;
        if (read_mpu6050_accel(&accel_data) == ESP_OK) {
            float magnitude = calculate_magnitude(
                accel_data.accel_x,
                accel_data.accel_y,
                accel_data.accel_z
            );
            
            if (magnitude > SHAKE_THRESHOLD) {
                if (shake_count == 0) {
                    shake_start_time = esp_timer_get_time() / 1000;
                }
                shake_count++;
                
                int64_t current_time = esp_timer_get_time() / 1000;
                if (shake_count >= SHAKE_SAMPLES_THRESHOLD && 
                    (current_time - shake_start_time) <= SHAKE_DURATION_MS) {
                    
                    if (!wifi_requested && !movement_detected) {
                        ESP_LOGI(TAG, "Shake detected! Starting WiFi");
                        wifi_requested = true;
                        movement_detected = true;
                        
                        esp_err_t err = wifi_manager_start();
                        if (err != ESP_OK) {
                            ESP_LOGE(TAG, "Failed to start WiFi manager: %s", esp_err_to_name(err));
                            wifi_requested = false;
                            movement_detected = false;
                            shake_count = 0;
                            continue;
                        }

                        err = captive_portal_init(wifi_netif);
                        if (err != ESP_OK) {
                            ESP_LOGE(TAG, "Failed to init captive portal: %s", esp_err_to_name(err));
                            wifi_manager_stop();
                            wifi_requested = false;
                            movement_detected = false;
                            shake_count = 0;
                            continue;
                        }

                        ESP_LOGI(TAG, "WiFi and captive portal started successfully");
                    }
                    shake_count = 0;
                }
            } else {
                int64_t current_time = esp_timer_get_time() / 1000;
                if ((current_time - shake_start_time) > SHAKE_DURATION_MS) {
                    shake_count = 0;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}


// Task synchronization
static EventGroupHandle_t system_events;
#define LLM_COMPLETE_BIT BIT0


// Funzione per inizializzare il pulsante BOOT
static void init_boot_button(void) {
    ESP_LOGI(TAG, "Initializing R3 button on GPIO3");
    
    // Configurazione più dettagliata
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BOOT_BUTTON_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    
    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure R3 button: %s", esp_err_to_name(ret));
        return;
    }
    
    // Test iniziale del pulsante
    int level = gpio_get_level(BOOT_BUTTON_PIN);
    ESP_LOGI(TAG, "R3 button initialized, initial state: %d", level);
}

static void button_monitor_task(void *pvParameters) {
    ESP_LOGI(TAG, "Button monitor task started");
    
    if (wifi_netif == NULL) {
        ESP_LOGE(TAG, "WiFi netif not initialized!");
        vTaskDelete(NULL);
        return;
    }

    int counter = 0;
    while(1) {
        int level = gpio_get_level(BOOT_BUTTON_PIN);
        
        if (counter++ % 50 == 0) {
            ESP_LOGD(TAG, "R3 button current state: %d", level);  // Cambiato a LOGD
        }

        if (level == 0) {
            ESP_LOGI(TAG, "Button press detected");
            vTaskDelay(pdMS_TO_TICKS(50));
            
            level = gpio_get_level(BOOT_BUTTON_PIN);
            if (level == 0) {
                ESP_LOGI(TAG, "Button press confirmed after debounce");
                
                if (!wifi_requested) {
                    ESP_LOGI(TAG, "Starting WiFi and captive portal");
                    wifi_requested = true;
                    movement_detected = false;  // Reset questo flag
                    
                    esp_err_t err = wifi_manager_start();
                    if (err != ESP_OK) {
                        ESP_LOGE(TAG, "Failed to start WiFi manager: %s", esp_err_to_name(err));
                        wifi_requested = false;
                        continue;
                    }

                    err = captive_portal_init(wifi_netif);
                    if (err != ESP_OK) {
                        ESP_LOGE(TAG, "Failed to init captive portal: %s", esp_err_to_name(err));
                        wifi_manager_stop();
                        wifi_requested = false;
                        continue;
                    }

                    ESP_LOGI(TAG, "WiFi and captive portal started successfully");
                }
                
                while(gpio_get_level(BOOT_BUTTON_PIN) == 0) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
                ESP_LOGI(TAG, "Button released");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}



// LLM parameters structure
typedef struct {
    Transformer* transformer;
    Tokenizer* tokenizer;
    Sampler* sampler;
    int steps;
    generated_complete_cb callback;
} LLMParams;

// Forward declarations
static void generate_complete_cb(float tk_s);
static esp_err_t init_storage(void);

// Storage initialization
static esp_err_t init_storage(void) {
    ESP_LOGI(TAG, "Initializing SPIFFS");
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/data",
        .partition_label = NULL,
        .max_files = 5,
        .format_if_mount_failed = false
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        return ret;
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info(NULL, &total, &used);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
    }
    return ret;
}

// LLM task
static void llm_task(void *pvParameters) {
    LLMParams* params = (LLMParams*)pvParameters;
    bool initial_dream_generated = false;
    int disconnect_counter = 0;  // Sposta qui la dichiarazione
    
    while(1) {
        if (!initial_dream_generated) {
            generate(params->transformer, params->tokenizer, params->sampler,
                    NULL, params->steps, params->callback);
            initial_dream_generated = true;
            ESP_LOGI(TAG, "Initial dream generated, waiting for button/trigger");
            continue;
        }

        // Se il WiFi è attivo e c'è un client connesso, resettiamo il contatore
        if (wifi_manager_get_state() == WIFI_STATE_CLIENT_CONNECTED) {
            disconnect_counter = 0;
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        
        // Se il WiFi è attivo ma non ci sono client
        if (wifi_manager_get_state() == WIFI_STATE_ON) {
            disconnect_counter++;
            // Aumentiamo il tempo di attesa a 30 secondi (300 * 100ms)
            if (disconnect_counter >= 300) {
                disconnect_counter = 0;
                ESP_LOGI(TAG, "No clients connected for 30 seconds, stopping WiFi");
                wifi_manager_stop();
                wifi_requested = false;
                movement_detected = false;  // Reset anche questo flag
                
                generate(params->transformer, params->tokenizer, params->sampler,
                        NULL, params->steps, params->callback);
                ESP_LOGI(TAG, "New dream generated after WiFi disconnection");
            }
        } else {
            disconnect_counter = 0;  // Reset counter when WiFi is off
        }
        
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}



// Callback for LLM generation completion
static void generate_complete_cb(float tk_s) {
    ESP_LOGI(TAG, "Generation complete: %.2f tok/s", tk_s);
}

void app_main(void) {
    // Create event group for synchronization
    system_events = xEventGroupCreate();
    
    // Initialize boot button
    init_boot_button();

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize matrix and storage
    ESP_ERROR_CHECK(matrix_init());
    matrix_clear();
    matrix_set_brightness(40);
    ESP_ERROR_CHECK(init_storage());

    // Initialize networking stack and WiFi
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(wifi_manager_init(&wifi_netif));

    // Initialize I2C and MPU6050
    esp_err_t i2c_ret = init_i2c_master();
    if (i2c_ret == ESP_OK) {
        esp_err_t mpu_ret = init_mpu6050();
        if (mpu_ret == ESP_OK) {
            xTaskCreate(motion_monitor_task, "motion_monitor", 4096, NULL, 
                       tskIDLE_PRIORITY + 3, NULL);
        } else {
            ESP_LOGE(TAG, "MPU6050 initialization failed: %s", esp_err_to_name(mpu_ret));
        }
    } else {
        ESP_LOGE(TAG, "I2C initialization failed: %s", esp_err_to_name(i2c_ret));
    }

    // Prepare LLM parameters
    Transformer* transformer = malloc(sizeof(Transformer));
    Tokenizer* tokenizer = malloc(sizeof(Tokenizer));
    Sampler* sampler = malloc(sizeof(Sampler));
    
    char *checkpoint_path = "/data/aidreams260K.bin";
    char *tokenizer_path = "/data/tok512.bin";
    float temperature = 0.7f;
    float topp = 0.8f;
    int steps = 1024;

    ESP_LOGI(TAG, "Loading model from %s", checkpoint_path);
    build_transformer(transformer, checkpoint_path);

    if (steps == 0 || steps > transformer->config.seq_len) {
        steps = transformer->config.seq_len;
    }

    build_tokenizer(tokenizer, tokenizer_path, transformer->config.vocab_size);
    build_sampler(sampler, transformer->config.vocab_size, temperature, topp, esp_random());

    // Create LLM parameters
    LLMParams* llm_params = malloc(sizeof(LLMParams));
    llm_params->transformer = transformer;
    llm_params->tokenizer = tokenizer;
    llm_params->sampler = sampler;
    llm_params->steps = steps;
    llm_params->callback = generate_complete_cb;

    // Create button monitor task
    BaseType_t task_created = xTaskCreate(
        button_monitor_task, 
        "button_monitor", 
        4096,
        NULL, 
        tskIDLE_PRIORITY + 3,
        NULL
    );

    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create button monitor task!");
    } else {
        ESP_LOGI(TAG, "Button monitor task created successfully");
    }

    // Create LLM task
    xTaskCreate(llm_task, "llm_task", MAIN_TASK_STACK_SIZE,
                llm_params, LLM_TASK_PRIORITY, NULL);

    ESP_LOGI(TAG, "Initialization complete - Press R3 button or shake device to enable WiFi");
}