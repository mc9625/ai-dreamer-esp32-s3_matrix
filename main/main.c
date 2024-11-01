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

// Matrix pattern constants
#define MIN_INITIAL_NODES 20
#define MAX_INITIAL_NODES 40

#define MIN(a, b) ((a) < (b) ? (a) : (b))

static const char *TAG = "MAIN";

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
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find SPIFFS partition");
        } else {
            ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        }
        return ret;
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info(NULL, &total, &used);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
    }
    
    return ret;
}

static void generate_complete_cb(float tk_s) {
    printf("%.2f tok/s\n", tk_s);
}

static unsigned long long get_random_seed(void) {
    unsigned long long high = esp_random();
    unsigned long long low = esp_random();
    unsigned long long seed = (high << 32) | low;
    
    seed ^= (unsigned long long)time(NULL);
    
    if (seed < 1000000) {
        seed += 1000000;
    }
    
    return seed;
}

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

    // Array per tenere traccia della luminosità corrente dei LED
    uint8_t current_brightness[MATRIX_COLS][MATRIX_ROWS] = {0};

    while (nodes_created < total_nodes || active_clusters > 0) {
        // Aggiungi un nuovo cluster se possibile
        if (nodes_created < total_nodes && active_clusters < MAX_CONCURRENT_NODES) {
            int nodes_in_cluster = 1 + (esp_random() % MAX_NODES_PER_CLUSTER);
            int base_idx = active_clusters * MAX_NODES_PER_CLUSTER;

            for (int i = 0; i < nodes_in_cluster && nodes_created < total_nodes; i++) {
                int x = esp_random() % MATRIX_COLS;
                int y = esp_random() % MATRIX_ROWS;

                // Se il LED è già acceso, aumenta la luminosità solo di quel LED
                if (current_brightness[x][y] < NORMAL_BRIGHTNESS) {
                    active_nodes[base_idx + i].x = x;
                    active_nodes[base_idx + i].y = y;
                    active_nodes[base_idx + i].step = 0;
                    active_nodes[base_idx + i].fade_steps = FADE_STEPS + (esp_random() % 10);
                    active_nodes[base_idx + i].delay = FADE_DELAY_MS + (esp_random() % 20);
                    active_nodes[base_idx + i].completed = false;
                    nodes_created++;
                } else {
                    // Se il LED è già acceso, aumenta solo la luminosità
                    current_brightness[x][y] = MIN(current_brightness[x][y] + 10, NORMAL_BRIGHTNESS);
                    // Qui puoi aggiungere il codice per aggiornare il LED alla nuova luminosità
                    rgb_color_t updated_color = {.r = 0, .g = 0, .b = current_brightness[x][y]};
                    matrix_set_pixel(x, y, updated_color); // Aggiorna il pixel con la nuova luminosità
                }
            }
            active_clusters++;
        }

        // Animazione dei nodi attivi
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
                        current_brightness[node->x][node->y] = brightness; // Aggiorna la luminosità corrente
                    } else {
                        rgb_color_t final_color = {.r = 0, .g = 0, .b = NORMAL_BRIGHTNESS};
                        matrix_set_pixel(node->x, node->y, final_color);
                        node->completed = true;
                    }
                }
            }

            if (cluster_completed) {
                // Rimuovi il cluster completato
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

void activate_new_node_task(void *arg) {
    rgb_color_t blue = {.r = 0, .g = 0, .b = 60};
    int x = *((int*)arg);
    int y = *((int*)arg + 1);

    fade_in_single_pixel(x, y, blue);
    free(arg);
    vTaskDelete(NULL);
}

void app_main(void) {
    // Initialize matrix and show initial patterns

    ESP_ERROR_CHECK(matrix_init());
    matrix_clear();
    matrix_set_brightness(40);
    //test_matrix(); 
    //initialize_matrix_pattern();

    // Initialize storage and LLM
    ESP_ERROR_CHECK(init_storage());

    // LLM configuration
    char *checkpoint_path = "/data/aidreams260K.bin";
    char *tokenizer_path = "/data/tok512.bin";
    float temperature = 0.8f;
    float topp = 0.3f;
    int steps = 512;
    unsigned long long rng_seed = get_random_seed();

    // Initialize transformer components
    Transformer transformer;
    ESP_LOGI(TAG, "Loading model from %s", checkpoint_path);
    build_transformer(&transformer, checkpoint_path);
    
    if (steps == 0 || steps > transformer.config.seq_len) {
        steps = transformer.config.seq_len;
    }

    Tokenizer tokenizer;
    build_tokenizer(&tokenizer, tokenizer_path, transformer.config.vocab_size);

    Sampler sampler;
    ESP_LOGI(TAG, "Creating sampler with temperature=%f, topp=%f", temperature, topp);
    build_sampler(&sampler, transformer.config.vocab_size, temperature, topp, rng_seed);

    // Start generation
    generate(&transformer, &tokenizer, &sampler, NULL, steps, &generate_complete_cb);
    free_transformer(&transformer);
    free_tokenizer(&tokenizer);
    free_sampler(&sampler);
}