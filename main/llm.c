/* Inference for Llama-2 Transformer model in pure C */
/**
 * Original author of this:
 * https://github.com/karpathy/llama2.c 
 * 
 * Slight modifications added to make it ESP32 friendly
 */

#include "llm.h"
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <time.h>
#include <math.h>
#include <string.h>
#include <fcntl.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_dsp.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "captive_portal.h"

#define MAP_FAILED NULL
#define munmap(ptr, length) custom_munmap(ptr)
#define close(fd) custom_close(fd)

#define TASK_0_BIT (1 << 0)
#define TASK_1_BIT (1 << 1)
#define FORWARD_TASK_1 (1 << 2)
#define FORWARD_TASK_2 (1 << 3)
#define READY_BIT (1 << 3)
#define ALL_SYNC_BITS (TASK_0_BIT | TASK_1_BIT)
#define ALL_FORWARD_TASKS (FORWARD_TASK_1 | FORWARD_TASK_2)

#define DISABLE_DSP_OPTIMIZATIONS

static char output_buffer[MAX_LLM_OUTPUT] = {0};
static size_t output_pos = 0;

v4sf random_f32(unsigned long long *state);

#include "esp_log.h"

typedef struct
{
    v4sf *xout;
    v4sf *x;
    v4sf *w;
    int start;
    int end;
    int n;
    int d;
    int task_num;
} MatMulTaskParams;

typedef struct
{
    RunState *s;
    TransformerWeights *w;
    Config *p;
    int pos;
    int start;
    int loff;
    int end;
    int dim;
    int kv_dim;
    int kv_mul;
    int hidden_dim;
    int head_size;
    int task_num;
} ForwardTaskParams;

EventGroupHandle_t xEventGroup;
EventGroupHandle_t ForwardEventGroup;

static const char *TAG = "LLM";
TaskHandle_t handle_forward_task = NULL;
TaskHandle_t matmul_task_2 = NULL;

ForwardTaskParams *forward_params = NULL;
MatMulTaskParams *matmul_params = NULL;

SemaphoreHandle_t semaDataReady;
SemaphoreHandle_t semaForwardDataReady;


void matmul_task(void *params);
void forward_task(void *params);

void custom_munmap(void *ptr)
{
    free(ptr);
}

int custom_close(int fd)
{
    // Since there are no actual file descriptors to close, simply return 0 (success)
    return 0;
}

void chat(Transformer *transformer, Tokenizer *tokenizer, Sampler *sampler,
          char *cli_user_prompt, char *cli_system_prompt, int steps);

void malloc_run_state(RunState *s, Config *p)
{
    // we calloc instead of malloc to keep valgrind happy
    int kv_dim = (p->dim * p->n_kv_heads) / p->n_heads;
    s->x = calloc(p->dim, sizeof(v4sf));
    s->xb = calloc(p->dim, sizeof(v4sf));
    s->xb2 = calloc(p->dim, sizeof(v4sf));
    s->hb = calloc(p->hidden_dim, sizeof(v4sf));
    s->hb2 = calloc(p->hidden_dim, sizeof(v4sf));
    s->q = calloc(p->dim, sizeof(v4sf));
    s->key_cache = calloc(p->n_layers * p->seq_len * kv_dim, sizeof(v4sf));
    s->value_cache = calloc(p->n_layers * p->seq_len * kv_dim, sizeof(v4sf));
    s->att = calloc(p->n_heads * p->seq_len, sizeof(v4sf));
    s->logits = calloc(p->vocab_size, sizeof(v4sf));
    // ensure all mallocs went fine
    if (!s->x || !s->xb || !s->xb2 || !s->hb || !s->hb2 || !s->q || !s->key_cache || !s->value_cache || !s->att || !s->logits)
    {
        fprintf(stderr, "malloc failed!\n");
        exit(EXIT_FAILURE);
    }
}

void free_run_state(RunState *s)
{
    free(s->x);
    free(s->xb);
    free(s->xb2);
    free(s->hb);
    free(s->hb2);
    free(s->q);
    free(s->att);
    free(s->logits);
    free(s->key_cache);
    free(s->value_cache);
}

void memory_map_weights(TransformerWeights *w, Config *p, v4sf *ptr, int shared_weights)
{
    int head_size = p->dim / p->n_heads;
    // make sure the multiplications below are done in 64bit to fit the parameter counts of 13B+ models
    unsigned long long n_layers = p->n_layers;
    w->token_embedding_table = ptr;
    ptr += p->vocab_size * p->dim;
    w->rms_att_weight = ptr;
    ptr += n_layers * p->dim;
    w->wq = ptr;
    ptr += n_layers * p->dim * (p->n_heads * head_size);
    w->wk = ptr;
    ptr += n_layers * p->dim * (p->n_kv_heads * head_size);
    w->wv = ptr;
    ptr += n_layers * p->dim * (p->n_kv_heads * head_size);
    w->wo = ptr;
    ptr += n_layers * (p->n_heads * head_size) * p->dim;
    w->rms_ffn_weight = ptr;
    ptr += n_layers * p->dim;
    w->w1 = ptr;
    ptr += n_layers * p->dim * p->hidden_dim;
    w->w2 = ptr;
    ptr += n_layers * p->hidden_dim * p->dim;
    w->w3 = ptr;
    ptr += n_layers * p->dim * p->hidden_dim;
    w->rms_final_weight = ptr;
    ptr += p->dim;
    ptr += p->seq_len * head_size / 2; // skip what used to be freq_cis_real (for RoPE)
    ptr += p->seq_len * head_size / 2; // skip what used to be freq_cis_imag (for RoPE)
    w->wcls = shared_weights ? w->token_embedding_table : ptr;
}

