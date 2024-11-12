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
#define I2C_MASTER_SCL_IO           12    // SCL pin (come Arduino)
#define I2C_MASTER_SDA_IO           11    // SDA pin (come Arduino)
#define I2C_MASTER_NUM              0     // Numero controller I2C
#define I2C_MASTER_FREQ_HZ          400000 // 400KHz
#define I2C_MASTER_TIMEOUT_MS       100   // Timeout per operazioni I2C
#define QMI8658_ADDR               0x6B   // Indirizzo I2C del QMI8658

// Soglie per il rilevamento movimento
#define ACCEL_THRESHOLD_HIGH       0.15f  // Soglia accelerazione positiva
#define ACCEL_Z_HIGH              -0.9f   // Soglia Z alta
#define ACCEL_Z_LOW               -1.1f   // Soglia Z bassa

#define MOVEMENT_THRESHOLD      1.5f   // Soglia per il movimento significativo
#define MOVEMENT_COUNT         100     // Numero di campioni sopra soglia necessari
#define MOVEMENT_WINDOW_MS     2000    // Finestra temporale per il movimento (2 secondi)

#define VERTICAL_THRESHOLD    0.8f    // Quando un asse è quasi verticale (circa 80% di g)
#define VERTICAL_TIME_MS     1000    // Tempo minimo in verticale (1 secondo)
#define FLIP_THRESHOLD       0.5f    // Soglia per rilevare il flip veloce
#define FLIP_WINDOW_MS       500    
#define SHAKE_THRESHOLD      2.0f    // Soglia di accelerazione per lo shake
#define SHAKE_COUNT         10       // Numero minimo di shake necessari
#define SHAKE_WINDOW_MS     1000     // Finestra temporale per gli shake (1 secondo)


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
    float x;
    float y;
    float z;
} qmi8658_data_t;

typedef enum {
    WAIT_VERTICAL,    // In attesa che il dispositivo sia verticale
    WAIT_FLIP,       // In attesa del flip dopo la posizione verticale
} device_state_t;


static uint8_t X_EN = 0, Y_EN = 0;
static uint8_t Time_X_A = 0, Time_X_B = 0, Time_Y_A = 0, Time_Y_B = 0;
// I2C Master Initialization

static esp_err_t read_qmi8658_accel(qmi8658_data_t *accel_data) {
    uint8_t data[6];
    uint8_t reg = 0x35;  // Registro dati accelerometro
    
    esp_err_t ret = i2c_master_write_read_device(I2C_MASTER_NUM, QMI8658_ADDR,
                                                &reg, 1, data, sizeof(data),
                                                pdMS_TO_TICKS(100));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read accelerometer data: %s", esp_err_to_name(ret));
        return ret;
    }

    // Conversione dati raw
    accel_data->x = (int16_t)((data[1] << 8) | data[0]) / 8192.0f;
    accel_data->y = (int16_t)((data[3] << 8) | data[2]) / 8192.0f;
    accel_data->z = (int16_t)((data[5] << 8) | data[4]) / 8192.0f;
    
    return ESP_OK;
}

static esp_err_t init_qmi8658(void) {
    ESP_LOGI(TAG, "Initializing QMI8658...");
    
    // Test connessione base
    uint8_t who_am_i_reg = 0x00;
    uint8_t who_am_i_value;
    
    esp_err_t ret = i2c_master_write_read_device(I2C_MASTER_NUM, QMI8658_ADDR,
                                                &who_am_i_reg, 1, &who_am_i_value, 1,
                                                pdMS_TO_TICKS(100));
                                                
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read WHO_AM_I register");
        return ret;
    }

    // Setup dispositivo
    uint8_t wake_cmd[] = {0x15, 0x00};
    ret = i2c_master_write_to_device(I2C_MASTER_NUM, QMI8658_ADDR,
                                    wake_cmd, sizeof(wake_cmd),
                                    pdMS_TO_TICKS(100));
    if (ret != ESP_OK) return ret;

    // Configura accelerometro
    uint8_t acc_config[] = {0x16, 0x62};
    ret = i2c_master_write_to_device(I2C_MASTER_NUM, QMI8658_ADDR,
                                    acc_config, sizeof(acc_config),
                                    pdMS_TO_TICKS(100));
    if (ret != ESP_OK) return ret;

    // Configura giroscopio
    uint8_t gyro_config[] = {0x17, 0x63};
    ret = i2c_master_write_to_device(I2C_MASTER_NUM, QMI8658_ADDR,
                                    gyro_config, sizeof(gyro_config),
                                    pdMS_TO_TICKS(100));
    if (ret != ESP_OK) return ret;

    ESP_LOGI(TAG, "QMI8658 initialization successful");
    return ESP_OK;
}


