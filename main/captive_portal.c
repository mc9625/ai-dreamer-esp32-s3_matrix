#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "lwip/dns.h"
#include "lwip/err.h"
#include "lwip/pbuf.h"
#include "lwip/udp.h"
#include "esp_netif.h"
#include "captive_portal.h"

static const char *TAG = "CAPTIVE_PORTAL";

// Global state
static struct udp_pcb *dns_pcb = NULL;
static httpd_handle_t http_server = NULL;
char llm_output_buffer[MAX_LLM_OUTPUT] = {0};
size_t llm_output_len = 0;

// DNS Constants
#define DNS_PORT                53
#define DNS_MAX_LEN            512
#define DNS_QR_RESPONSE        0x8000
#define DNS_AA_FLAG            0x0400
#define DNS_TTL               300

// DNS Structures
typedef struct __attribute__((__packed__)) {
    uint16_t id;
    uint16_t flags;
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t nscount;
    uint16_t arcount;
} dns_header_t;

typedef struct __attribute__((__packed__)) {
    uint16_t type;
    uint16_t class;
} dns_question_t;

typedef struct __attribute__((__packed__)) {
    uint16_t ptr;
    uint16_t type;
    uint16_t class;
    uint32_t ttl;
    uint16_t rdlength;
    uint32_t rdata;
} dns_answer_t;

// DNS server callback
static void dns_recv_callback(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                            const ip_addr_t *addr, u16_t port) {
    if (!p || p->len < sizeof(dns_header_t)) {
        if (p) pbuf_free(p);
        return;
    }

    ESP_LOGI(TAG, "DNS request from " IPSTR, IP2STR(&addr->u_addr.ip4));

    // Create response buffer
    struct pbuf *resp = pbuf_alloc(PBUF_TRANSPORT, DNS_MAX_LEN, PBUF_RAM);
    if (!resp) {
        pbuf_free(p);
        return;
    }

    // Copy and modify DNS header
    dns_header_t *qhdr = (dns_header_t *)p->payload;
    dns_header_t *rhdr = (dns_header_t *)resp->payload;
    
    rhdr->id = qhdr->id;
    rhdr->flags = htons(DNS_QR_RESPONSE | DNS_AA_FLAG);
    rhdr->qdcount = qhdr->qdcount;
    rhdr->ancount = htons(1);
    rhdr->nscount = 0;
    rhdr->arcount = 0;

    // Copy question section
    size_t question_len = p->len - sizeof(dns_header_t);
    memcpy((uint8_t*)resp->payload + sizeof(dns_header_t),
           (uint8_t*)p->payload + sizeof(dns_header_t),
           question_len);

    // Add answer section
    dns_answer_t *answer = (dns_answer_t *)((uint8_t*)resp->payload + sizeof(dns_header_t) + question_len);
    answer->ptr = htons(0xC00C);  // Compression pointer to question
    answer->type = htons(1);      // A record
    answer->class = htons(1);     // IN class
    answer->ttl = htonl(DNS_TTL);
    answer->rdlength = htons(4);  // IPv4 = 4 bytes
    answer->rdata = htonl(0xC0A80401);  // 192.168.4.1

    size_t total_len = sizeof(dns_header_t) + question_len + sizeof(dns_answer_t);
    pbuf_realloc(resp, total_len);

    udp_sendto(pcb, resp, addr, port);
    pbuf_free(resp);
    pbuf_free(p);
}