void read_checkpoint(char *checkpoint, Config *config, TransformerWeights *weights,
                     int *fd, v4sf **data, size_t *file_size)
{
    FILE *file = fopen(checkpoint, "rb");
    if (!file)
    {
        ESP_LOGE(TAG, "Couldn't open file %s", checkpoint);
        exit(EXIT_FAILURE);
    }
    // read in the config header
    if (fread(config, sizeof(Config), 1, file) != 1)
    {
        exit(EXIT_FAILURE);
    }
    // negative vocab size is hacky way of signaling unshared weights. bit yikes.
    int shared_weights = config->vocab_size > 0 ? 1 : 0;
    config->vocab_size = abs(config->vocab_size);
    ESP_LOGI(TAG, "Vocab size if %d", config->vocab_size);
    // figure out the file size
    fseek(file, 0, SEEK_END); // move file pointer to end of file
    *file_size = ftell(file); // get the file size, in bytes
    fseek(file, 0, SEEK_SET); // move back to beginning for reading
    ESP_LOGI(TAG, "File size: %zu bytes", *file_size);
    ESP_LOGI(TAG, "Free ram available: %lu", esp_get_free_heap_size());
    *data = malloc(*file_size);
    if (*data == NULL)
    {
        ESP_LOGE(TAG, "Malloc operation failed");
        exit(EXIT_FAILURE);
    }
    // Read the entire file into memory
    size_t bytes_read = fread(*data, 1, *file_size, file);
    if (bytes_read != *file_size)
    {
        ESP_LOGE(TAG, "Failed to read file into memory");
        ESP_LOGE(TAG, "Bytes read %zu bytes", bytes_read);
        exit(EXIT_FAILURE);
    }
    fclose(file);

    ESP_LOGI(TAG, "Successfully read LLM into memory");
    ESP_LOGI(TAG, "Free ram available: %lu", esp_get_free_heap_size());
    v4sf *weights_ptr = *data + sizeof(Config) / sizeof(v4sf);
    memory_map_weights(weights, config, weights_ptr, shared_weights);
    ESP_LOGI(TAG, "Successfully read checkpoint");
}

void build_transformer(Transformer *t, char *checkpoint_path)
{
    // read in the Config and the Weights from the checkpoint
    read_checkpoint(checkpoint_path, &t->config, &t->weights, &t->fd, &t->data, &t->file_size);
    // allocate the RunState buffers
    malloc_run_state(&t->state, &t->config);
    ESP_LOGI(TAG, "Transformer successfully built");

    // FreeRTos Tasks
    xEventGroup = xEventGroupCreate();
    ForwardEventGroup = xEventGroupCreate();
    semaDataReady = xSemaphoreCreateBinary();
    semaForwardDataReady = xSemaphoreCreateBinary();
    xSemaphoreGive(semaDataReady);
    xSemaphoreTake(semaDataReady, portMAX_DELAY);
    xSemaphoreGive(semaForwardDataReady);
    xSemaphoreTake(semaForwardDataReady, portMAX_DELAY);

    matmul_params = malloc(sizeof(MatMulTaskParams));
    forward_params = malloc(sizeof(ForwardTaskParams));
    xTaskCreatePinnedToCore(matmul_task, "MatMul2", 2048, matmul_params, 19, &matmul_task_2, 1);             // Run on Core 1
    xTaskCreatePinnedToCore(forward_task, "ForwardTask", 2048, forward_params, 19, &handle_forward_task, 1); // Run on Core 1
    ESP_LOGI(TAG, "Created FreeRTOS Tasks");
}

void free_transformer(Transformer *t)
{
    // close the memory mapping
    if (t->data != MAP_FAILED)
    {
        munmap(t->data, t->file_size);
    }
    if (t->fd != -1)
    {
        close(t->fd);
    }
    // free the RunState buffers
    free_run_state(&t->state);
}

// ----------------------------------------------------------------------------
// neural net blocks; the dynamics of the Transformer

void rmsnorm(v4sf *o, v4sf *x, v4sf *weight, int size)
{
    // calculate sum of squares
    v4sf ss = 0.0f;
    for (int j = 0; j < size; j++)
    {
        ss += x[j] * x[j];
    }
    ss /= size;
    ss += 1e-5f;
    ss = 1.0f / sqrtf(ss);
    // normalize and scale
    for (int j = 0; j < size; j++)
    {
        o[j] = weight[j] * (ss * x[j]);
    }
}

void softmax(v4sf *x, int size)
{
    // find max value (for numerical stability)
    v4sf max_val = x[0];
    for (int i = 1; i < size; i++)
    {
        if (x[i] > max_val)
        {
            max_val = x[i];
        }
    }
    // exp and sum
    v4sf sum = 0.0f;
    for (int i = 0; i < size; i++)
    {
        x[i] = expf(x[i] - max_val);
        sum += x[i];
    }
    // normalize
    for (int i = 0; i < size; i++)
    {
        x[i] /= sum;
    }
}

void matmul_task(void *params)
{
    const TickType_t xDelay = 1 / portTICK_PERIOD_MS;
    MatMulTaskParams *p = (MatMulTaskParams *)params;
    TaskHandle_t current_task = xTaskGetCurrentTaskHandle();
    //char *tName = pcTaskGetName(current_task);
    // ESP_LOGI(TAG, "Created Task %s", tName);
    for (;;)
    {
        if (xSemaphoreTake(semaDataReady, portMAX_DELAY) == pdTRUE)
        {
            //   ESP_LOGI(TAG, "Started Task %s", tName);
            for (int i = p->start; i < p->end; i++)
            {
                v4sf val = 0.0f;
                v4sf *row = &p->w[i * p->n]; // Pointer to the start of the current row in matrix w
                dsps_dotprod_f32_aes3(row, p->x, &val, p->n);
                p->xout[i] = val;
            }
            //    ESP_LOGI(TAG, "Completed task %s", tName);
            xSemaphoreGive(semaDataReady);
            xEventGroupSync(xEventGroup, p->task_num, ALL_SYNC_BITS, portMAX_DELAY);
        }
    }
}

