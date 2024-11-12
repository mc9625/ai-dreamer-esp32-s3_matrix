#ifndef CAPTIVE_PORTAL_H
#define CAPTIVE_PORTAL_H

#include "esp_netif.h"
#include <stddef.h>

// Configurazione debug - commentare per disabilitare
#define CP_DEBUG_MODE
#define CP_TRACE_DNS
#define CP_TRACE_HTTP
#define CP_TRACE_MEMORY

// Buffer per l'output LLM
#define MAX_LLM_OUTPUT 8192
extern char llm_output_buffer[MAX_LLM_OUTPUT];
extern size_t llm_output_len;

// Funzioni pubbliche
esp_err_t captive_portal_init(esp_netif_t *ap_netif);
void captive_portal_set_llm_output(const char* text);

#endif // CAPTIVE_PORTAL_H