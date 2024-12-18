#include <stdio.h>
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_spiffs.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "llm.h"
#include "ws_matrix.h"
#include "wifi_manager.h"
#include "motion_sensor.h"
#include "button_manager.h"
#include "captive_portal.h"

static const char *TAG = "MAIN";
static EventGroupHandle_t system_events;
static bool wifi_requested = false;

// LLM parameters structure
typedef struct {
    Transformer* transformer;
    Tokenizer* tokenizer;
    Sampler* sampler;
    int steps;
    generated_complete_cb callback;
    bool dream_ready;
} LLMParams;


// Forward declarations
void generation_complete_callback(float tk_s) {
    ESP_LOGI(TAG, "Generation complete: %.2f tok/s", tk_s);
}


static esp_err_t init_storage(void);
static void wifi_start_callback(void);

// Callback for motion detection and button press
static void wifi_start_callback(void) {
    if (!wifi_requested) {
        ESP_LOGI(TAG, "Starting WiFi and captive portal");
        pause_animations();  // Pause animations before starting WiFi
        esp_err_t err = wifi_manager_start();
        if (err == ESP_OK) {
            wifi_requested = true;
            err = captive_portal_init(wifi_manager_get_netif());
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to init captive portal");
                wifi_manager_stop();
                wifi_requested = false;
                resume_animations();  // Resume animations if failed
            }
        } else {
            ESP_LOGE(TAG, "Failed to start WiFi");
            resume_animations();  // Resume animations if failed
        }
    }
}

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
    bool initial_generation = true;
    int disconnect_counter = 0;
    
    ESP_LOGI("LLM_TASK", "Starting LLM task");
    
    // Wait for matrix pattern to complete before starting
    ESP_LOGI("LLM_TASK", "Waiting for matrix pattern to complete...");
    while (!(xEventGroupGetBits(matrix_events) & MATRIX_PATTERN_COMPLETE_BIT)) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGI("LLM_TASK", "Matrix pattern complete, proceeding with generation");
    
    while(1) {
        if (initial_generation) {
            // Generate first dream
            generate(params->transformer, params->tokenizer, params->sampler,
                    NULL, params->steps, params->callback);
            
            // Start first animation
            animate_dream(llm_output_buffer);
            initial_generation = false;
            continue;
        }
        
        // Check if we need to generate new content
        EventBits_t bits = xEventGroupGetBits(animation_events);
        bool needs_generation = (bits & GENERATION_NEEDED_BIT) != 0;
        bool animation_active = (bits & ANIMATION_IN_PROGRESS_BIT) != 0;
        
        if (needs_generation && !animation_active && is_animation_enabled()) {
            ESP_LOGI("LLM_TASK", "Starting new generation");
            
            // Clear generation flag before starting
            xEventGroupClearBits(animation_events, GENERATION_NEEDED_BIT);
            
            // Generate new content
            generate(params->transformer, params->tokenizer, params->sampler,
                    NULL, params->steps, params->callback);
            
            // Start animation if no animation is currently running
            animate_dream(llm_output_buffer);
        }
        
        // Handle WiFi state
        wifi_state_t wifi_state = wifi_manager_get_state();
        if (wifi_state == WIFI_STATE_CLIENT_CONNECTED) {
            disconnect_counter = 0;
        } else if (wifi_state == WIFI_STATE_ON) {
            disconnect_counter++;
            if (disconnect_counter >= 300) {
                disconnect_counter = 0;
                ESP_LOGI("LLM_TASK", "No clients connected for 30 seconds, stopping WiFi");
                wifi_manager_stop();
                wifi_requested = false;
            }
        } else {
            disconnect_counter = 0;
        }
        
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void app_main(void) {
    // Create event group for synchronization
    system_events = xEventGroupCreate();
    
    // Initialize NVS first
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize SPIFFS before loading matrix pattern
    ESP_ERROR_CHECK(init_storage());

    // Initialize matrix base hardware
    ESP_ERROR_CHECK(matrix_init());
    matrix_clear();
    matrix_set_brightness(40);

    // Initialize networking components
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(wifi_manager_init());

    // Initialize input devices with callback
    ESP_ERROR_CHECK(button_manager_init(wifi_start_callback));
    //ESP_ERROR_CHECK(motion_sensor_init(wifi_start_callback));

    // Create matrix pattern task separately
    xTaskCreate(matrix_pattern_task, "matrix_pattern", 4096, NULL, 5, NULL);

    // Prepare LLM
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

    // Create LLM parameters with the new callback
    LLMParams* llm_params = malloc(sizeof(LLMParams));
    llm_params->transformer = transformer;
    llm_params->tokenizer = tokenizer;
    llm_params->sampler = sampler;
    llm_params->steps = steps;
    llm_params->callback = generation_complete_callback;  // Use the new non-static callback

    // Create LLM task
    xTaskCreate(llm_task, "llm_task", 16384,
                llm_params, 5, NULL);

    ESP_LOGI(TAG, "Initialization complete - Press button or shake device to enable WiFi");
}