void forward_task(void *params)
{
    const TickType_t xDelay = 1 / portTICK_PERIOD_MS;
    ForwardTaskParams *t_params = (ForwardTaskParams *)params;
    TaskHandle_t current_task = xTaskGetCurrentTaskHandle();
    //char *tName = pcTaskGetName(current_task);
    // ESP_LOGI(TAG, "Created Task %s", tName);
    for (;;)
    {
        if (xSemaphoreTake(semaForwardDataReady, portMAX_DELAY) == pdTRUE)
        {
            //   ESP_LOGI(TAG, "Started Task %s", tName);
            int h;
            // #pragma omp parallel for private(h)
            for (h = t_params->start; h < t_params->end; h++)
            {
                // get the query vector for this head
                v4sf *q = t_params->s->q + h * t_params->head_size;
                // attention scores for this head
                v4sf *att = t_params->s->att + h * t_params->p->seq_len;
                // iterate over all timesteps, including the current one
                for (int t = 0; t <= t_params->pos; t++)
                {
                    // get the key vector for this head and at this timestep
                    v4sf *k = t_params->s->key_cache + t_params->loff + t * t_params->kv_dim + (h / t_params->kv_mul) * t_params->head_size;
                    // calculate the attention score as the dot product of q and k
                    v4sf score = 0.0f;
                    for (int i = 0; i < t_params->head_size; i++)
                    {
                        score += q[i] * k[i];
                    }
                    score /= sqrtf(t_params->head_size);
                    // save the score to the attention buffer
                    att[t] = score;
                }

                // softmax the scores to get attention weights, from 0..pos inclusively
                softmax(att, t_params->pos + 1);

                // weighted sum of the values, store back into xb
                v4sf *xb = t_params->s->xb + h * t_params->head_size;
                memset(xb, 0, t_params->head_size * sizeof(v4sf));
                for (int t = 0; t <= t_params->pos; t++)
                {
                    // get the value vector for this head and at this timestep
                    v4sf *v = t_params->s->value_cache + t_params->loff + t * t_params->kv_dim + (h / t_params->kv_mul) * t_params->head_size;
                    // get the attention weight for this timestep
                    v4sf a = att[t];
                    // accumulate the weighted value into xb
                    for (int i = 0; i < t_params->head_size; i++)
                    {
                        xb[i] += a * v[i];
                    }
                }
            }
            //   ESP_LOGI(TAG, "Completed task %s", tName);
            xSemaphoreGive(semaForwardDataReady);
            xEventGroupSync(ForwardEventGroup, t_params->task_num, ALL_FORWARD_TASKS, portMAX_DELAY);
        }
    }
}

void matmul(v4sf *xout, v4sf *x, v4sf *w, int n, int d)
{

    // d is the number of rows
    // n is the number of columns
    // d X n
    *matmul_params = (MatMulTaskParams){xout, x, w, d / 2, d, n, d, TASK_1_BIT};
    xSemaphoreGive(semaDataReady);
    for (int i = 0; i < d / 2; i++)
    {
        v4sf val = 0.0f;
        v4sf *row = &w[i * n]; // Pointer to the start of the current row in matrix w
        dsps_dotprod_f32_aes3(row, x, &val, n);
        xout[i] = val;
    }
    if (xSemaphoreTake(semaDataReady, portMAX_DELAY) == pdTRUE)
    {
        xEventGroupSync(xEventGroup,
                        TASK_0_BIT,
                        ALL_SYNC_BITS,
                        portMAX_DELAY);

        xEventGroupClearBits(xEventGroup, ALL_SYNC_BITS);
    }
    //   ESP_LOGI(TAG, "Completed MatMul tasks");
}

