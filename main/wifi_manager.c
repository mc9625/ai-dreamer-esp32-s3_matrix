#include "wifi_manager.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_event.h"
#include "lwip/dns.h"
#include "esp_mac.h"

static const char *TAG = "WIFI_MANAGER";

static wifi_state_t wifi_state = WIFI_STATE_OFF;
static esp_netif_t *wifi_netif = NULL;

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                             int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_AP_START:
                wifi_state = WIFI_STATE_ON;
                ESP_LOGI(TAG, "AP Started");
                break;
                
            case WIFI_EVENT_AP_STOP:
                wifi_state = WIFI_STATE_OFF;
                ESP_LOGI(TAG, "AP Stopped");
                break;
                
            case WIFI_EVENT_AP_STACONNECTED: {
                wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
                ESP_LOGI(TAG, "Station connected, MAC: " MACSTR ", AID: %d",
                        MAC2STR(event->mac), event->aid);
                wifi_state = WIFI_STATE_CLIENT_CONNECTED;
                break;
            }
            
            case WIFI_EVENT_AP_STADISCONNECTED: {
                wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
                ESP_LOGI(TAG, "Station disconnected, MAC: " MACSTR ", AID: %d",
                        MAC2STR(event->mac), event->aid);
                wifi_state = WIFI_STATE_ON;
                break;
            }
        }
    }
}

esp_err_t wifi_manager_init(void)
{
    // Create default AP
    wifi_netif = esp_netif_create_default_wifi_ap();
    if (!wifi_netif) {
        ESP_LOGE(TAG, "Failed to create netif");
        return ESP_FAIL;
    }

    // Initialize WiFi
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Register event handler
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                      ESP_EVENT_ANY_ID,
                                                      &wifi_event_handler,
                                                      NULL,
                                                      NULL));
    return ESP_OK;
}

esp_err_t wifi_manager_start(void)
{
    if (wifi_state != WIFI_STATE_OFF) {
        return ESP_OK;
    }

    // Configure AP
    wifi_config_t wifi_config = {
        .ap = {
            .ssid = "ESP32-AI-DREAMER",
            .ssid_len = strlen("ESP32-AI-DREAMER"),
            .channel = 1,
            .password = "",
            .max_connection = 4,
            .authmode = WIFI_AUTH_OPEN,
            .pmf_cfg = {
                .required = false
            },
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Configure static IP and DNS
    esp_netif_ip_info_t ip_info = {0};
    IP4_ADDR(&ip_info.ip, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.gw, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);

    ESP_ERROR_CHECK(esp_netif_dhcps_stop(wifi_netif));
    ESP_ERROR_CHECK(esp_netif_set_ip_info(wifi_netif, &ip_info));

    // Configure DNS
    esp_netif_dns_info_t dns_info = {0};
    IP4_ADDR(&dns_info.ip.u_addr.ip4, 192, 168, 4, 1);
    dns_info.ip.type = IPADDR_TYPE_V4;
    ESP_ERROR_CHECK(esp_netif_set_dns_info(wifi_netif, ESP_NETIF_DNS_MAIN, &dns_info));

    // Restart DHCP server
    ESP_ERROR_CHECK(esp_netif_dhcps_start(wifi_netif));

    ESP_LOGI(TAG, "WiFi AP started with IP: 192.168.4.1");
    return ESP_OK;
}

esp_err_t wifi_manager_stop(void)
{
    if (wifi_state == WIFI_STATE_OFF) {
        return ESP_OK;
    }

    ESP_ERROR_CHECK(esp_wifi_stop());
    wifi_state = WIFI_STATE_OFF;
    ESP_LOGI(TAG, "WiFi AP stopped");
    return ESP_OK;
}

wifi_state_t wifi_manager_get_state(void)
{
    return wifi_state;
}

esp_netif_t* wifi_manager_get_netif(void)
{
    return wifi_netif;
}