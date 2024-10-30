#include <stdio.h>
#include <inttypes.h>
#include "esp_spiffs.h"
#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_log.h"
#include <time.h>
#include "llm.h"
#include <string.h>
#include "esp_random.h"

#include "ws_matrix.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define MIN_INITIAL_NODES 20
#define MAX_INITIAL_NODES 40

static const char *TAG = "MAIN";

#define FADE_STEPS 20
#define FADE_DELAY_MS 30
#define FLASH_BRIGHTNESS 180
#define NORMAL_BRIGHTNESS 60


/**
 * @brief intializes SPIFFS storage
 * 
 */
void init_storage(void)
{

    ESP_LOGI(TAG, "Initializing SPIFFS");

    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/data",
        .partition_label = NULL,
        .max_files = 5,
        .format_if_mount_failed = false};

    esp_err_t ret = esp_vfs_spiffs_register(&conf);

    if (ret != ESP_OK)
    {
        if (ret == ESP_FAIL)
        {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        }
        else if (ret == ESP_ERR_NOT_FOUND)
        {
            ESP_LOGE(TAG, "Failed to find SPIFFS partition");
        }
        else
        {
            ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        }
        return;
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info(NULL, &total, &used);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret));
    }
    else
    {
        ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
    }
}

/**
 * @brief Outputs to display
 * 
 * @param text The text to output
 */
void write_display(char *text)
{
    printf(text);
}

/**
 * @brief Callbacks once generation is done
 * 
 * @param tk_s The number of tokens per second generated
 */
void generate_complete_cb(float tk_s)
{
    char buffer[50];
    sprintf(buffer, "%.2f tok/s", tk_s);
    write_display(buffer);
}

/**
 * @brief Draws a llama onscreen
 * 
 */
unsigned long long get_random_seed(void) {
    // Get a high-quality 64-bit seed combining two 32-bit values
    unsigned long long high = esp_random();
    unsigned long long low = esp_random();
    unsigned long long seed = (high << 32) | low;
    
    // Add a small amount of time-based entropy
    seed ^= (unsigned long long)time(NULL);
    
    // Make sure we never return 0 or small values
    if (seed < 1000000) {
        seed += 1000000;
    }
    
    return seed;
}

void initialize_matrix_pattern(void) {
    rgb_color_t blue = {.r = 0, .g = 0, .b = 60};
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

    while(nodes_created < total_nodes || active_clusters > 0) {
        // Add new cluster if possible
        if(nodes_created < total_nodes && active_clusters < MAX_CONCURRENT_NODES) {
            int nodes_in_cluster = 1 + (esp_random() % MAX_NODES_PER_CLUSTER);
            int base_idx = active_clusters * MAX_NODES_PER_CLUSTER;
            
            for(int i = 0; i < nodes_in_cluster && nodes_created < total_nodes; i++) {
                active_nodes[base_idx + i].x = esp_random() % MATRIX_COLS;
                active_nodes[base_idx + i].y = esp_random() % MATRIX_ROWS;
                active_nodes[base_idx + i].step = 0;
                active_nodes[base_idx + i].fade_steps = FADE_STEPS + (esp_random() % 10);
                active_nodes[base_idx + i].delay = FADE_DELAY_MS + (esp_random() % 20);
                active_nodes[base_idx + i].completed = false;
                nodes_created++;
            }
            active_clusters++;
        }

        // Animate all active nodes
        for(int c = 0; c < active_clusters; c++) {
            int cluster_completed = true;
            int base_idx = c * MAX_NODES_PER_CLUSTER;
            
            for(int i = 0; i < MAX_NODES_PER_CLUSTER; i++) {
                AnimNode* node = &active_nodes[base_idx + i];
                if(node->step <= node->fade_steps && !node->completed) {
                    cluster_completed = false;
                    if(node->step < node->fade_steps) {
                        uint8_t brightness = (node->step * NORMAL_BRIGHTNESS) / node->fade_steps;
                        rgb_color_t curr_color = {.r = 0, .g = 0, .b = brightness};
                        matrix_set_pixel(node->x, node->y, curr_color);
                        node->step++;
                    } else {
                        rgb_color_t final_color = {.r = 0, .g = 0, .b = NORMAL_BRIGHTNESS};
                        matrix_set_pixel(node->x, node->y, final_color);
                        node->completed = true;
                    }
                }
            }

            if(cluster_completed) {
                // Remove this cluster by shifting others down
                if(c < active_clusters - 1) {
                    for(int j = 0; j < MAX_NODES_PER_CLUSTER; j++) {
                        active_nodes[base_idx + j] = active_nodes[base_idx + j + MAX_NODES_PER_CLUSTER];
                    }
                }
                active_clusters--;
                c--; // Recheck this position
            }
        }
        
        matrix_show();
        vTaskDelay(pdMS_TO_TICKS(FADE_DELAY_MS));
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(matrix_init());
    matrix_set_brightness(40);
    test_matrix(); 
    
    // Show initial pattern
    initialize_matrix_pattern();

    write_display("Loading Model");
    init_storage();

    // default parameters
    char *checkpoint_path = "/data/aidreams260K.bin"; // e.g. out/model.bin
    char *tokenizer_path = "/data/tok512.bin";
    float temperature = 0.8f;        // 0.0 = greedy deterministic. 1.0 = original. don't set higher
    float topp = 0.9f;               // top-p in nucleus sampling. 1.0 = off. 0.9 works well, but slower
    int steps = 256;                 // number of steps to run for
    char *prompt = NULL;             // prompt string
    // In app_main
    unsigned long long rng_seed = get_random_seed();

    if (rng_seed <= 0) {
        // Use time as base, but mix in some hardware entropy
        rng_seed = (unsigned long long)time(NULL);
        // XOR with a single hardware random value
        rng_seed ^= esp_random();
    }



    // build the Transformer via the model .bin file
    Transformer transformer;
    ESP_LOGI(TAG, "LLM Path is %s", checkpoint_path);
    build_transformer(&transformer, checkpoint_path);
    if (steps == 0 || steps > transformer.config.seq_len)
        steps = transformer.config.seq_len; // override to ~max length

    // build the Tokenizer via the tokenizer .bin file
    Tokenizer tokenizer;
    build_tokenizer(&tokenizer, tokenizer_path, transformer.config.vocab_size);

    // build the Sampler
    Sampler sampler;
    ESP_LOGI(TAG, "Creating sampler with temperature=%f, topp=%f", temperature, topp);
    build_sampler(&sampler, transformer.config.vocab_size, temperature, topp, rng_seed);

    // run!

    generate(&transformer, &tokenizer, &sampler, prompt, steps, &generate_complete_cb);
}