v4sf *forward(Transformer *transformer, int token, int pos)
{
    ESP_LOGD(TAG, "ram available: %lu", esp_get_free_heap_size());

    // a few convenience variables
    Config *p = &transformer->config;
    TransformerWeights *w = &transformer->weights;
    RunState *s = &transformer->state;
    v4sf *x = s->x;
    int dim = p->dim;
    int kv_dim = (p->dim * p->n_kv_heads) / p->n_heads;
    int kv_mul = p->n_heads / p->n_kv_heads; // integer multiplier of the kv sharing in multiquery
    int hidden_dim = p->hidden_dim;
    int head_size = dim / p->n_heads;

    // copy the token embedding into x
    v4sf *content_row = w->token_embedding_table + token * dim;
    ESP_LOGD(TAG, "Content row: %f", *content_row);
    memcpy(x, content_row, dim * sizeof(*x));

    for (int i = 0; i < dim; i++) {
        x[i] += random_f32(&transformer->state.rng_state) * 0.01f;
    }

    // forward all the layers
    for (unsigned long long l = 0; l < p->n_layers; l++)
    {
        ESP_LOGD(TAG, "X: %f, Weights %f", *x, *w->rms_att_weight);
        // attention rmsnorm
        rmsnorm(s->xb, x, w->rms_att_weight + l * dim, dim);

        // key and value point to the kv cache
        int loff = l * p->seq_len * kv_dim; // kv cache layer offset for convenience
        s->k = s->key_cache + loff + pos * kv_dim;
        s->v = s->value_cache + loff + pos * kv_dim;

        // qkv matmuls for this position
        matmul(s->q, s->xb, w->wq + l * dim * dim, dim, dim);
        matmul(s->k, s->xb, w->wk + l * dim * kv_dim, dim, kv_dim);
        matmul(s->v, s->xb, w->wv + l * dim * kv_dim, dim, kv_dim);

        // RoPE relative positional encoding: complex-valued rotate q and k in each head
        for (int i = 0; i < dim; i += 2)
        {
            int head_dim = i % head_size;
            v4sf freq = 1.0f / powf(10000.0f, head_dim / (v4sf)head_size);
            v4sf val = pos * freq;
            v4sf fcr = cosf(val);
            v4sf fci = sinf(val);
            int rotn = i < kv_dim ? 2 : 1; // how many vectors? 2 = q & k, 1 = q only
            for (int v = 0; v < rotn; v++)
            {
                v4sf *vec = v == 0 ? s->q : s->k; // the vector to rotate (query or key)
                v4sf v0 = vec[i];
                v4sf v1 = vec[i + 1];
                vec[i] = v0 * fcr - v1 * fci;
                vec[i + 1] = v0 * fci + v1 * fcr;
            }
        }
        // start task
        *forward_params = (ForwardTaskParams){
            .s = s,
            .w = w,
            .p = p,
            .pos = pos,
            .start = p->n_heads / 2,
            .loff = loff,
            .end = p->n_heads,
            .dim = dim,
            .kv_dim = kv_dim,
            .kv_mul = kv_mul,
            .hidden_dim = hidden_dim,
            .head_size = head_size,
            .task_num = FORWARD_TASK_1,
        };
        xSemaphoreGive(semaForwardDataReady);

        // multihead attention. iterate over all heads
        int h;
        // #pragma omp parallel for private(h)
        for (h = 0; h < (p->n_heads / 2); h++)
        {
            // get the query vector for this head
            v4sf *q = s->q + h * head_size;
            // attention scores for this head
            v4sf *att = s->att + h * p->seq_len;
            // iterate over all timesteps, including the current one
            for (int t = 0; t <= pos; t++)
            {
                // get the key vector for this head and at this timestep
                v4sf *k = s->key_cache + loff + t * kv_dim + (h / kv_mul) * head_size;
                // calculate the attention score as the dot product of q and k
                v4sf score = 0.0f;
                for (int i = 0; i < head_size; i++)
                {
                    score += q[i] * k[i];
                }
                score /= sqrtf(head_size);
                // save the score to the attention buffer
                att[t] = score;
            }

            // softmax the scores to get attention weights, from 0..pos inclusively
            softmax(att, pos + 1);

            // weighted sum of the values, store back into xb
            v4sf *xb = s->xb + h * head_size;
            memset(xb, 0, head_size * sizeof(v4sf));
            for (int t = 0; t <= pos; t++)
            {
                // get the value vector for this head and at this timestep
                v4sf *v = s->value_cache + loff + t * kv_dim + (h / kv_mul) * head_size;
                // get the attention weight for this timestep
                v4sf a = att[t];
                // accumulate the weighted value into xb
                for (int i = 0; i < head_size; i++)
                {
                    xb[i] += a * v[i];
                }
            }
        }
        if (xSemaphoreTake(semaForwardDataReady, portMAX_DELAY) == pdTRUE)
        {

            xEventGroupSync(ForwardEventGroup,
                            FORWARD_TASK_2,
                            ALL_FORWARD_TASKS,
                            portMAX_DELAY);

            xEventGroupClearBits(ForwardEventGroup, ALL_FORWARD_TASKS);

            // final matmul to get the output of the attention
            matmul(s->xb2, s->xb, w->wo + l * dim * dim, dim, dim);

            // residual connection back into x
            for (int i = 0; i < dim; i++)
            {
                x[i] += s->xb2[i];
            }

            // ffn rmsnorm
            rmsnorm(s->xb, x, w->rms_ffn_weight + l * dim, dim);

            // Now for FFN in PyTorch we have: self.w2(F.silu(self.w1(x)) * self.w3(x))
            // first calculate self.w1(x) and self.w3(x)
            matmul(s->hb, s->xb, w->w1 + l * dim * hidden_dim, dim, hidden_dim);
            matmul(s->hb2, s->xb, w->w3 + l * dim * hidden_dim, dim, hidden_dim);

            // SwiGLU non-linearity
            for (int i = 0; i < hidden_dim; i++)
            {
                v4sf val = s->hb[i];
                // silu(x)=x*σ(x), where σ(x) is the logistic sigmoid
                val *= (1.0f / (1.0f + expf(-val)));
                // elementwise multiply with w3(x)
                val *= s->hb2[i];
                s->hb[i] = val;
            }

            // final matmul to get the output of the ffn
            matmul(s->xb, s->hb, w->w2 + l * dim * hidden_dim, hidden_dim, dim);

            // residual connection
            for (int i = 0; i < dim; i++)
            {
                x[i] += s->xb[i];
            }
        }
    }

    // final rmsnorm
    rmsnorm(x, x, w->rms_final_weight, dim);

    // classifier into logits
    matmul(s->logits, x, w->wcls, p->dim, p->vocab_size);
    return s->logits;
}

// ----------------------------------------------------------------------------
// The Byte Pair Encoding (BPE) Tokenizer that translates strings <-> tokens

int compare_tokens(const void *a, const void *b)
{
    return strcmp(((TokenIndex *)a)->str, ((TokenIndex *)b)->str);
}

void build_tokenizer(Tokenizer *t, char *tokenizer_path, int vocab_size)
{
    // i should have written the vocab_size into the tokenizer file... sigh
    ESP_LOGI(TAG, "Vocab size is %d\n", vocab_size);
    t->vocab_size = vocab_size;
    // malloc space to hold the scores and the strings
    t->vocab = (char **)malloc(vocab_size * sizeof(char *));
    t->vocab_scores = (v4sf *)malloc(vocab_size * sizeof(v4sf));
    t->sorted_vocab = NULL; // initialized lazily
    for (int i = 0; i < 256; i++)
    {
        t->byte_pieces[i * 2] = (unsigned char)i;
        t->byte_pieces[i * 2 + 1] = '\0';
    }
    // read in the file
    FILE *file = fopen(tokenizer_path, "rb");
    if (!file)
    {
        ESP_LOGE(TAG, "couldn't load %s", tokenizer_path);
        exit(EXIT_FAILURE);
    }
    ESP_LOGI(TAG, "Opened Tokenizer File");
    if (fread(&t->max_token_length, sizeof(int), 1, file) != 1)
    {
        ESP_LOGE(TAG, "failed read");
        exit(EXIT_FAILURE);
    }
    int len;
    for (int i = 0; i < vocab_size; i++)
    {
        if (fread(t->vocab_scores + i, sizeof(v4sf), 1, file) != 1)
        {
            ESP_LOGE(TAG, "failed read vocab scores");
            exit(EXIT_FAILURE);
        }
        if (fread(&len, sizeof(int), 1, file) != 1)
        {
            ESP_LOGE(TAG, "failed read len");
            exit(EXIT_FAILURE);
        }
        t->vocab[i] = (char *)malloc(len + 1);
        if (fread(t->vocab[i], len, 1, file) != 1)
        {
            ESP_LOGE(TAG, "failed read vocab");
            exit(EXIT_FAILURE);
        }
        t->vocab[i][len] = '\0'; // add the string terminating token
    }
    fclose(file);
    ESP_LOGI(TAG, "Tokenizer successfully built");
}

