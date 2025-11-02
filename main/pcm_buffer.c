#include "pcm_buffer.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"

#define PCM_BUFFER_TAG "pcm_buffer"

static RingbufHandle_t s_ring = NULL;
static size_t s_bytes_per_sample = 0;
static size_t s_capacity_bytes = 0;

bool pcm_buffer_init(const pcm_buffer_config_t *cfg)
{
    if (!cfg || cfg->bytes_per_sample == 0 || cfg->capacity_bytes == 0) {
        ESP_LOGE(PCM_BUFFER_TAG, "Invalid configuration");
        return false;
    }

    if (s_ring) {
        vRingbufferDelete(s_ring);
        s_ring = NULL;
    }

    s_ring = xRingbufferCreate(cfg->capacity_bytes, RINGBUF_TYPE_NOSPLIT);
    if (!s_ring) {
        ESP_LOGE(PCM_BUFFER_TAG, "Failed to allocate ring buffer (capacity: %u bytes)", (unsigned int)cfg->capacity_bytes);
        return false;
    }
    ESP_LOGI(PCM_BUFFER_TAG, "Ring buffer created: %u bytes", (unsigned int)cfg->capacity_bytes);

    s_bytes_per_sample = cfg->bytes_per_sample;
    s_capacity_bytes = cfg->capacity_bytes;
    return true;
}

void pcm_buffer_reset(void)
{
    if (s_ring) {
        // Drain the ringbuffer by reading and discarding all items
        size_t item_size;
        void *item;
        while ((item = xRingbufferReceive(s_ring, &item_size, 0)) != NULL) {
            vRingbufferReturnItem(s_ring, item);
        }
    }
}

size_t pcm_buffer_push(const int16_t *samples, size_t sample_count)
{
    if (!s_ring || !samples || sample_count == 0) {
        return 0;
    }

    const size_t bytes_to_write = sample_count * s_bytes_per_sample;
    BaseType_t ok = xRingbufferSend(s_ring, (const void *)samples, bytes_to_write, 0);
    if (ok != pdTRUE) {
        ESP_LOGW(PCM_BUFFER_TAG, "Ring buffer full; dropping %u bytes", (unsigned int)bytes_to_write);
        return 0;
    }
    return sample_count;
}

size_t pcm_buffer_pop(uint8_t *dst, size_t max_bytes)
{
    if (!s_ring || !dst || max_bytes == 0) {
        return 0;
    }

    size_t out_size = 0;
    uint8_t *data = (uint8_t *)xRingbufferReceiveUpTo(s_ring, &out_size, pdMS_TO_TICKS(10), max_bytes);
    if (!data) {
        return 0;
    }

    memcpy(dst, data, out_size);
    vRingbufferReturnItem(s_ring, data);
    return out_size;
}

size_t pcm_buffer_size(void)
{
    if (!s_ring) {
        return 0;
    }
    size_t free_bytes = xRingbufferGetCurFreeSize(s_ring);
    return (free_bytes > s_capacity_bytes) ? 0 : (s_capacity_bytes - free_bytes);
}

size_t pcm_buffer_capacity(void)
{
    return s_capacity_bytes;
}