// HTTP handler
static esp_err_t http_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "HTTP Request: %s", req->uri);

    // Root page or detection endpoints
    if (strcmp(req->uri, "/") == 0 ||
        strcmp(req->uri, "/generate_204") == 0 ||
        strcmp(req->uri, "/hotspot-detect.html") == 0 ||
        strcmp(req->uri, "/connecttest.txt") == 0) {
        
        const char *html_template = 
            "<!DOCTYPE html><html><head>"
            "<meta name='viewport' content='width=device-width,initial-scale=1'>"
            "<meta http-equiv='refresh' content='5'>"
            "<style>"
            "body{font-family:system-ui;margin:20px;line-height:1.6;background:#f0f0f0}"
            ".container{max-width:800px;margin:0 auto;background:#fff;padding:20px;"
            "border-radius:8px;box-shadow:0 2px 4px rgba(0,0,0,0.1)}"
            "pre{white-space:pre-wrap;background:#f9f9f9;padding:15px;"
            "border-radius:4px;border:1px solid #ddd}"
            "h1{color:#333;text-align:center}"
            "</style></head>"
            "<body><div class='container'>"
            "<h1>AI Dreamer Output</h1>"
            "<pre>%s</pre>"
            "</div></body></html>";

        char *response = malloc(strlen(html_template) + strlen(llm_output_buffer) + 1);
        if (!response) return ESP_ERR_NO_MEM;

        sprintf(response, html_template, llm_output_buffer);
        httpd_resp_set_type(req, "text/html");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
        esp_err_t res = httpd_resp_send(req, response, strlen(response));
        free(response);
        return res;
    }

    // Redirect other requests
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, NULL, 0);
}

// DNS server setup
static esp_err_t start_dns_server(void) {
    dns_pcb = udp_new();
    if (!dns_pcb) return ESP_FAIL;

    if (udp_bind(dns_pcb, IP_ADDR_ANY, DNS_PORT) != ERR_OK) {
        udp_remove(dns_pcb);
        return ESP_FAIL;
    }

    udp_recv(dns_pcb, dns_recv_callback, NULL);
    return ESP_OK;
}

// HTTP server setup
static esp_err_t start_http_server(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 8;
    config.stack_size = 8192;
    config.uri_match_fn = httpd_uri_match_wildcard;

    if (httpd_start(&http_server, &config) != ESP_OK) {
        return ESP_FAIL;
    }

    httpd_uri_t uri_handler = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = http_handler,
        .user_ctx = NULL
    };

    if (httpd_register_uri_handler(http_server, &uri_handler) != ESP_OK) {
        httpd_stop(http_server);
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t captive_portal_init(esp_netif_t *ap_netif) {
    ESP_LOGI(TAG, "Initializing captive portal");

    // Configure network
    esp_netif_ip_info_t ip_info = {
        .ip.addr = ESP_IP4TOADDR(192, 168, 4, 1),
        .gw.addr = ESP_IP4TOADDR(192, 168, 4, 1),
        .netmask.addr = ESP_IP4TOADDR(255, 255, 255, 0)
    };

    ESP_ERROR_CHECK(esp_netif_dhcps_stop(ap_netif));
    ESP_ERROR_CHECK(esp_netif_set_ip_info(ap_netif, &ip_info));

    // Configure DNS info
    esp_netif_dns_info_t dns_info = {
        .ip.u_addr.ip4.addr = ip_info.ip.addr,
        .ip.type = ESP_IPADDR_TYPE_V4
    };

    // Set DNS server address
    ESP_ERROR_CHECK(esp_netif_set_dns_info(ap_netif, ESP_NETIF_DNS_MAIN, &dns_info));
    
    // Enable DNS option in DHCP
    uint8_t dns_offer = 1;
    ESP_ERROR_CHECK(esp_netif_dhcps_option(
        ap_netif,
        ESP_NETIF_OP_SET,
        ESP_NETIF_DOMAIN_NAME_SERVER,
        &dns_offer,
        sizeof(dns_offer)
    ));

    ESP_ERROR_CHECK(esp_netif_dhcps_start(ap_netif));
    ESP_ERROR_CHECK(start_dns_server());
    ESP_ERROR_CHECK(start_http_server());

    return ESP_OK;
}

void captive_portal_set_llm_output(const char* text) {
    if (!text) return;
    
    size_t len = strlen(text);
    if (len >= MAX_LLM_OUTPUT) {
        len = MAX_LLM_OUTPUT - 1;
    }
    memcpy(llm_output_buffer, text, len);
    llm_output_buffer[len] = '\0';
    llm_output_len = len;
}