void free_tokenizer(Tokenizer *t)
{
    for (int i = 0; i < t->vocab_size; i++)
    {
        free(t->vocab[i]);
    }
    free(t->vocab);
    free(t->vocab_scores);
    free(t->sorted_vocab);
}

char *decode(Tokenizer *t, int prev_token, int token)
{
    char *piece = t->vocab[token];
    // following BOS (1) token, sentencepiece decoder strips any leading whitespace (see PR #89)
    if (prev_token == 1 && piece[0] == ' ')
    {
        piece++;
    }
    // careful, some tokens designate raw bytes, and look like e.g. '<0x01>'
    // parse this and convert and return the actual byte
    unsigned char byte_val;
    if (sscanf(piece, "<0x%02hhX>", &byte_val) == 1)
    {
        piece = (char *)t->byte_pieces + byte_val * 2;
    }
    return piece;
}

void safe_printf(char *piece) {
    if (piece == NULL || piece[0] == '\0') {
        return;
    }

    // Make a copy of the piece so we don't modify the original
    char *temp_piece = malloc(strlen(piece) + 1);
    if (!temp_piece) {
        return;  // Memory allocation failed
    }
    strcpy(temp_piece, piece);

    // Ignore the initial " if it's the first character
    if (output_pos == 0 && temp_piece[0] == '"') {
        memmove(temp_piece, temp_piece + 1, strlen(temp_piece));
    }

    // Ignore special tokens "<s>" and "</s>"
    if (strcmp(temp_piece, "<s>") == 0 || strcmp(temp_piece, "</s>") == 0) {
        free(temp_piece);
        return;
    }

    // Handle the case where <s> or </s> is part of the string
    char *pos;
    while ((pos = strstr(temp_piece, "<s>")) != NULL) {
        memmove(pos, pos + 3, strlen(pos + 3) + 1);
    }
    while ((pos = strstr(temp_piece, "</s>")) != NULL) {
        memmove(pos, pos + 4, strlen(pos + 4) + 1);
    }

    // Single character handling
    if (strlen(temp_piece) == 1) {
        unsigned char byte_val = temp_piece[0];
        if (!(isprint(byte_val) || isspace(byte_val))) {
            free(temp_piece);
            return;
        }
    }

    // Save to buffer and print
    size_t len = strlen(temp_piece);
    if (output_pos + len < MAX_LLM_OUTPUT - 1) {
        memcpy(output_buffer + output_pos, temp_piece, len);
        output_pos += len;
        output_buffer[output_pos] = '\0';
    }

    printf("%s", temp_piece);
    free(temp_piece);
}


int str_lookup(char *str, TokenIndex *sorted_vocab, int vocab_size)
{
    // efficiently find the perfect match for str in vocab, return its index or -1 if not found
    TokenIndex tok = {.str = str}; // acts as the key to search for
    TokenIndex *res = bsearch(&tok, sorted_vocab, vocab_size, sizeof(TokenIndex), compare_tokens);
    return res != NULL ? res->id : -1;
}

