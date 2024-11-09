#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/dns.h"
#include "esp_http_server.h"
#include "esp_mac.h"
#include "captive_portal.h"
#include "esp_log.h" 


static const char *TAG = "CAPTIVE_PORTAL";

// Global variables
char llm_output_buffer[MAX_LLM_OUTPUT] = {0};
size_t llm_output_len = 0;
static httpd_handle_t server = NULL;

// DNS query handler// DNS query handler
static void dns_query_handler(const char *name, ip_addr_t *addr) {
    addr->u_addr.ip4.addr = ipaddr_addr("192.168.4.1");
    ESP_LOGI(TAG, "DNS Query for %s -> redirecting to captive portal", name);
}

static void init_dns_redirect(void)
{
    ip_addr_t resolve_ip;
    resolve_ip.type = IPADDR_TYPE_V4;
    resolve_ip.u_addr.ip4.addr = ipaddr_addr("192.168.4.1");
    
    // Configura il DNS server per reindirizzare tutte le richieste al nostro IP
    dns_setserver(0, &resolve_ip);
    
    // Aggiungi più indirizzi DNS per intercettare tutte le richieste
    const ip_addr_t *dns_ip = dns_getserver(0);
    dns_init();
    
    ESP_LOGI(TAG, "DNS server configured at " IPSTR, IP2STR(&dns_ip->u_addr.ip4));
}

// HTML template
static const char* HTML_TEMPLATE = R"(
<!DOCTYPE html>
<html>
<head>
    <title>ESP32 LLM Output</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        body { font-family: Arial, sans-serif; margin: 20px; line-height: 1.6; }
        .container { max-width: 800px; margin: 0 auto; background: #f9f9f9; padding: 20px; border-radius: 8px; }
        pre { white-space: pre-wrap; word-wrap: break-word; background: #fff; padding: 15px; border-radius: 4px; }
    </style>
</head>
<body>
    <div class="container">
        <h1>ESP32 LLM Output</h1>
        <pre>%s</pre>
    </div>
</body>
</html>
)";

// HTTP handlers
static esp_err_t root_handler(httpd_req_t *req) {
    // Controlla se è una richiesta di verifica captive portal
    if (strstr(req->uri, "/hotspot-detect.html") != NULL ||
        strstr(req->uri, "/generate_204") != NULL ||
        strstr(req->uri, "/connecttest.txt") != NULL ||
        strstr(req->uri, "/redirect") != NULL ||
        strstr(req->uri, "/success.txt") != NULL) {
        
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/portal");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    // Pagina principale con l'output dell'LLM
    const char* html_start = 
        "<html><head>"
        "<meta name='viewport' content='width=device-width, initial-scale=1'>"
        "<style>"
        "body{font-family:Arial,sans-serif;margin:20px;line-height:1.6;}"
        ".container{max-width:800px;margin:0 auto;background:#f9f9f9;padding:20px;border-radius:8px;}"
        "pre{white-space:pre-wrap;word-wrap:break-word;background:#fff;padding:15px;border-radius:4px;}"
        "</style></head><body><div class='container'><h1>AI Dreamer Output</h1><pre>";
    
    const char* html_end = "</pre></div></body></html>";

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send_chunk(req, html_start, strlen(html_start));
    httpd_resp_send_chunk(req, llm_output_buffer, strlen(llm_output_buffer));
    httpd_resp_send_chunk(req, html_end, strlen(html_end));
    httpd_resp_send_chunk(req, NULL, 0);
    
    return ESP_OK;
}


static esp_err_t apple_captive_handler(httpd_req_t *req) {
    const char *response = "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>";
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, response, strlen(response));
    return ESP_OK;
}

// Initialize HTTP server
static esp_err_t start_webserver(void)
{
    if (server != NULL) {
        ESP_LOGW(TAG, "Web server already started");
        return ESP_OK;
    }
    
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 4;
    config.stack_size = 8192;
    
    esp_err_t ret = httpd_start(&server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start web server (error %d)", ret);
        return ret;
    }

    httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &root);

    httpd_uri_t captive = {
        .uri = "/hotspot-detect.html",
        .method = HTTP_GET,
        .handler = apple_captive_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &captive);

    return ESP_OK;
}

// Initialize DNS server
static void start_dns_server(void) {
    ip_addr_t resolve_ip;
    IP_ADDR4(&resolve_ip, 192, 168, 4, 1);
    
    dns_init();
    dns_setserver(0, &resolve_ip);
    ESP_LOGI(TAG, "DNS server initialized");
}

// Public functions
esp_err_t captive_portal_init(void)
{
    ESP_LOGI(TAG, "Initializing captive portal");

    // Inizializza il reindirizzamento DNS
    init_dns_redirect();
    
    // Configura il server web
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 8;
    config.stack_size = 8192;
    config.lru_purge_enable = true;
    config.core_id = 0;  // Esegui sul core 0
    
    httpd_handle_t server = NULL;
    esp_err_t ret = httpd_start(&server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start web server");
        return ret;
    }

    // Registra i gestori URI
    httpd_uri_t root = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = root_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &root);

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