static esp_err_t init_i2c_master(void) {
    ESP_LOGI(TAG, "Initializing I2C master for QMI8658...");
    
    // Reset pins
    gpio_reset_pin(I2C_MASTER_SCL_IO);
    gpio_reset_pin(I2C_MASTER_SDA_IO);
    
    gpio_set_direction(I2C_MASTER_SCL_IO, GPIO_MODE_INPUT_OUTPUT_OD);
    gpio_set_direction(I2C_MASTER_SDA_IO, GPIO_MODE_INPUT_OUTPUT_OD);
    
    gpio_set_level(I2C_MASTER_SCL_IO, 1);
    gpio_set_level(I2C_MASTER_SDA_IO, 1);
    
    ESP_LOGI(TAG, "Configuring I2C with SCL=%d, SDA=%d", I2C_MASTER_SCL_IO, I2C_MASTER_SDA_IO);
    
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 400000,
        .clk_flags = 0
    };
    
    esp_err_t err = i2c_param_config(I2C_MASTER_NUM, &conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C parameter configuration failed: %s", esp_err_to_name(err));
        return err;
    }

    // Prima rimuovi eventuali vecchie installazioni
    i2c_driver_delete(I2C_MASTER_NUM);
    vTaskDelay(pdMS_TO_TICKS(100));  // Piccolo delay

    err = i2c_driver_install(I2C_MASTER_NUM, conf.mode, 0, 0, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C driver installation failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "I2C master initialized successfully");
    return ESP_OK;
}



static esp_err_t safe_start_wifi_and_portal(void) {
    // Reset dei flag per sicurezza
    wifi_requested = false;
    movement_detected = false;

    // Ferma tutto prima di ricominciare
    wifi_manager_stop();
    vTaskDelay(pdMS_TO_TICKS(500));  // Attendi che tutto sia fermato
    
    ESP_LOGI(TAG, "Starting WiFi manager...");
    esp_err_t err = wifi_manager_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WiFi manager: %s", esp_err_to_name(err));
        return err;
    }

    // Attendi che il WiFi sia pronto
    vTaskDelay(pdMS_TO_TICKS(500));

    ESP_LOGI(TAG, "Initializing captive portal...");
    err = captive_portal_init(wifi_netif);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init captive portal: %s", esp_err_to_name(err));
        wifi_manager_stop();
        return err;
    }

    // Imposta i flag solo se tutto è andato a buon fine
    wifi_requested = true;
    movement_detected = true;
    
    ESP_LOGI(TAG, "WiFi and captive portal started successfully");
    return ESP_OK;
}


static void motion_monitor_task(void *pvParameters) {
    ESP_LOGI(TAG, "Motion monitor task started");
    
    vTaskDelay(pdMS_TO_TICKS(2000));  // Delay iniziale per stabilizzazione
    
    int shake_count = 0;
    int64_t shake_start_time = 0;
    float last_x = 0, last_y = 0, last_z = 0;
    
    while(1) {
        qmi8658_data_t accel_data;
        esp_err_t ret = read_qmi8658_accel(&accel_data);
        
        if (ret == ESP_OK) {
            float delta_x = fabsf(accel_data.x - last_x);
            float delta_y = fabsf(accel_data.y - last_y);
            float delta_z = fabsf(accel_data.z - last_z);
            
            float total_delta = sqrtf(delta_x * delta_x + delta_y * delta_y + delta_z * delta_z);
            
            // Verifica stato del WiFi usando l'interfaccia esistente
            int wifi_state = wifi_manager_get_state();
            if (wifi_state == WIFI_STATE_OFF) {
                wifi_requested = false;
                movement_detected = false;
            }
            
            if (total_delta > SHAKE_THRESHOLD) {
                if (shake_count == 0) {
                    shake_start_time = esp_timer_get_time() / 1000;
                }
                shake_count++;
                
                ESP_LOGD(TAG, "Shake detected! Count: %d, Delta: %.2f", shake_count, total_delta);
                
                int64_t current_time = esp_timer_get_time() / 1000;
                if (shake_count >= SHAKE_COUNT && 
                    (current_time - shake_start_time) <= SHAKE_WINDOW_MS) {
                    
                    ESP_LOGI(TAG, "Significant shake sequence detected!");
                    
                    if (wifi_state == WIFI_STATE_OFF) {
                        esp_err_t err = safe_start_wifi_and_portal();
                        if (err != ESP_OK) {
                            ESP_LOGE(TAG, "Failed to start WiFi: %s", esp_err_to_name(err));
                        }
                    } else {
                        ESP_LOGI(TAG, "WiFi already active, state: %d", wifi_state);
                    }
                    
                    shake_count = 0;
                }
            } else {
                int64_t current_time = esp_timer_get_time() / 1000;
                if ((current_time - shake_start_time) > SHAKE_WINDOW_MS) {
                    shake_count = 0;
                }
            }
            
            last_x = accel_data.x;
            last_y = accel_data.y;
            last_z = accel_data.z;
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
                esp_err_t err = safe_start_wifi_and_portal();
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to start WiFi and captive portal");
                }
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
    if (i2c_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize I2C: %s", esp_err_to_name(i2c_ret));
    } else {
        ESP_LOGI(TAG, "I2C initialized successfully, initializing QMI8658...");
        vTaskDelay(pdMS_TO_TICKS(100));  // Breve delay
        
        esp_err_t sensor_ret = init_qmi8658();
        if (sensor_ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize QMI8658: %s", esp_err_to_name(sensor_ret));
        } else {
            ESP_LOGI(TAG, "QMI8658 initialized successfully, starting monitor task...");
            xTaskCreate(motion_monitor_task, "motion_monitor", 4096, NULL, 
                    tskIDLE_PRIORITY + 3, NULL);
        }
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