void encode(Tokenizer *t, char *text, int8_t bos, int8_t eos, int *tokens, int *n_tokens)
{
    // encode the string text (input) into an upper-bound preallocated tokens[] array
    // bos != 0 means prepend the BOS token (=1), eos != 0 means append the EOS token (=2)
    if (text == NULL)
    {
        ESP_LOGE(TAG, "cannot encode NULL text");
        exit(EXIT_FAILURE);
    }

    if (t->sorted_vocab == NULL)
    {
        // lazily malloc and sort the vocabulary
        t->sorted_vocab = malloc(t->vocab_size * sizeof(TokenIndex));
        for (int i = 0; i < t->vocab_size; i++)
        {
            t->sorted_vocab[i].str = t->vocab[i];
            t->sorted_vocab[i].id = i;
        }
        qsort(t->sorted_vocab, t->vocab_size, sizeof(TokenIndex), compare_tokens);
    }

    // create a temporary buffer that will store merge candidates of always two consecutive tokens
    // *2 for concat, +1 for null terminator +2 for UTF8 (in case max_token_length is 1)
    char *str_buffer = malloc((t->max_token_length * 2 + 1 + 2) * sizeof(char));
    size_t str_len = 0;

    // start at 0 tokens
    *n_tokens = 0;

    // add optional BOS (=1) token, if desired
    if (bos)
        tokens[(*n_tokens)++] = 1;

    // add_dummy_prefix is true by default
    // so prepend a dummy prefix token to the input string, but only if text != ""
    // TODO: pretty sure this isn't correct in the general case but I don't have the
    // energy to read more of the sentencepiece code to figure out what it's doing
    if (text[0] != '\0')
    {
        int dummy_prefix = str_lookup(" ", t->sorted_vocab, t->vocab_size);
        tokens[(*n_tokens)++] = dummy_prefix;
    }

    // Okay UTF-8 time. This will get messy. Here is the reference from Wikipedia:
    // Code point ↔ UTF-8 conversion
    // First code point	Last code point	Byte 1	Byte 2	Byte 3	Byte 4
    // U+0000	U+007F	    0xxxxxxx
    // U+0080	U+07FF	    110xxxxx	10xxxxxx
    // U+0800	U+FFFF	    1110xxxx	10xxxxxx	10xxxxxx
    // U+10000	U+10FFFF    11110xxx	10xxxxxx	10xxxxxx	10xxxxxx

    // process the raw (UTF-8) byte sequence of the input string
    for (char *c = text; *c != '\0'; c++)
    {

        // reset buffer if the current byte is ASCII or a leading byte
        // 0xC0 is 11000000, so (*c & 0xC0) keeps the first 2 bits and zeros the rest
        // 0x80 is 10000000
        // in UTF-8, all continuation bytes start with "10" in first two bits
        // so in English this is: "if this byte is not a continuation byte"
        if ((*c & 0xC0) != 0x80)
        {
            // this byte must be either a leading byte (11...) or an ASCII char (0x...)
            // => reset our location, as we're starting a new UTF-8 codepoint
            str_len = 0;
        }

        // append the current byte to the buffer
        str_buffer[str_len++] = *c; // ++ is post-increment, incremented after this line
        str_buffer[str_len] = '\0';

        // while the next character is a continuation byte, continue appending
        // but if there are too many of them, just stop to avoid overruning str_buffer size.
        if ((*(c + 1) & 0xC0) == 0x80 && str_len < 4)
        {
            continue;
        }

        // ok c+1 is not a continuation byte, so we've read in a full codepoint
        int id = str_lookup(str_buffer, t->sorted_vocab, t->vocab_size);

        if (id != -1)
        {
            // we found this codepoint in vocab, add it as a token
            tokens[(*n_tokens)++] = id;
        }
        else
        {
            // byte_fallback encoding: just encode each byte as a token
            // +3 is here because the first 3 vocab elements are <unk>, <s>, </s>
            // so the individual bytes only start at index 3
            for (int i = 0; i < str_len; i++)
            {
                tokens[(*n_tokens)++] = (unsigned char)str_buffer[i] + 3;
            }
        }
        str_len = 0; // protect against a sequence of stray UTF8 continuation bytes
    }

    // merge the best consecutive pair each iteration, according the scores in vocab_scores
    while (1)
    {
        v4sf best_score = -1e10;
        int best_id = -1;
        int best_idx = -1;

        for (int i = 0; i < (*n_tokens - 1); i++)
        {
            // check if we can merge the pair (tokens[i], tokens[i+1])
            sprintf(str_buffer, "%s%s", t->vocab[tokens[i]], t->vocab[tokens[i + 1]]);
            int id = str_lookup(str_buffer, t->sorted_vocab, t->vocab_size);
            if (id != -1 && t->vocab_scores[id] > best_score)
            {
                // this merge pair exists in vocab! record its score and position
                best_score = t->vocab_scores[id];
                best_id = id;
                best_idx = i;
            }
        }

        if (best_idx == -1)
        {
            break; // we couldn't find any more pairs to merge, so we're done
        }

        // merge the consecutive pair (best_idx, best_idx+1) into new token best_id
        tokens[best_idx] = best_id;
        // delete token at position best_idx+1, shift the entire sequence back 1
        for (int i = best_idx + 1; i < (*n_tokens - 1); i++)
        {
            tokens[i] = tokens[i + 1];
        }
        (*n_tokens)--; // token length decreased
    }

    // add optional EOS (=2) token, if desired
    if (eos)
        tokens[(*n_tokens)++] = 2;

    free(str_buffer);
}

// ----------------------------------------------------------------------------
// The Sampler, which takes logits and returns a sampled token
// sampling can be done in a few ways: greedy argmax, sampling, top-p sampling

int sample_argmax(v4sf *probabilities, int n)
{
    // return the index that has the highest probability
    int max_i = 0;
    v4sf max_p = probabilities[0];
    for (int i = 1; i < n; i++)
    {
        if (probabilities[i] > max_p)
        {
            max_i = i;
            max_p = probabilities[i];
        }
    }
    return max_i;
}

int sample_mult(v4sf *probabilities, int n, v4sf coin)
{
    // sample index from probabilities (they must sum to 1!)
    // coin is a random number in [0, 1), usually from random_f32()
    v4sf cdf = 0.0f;
    for (int i = 0; i < n; i++)
    {
        cdf += probabilities[i];
        if (coin < cdf)
        {
            return i;
        }
    }
    return n - 1; // in case of rounding errors
}

int compare(const void *a, const void *b)
{
    ProbIndex *a_ = (ProbIndex *)a;
    ProbIndex *b_ = (ProbIndex *)b;
    if (a_->prob > b_->prob)
        return -1;
    if (a_->prob < b_->prob)
        return 1;
    return 0;
}

int sample_topp(v4sf *probabilities, int n, v4sf topp, ProbIndex *probindex, v4sf coin)
{
    // Controlli iniziali
    if (!probabilities || !probindex || n <= 0) {
        ESP_LOGE(TAG, "Invalid parameters");
        return 0;
    }

    // Allocazione temporanea in stack per sicurezza
    ProbIndex local_buffer[512];  // Assumiamo vocab_size <= 512
    int n0 = 0;
    float cutoff = (float)(1.0f - topp) / (float)(n - 1);

    // Prima fase: raccolta candidati con controlli di bounds
    for (int i = 0; i < n && n0 < 512; i++) {
        float prob = (float)probabilities[i];
        if (prob >= cutoff) {
            local_buffer[n0].index = i;
            local_buffer[n0].prob = prob;
            n0++;
        }
    }

    // Controllo se abbiamo candidati
    if (n0 == 0) {
        ESP_LOGW(TAG, "No candidates, using argmax");
        int max_idx = 0;
        float max_prob = probabilities[0];
        for (int i = 1; i < n; i++) {
            if (probabilities[i] > max_prob) {
                max_prob = probabilities[i];
                max_idx = i;
            }
        }
        return max_idx;
    }

    // Sort dei candidati in local_buffer
    for (int i = 0; i < n0 - 1; i++) {
        for (int j = 0; j < n0 - i - 1; j++) {
            if (local_buffer[j].prob < local_buffer[j + 1].prob) {
                ProbIndex temp = local_buffer[j];
                local_buffer[j] = local_buffer[j + 1];
                local_buffer[j + 1] = temp;
            }
        }
    }

    // Calcolo probabilità cumulativa e troncamento
    float cumsum = 0.0f;
    int last_idx = 0;
    
    for (int i = 0; i < n0; i++) {
        cumsum += local_buffer[i].prob;
        if (cumsum >= topp) {
            last_idx = i;
            break;
        }
    }

    // Sampling finale con controlli di sicurezza
    float r = (float)coin * cumsum;
    float current_sum = 0.0f;

    for (int i = 0; i <= last_idx; i++) {
        current_sum += local_buffer[i].prob;
        if (r <= current_sum && local_buffer[i].index >= 0 && local_buffer[i].index < n) {
            return local_buffer[i].index;
        }
    }

    // Fallback sicuro
    return local_buffer[0].index;
}
void build_sampler(Sampler *sampler, int vocab_size, float temperature, float topp, unsigned long long rng_seed) {
    sampler->vocab_size = vocab_size;
    sampler->temperature = temperature;
    sampler->topp = topp;

    // Usa un seed casuale unico per ogni generazione, basato sull'orologio interno
    unsigned long long true_random_seed = (unsigned long long)time(NULL) ^ esp_random();
    sampler->rng_state = rng_seed ? rng_seed : true_random_seed;

    ESP_LOGI(TAG, "Building sampler with temperature: %f, topp: %f, rng_seed: %llu", temperature, topp, sampler->rng_state);

    // Allocazione della memoria per probindex
    sampler->probindex = heap_caps_malloc(vocab_size * sizeof(ProbIndex), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!sampler->probindex) {
        ESP_LOGE(TAG, "Failed to allocate probindex");
        abort();
    }
}
void free_sampler(Sampler *sampler)
{
    free(sampler->probindex);
}

