#ifndef WS_MATRIX_H
#define WS_MATRIX_H

#include <stdint.h>
#include <stdbool.h>
#include "driver/gpio.h"
#include "driver/rmt.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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

#define FADE_MAX_INTENSITY 255  
#define FLASH_INTENSITY 255    
#define FLASH_DURATION_MS 100 

// Pattern initialization constants
#define MIN_INITIAL_NODES 20
#define MAX_INITIAL_NODES 40
#define MIN(a, b) ((a) < (b) ? (a) : (b))

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
void matrix_pattern_task(void *pvParameters);  // Nuovo task separato
void activate_new_node_task(void *arg);

#endif /* WS_MATRIX_H */