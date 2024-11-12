#include "ws_matrix.h"
#include <string.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
#include "esp_random.h"

// WS2812B timing (in RMT ticks, 1 tick = 25ns with clock divider of 2)
#define RMT_CLK_DIV 2
#define T0H 12    // 0.4us
#define T0L 28    // 0.85us  
#define T1H 24    // 0.8us
#define T1L 16    // 0.45us

static const char* TAG = "WS_MATRIX";

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

void activate_new_node_task(void *arg) {
    rgb_color_t blue = {.r = 0, .g = 0, .b = 60};
    int x = *((int*)arg);
    int y = *((int*)arg + 1);

    fade_in_single_pixel(x, y, blue);
    free(arg);
    vTaskDelete(NULL);
}

// Global state variables
static rmt_channel_t rmt_channel = RMT_CHANNEL_0;
static rgb_color_t framebuffer[MATRIX_ROWS][MATRIX_COLS] = {0};
static uint8_t brightness = DEFAULT_BRIGHTNESS;
static bool node_active[MATRIX_ROWS][MATRIX_COLS] = {0};
static int total_active_nodes = 0;

esp_err_t matrix_init(void) {
    ESP_LOGI(TAG, "Initializing LED matrix...");
    
    gpio_reset_pin(RGB_CONTROL_PIN);
    gpio_set_direction(RGB_CONTROL_PIN, GPIO_MODE_OUTPUT);
    
    rmt_config_t config = RMT_DEFAULT_CONFIG_TX(RGB_CONTROL_PIN, rmt_channel);
    config.clk_div = RMT_CLK_DIV;
    config.mem_block_num = 1;
    
    ESP_ERROR_CHECK(rmt_config(&config));
    ESP_ERROR_CHECK(rmt_driver_install(config.channel, 0, 0));
    
    matrix_clear();
    vTaskDelay(pdMS_TO_TICKS(100));
    
    ESP_LOGI(TAG, "Matrix initialization complete");
    return ESP_OK;
}

static void ws2812_send_pixel(rgb_color_t color) {
    static rmt_item32_t pixels[24];
    
    // Apply brightness
    uint8_t r = (color.r * brightness) / 255;
    uint8_t g = (color.g * brightness) / 255;
    uint8_t b = (color.b * brightness) / 255;
    
    // WS2812B expects GRB order
    for(int i = 0; i < 8; i++) {
        bool bit = g & (1 << (7-i));
        pixels[i] = (rmt_item32_t){
            .duration0 = bit ? T1H : T0H,
            .level0 = 1,
            .duration1 = bit ? T1L : T0L,
            .level1 = 0,
        };
    }
    for(int i = 0; i < 8; i++) {
        bool bit = r & (1 << (7-i));
        pixels[i+8] = (rmt_item32_t){
            .duration0 = bit ? T1H : T0H,
            .level0 = 1,
            .duration1 = bit ? T1L : T0L,
            .level1 = 0,
        };
    }
    for(int i = 0; i < 8; i++) {
        bool bit = b & (1 << (7-i));
        pixels[i+16] = (rmt_item32_t){
            .duration0 = bit ? T1H : T0H,
            .level0 = 1,
            .duration1 = bit ? T1L : T0L,
            .level1 = 0,
        };
    }
    
    rmt_write_items(rmt_channel, pixels, 24, true);
    rmt_wait_tx_done(rmt_channel, portMAX_DELAY);
}

void matrix_set_pixel(uint8_t x, uint8_t y, rgb_color_t color) {
    if (x < MATRIX_COLS && y < MATRIX_ROWS) {
        framebuffer[y][x] = color;
        ESP_LOGD(TAG, "Set pixel (%d,%d) to RGB(%d,%d,%d)", x, y, color.r, color.g, color.b);
    }
}

void matrix_show(void) {
    for (int y = 0; y < MATRIX_ROWS; y++) {
        for (int x = 0; x < MATRIX_COLS; x++) {
            ws2812_send_pixel(framebuffer[y][x]);
        }
    }
    vTaskDelay(pdMS_TO_TICKS(1));
}

