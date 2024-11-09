#include <stdio.h>
#include <string.h>
#include "esp_spiffs.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "freertos/event_groups.h"
#include "llm.h"
#include "ws_matrix.h"
#include "captive_portal.h"

// Constants
#define MIN_INITIAL_NODES 20
#define MAX_INITIAL_NODES 40
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAIN_TASK_STACK_SIZE 16384
#define LLM_TASK_PRIORITY    5

// Task synchronization
static EventGroupHandle_t system_events;
#define LLM_COMPLETE_BIT BIT0

static const char *TAG = "MAIN";
static esp_netif_t *wifi_netif = NULL;

// Struttura per i parametri LLM
typedef struct {
    Transformer* transformer;
    Tokenizer* tokenizer;
    Sampler* sampler;
    int steps;
    generated_complete_cb callback;
} LLMParams;

// Forward declarations delle funzioni
static void generate_complete_cb(float tk_s);
static esp_err_t init_storage(void);
static esp_err_t init_wifi(void);
void initialize_matrix_pattern(void);
void activate_new_node_task(void *arg);

// Task LLM
static void llm_task(void *pvParameters)
{
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

// WiFi event handler
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                             int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_AP_STACONNECTED) {
            wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
        } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
            wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
        }
    }
}

// Storage initialization
static esp_err_t init_storage(void)
{
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

// WiFi initialization
static esp_err_t init_wifi(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.static_rx_buf_num = 3;
    cfg.dynamic_rx_buf_num = 8;
    cfg.static_tx_buf_num = 6;
    cfg.cache_tx_buf_num = 0;
    cfg.wifi_task_core_id = 0;

    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                      ESP_EVENT_ANY_ID,
                                                      &wifi_event_handler,
                                                      NULL,
                                                      NULL));

    wifi_netif = esp_netif_create_default_wifi_ap();

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = "ESP32-AI-DREAMER",
            .ssid_len = strlen("ESP32-AI-DREAMER"),
            .channel = 1,
            .password = "",
            .max_connection = 1,
            .authmode = WIFI_AUTH_OPEN
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    return ESP_OK;
}

// Callback for LLM generation completion
static void generate_complete_cb(float tk_s) {
    ESP_LOGI(TAG, "Generation complete: %.2f tok/s", tk_s);
}

// Matrix pattern initialization
void initialize_matrix_pattern(void) {
    matrix_clear();

    typedef struct {
        int x;
        int y;
        int step;
        int fade_steps;
        int delay;
        bool completed;
    } AnimNode;

    #define MAX_CONCURRENT_NODES 2
    #define MAX_NODES_PER_CLUSTER 3

    AnimNode active_nodes[MAX_NODES_PER_CLUSTER * MAX_CONCURRENT_NODES] = {0};
    int total_nodes = MIN_INITIAL_NODES + (esp_random() % (MAX_INITIAL_NODES - MIN_INITIAL_NODES));
    int nodes_created = 0;
    int active_clusters = 0;

    uint8_t current_brightness[MATRIX_COLS][MATRIX_ROWS] = {0};

    while (nodes_created < total_nodes || active_clusters > 0) {
        if (nodes_created < total_nodes && active_clusters < MAX_CONCURRENT_NODES) {
            int nodes_in_cluster = 1 + (esp_random() % MAX_NODES_PER_CLUSTER);
            int base_idx = active_clusters * MAX_NODES_PER_CLUSTER;

            for (int i = 0; i < nodes_in_cluster && nodes_created < total_nodes; i++) {
                int x = esp_random() % MATRIX_COLS;
                int y = esp_random() % MATRIX_ROWS;

                if (current_brightness[x][y] < NORMAL_BRIGHTNESS) {
                    active_nodes[base_idx + i].x = x;
                    active_nodes[base_idx + i].y = y;
                    active_nodes[base_idx + i].step = 0;
                    active_nodes[base_idx + i].fade_steps = FADE_STEPS + (esp_random() % 10);
                    active_nodes[base_idx + i].delay = FADE_DELAY_MS + (esp_random() % 20);
                    active_nodes[base_idx + i].completed = false;
                    nodes_created++;
                } else {
                    current_brightness[x][y] = MIN(current_brightness[x][y] + 10, NORMAL_BRIGHTNESS);
                    rgb_color_t updated_color = {.r = 0, .g = 0, .b = current_brightness[x][y]};
                    matrix_set_pixel(x, y, updated_color);
                }
            }
            active_clusters++;
        }

        for (int c = 0; c < active_clusters; c++) {
            int cluster_completed = true;
            int base_idx = c * MAX_NODES_PER_CLUSTER;

            for (int i = 0; i < MAX_NODES_PER_CLUSTER; i++) {
                AnimNode *node = &active_nodes[base_idx + i];
                if (node->step <= node->fade_steps && !node->completed) {
                    cluster_completed = false;
                    if (node->step < node->fade_steps) {
                        uint8_t brightness = (node->step * NORMAL_BRIGHTNESS) / node->fade_steps;
                        rgb_color_t curr_color = {.r = 0, .g = 0, .b = brightness};
                        matrix_set_pixel(node->x, node->y, curr_color);
                        node->step++;
                        current_brightness[node->x][node->y] = brightness;
                    } else {
                        rgb_color_t final_color = {.r = 0, .g = 0, .b = NORMAL_BRIGHTNESS};
                        matrix_set_pixel(node->x, node->y, final_color);
                        node->completed = true;
                    }
                }
            }

            if (cluster_completed) {
                if (c < active_clusters - 1) {
                    for (int j = 0; j < MAX_NODES_PER_CLUSTER; j++) {
                        active_nodes[base_idx + j] = active_nodes[base_idx + j + MAX_NODES_PER_CLUSTER];
                    }
                }
                active_clusters--;
                c--;
            }
        }

        matrix_show();
        vTaskDelay(pdMS_TO_TICKS(FADE_DELAY_MS));
    }
}

// Task for new node activation
void activate_new_node_task(void *arg) {
    rgb_color_t blue = {.r = 0, .g = 0, .b = 60};
    int x = *((int*)arg);
    int y = *((int*)arg + 1);

    fade_in_single_pixel(x, y, blue);
    free(arg);
    vTaskDelete(NULL);
}

// Main application
void app_main(void)
{
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
    int steps = 640;

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

    // Ensure LED animations are complete
    matrix_clear();
    vTaskDelay(pdMS_TO_TICKS(1000));

    // Initialize WiFi and captive portal
    ESP_ERROR_CHECK(init_wifi());
    vTaskDelay(pdMS_TO_TICKS(1000));
    ESP_ERROR_CHECK(captive_portal_init());
}