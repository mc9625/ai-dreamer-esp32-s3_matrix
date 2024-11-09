#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/dns.h"
#include "esp_http_server.h"
#include "esp_mac.h"
#include "captive_portal.h"
#include "esp_log.h"
#include "lwip/apps/netbiosns.h"
#include "esp_netif_types.h"
#include "esp_netif.h"

static const char *TAG = "CAPTIVE_PORTAL";

// Global variables
char llm_output_buffer[MAX_LLM_OUTPUT] = {0};
size_t llm_output_len = 0;
static httpd_handle_t server = NULL;

// Configure DHCP with Apple's requirements
static esp_err_t configure_dhcp(esp_netif_t *netif) {
    ESP_LOGI(TAG, "Configuring DHCP server...");
    
    // Stop DHCP server temporarily
    ESP_ERROR_CHECK(esp_netif_dhcps_stop(netif));
    
    // Set DNS server address
    esp_netif_dns_info_t dns_info = {
        .ip.u_addr.ip4.addr = esp_netif_ip4_makeu32(192, 168, 4, 1)
    };
    dns_info.ip.type = ESP_IPADDR_TYPE_V4;
    ESP_ERROR_CHECK(esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns_info));

    // Enable DNS option
    esp_netif_dhcp_option_mode_t opt_mode = ESP_NETIF_OP_SET;
    esp_netif_dhcp_option_id_t opt_id = ESP_NETIF_DOMAIN_NAME_SERVER;
    uint8_t enable = 1;
    ESP_ERROR_CHECK(esp_netif_dhcps_option(netif, opt_mode, opt_id, &enable, sizeof(enable)));
    
    // Set Option 114 (Captive Portal URL) using the correct identifier
    const char *captive_url = "http://192.168.4.1/portal";
    ESP_ERROR_CHECK(esp_netif_dhcps_option(netif, opt_mode, ESP_NETIF_CAPTIVEPORTAL_URI, 
                                          (void*)captive_url, strlen(captive_url)));
    
    // Configure IP range
    esp_netif_ip_info_t ip_info = {
        .ip.addr = esp_netif_ip4_makeu32(192, 168, 4, 1),
        .netmask.addr = esp_netif_ip4_makeu32(255, 255, 255, 0),
        .gw.addr = esp_netif_ip4_makeu32(192, 168, 4, 1)
    };
    ESP_ERROR_CHECK(esp_netif_set_ip_info(netif, &ip_info));
    
    // Start DHCP server with new configuration
    ESP_ERROR_CHECK(esp_netif_dhcps_start(netif));
    
    ESP_LOGI(TAG, "DHCP server configured successfully");
    return ESP_OK;
}

// Function to check if a request is from Apple CNA
static bool is_captive_portal_check(httpd_req_t *req) {
    const char *uri = req->uri;
    return (strstr(uri, "/hotspot-detect.html") != NULL ||
            strstr(uri, "/generate_204") != NULL ||
            strstr(uri, "/connecttest.txt") != NULL ||
            strstr(uri, "/redirect") != NULL ||
            strstr(uri, "/success.txt") != NULL ||
            strstr(uri, "/ncsi.txt") != NULL ||
            strstr(uri, "/fwlink/") != NULL);
}

// Root handler for all requests
static esp_err_t root_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "Received request for URI: %s", req->uri);
    
    // Check user agent for Apple devices
    char *user_agent = NULL;
    size_t ua_len = httpd_req_get_hdr_value_len(req, "User-Agent");
    if (ua_len > 0) {
        user_agent = malloc(ua_len + 1);
        if (user_agent) {
            httpd_req_get_hdr_value_str(req, "User-Agent", user_agent, ua_len + 1);
            ESP_LOGI(TAG, "User Agent: %s", user_agent);
        }
    }

    // Special handling for Apple CNA requests
    if (is_captive_portal_check(req)) {
        ESP_LOGI(TAG, "Captive portal check detected");
        
        // For Apple devices, return a 200 OK with specific content
        if (user_agent && strstr(user_agent, "CaptiveNetworkSupport")) {
            free(user_agent);
            httpd_resp_set_status(req, "200 OK");
            httpd_resp_set_type(req, "text/html");
            const char *success_response = "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>";
            httpd_resp_send(req, success_response, strlen(success_response));
            return ESP_OK;
        }
        
        free(user_agent);
        // For other devices, redirect to portal
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/portal");
        httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    free(user_agent);

    // For all other requests, serve the main page
    const char* html_template = 
        "<!DOCTYPE html>"
        "<html>"
        "<head>"
        "<meta name='viewport' content='width=device-width, initial-scale=1'>"
        "<title>AI Dreamer Output</title>"
        "<style>"
        "body{font-family:Arial,sans-serif;margin:20px;line-height:1.6;background:#f0f0f0;}"
        ".container{max-width:800px;margin:0 auto;background:#fff;padding:20px;border-radius:8px;box-shadow:0 2px 4px rgba(0,0,0,0.1);}"
        "pre{white-space:pre-wrap;word-wrap:break-word;background:#f9f9f9;padding:15px;border-radius:4px;border:1px solid #ddd;}"
        "h1{color:#333;text-align:center;}"
        "</style>"
        "</head>"
        "<body>"
        "<div class='container'>"
        "<h1>AI Dreamer Output</h1>"
        "<pre>%s</pre>"
        "</div>"
        "</body>"
        "</html>";

    char *response = malloc(strlen(html_template) + strlen(llm_output_buffer) + 1);
    if (response == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for response");
        return ESP_ERR_NO_MEM;
    }

    sprintf(response, html_template, llm_output_buffer);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    httpd_resp_send(req, response, strlen(response));
    free(response);
    
    return ESP_OK;
}

esp_err_t captive_portal_init(esp_netif_t *ap_netif) {
    ESP_LOGI(TAG, "Initializing captive portal");

    // Configure DNS
    ip_addr_t dns_addr;
    dns_addr.type = IPADDR_TYPE_V4;
    dns_addr.u_addr.ip4.addr = esp_netif_htonl(esp_netif_ip4_makeu32(192, 168, 4, 1));
    dns_setserver(0, &dns_addr);

    // Start NetBIOS
    netbiosns_init();
    netbiosns_set_name("espressif");

    // Configure and start HTTP server
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 8;
    config.stack_size = 8192;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.lru_purge_enable = true;

    esp_err_t ret = httpd_start(&server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return ret;
    }

    // Register URI handler for all paths
    httpd_uri_t uri_catch_all = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = root_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &uri_catch_all);

    ESP_LOGI(TAG, "Captive portal initialized successfully");
    return ESP_OK;
}

void captive_portal_set_llm_output(const char* text) {
    if (text) {
        size_t len = strlen(text);
        if (len >= MAX_LLM_OUTPUT) {
            len = MAX_LLM_OUTPUT - 1;
        }
        memcpy(llm_output_buffer, text, len);
        llm_output_buffer[len] = '\0';
        llm_output_len = len;
    }
}