void matrix_clear(void) {
    rgb_color_t black = {0, 0, 0};
    for (int y = 0; y < MATRIX_ROWS; y++) {
        for (int x = 0; x < MATRIX_COLS; x++) {
            framebuffer[y][x] = black;
            node_active[y][x] = false;
        }
    }
    total_active_nodes = 0;
    matrix_show();
}

void matrix_set_brightness(uint8_t new_brightness) {
    brightness = (new_brightness > MAX_BRIGHTNESS) ? MAX_BRIGHTNESS : new_brightness;
}

static rgb_color_t dim_color(rgb_color_t color, uint8_t intensity) {
    rgb_color_t dimmed;
    dimmed.r = (color.r * intensity) / FADE_MAX_INTENSITY;
    dimmed.g = (color.g * intensity) / FADE_MAX_INTENSITY;
    dimmed.b = (color.b * intensity) / FADE_MAX_INTENSITY;
    return dimmed;
}

void fade_in_single_pixel(uint8_t x, uint8_t y, rgb_color_t color) {
    // Prima fase: Fade in graduale
    for (int step = 0; step <= FADE_STEPS; step++) {
        int intensity = (step * FADE_MAX_INTENSITY) / FADE_STEPS;
        rgb_color_t dimmed = dim_color(color, intensity);
        matrix_set_pixel(x, y, dimmed);
        matrix_show();
        vTaskDelay(pdMS_TO_TICKS(FADE_DELAY_MS));
    }

    // Seconda fase: Flash bianco
    rgb_color_t white = {
        .r = FLASH_INTENSITY,
        .g = FLASH_INTENSITY,
        .b = FLASH_INTENSITY
    };
    matrix_set_pixel(x, y, white);
    matrix_show();
    vTaskDelay(pdMS_TO_TICKS(FLASH_DURATION_MS));

    // Fase finale: Torna al colore target
    matrix_set_pixel(x, y, color);
    matrix_show();

    // Aggiorna lo stato del nodo
    if (!node_active[y][x]) {
        node_active[y][x] = true;
        total_active_nodes++;
    }
}

void test_matrix(void) {
    ESP_LOGI(TAG, "Starting matrix test");
    
    // Adjusted Google colors
    rgb_color_t google_blue = {.r = 0, .g = 0, .b = 255};
    rgb_color_t google_red = {.r = 255, .g = 0, .b = 0};
    rgb_color_t google_yellow = {.r = 255, .g = 180, .b = 0};
    rgb_color_t google_green = {.r = 0, .g = 255, .b = 0};
    
    matrix_clear();
    vTaskDelay(pdMS_TO_TICKS(500));
    
    const struct {
        int x;
        int y;
        rgb_color_t color;
    } leds[] = {
        {2, 3, google_blue},
        {3, 3, google_red},
        {4, 3, google_yellow},
        {5, 3, google_green}
    };
    
    const int QUICK_FADE_STEPS = 10;     
    const int QUICK_FADE_DELAY = 15;     
    
    for (int led = 0; led < 4; led++) {
        for (int step = 0; step <= QUICK_FADE_STEPS; step++) {
            uint8_t step_brightness = (step * 255) / QUICK_FADE_STEPS;
            
            rgb_color_t temp_color = {
                .r = (leds[led].color.r * step_brightness) / 255,
                .g = (leds[led].color.g * step_brightness) / 255,
                .b = (leds[led].color.b * step_brightness) / 255
            };
            
            matrix_set_pixel(leds[led].x, leds[led].y, temp_color);
            matrix_show();
            vTaskDelay(pdMS_TO_TICKS(QUICK_FADE_DELAY));
        }
        
        rgb_color_t normal_color = {
            .r = (leds[led].color.r * 60) / 255,
            .g = (leds[led].color.g * 60) / 255,
            .b = (leds[led].color.b * 60) / 255
        };
        
        matrix_set_pixel(leds[led].x, leds[led].y, normal_color);
        matrix_show();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    
    vTaskDelay(pdMS_TO_TICKS(1000));
    matrix_clear();
}