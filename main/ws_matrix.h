#ifndef WS_MATRIX_H
#define WS_MATRIX_H

#include "driver/rmt.h"  // Torniamo al driver legacy
#include "driver/gpio.h"
#include "esp_log.h"
#include <stdint.h>

// Matrix configuration
#define RGB_CONTROL_PIN GPIO_NUM_14
#define MATRIX_ROWS      8
#define MATRIX_COLS      8
#define RGB_COUNT        64

// Animation and brightness settings
#define DEFAULT_BRIGHTNESS 40
#define MAX_BRIGHTNESS 120
#define FADE_STEPS 40
#define FADE_DELAY_MS 30
#define NORMAL_BRIGHTNESS 60

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} rgb_color_t;

esp_err_t matrix_init(void);
void matrix_set_pixel(uint8_t x, uint8_t y, rgb_color_t color);
void matrix_set_brightness(uint8_t brightness);
void matrix_show(void);
void matrix_clear(void);
void test_matrix(void);
void fade_in_single_pixel(uint8_t x, uint8_t y, rgb_color_t color);
void activate_new_node_task(void *arg);

#endif