unsigned int random_u32(unsigned long long *state)
{
    // xorshift rng: https://en.wikipedia.org/wiki/Xorshift#xorshift.2A
    ESP_LOGD(TAG, "RNG state before: %llu", *state);
    *state ^= *state >> 12;
    ESP_LOGD(TAG, "After first shift: %llu", *state);
    *state ^= *state << 25;
    ESP_LOGD(TAG, "After second shift: %llu", *state);
    *state ^= *state >> 27;
    ESP_LOGD(TAG, "After third shift: %llu", *state);
    unsigned int result = (*state * 0x2545F4914F6CDD1Dull) >> 32;
    ESP_LOGD(TAG, "Final result: %u", result);
    return result;
}
v4sf random_f32(unsigned long long *state)
{
    return (random_u32(state) >> 8) / 16777216.0f;
}

void reset_run_state(RunState *s, Config *p) {
    int kv_dim = (p->dim * p->n_kv_heads) / p->n_heads;
    
    // Reset all buffers to zero
    memset(s->x, 0, p->dim * sizeof(v4sf));
    memset(s->xb, 0, p->dim * sizeof(v4sf));
    memset(s->xb2, 0, p->dim * sizeof(v4sf));
    memset(s->hb, 0, p->hidden_dim * sizeof(v4sf));
    memset(s->hb2, 0, p->hidden_dim * sizeof(v4sf));
    memset(s->q, 0, p->dim * sizeof(v4sf));
    memset(s->key_cache, 0, p->n_layers * p->seq_len * kv_dim * sizeof(v4sf));
    memset(s->value_cache, 0, p->n_layers * p->seq_len * kv_dim * sizeof(v4sf));
    memset(s->att, 0, p->n_heads * p->seq_len * sizeof(v4sf));
    memset(s->logits, 0, p->vocab_size * sizeof(v4sf));
}

int sample(Sampler *sampler, v4sf *logits) {
    // Validate and apply temperature
    if (sampler->temperature < 0.0f || sampler->temperature > 2.0f) {
        ESP_LOGE(TAG, "Invalid temperature: %f, resetting to 1.0", sampler->temperature);
        sampler->temperature = 1.0f;
    }
    if (sampler->temperature != 1.0f) {
        for (int q = 0; q < sampler->vocab_size; q++) {
            logits[q] /= sampler->temperature;
        }
    }

    for (int i = 0; i < sampler->vocab_size; i++) {
        logits[i] += (random_f32(&sampler->rng_state) - 0.5f) * 0.2f; 
    }
    // Apply softmax to logits
    softmax(logits, sampler->vocab_size);

    // Implement top-p sampling with cumulative probability check
    float cumulative_prob = 0.0f;
    int *indices = malloc(sampler->vocab_size * sizeof(int));

    for (int i = 0; i < sampler->vocab_size; i++) {
        indices[i] = i;
    }

    // Sort logits by probability
    for (int i = 0; i < sampler->vocab_size - 1; i++) {
        for (int j = i + 1; j < sampler->vocab_size; j++) {
            if (logits[indices[i]] < logits[indices[j]]) {
                int temp = indices[i];
                indices[i] = indices[j];
                indices[j] = temp;
            }
        }
    }

    // Accumulate probabilities and select index
    int selected_index = -1;
    for (int i = 0; i < sampler->vocab_size; i++) {
        int idx = indices[i];
        cumulative_prob += logits[idx];
        if (cumulative_prob >= sampler->topp) {
            selected_index = idx;
            break;
        }
    }

    free(indices);
    
    return (selected_index != -1) ? selected_index : sample_argmax(logits, sampler->vocab_size);
}

// ----------------------------------------------------------------------------
// utilities: time

long time_in_ms()
{
    // return time in milliseconds, for benchmarking the model speed
    struct timespec time;
    clock_gettime(CLOCK_REALTIME, &time);
    return time.tv_sec * 1000 + time.tv_nsec / 1000000;
}


// ----------------------------------------------------------------------------
// generation loop

