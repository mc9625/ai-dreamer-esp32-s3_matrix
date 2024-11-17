#include "ws_matrix.h"
#include <string.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
#include "esp_random.h"
#include <math.h>
#include "esp_timer.h"

// WS2812B timing (in RMT ticks, 1 tick = 25ns with clock divider of 2)
#define RMT_CLK_DIV 2
#define T0H 12    // 0.4us
#define T0L 28    // 0.85us  
#define T1H 24    // 0.8us
#define T1L 16    // 0.45us

static const char *TAG = "WS_MATRIX";
static EventGroupHandle_t matrix_events = NULL;
EventGroupHandle_t animation_events = NULL; 


bool wait_matrix_pattern_complete(void) {
    if (!matrix_events) {
        return false;
    }
    EventBits_t bits = xEventGroupWaitBits(matrix_events, 
                                          MATRIX_PATTERN_COMPLETE_BIT,
                                          pdTRUE, pdTRUE, portMAX_DELAY);
    return (bits & MATRIX_PATTERN_COMPLETE_BIT) != 0;
}


static bool animation_enabled = true;

bool is_animation_enabled(void) {
    return animation_enabled;
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

void matrix_pattern_task(void *pvParameters) {
    // Initial small delay for system stability
    vTaskDelay(pdMS_TO_TICKS(100));
    
    // Clear initial state
    matrix_clear();
    matrix_show();
    
    // CRITICAL: Signal that we're ready for LLM to start, do this early
    xEventGroupSetBits(matrix_events, MATRIX_PATTERN_COMPLETE_BIT);
    ESP_LOGI(TAG, "Matrix ready for LLM start");
    
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
                }
                
                vTaskDelay(pdMS_TO_TICKS(1));
            }
            active_clusters++;
        }

        for (int c = 0; c < active_clusters; c++) {
            bool cluster_completed = true;
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
                        node->completed = true;
                    }
                }
            }

            if (cluster_completed) {
                if (c < active_clusters - 1) {
                    memcpy(&active_nodes[base_idx], 
                           &active_nodes[base_idx + MAX_NODES_PER_CLUSTER], 
                           MAX_NODES_PER_CLUSTER * sizeof(AnimNode));
                }
                active_clusters--;
                c--;
            }
        }

        matrix_show();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    
    ESP_LOGI(TAG, "Matrix pattern initialization complete");
    vTaskDelete(NULL);
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
    
    // Initialize event groups first
    if (matrix_events == NULL) {
        matrix_events = xEventGroupCreate();
        if (!matrix_events) {
            ESP_LOGE(TAG, "Failed to create matrix event group");
            return ESP_ERR_NO_MEM;
        }
        // Clear any existing bits
        xEventGroupClearBits(matrix_events, 0xFF);
    }
    
    if (animation_events == NULL) {
        animation_events = xEventGroupCreate();
        if (!animation_events) {
            ESP_LOGE(TAG, "Failed to create animation event group");
            vEventGroupDelete(matrix_events);
            return ESP_ERR_NO_MEM;
        }
        // Clear any existing bits and set initial state
        xEventGroupClearBits(animation_events, 0xFF);
        xEventGroupSetBits(animation_events, GENERATION_NEEDED_BIT);
    }
    
    // Initialize hardware
    gpio_reset_pin(RGB_CONTROL_PIN);
    gpio_set_direction(RGB_CONTROL_PIN, GPIO_MODE_OUTPUT);
    
    rmt_config_t config = RMT_DEFAULT_CONFIG_TX(RGB_CONTROL_PIN, rmt_channel);
    config.clk_div = RMT_CLK_DIV;
    config.mem_block_num = 1;
    
    ESP_ERROR_CHECK(rmt_config(&config));
    ESP_ERROR_CHECK(rmt_driver_install(config.channel, 0, 0));
    
    // Clear the matrix and set initial state
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

