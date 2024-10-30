#include "ws_matrix.h"
#include <string.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define DEFAULT_BRIGHTNESS 40  // Safe default brightness level
#define MAX_BRIGHTNESS 60     // Maximum safe brightness limit
#define FADE_STEPS 20
#define FADE_DELAY_MS 30
#define FLASH_BRIGHTNESS 180
#define NORMAL_BRIGHTNESS 60

static const char* TAG = "WS_MATRIX";

// WS2812B timing (in RMT ticks, 1 tick = 25ns with clock divider of 2)
#define RMT_CLK_DIV 2
#define T0H 12     // 0.4us
#define T0L 28     // 0.85us  
#define T1H 24     // 0.8us
#define T1L 16     // 0.45us

static rmt_channel_t rmt_channel = RMT_CHANNEL_0;
static rgb_color_t framebuffer[MATRIX_ROWS][MATRIX_COLS] = {0};
static uint8_t brightness = 40;

static void fade_in_pixel(uint8_t x, uint8_t y, rgb_color_t color) {
    rgb_color_t temp_color = color;
    
    // Fade in
    for(int i = 0; i <= FADE_STEPS; i++) {
        brightness = (i * NORMAL_BRIGHTNESS) / FADE_STEPS;
        matrix_set_pixel(x, y, temp_color);
        matrix_show();
        vTaskDelay(pdMS_TO_TICKS(FADE_DELAY_MS));
    }
    
    // Flash white
    brightness = FLASH_BRIGHTNESS;
    temp_color.r = temp_color.g = temp_color.b = 255;
    matrix_set_pixel(x, y, temp_color);
    matrix_show();
    vTaskDelay(pdMS_TO_TICKS(50));
    
    // Return to blue
    brightness = NORMAL_BRIGHTNESS;
    matrix_set_pixel(x, y, color);
    matrix_show();
}

esp_err_t matrix_init(void) {
    ESP_LOGI(TAG, "Initializing LED matrix...");
    
    gpio_reset_pin(RGB_CONTROL_PIN);
    gpio_set_direction(RGB_CONTROL_PIN, GPIO_MODE_OUTPUT);
    
    rmt_config_t config = RMT_DEFAULT_CONFIG_TX(RGB_CONTROL_PIN, rmt_channel);
    config.clk_div = RMT_CLK_DIV;
    config.mem_block_num = 1;
    
    ESP_ERROR_CHECK(rmt_config(&config));
    ESP_ERROR_CHECK(rmt_driver_install(config.channel, 0, 0));
    
    // Reset all LEDs
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
        }
    }
    matrix_show();
}

void matrix_set_brightness(uint8_t new_brightness) {
    brightness = (new_brightness > MAX_BRIGHTNESS) ? MAX_BRIGHTNESS : new_brightness;
}

uint32_t matrix_color(uint8_t r, uint8_t g, uint8_t b) {
    return (uint32_t)(r << 16) | (g << 8) | b;
}



void test_matrix(void) {
    ESP_LOGI(TAG, "Starting matrix test");
    rgb_color_t blue = {.r = 0, .g = 0, .b = 60};
    
    matrix_clear();
    vTaskDelay(pdMS_TO_TICKS(500));
    
    fade_in_pixel(0, 0, blue);
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    matrix_clear();
}