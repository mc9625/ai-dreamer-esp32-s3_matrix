#ifndef LED_MATRIX_H
#define LED_MATRIX_H

#include "esp_err.h"
#include <stdint.h>

// Configuration for WS2812 RGB LED Matrix
#define LED_MATRIX_WIDTH 8
#define LED_MATRIX_HEIGHT 8
#define LED_PIN 4  // GPIO pin for data
#define LED_CHANNEL 0

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} rgb_color_t;

esp_err_t led_matrix_init(void);
void led_matrix_set_pixel(uint8_t x, uint8_t y, rgb_color_t color);
void led_matrix_clear(void);
void led_matrix_show(void);

// Text scrolling functions
void led_matrix_scroll_text(const char* text, rgb_color_t color, int scroll_speed_ms);
void led_matrix_stop_scroll(void);

#endif