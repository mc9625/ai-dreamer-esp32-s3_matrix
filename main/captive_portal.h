#ifndef CAPTIVE_PORTAL_H
#define CAPTIVE_PORTAL_H

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/dns.h"
#include <string.h>

#define MAX_LLM_OUTPUT 8192

// Declare buffer as extern
extern char llm_output_buffer[MAX_LLM_OUTPUT];
extern size_t llm_output_len;

esp_err_t captive_portal_init(esp_netif_t *ap_netif);
void captive_portal_set_llm_output(const char* text);

#endif