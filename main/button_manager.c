#include "button_manager.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "BUTTON_MANAGER";
static TaskHandle_t button_task_handle = NULL;
static button_callback_t button_cb = NULL;

static void button_monitor_task(void *pvParameters) {
    while(1) {
        if (gpio_get_level(BOOT_BUTTON_PIN) == 0) {  // Button pressed
            vTaskDelay(pdMS_TO_TICKS(50));  // Debounce
            
            if (gpio_get_level(BOOT_BUTTON_PIN) == 0) {
                ESP_LOGI(TAG, "Button press confirmed");
                
                if (button_cb) {
                    button_cb();
                }
                
                // Wait for button release
                while(gpio_get_level(BOOT_BUTTON_PIN) == 0) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

esp_err_t button_manager_init(button_callback_t button_pressed_cb) {
    if (button_task_handle != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BOOT_BUTTON_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    
    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) return ret;

    button_cb = button_pressed_cb;
    
    BaseType_t task_ret = xTaskCreate(button_monitor_task, "button_monitor", 
                                     4096, NULL, tskIDLE_PRIORITY + 3, 
                                     &button_task_handle);
    
    return (task_ret == pdPASS) ? ESP_OK : ESP_FAIL;
}

esp_err_t button_manager_stop(void) {
    if (button_task_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    vTaskDelete(button_task_handle);
    button_task_handle = NULL;
    return ESP_OK;
}