void fade_in_single_pixel(int x, int y, rgb_color_t final_color) {
    // Fade in from 0 to max brightness (NORMAL_BRIGHTNESS)
    for (int step = 0; step <= FADE_STEPS; step++) {
        uint8_t intensity = (step * NORMAL_BRIGHTNESS) / FADE_STEPS;
        rgb_color_t curr_color = {.r = 0, .g = 0, .b = intensity};
        matrix_set_pixel(x, y, curr_color);
        matrix_show();
        vTaskDelay(pdMS_TO_TICKS(FADE_DELAY_MS));
    }
    
    // Flash white at full brightness
    rgb_color_t white = {.r = FLASH_INTENSITY, .g = FLASH_INTENSITY, .b = FLASH_INTENSITY};
    matrix_set_pixel(x, y, white);
    matrix_show();
    vTaskDelay(pdMS_TO_TICKS(100));
    
    // Set final blue color at NORMAL_BRIGHTNESS
    matrix_set_pixel(x, y, final_color);
    matrix_show();
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

void animate_dream(const char* dream_text) {
    if (!animation_enabled) {
        ESP_LOGI(TAG, "Animation skipped - disabled");
        if (animation_events) {
            xEventGroupSetBits(animation_events, GENERATION_NEEDED_BIT);
        }
        return;
    }
    
    ESP_LOGI(TAG, "Starting dream animation");
    
    if (animation_events) {
        // Set animation state
        xEventGroupClearBits(animation_events, GENERATION_NEEDED_BIT);
        xEventGroupSetBits(animation_events, ANIMATION_IN_PROGRESS_BIT);
    }
    
    typedef struct {
        int x;
        int y;
        int adjacent_count;
    } LedPosition;
    
    // Trova tutti i LED accesi e conta i loro vicini
    LedPosition active_leds[MATRIX_ROWS * MATRIX_COLS];
    int num_active = 0;
    
    for (int y = 0; y < MATRIX_ROWS; y++) {
        for (int x = 0; x < MATRIX_COLS; x++) {
            if (framebuffer[y][x].b > 0) {
                active_leds[num_active].x = x;
                active_leds[num_active].y = y;
                
                // Conta LED adiacenti
                int adj_count = 0;
                for (int dy = -1; dy <= 1; dy++) {
                    for (int dx = -1; dx <= 1; dx++) {
                        if (dx == 0 && dy == 0) continue;
                        int nx = x + dx;
                        int ny = y + dy;
                        if (nx >= 0 && nx < MATRIX_COLS && ny >= 0 && ny < MATRIX_ROWS) {
                            if (framebuffer[ny][nx].b > 0) {
                                adj_count++;
                            }
                        }
                    }
                }
                active_leds[num_active].adjacent_count = adj_count;
                num_active++;
            }
        }
    }
    
    ESP_LOGI(TAG, "Found %d active LEDs to animate", num_active);
    
    // Ordina LED per numero di adiacenti (i più isolati prima)
    for (int i = 0; i < num_active - 1; i++) {
        for (int j = i + 1; j < num_active; j++) {
            if (active_leds[j].adjacent_count < active_leds[i].adjacent_count) {
                LedPosition temp = active_leds[i];
                active_leds[i] = active_leds[j];
                active_leds[j] = temp;
            }
        }
    }
    
    // Spegni i LED più isolati fino a lasciarne circa 30
    int leds_to_turnoff = num_active - 30;
    if (leds_to_turnoff > 0) {
        for (int i = 0; i < leds_to_turnoff; i++) {
            int x = active_leds[i].x;
            int y = active_leds[i].y;
            
            // Fade in veloce a full brightness
            for (int step = 0; step <= FADE_STEPS; step++) {
                uint8_t intensity = NORMAL_BRIGHTNESS + 
                                ((FADE_MAX_INTENSITY - NORMAL_BRIGHTNESS) * step) / FADE_STEPS;
                rgb_color_t curr_color = {.r = 0, .g = 0, .b = intensity};
                matrix_set_pixel(x, y, curr_color);
                matrix_show();
                vTaskDelay(pdMS_TO_TICKS(FADE_DELAY_MS));
            }
            
            // Flash bianco
            rgb_color_t white = {.r = FLASH_INTENSITY, .g = FLASH_INTENSITY, .b = FLASH_INTENSITY};
            matrix_set_pixel(x, y, white);
            matrix_show();
            vTaskDelay(pdMS_TO_TICKS(50));
            
            // Fade out veloce
            for (int step = FADE_STEPS; step >= 0; step--) {
                uint8_t intensity = (step * FADE_MAX_INTENSITY) / FADE_STEPS;
                rgb_color_t curr_color = {.r = 0, .g = 0, .b = intensity};
                matrix_set_pixel(x, y, curr_color);
                matrix_show();
                vTaskDelay(pdMS_TO_TICKS(FADE_DELAY_MS));
            }
            
            rgb_color_t off = {.r = 0, .g = 0, .b = 0};
            matrix_set_pixel(x, y, off);
            matrix_show();
        }
    }
    
    const int COLOR_TRANSITION_STEPS = FADE_STEPS * 2;  // Più passi per una transizione più smooth
    rgb_color_t final_color = {.r = 0, .g = 0, .b = LIGHT_BLUE_B};
    
    for (int step = 0; step <= COLOR_TRANSITION_STEPS; step++) {
        for (int y = 0; y < MATRIX_ROWS; y++) {
            for (int x = 0; x < MATRIX_COLS; x++) {
                if (framebuffer[y][x].b > 0) {
                    float ratio = (float)step / COLOR_TRANSITION_STEPS;
                    uint8_t new_blue = NORMAL_BRIGHTNESS + 
                                     (uint8_t)((LIGHT_BLUE_B - NORMAL_BRIGHTNESS) * ratio);
                    rgb_color_t curr_color = {.r = 0, .g = 0, .b = new_blue};
                    matrix_set_pixel(x, y, curr_color);
                }
            }
        }
        matrix_show();
        vTaskDelay(pdMS_TO_TICKS(FADE_DELAY_MS));
    }
    
    // Effetto pulse più smooth e pronunciato
    const int PULSE_DURATION_MS = 20000;
    const uint8_t PULSE_MIN_BRIGHTNESS = 80;   // Più basso per un pulse più evidente
    const uint8_t PULSE_MAX_BRIGHTNESS = 255;  // Full brightness
    const int PULSE_TRANSITION_STEPS = 100;    // Più steps per un pulse più smooth
    
    unsigned long start_time = esp_timer_get_time() / 1000;
    
    while ((esp_timer_get_time() / 1000) - start_time < PULSE_DURATION_MS) {
        // Pulse up super smooth
        for (int step = 0; step <= PULSE_TRANSITION_STEPS; step++) {
            float ratio = (float)step / PULSE_TRANSITION_STEPS;
            // Uso di sin per una transizione più smooth
            float smooth_ratio = (sinf(ratio * M_PI - M_PI/2) + 1) / 2;
            uint8_t intensity = PULSE_MIN_BRIGHTNESS + 
                              (uint8_t)((PULSE_MAX_BRIGHTNESS - PULSE_MIN_BRIGHTNESS) * smooth_ratio);
            
            for (int y = 0; y < MATRIX_ROWS; y++) {
                for (int x = 0; x < MATRIX_COLS; x++) {
                    if (framebuffer[y][x].b > 0) {
                        rgb_color_t curr_color = {.r = 0, .g = 0, .b = intensity};
                        matrix_set_pixel(x, y, curr_color);
                    }
                }
            }
            matrix_show();
            vTaskDelay(pdMS_TO_TICKS(PULSE_DELAY_MS/2));  // Delay più corto per smoothness
        }
        
        // Pulse down super smooth
        for (int step = PULSE_TRANSITION_STEPS; step >= 0; step--) {
            float ratio = (float)step / PULSE_TRANSITION_STEPS;
            float smooth_ratio = (sinf(ratio * M_PI - M_PI/2) + 1) / 2;
            uint8_t intensity = PULSE_MIN_BRIGHTNESS + 
                              (uint8_t)((PULSE_MAX_BRIGHTNESS - PULSE_MIN_BRIGHTNESS) * smooth_ratio);
            
            for (int y = 0; y < MATRIX_ROWS; y++) {
                for (int x = 0; x < MATRIX_COLS; x++) {
                    if (framebuffer[y][x].b > 0) {
                        rgb_color_t curr_color = {.r = 0, .g = 0, .b = intensity};
                        matrix_set_pixel(x, y, curr_color);
                    }
                }
            }
            matrix_show();
            vTaskDelay(pdMS_TO_TICKS(PULSE_DELAY_MS/2));
        }
    }
    
    // Reset finale più veloce
    for (int step = FADE_STEPS; step >= 0; step--) {
        for (int y = 0; y < MATRIX_ROWS; y++) {
            for (int x = 0; x < MATRIX_COLS; x++) {
                if (framebuffer[y][x].b > 0) {
                    uint8_t intensity = (step * NORMAL_BRIGHTNESS) / FADE_STEPS;
                    rgb_color_t curr_color = {.r = 0, .g = 0, .b = intensity};
                    matrix_set_pixel(x, y, curr_color);
                }
            }
        }
        matrix_show();
        vTaskDelay(pdMS_TO_TICKS(FADE_DELAY_MS/2));
    }
    
    matrix_clear();
    matrix_show();
    
    ESP_LOGI(TAG, "Dream animation complete");
    if (animation_events) {
        // Clear animation flag first
        xEventGroupClearBits(animation_events, ANIMATION_IN_PROGRESS_BIT);
        vTaskDelay(pdMS_TO_TICKS(50)); // Small delay to ensure state change
        // Set generation needed flag
        xEventGroupSetBits(animation_events, GENERATION_NEEDED_BIT);
    };
}

void pause_animations(void) {
    animation_enabled = false;
    if (animation_events) {
        // Wait for any current animation to complete or interrupt
        xEventGroupWaitBits(animation_events, ANIMATION_IN_PROGRESS_BIT, 
                           pdTRUE, pdTRUE, pdMS_TO_TICKS(1000));
    }
    matrix_clear();
    matrix_show();
}

void resume_animations(void) {
    animation_enabled = true;
}