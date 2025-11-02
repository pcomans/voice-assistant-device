#include "pcm_buffer.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"

#define PCM_BUFFER_TAG "pcm_buffer"

static RingbufHandle_t s_ring = NULL;
static uint8_t *s_ring_storage = NULL;
static StaticRingbuffer_t *s_ring_struct = NULL;
static size_t s_bytes_per_sample = 0;
static size_t s_capacity_bytes = 0;

bool pcm_buffer_init(const pcm_buffer_config_t *cfg)
{
    if (!cfg || cfg->bytes_per_sample == 0 || cfg->capacity_bytes == 0) {
        ESP_LOGE(PCM_BUFFER_TAG, "Invalid configuration");
        return false;
    }

    // Clean up existing buffer
    if (s_ring) {
        vRingbufferDelete(s_ring);
        s_ring = NULL;
    }
    if (s_ring_storage) {
        free(s_ring_storage);
        s_ring_storage = NULL;
    }
    if (s_ring_struct) {
        free(s_ring_struct);
        s_ring_struct = NULL;
    }

    // Allocate static ring buffer from SPIRAM
    // For BYTEBUF type, storage size equals buffer capacity
    size_t ring_storage_size = cfg->capacity_bytes;

    // Log available SPIRAM
    size_t spiram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(PCM_BUFFER_TAG, "Attempting to allocate %u bytes, SPIRAM available: %u bytes",
             (unsigned int)ring_storage_size, (unsigned int)spiram_free);

    // Large ring buffer - must use SPIRAM
    s_ring_storage = heap_caps_malloc(ring_storage_size, MALLOC_CAP_SPIRAM);
    if (!s_ring_storage) {
        ESP_LOGE(PCM_BUFFER_TAG, "Failed to allocate %u bytes from SPIRAM for ring buffer storage", (unsigned int)ring_storage_size);
        return false;
    }
    ESP_LOGI(PCM_BUFFER_TAG, "Allocated %u bytes for ring buffer storage", (unsigned int)ring_storage_size);

    s_ring_struct = heap_caps_malloc(sizeof(StaticRingbuffer_t), MALLOC_CAP_INTERNAL);
    if (!s_ring_struct) {
        ESP_LOGE(PCM_BUFFER_TAG, "Failed to allocate ring buffer struct");
        free(s_ring_storage);
        s_ring_storage = NULL;
        return false;
    }

    s_ring = xRingbufferCreateStatic(cfg->capacity_bytes, RINGBUF_TYPE_BYTEBUF, s_ring_storage, s_ring_struct);
    if (!s_ring) {
        ESP_LOGE(PCM_BUFFER_TAG, "Failed to create static ring buffer");
        free(s_ring_storage);
        free(s_ring_struct);
        s_ring_storage = NULL;
        s_ring_struct = NULL;
        return false;
    }

    ESP_LOGI(PCM_BUFFER_TAG, "Ring buffer created (BYTEBUF): %u bytes (storage: %u bytes, in SPIRAM)",
             (unsigned int)cfg->capacity_bytes, (unsigned int)ring_storage_size);

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