void generate(Transformer *transformer, Tokenizer *tokenizer, Sampler *sampler,
             char *prompt, int steps, generated_complete_cb cb_done) {
    // Reset output buffer
    output_pos = 0;
    output_buffer[0] = '\0';

    sampler->rng_state = (unsigned long long)time(NULL) ^ esp_random();
    ESP_LOGI(TAG, "Sampler RNG state reset: %llu", sampler->rng_state);
    
    reset_run_state(&transformer->state, &transformer->config);
    char *empty_prompt = "";
    if (prompt == NULL) {
        prompt = empty_prompt;
    }

    int num_prompt_tokens = 0;
    int *prompt_tokens = (int *)malloc((strlen(prompt) + 3) * sizeof(int));
    encode(tokenizer, prompt, 1, 0, prompt_tokens, &num_prompt_tokens);
    if (num_prompt_tokens < 1) {
        ESP_LOGE(TAG, "something is wrong, expected at least 1 prompt token");
        exit(EXIT_FAILURE);
    }

    long start = 0;               
    int next;                     
    int token = prompt_tokens[0]; 
    int pos = 0;                  
    int prev_x = -1, prev_y = -1;
    
    const int MIN_ACTIVE_NODES = 45;  // Minimo numero di LED da mantenere accesi
    const int MAX_ACTIVE_NODES = 55;  // Massimo numero di LED accesi
    int active_nodes = 0;
    bool led_matrix[MATRIX_ROWS][MATRIX_COLS] = {0};  // Tiene traccia dei LED accesi
    
    int tokens_since_last_end = 0;
    bool in_sentence = false;

    while (pos < steps) {
        sampler->rng_state ^= (unsigned long long)pos * 6364136223846793005ULL + 1;

        v4sf *logits = forward(transformer, token, pos);

        if (pos < num_prompt_tokens - 1) {
            next = prompt_tokens[pos + 1];
        } else {
            next = sample(sampler, logits);
        }
        pos++;
        tokens_since_last_end++;

        char *piece = decode(tokenizer, token, next);
        
        if (piece && piece[0] != '\0') {
            if (piece[0] == '.' || piece[0] == '!' || piece[0] == '?') {
                tokens_since_last_end = 0;
                in_sentence = false;
            } else if (!isspace((unsigned char)piece[0])) {
                in_sentence = true;
            }
        }

        // LED Matrix logic
        if (active_nodes < MAX_ACTIVE_NODES) {
            // Usa i logits per determinare le coordinate 2D
            v4sf max_logit = -1e10;
            v4sf min_logit = 1e10;
            
            for (int i = 0; i < transformer->config.vocab_size; i++) {
                if (logits[i] > max_logit) max_logit = logits[i];
                if (logits[i] < min_logit) min_logit = logits[i];
            }
            
            v4sf l1 = logits[next];
            v4sf l2 = logits[(next + 1) % transformer->config.vocab_size];
            
            int x = (int)(((l1 - min_logit) / (max_logit - min_logit)) * (MATRIX_COLS - 1));
            int y = (int)(((l2 - min_logit) / (max_logit - min_logit)) * (MATRIX_ROWS - 1));
            
            bool led_activated = false;
            
            // Prova ad attivare il LED nella posizione calcolata
            if (!led_matrix[y][x]) {
                led_matrix[y][x] = true;
                int *coords = malloc(2 * sizeof(int));
                if (coords != NULL) {
                    coords[0] = x;
                    coords[1] = y;
                    xTaskCreate(activate_new_node_task, "activate_node", 2048, coords, 5, NULL);
                    active_nodes++;
                    prev_x = x;
                    prev_y = y;
                    led_activated = true;
                }
            }
            
            // Se non abbiamo attivato il LED e siamo sotto il minimo, cerca altre posizioni
            if (!led_activated && active_nodes < MIN_ACTIVE_NODES) {
                // Prima prova posizioni adiacenti
                for (int dy = -1; dy <= 1 && !led_activated; dy++) {
                    for (int dx = -1; dx <= 1 && !led_activated; dx++) {
                        if (dx == 0 && dy == 0) continue;
                        
                        int new_x = (prev_x + dx + MATRIX_COLS) % MATRIX_COLS;
                        int new_y = (prev_y + dy + MATRIX_ROWS) % MATRIX_ROWS;
                        
                        if (!led_matrix[new_y][new_x]) {
                            led_matrix[new_y][new_x] = true;
                            int *coords = malloc(2 * sizeof(int));
                            if (coords != NULL) {
                                coords[0] = new_x;
                                coords[1] = new_y;
                                xTaskCreate(activate_new_node_task, "activate_node", 2048, coords, 5, NULL);
                                active_nodes++;
                                prev_x = new_x;
                                prev_y = new_y;
                                led_activated = true;
                            }
                        }
                    }
                }
                
                // Se ancora non abbiamo attivato un LED, prova posizioni casuali
                if (!led_activated) {
                    int attempts = 0;
                    while (!led_activated && attempts < 10) {
                        int new_x = esp_random() % MATRIX_COLS;
                        int new_y = esp_random() % MATRIX_ROWS;
                        
                        if (!led_matrix[new_y][new_x]) {
                            led_matrix[new_y][new_x] = true;
                            int *coords = malloc(2 * sizeof(int));
                            if (coords != NULL) {
                                coords[0] = new_x;
                                coords[1] = new_y;
                                xTaskCreate(activate_new_node_task, "activate_node", 2048, coords, 5, NULL);
                                active_nodes++;
                                prev_x = new_x;
                                prev_y = new_y;
                                led_activated = true;
                            }
                        }
                        attempts++;
                    }
                }
            }
        }

        safe_printf(piece);
        fflush(stdout);
        token = next;

        if (start == 0) {
            start = time_in_ms();
        }

        if (pos > steps * 0.8 && !in_sentence) {
            break;
        }
    }

    if (in_sentence) {
        printf(".");
    }
    printf("\n");

    if (pos > 1) {
        long end = time_in_ms();
        float tks = (pos - 1) / (double)(end - start) * 1000;
        fprintf(stderr, "achieved tok/s: %f\n", tks);
        cb_done(tks);
    }
    
    captive_portal_set_llm_output(output_buffer);
    free(prompt_tokens);
}

void read_stdin(const char *guide, char *buffer, size_t bufsize)
{
    // read a line from stdin, up to but not including \n
    printf("%s", guide);
    if (fgets(buffer, bufsize, stdin) != NULL)
    {
        size_t len = strlen(buffer);
        if (len > 0 && buffer[len - 1] == '\n')
        {
            buffer[len - 1] = '\0'; // strip newline
        }
    }
}
