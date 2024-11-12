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

#include "llm.h"
#include "ws_matrix.h"
#include "wifi_manager.h"
#include "captive_portal.h"

static const char *TAG = "MAIN";

// Constants
#define MAIN_TASK_STACK_SIZE 16384
#define LLM_TASK_PRIORITY    5

// Task synchronization
static EventGroupHandle_t system_events;
#define LLM_COMPLETE_BIT BIT0

static esp_netif_t *wifi_netif = NULL;

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
    
    // Generate text
    generate(params->transformer, params->tokenizer, params->sampler,
             NULL, params->steps, params->callback);
    
    // Free resources
    free_transformer(params->transformer);
    free_tokenizer(params->tokenizer);
    free_sampler(params->sampler);
    free(params);
    
    // Signal completion
    xEventGroupSetBits(system_events, LLM_COMPLETE_BIT);
    vTaskDelete(NULL);
}

// Callback for LLM generation completion
static void generate_complete_cb(float tk_s) {
    ESP_LOGI(TAG, "Generation complete: %.2f tok/s", tk_s);
}

void app_main(void) {
    // Create event group for synchronization
    system_events = xEventGroupCreate();
    
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

    // Create LLM task
    xTaskCreate(llm_task, "llm_task", MAIN_TASK_STACK_SIZE,
                llm_params, LLM_TASK_PRIORITY, NULL);

    // Wait for LLM completion
    xEventGroupWaitBits(system_events, LLM_COMPLETE_BIT,
                       pdTRUE, pdTRUE, portMAX_DELAY);

    // Initialize networking stack and WiFi
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(wifi_manager_init(&wifi_netif));
    
    // Initialize captive portal
    vTaskDelay(pdMS_TO_TICKS(1000));
    ESP_ERROR_CHECK(captive_portal_init(wifi_netif));
    
    ESP_LOGI(TAG, "Initialization complete");
}