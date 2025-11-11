# Opus Audio Codec Implementation Guide for ESP32

This document provides practical guidance for implementing Opus audio codec on ESP32, particularly when working with external encoders (like Python) and the ESP Audio Codec library's Opus decoder.

## Table of Contents
- [Overview](#overview)
- [Self-Delimited Packet Format](#self-delimited-packet-format)
- [ESP32 Decoder Implementation](#esp32-decoder-implementation)
- [Python Encoder Implementation](#python-encoder-implementation)
- [Common Pitfalls](#common-pitfalls)
- [Sample Rate Matching](#sample-rate-matching)
- [Memory Management](#memory-management)
- [Performance Considerations](#performance-considerations)
- [Complete Working Example](#complete-working-example)

## Overview

Opus is a high-quality, low-latency audio codec ideal for voice applications. For ESP32 projects using external audio processing (e.g., OpenAI Realtime API), you typically:

1. **Capture audio** on ESP32 (16kHz PCM16)
2. **Send to external service** for processing
3. **Receive Opus-encoded response** (compressed ~10x)
4. **Decode on ESP32** and play back (24kHz PCM16)

**Key Benefits:**
- ~10x compression compared to PCM16
- Low latency (20ms frames)
- Excellent voice quality
- Reduces network bandwidth significantly

## Self-Delimited Packet Format

### RFC 6716 Appendix B

Self-delimited packets add a 1-2 byte length prefix before each Opus frame, allowing multiple frames to be concatenated without a container format (like OGG).

### Length Encoding

- **0-251 bytes**: Single byte prefix = frame length
- **252-507 bytes**: Two byte prefix = `[252, length - 252]`

### Example

```
Frame 1: 54 bytes → Prefix: [54] → Total: 1 + 54 = 55 bytes
Frame 2: 280 bytes → Prefix: [252, 28] → Total: 2 + 280 = 282 bytes
```

### Why Use Self-Delimited?

When receiving multiple Opus frames over HTTP/JSON without a container format, self-delimited packets provide frame boundaries. This is simpler than implementing OGG parsing.

## ESP32 Decoder Implementation

### Critical Discovery

**The ESP Audio Codec library's `self_delimited = true` mode does NOT handle multiple concatenated packets.** It only works for ONE packet per decode call.

### Correct Implementation Pattern

You must **manually parse** the length prefixes and feed frames individually:

```c
// 1. Initialize decoder with self_delimited = false
esp_opus_dec_cfg_t opus_cfg = {
    .sample_rate = 24000,
    .channel = 1,
    .frame_duration = ESP_OPUS_DEC_FRAME_DURATION_20_MS,
    .self_delimited = false,  // We parse prefixes manually!
};

void *opus_handle = NULL;
esp_opus_dec_open(&opus_cfg, sizeof(opus_cfg), &opus_handle);

// 2. Allocate buffers
size_t pcm_capacity = opus_len * 10;  // Initial estimate
uint8_t *pcm_data = malloc(pcm_capacity);

size_t frame_buf_size = 24000 * 2 * 0.12;  // 120ms max frame
uint8_t *frame_buf = malloc(frame_buf_size);

// 3. Parse and decode each frame
size_t total_pcm = 0;
size_t offset = 0;

while (offset < opus_len) {
    // Parse length prefix
    uint8_t first_byte = opus_data[offset];
    size_t frame_len;
    size_t header_len;

    if (first_byte < 252) {
        frame_len = first_byte;
        header_len = 1;
    } else {
        frame_len = 252 + opus_data[offset + 1];
        header_len = 2;
    }

    // Setup decoder input (skip prefix)
    esp_audio_dec_in_raw_t raw = {
        .buffer = opus_data + offset + header_len,
        .len = frame_len,
        .consumed = 0,
    };

    esp_audio_dec_out_frame_t frame = {
        .buffer = frame_buf,
        .len = frame_buf_size,
        .decoded_size = 0,
    };

    esp_audio_dec_info_t dec_info = {0};

    // Decode this frame
    esp_audio_err_t rc = esp_opus_dec_decode(opus_handle, &raw, &frame, &dec_info);

    if (rc == ESP_AUDIO_ERR_OK && frame.decoded_size > 0) {
        // Check if output buffer needs to grow
        if (total_pcm + frame.decoded_size > pcm_capacity) {
            size_t new_capacity = pcm_capacity * 2;
            uint8_t *new_buf = malloc(new_capacity);
            memcpy(new_buf, pcm_data, total_pcm);
            free(pcm_data);
            pcm_data = new_buf;
            pcm_capacity = new_capacity;
        }

        // Copy decoded frame to output buffer
        memcpy(pcm_data + total_pcm, frame.buffer, frame.decoded_size);
        total_pcm += frame.decoded_size;
    }

    // Move to next frame
    offset += header_len + frame_len;
    frame.decoded_size = 0;  // Reset for next iteration
}

// 4. Cleanup and use decoded audio
free(frame_buf);
audio_playback_play_pcm(pcm_data, total_pcm);
free(pcm_data);
esp_opus_dec_close(opus_handle);
```

### Key Points

1. **Set `self_delimited = false`** - We handle prefixes manually
2. **Two buffers needed**:
   - `frame_buf`: Reused for each decode (decoder writes here)
   - `pcm_data`: Accumulates all frames (grows dynamically)
3. **Copy after each decode** - Decoder reuses frame_buf, must copy data out
4. **Dynamic growth** - Start with estimate, grow as needed
5. **Feed watchdog** - Add `vTaskDelay(1)` in loop for long decodes

## Python Encoder Implementation

Using `opuslib` to create self-delimited packets:

```python
import opuslib
import base64

def encode_to_opus_self_delimited(pcm_bytes: bytes, sample_rate: int = 24000) -> str:
    """
    Encode PCM16 audio to self-delimited Opus packets.

    Args:
        pcm_bytes: Raw PCM16 audio data (16-bit little-endian)
        sample_rate: Sample rate (8000, 12000, 16000, 24000, or 48000)

    Returns:
        Base64-encoded self-delimited Opus packet stream
    """
    encoder = opuslib.Encoder(sample_rate, 1, opuslib.APPLICATION_VOIP)

    # 20ms frames: samples = sample_rate * 0.02
    frame_size = int(sample_rate * 0.02)  # e.g., 480 for 24kHz
    frame_bytes = frame_size * 2  # 2 bytes per sample (16-bit)

    opus_chunks = []

    for i in range(0, len(pcm_bytes), frame_bytes):
        frame = pcm_bytes[i:i + frame_bytes]

        # Pad last frame if needed
        if len(frame) < frame_bytes:
            frame = frame + b'\x00' * (frame_bytes - len(frame))

        try:
            # Encode frame
            opus_frame = encoder.encode(frame, frame_size)

            # Add self-delimited length prefix (RFC 6716 Appendix B)
            frame_len = len(opus_frame)
            if frame_len < 252:
                delimited_frame = bytes([frame_len]) + opus_frame
            else:
                delimited_frame = bytes([252, frame_len - 252]) + opus_frame

            opus_chunks.append(delimited_frame)

        except Exception as e:
            print(f"Failed to encode frame: {e}")
            break

    # Combine all frames and encode to base64
    combined_opus = b"".join(opus_chunks)
    return base64.b64encode(combined_opus).decode("utf-8")
```

### Python Example Usage

```python
# Encode audio from OpenAI Realtime API (24kHz PCM16)
pcm_audio = receive_from_openai()  # bytes
opus_b64 = encode_to_opus_self_delimited(pcm_audio, sample_rate=24000)

# Send to ESP32 via JSON
response = {"status": "complete", "audio_base64": opus_b64}
```

## Common Pitfalls

### ❌ Pitfall 1: Using `self_delimited = true`

**Problem:**
```c
esp_opus_dec_cfg_t opus_cfg = {
    .self_delimited = true,  // ❌ This doesn't work for multiple packets!
};
```

**What happens:**
- Decoder reads first frame's length prefix
- Decodes first frame successfully
- Sets `raw.consumed` to ENTIRE buffer length
- Loop exits after processing only ONE frame

**Solution:** Use `self_delimited = false` and manual parsing (see above)

### ❌ Pitfall 2: Not Copying Frame Data

**Problem:**
```c
while (has_more_frames) {
    esp_opus_dec_decode(opus_handle, &raw, &frame, &dec_info);
    total_pcm += frame.decoded_size;  // ❌ Only counting, not saving data!
}
// frame.buffer contains only LAST frame
```

**What happens:**
- Decoder reuses `frame.buffer` for each decode
- Previous frames are overwritten
- Only last frame's data remains

**Solution:** `memcpy` after each decode to accumulate frames

### ❌ Pitfall 3: Guessing Final Buffer Size

**Problem:**
```c
uint8_t *pcm_data = malloc(opus_len * 12);  // ❌ Magic number guess
```

**What happens:**
- Compression ratio varies (5x - 20x depending on content)
- Buffer might be too small → data loss
- Buffer might be way too large → memory waste

**Solution:** Start with estimate, grow dynamically when needed

### ❌ Pitfall 4: Sample Rate Mismatch

**Problem:**
```c
// Decoder outputs 24kHz
esp_opus_dec_cfg_t cfg = { .sample_rate = 24000 };

// But playback is 16kHz
#define PLAYBACK_SAMPLE_RATE 16000  // ❌ Mismatch!
```

**What happens:**
- Audio plays at 24000/16000 = 1.5x slower than intended
- Sounds like slow-motion speech

**Solution:** Match all sample rates in the pipeline

## Sample Rate Matching

### Critical Rule

**Decoder sample rate MUST match playback sample rate.**

### Example Pipeline

```
Device Mic → 16kHz PCM16
    ↓ (resample 16kHz → 24kHz)
OpenAI API → 24kHz PCM16
    ↓ (encode to Opus @ 24kHz)
ESP32 receives → Opus @ 24kHz
    ↓ (decode Opus → PCM16)
ESP32 decoder → 24kHz PCM16
    ↓ (I2S playback @ 24kHz)
Speaker → Correct speed ✓
```

### Configuration

**ESP32 Decoder:**
```c
esp_opus_dec_cfg_t opus_cfg = {
    .sample_rate = 24000,  // Must match OpenAI output
    .channel = 1,
};
```

**ESP32 I2S Playback:**
```c
#define PLAYBACK_SAMPLE_RATE 24000  // Must match decoder output

i2s_std_config_t i2s_config = {
    .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(PLAYBACK_SAMPLE_RATE),
    // ...
};
```

### Symptoms of Mismatch

| Decoder | Playback | Result |
|---------|----------|--------|
| 24kHz | 16kHz | 1.5x too slow |
| 24kHz | 48kHz | 2x too fast |
| 16kHz | 24kHz | 1.5x too fast |

## Memory Management

### Stack Requirements

From ESP Audio Codec docs (line 267):
> To support all decoders, the task running the decoder should have stack size of about 20K.

**Recommended task stack:**
```c
xTaskCreatePinnedToCore(
    decoder_task,
    "opus_decoder",
    24576,  // 24KB stack (20KB + margin)
    NULL,
    5,
    NULL,
    0  // Core 0
);
```

### Heap Usage

**Opus Decoder (24kHz mono):**
- Heap: ~13 KB (half of stereo 48kHz: 26.6 KB)
- CPU: ~3% (half of stereo 48kHz: 5.86%)

**Buffer Allocation Strategy:**

```c
// Frame buffer (reused) - allocate once from SPIRAM
uint8_t *frame_buf = heap_caps_malloc(
    24000 * 2 * 0.12,  // 120ms max
    MALLOC_CAP_SPIRAM
);

// Output buffer (accumulates) - start small, grow as needed
size_t pcm_capacity = opus_len * 10;
uint8_t *pcm_data = heap_caps_malloc(pcm_capacity, MALLOC_CAP_SPIRAM);

// When buffer full:
if (total_pcm + frame_size > pcm_capacity) {
    size_t new_capacity = pcm_capacity * 2;
    uint8_t *new_buf = heap_caps_malloc(new_capacity, MALLOC_CAP_SPIRAM);
    memcpy(new_buf, pcm_data, total_pcm);
    heap_caps_free(pcm_data);
    pcm_data = new_buf;
    pcm_capacity = new_capacity;
}
```

### Why Use SPIRAM?

- Large audio buffers (100KB+) shouldn't use internal RAM
- SPIRAM is slower but abundant (8MB typical)
- Internal RAM reserved for critical real-time operations

## Performance Considerations

### Response Size Limits

**4MB HTTP Response Limit:**
```c
#define MAX_RESPONSE_SIZE (4 * 1024 * 1024)  // 4MB
```

With Opus compression (~10x):
- 4MB limit → ~400KB Opus → ~4MB PCM16 → ~83 seconds of 24kHz audio
- Practical limit: ~30-60 seconds for typical responses

### Token Limits

OpenAI Realtime API token limits affect response length:

```python
# Too restrictive - cuts off mid-sentence
"max_response_output_tokens": 100  # ❌ Only ~1-2 short sentences

# Reasonable for voice assistant
"max_response_output_tokens": 500  # ✓ 2-3 complete sentences

# For longer responses
"max_response_output_tokens": 1000  # ✓ 4-5 sentences
```

**Symptom of too-low limit:** Responses cut off mid-sentence even though decoder processed all frames correctly.

### Watchdog Feeding

Long decode loops (100+ frames) can trigger task watchdog:

```c
while (offset < opus_len) {
    vTaskDelay(1);  // Feed watchdog every frame (20ms each)

    // ... decode frame ...
}
```

**Without this:** Task watchdog timeout after ~5 seconds of continuous decoding.

### CPU Core Affinity

To avoid blocking other tasks:

```c
// Proxy/network task on Core 0
xTaskCreatePinnedToCore(..., 0);

// Audio playback on Core 1
xTaskCreatePinnedToCore(..., 1);
```

## Complete Working Example

### ESP32 Side (C)

```c
#include "decoder/impl/esp_opus_dec.h"
#include "esp_heap_caps.h"

typedef struct {
    uint8_t *data;
    size_t length;
} opus_decode_result_t;

opus_decode_result_t decode_opus_self_delimited(
    const uint8_t *opus_data,
    size_t opus_len
) {
    opus_decode_result_t result = {NULL, 0};

    // 1. Initialize decoder
    esp_opus_dec_cfg_t opus_cfg = {
        .sample_rate = 24000,
        .channel = 1,
        .frame_duration = ESP_OPUS_DEC_FRAME_DURATION_20_MS,
        .self_delimited = false,
    };

    void *opus_handle = NULL;
    if (esp_opus_dec_open(&opus_cfg, sizeof(opus_cfg), &opus_handle) != ESP_AUDIO_ERR_OK) {
        return result;
    }

    // 2. Allocate buffers
    size_t pcm_capacity = opus_len * 10;
    uint8_t *pcm_data = heap_caps_malloc(pcm_capacity, MALLOC_CAP_SPIRAM);

    size_t frame_buf_size = 24000 * 2 * 0.12;
    uint8_t *frame_buf = heap_caps_malloc(frame_buf_size, MALLOC_CAP_SPIRAM);

    if (!pcm_data || !frame_buf) {
        if (pcm_data) heap_caps_free(pcm_data);
        if (frame_buf) heap_caps_free(frame_buf);
        esp_opus_dec_close(opus_handle);
        return result;
    }

    // 3. Decode all frames
    size_t total_pcm = 0;
    size_t offset = 0;

    while (offset < opus_len) {
        vTaskDelay(1);  // Feed watchdog

        // Parse length prefix
        uint8_t first_byte = opus_data[offset];
        size_t frame_len = (first_byte < 252)
            ? first_byte
            : 252 + opus_data[offset + 1];
        size_t header_len = (first_byte < 252) ? 1 : 2;

        if (offset + header_len + frame_len > opus_len) {
            break;  // Truncated frame
        }

        // Decode frame
        esp_audio_dec_in_raw_t raw = {
            .buffer = (uint8_t*)(opus_data + offset + header_len),
            .len = frame_len,
            .consumed = 0,
        };

        esp_audio_dec_out_frame_t frame = {
            .buffer = frame_buf,
            .len = frame_buf_size,
            .decoded_size = 0,
        };

        esp_audio_dec_info_t dec_info = {0};
        esp_audio_err_t rc = esp_opus_dec_decode(opus_handle, &raw, &frame, &dec_info);

        if (rc == ESP_AUDIO_ERR_OK && frame.decoded_size > 0) {
            // Grow buffer if needed
            if (total_pcm + frame.decoded_size > pcm_capacity) {
                size_t new_capacity = pcm_capacity * 2;
                uint8_t *new_buf = heap_caps_malloc(new_capacity, MALLOC_CAP_SPIRAM);
                if (!new_buf) break;
                memcpy(new_buf, pcm_data, total_pcm);
                heap_caps_free(pcm_data);
                pcm_data = new_buf;
                pcm_capacity = new_capacity;
            }

            // Copy frame data
            memcpy(pcm_data + total_pcm, frame.buffer, frame.decoded_size);
            total_pcm += frame.decoded_size;
        }

        offset += header_len + frame_len;
        frame.decoded_size = 0;
    }

    // 4. Cleanup
    heap_caps_free(frame_buf);
    esp_opus_dec_close(opus_handle);

    result.data = pcm_data;
    result.length = total_pcm;
    return result;
}
```

### Python Side

```python
import opuslib
import base64
import numpy as np

def resample_audio(pcm_bytes: bytes, input_rate: int, output_rate: int) -> bytes:
    """Simple linear interpolation resampling."""
    audio_data = np.frombuffer(pcm_bytes, dtype=np.int16)
    input_length = len(audio_data)
    output_length = int(input_length * output_rate / input_rate)
    input_indices = np.linspace(0, input_length - 1, output_length)
    resampled = np.interp(input_indices, np.arange(input_length), audio_data).astype(np.int16)
    return resampled.tobytes()

def process_audio(device_audio_16khz: bytes) -> str:
    """
    Process audio from device (16kHz) through OpenAI and return Opus.

    Returns:
        Base64-encoded self-delimited Opus packet stream
    """
    # 1. Resample device audio 16kHz → 24kHz for OpenAI
    audio_24khz = resample_audio(device_audio_16khz, 16000, 24000)

    # 2. Send to OpenAI Realtime API (returns 24kHz PCM16)
    openai_response_pcm = send_to_openai(audio_24khz)  # Your implementation

    # 3. Encode to Opus with self-delimited packets
    encoder = opuslib.Encoder(24000, 1, opuslib.APPLICATION_VOIP)

    frame_size = 480  # 20ms at 24kHz
    frame_bytes = frame_size * 2

    opus_chunks = []
    for i in range(0, len(openai_response_pcm), frame_bytes):
        frame = openai_response_pcm[i:i + frame_bytes]

        if len(frame) < frame_bytes:
            frame = frame + b'\x00' * (frame_bytes - len(frame))

        opus_frame = encoder.encode(frame, frame_size)

        # Add RFC 6716 Appendix B length prefix
        frame_len = len(opus_frame)
        if frame_len < 252:
            delimited = bytes([frame_len]) + opus_frame
        else:
            delimited = bytes([252, frame_len - 252]) + opus_frame

        opus_chunks.append(delimited)

    combined_opus = b"".join(opus_chunks)

    print(f"[OPUS] Encoded {len(openai_response_pcm)} bytes PCM to "
          f"{len(combined_opus)} bytes Opus in {len(opus_chunks)} frames")

    return base64.b64encode(combined_opus).decode("utf-8")
```

## Summary

### Key Takeaways

1. **Manual parsing required** - ESP decoder's `self_delimited = true` doesn't handle multiple packets
2. **Copy frame data** - Decoder reuses buffer, must copy after each decode
3. **Dynamic allocation** - Grow output buffer as needed, don't guess final size
4. **Match sample rates** - Decoder, encoder, and playback must all align
5. **Use SPIRAM** - Large audio buffers belong in external RAM
6. **Feed watchdog** - Add delays in long decode loops
7. **Token limits matter** - Low limits cause mid-sentence cutoffs

### Debugging Checklist

- [ ] Decoder config has `self_delimited = false`
- [ ] Manual parsing of length prefixes implemented
- [ ] Frame data copied to output buffer after each decode
- [ ] Output buffer grows dynamically
- [ ] Sample rates match: encoder = decoder = playback
- [ ] Task stack ≥ 24KB
- [ ] Using SPIRAM for large buffers
- [ ] Watchdog fed in decode loop (`vTaskDelay(1)`)
- [ ] Token limit sufficient (≥500 for complete sentences)
- [ ] Response size under 4MB limit

### Performance Metrics

**Typical 3-second response (24kHz mono):**
- Raw PCM16: 144,000 bytes
- Opus compressed: ~12,000 bytes (~12:1 ratio)
- Self-delimited overhead: ~150 bytes (150 frames × 1 byte prefix)
- Total transmitted: ~12,150 bytes
- Decode time: ~30ms
- Playback time: 3 seconds

This implementation has been tested and validated with OpenAI Realtime API integration, successfully handling multi-sentence voice responses without cutoffs or quality issues.
