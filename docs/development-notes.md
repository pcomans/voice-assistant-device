# ESP32-S3-Touch-LCD-1.85 Audio Development Guide

**Last Updated**: November 2025
**Current Architecture**: HTTP Streaming (Option C)

## Current Implementation Overview

This document covers hardware-level audio implementation details. For architecture and API documentation, see:
- **Architecture**: `../README.md` - Current streaming implementation
- **Proxy API**: `../../smart_assistant_proxy/README.md`
- **Future Plans**: `implementation-plan-webrtc.md`

### Audio Flow (Current)

**Capture**:
```
MEMS Mic → I2S1 (32-bit) → 16-bit conversion (>>14) → 100ms chunks → HTTP POST to proxy
```

**Playback**:
```
HTTP stream (raw 24kHz PCM) → FreeRTOS Ring Buffer → I2S0 (16-bit) → PCM5101 DAC → Speaker
```

**Key Characteristics**:
- **No custom PCM buffer** for capture (streaming chunks sent immediately)
- **FreeRTOS ring buffer** for playback (built-in, 2-second pre-buffering)
- **Upload**: Base64-encoded PCM in JSON payload (for HTTP compatibility)
- **Download**: Raw binary PCM stream (no JSON, no base64 decoding needed)
- **Latency**: 176-426ms total (10x better than previous batch processing)

---

## Hardware Overview

**Board**: Waveshare ESP32-S3-Touch-LCD-1.85
**MCU**: ESP32-S3 (dual-core Xtensa LX7, 8MB PSRAM, WiFi/BLE)
**Audio Components**:
- **MEMS Microphone**: Connected via I2S1
- **DAC/Speaker**: PCM5101 connected via I2S0
- **Demo Code**: `/Users/philipp/code/esp-idf-projects/ESP-IDF/ESP32-S3-Touch-LCD-1.85C-Test/`

---

## I2S Audio Capture (MEMS Microphone)

### Hardware Configuration
- **I2S Port**: I2S_NUM_1
- **GPIO Pins**:
  - BCLK: GPIO 15
  - WS (LRCLK): GPIO 2
  - DIN: GPIO 39
  - MCLK: Not connected

### Critical Settings from Demo Code

**Reference**: `ESP-IDF/ESP32-S3-Touch-LCD-1.85C-Test/main/MIC_Driver/MIC_Speech.c` lines 26-41

```c
// MEMS mic outputs 32-bit I2S data
i2s_std_config_t std_cfg = I2S_CONFIG_DEFAULT(
    16000,                        // Sample rate: 16 kHz
    I2S_SLOT_MODE_MONO,          // Mono audio
    I2S_DATA_BIT_WIDTH_32BIT     // 32-bit I2S (NOT 16-bit!)
);

// Use RIGHT channel for this MEMS mic
std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_RIGHT;
```

### Data Conversion Required

MEMS microphones output 24/32-bit data that must be converted to 16-bit PCM:

**Reference**: `MIC_Speech.c` lines 59-64

```c
// Read 32-bit I2S samples
i2s_channel_read(rx_handle, i2s_buffer, bytes, &bytes_read, portMAX_DELAY);

// Convert: Right-shift by 14 bits (amplifies and converts to 16-bit)
for (int i = 0; i < sample_count; i++) {
    pcm_buffer[i] = (int16_t)(i2s_buffer[i] >> 14);
}
```

**Why right-shift 14 bits?**
- Bits 29:13 contain the actual signal (amplified)
- Bits 12:0 are low/padding bits
- This conversion amplifies the mic signal for better SNR

### Key Patterns

1. **Always use `portMAX_DELAY`** for I2S reads - ensures data is ready
2. **Keep I2S channel running** - don't repeatedly enable/disable
3. **Use `I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG`** for capture (PHILIPS with 'S')
4. **Pin capture task to core 0** for consistent performance

---

## I2S Audio Playback (Speaker/DAC)

### Hardware Configuration
- **I2S Port**: I2S_NUM_0
- **DAC Chip**: PCM5101
- **GPIO Pins**:
  - BCLK: GPIO 48
  - WS (LRCLK): GPIO 38
  - DOUT: GPIO 47
  - MCLK: Not connected

### Critical Settings from Demo Code

**Reference**: `ESP-IDF/ESP32-S3-Touch-LCD-1.85C-Test/main/Audio_Driver/PCM5101.c` lines 78-84

```c
i2s_std_config_t std_cfg = {
    .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100),  // Or 16000 for voice
    .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(  // PHILIP without 'S'!
        I2S_DATA_BIT_WIDTH_16BIT,
        I2S_SLOT_MODE_MONO  // or STEREO
    ),
    .gpio_cfg = BSP_I2S_GPIO_CFG,
};

i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
chan_cfg.auto_clear = true;  // Prevents audio artifacts
```

### CRITICAL: PHILIP vs PHILIPS

| Use Case | Macro | Notes |
|----------|-------|-------|
| **Capture (Mic)** | `I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG` | With 'S' |
| **Playback (Speaker)** | `I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG` | Without 'S' |

**Do NOT** manually set `slot_mask` for playback - use the default both-channel configuration even for mono. Setting `slot_mask` causes silent playback on this hardware.

### Software Volume Control

**Reference**: `PCM5101.c` lines 13-26

```c
// In-place volume scaling before I2S write
int16_t *samples = (int16_t *)audio_buffer;
size_t sample_count = len / sizeof(int16_t);
float volume_factor = volume_percent / 100.0f;  // 0-100 → 0.0-1.0

for (size_t i = 0; i < sample_count; i++) {
    samples[i] = (int16_t)(samples[i] * volume_factor);
}

i2s_channel_write(tx_chan, audio_buffer, len, &bytes_written, timeout);
```

### Key Patterns

1. **Non-blocking playback**: Run I2S writes in background task
2. **Event callbacks**: Notify app of STARTED/COMPLETED/ERROR events
3. **Pin playback task to core 1** (demo uses `coreID = 1`)
4. **Dynamic sample rate**: Reconfigure I2S clock without reinitializing channel

---

## Memory Management Best Practices

### SPIRAM Allocation Pattern

**Reference**: Demo code allocates large buffers from PSRAM when available

```c
// Try SPIRAM first, fall back to internal RAM
uint8_t *buffer = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
if (!buffer) {
    buffer = heap_caps_malloc(size, MALLOC_CAP_INTERNAL);
}
assert(buffer != NULL && "Critical allocation failed");
```

### Audio Buffer Sizing

With 8MB PSRAM, you can support long recordings:

| Duration | Memory (16kHz mono 16-bit) | Use Case |
|----------|---------------------------|----------|
| 3 sec | 96 KB | Very short phrases |
| 5 sec | 160 KB | Short questions |
| 10 sec | 320 KB | Normal conversations |
| 15 sec | 480 KB | Longer explanations |
| 30 sec | 960 KB | Extended speech |
| 60 sec | 1.92 MB | Long-form recording |

Formula: `bytes = sample_rate × duration_sec × 2 (bytes per sample)`

### Task Stack Sizes

Based on demo code and current implementation:
- Audio capture (streaming): 4KB
- Audio playback (buffered): 8KB (higher priority, needs more stack for ring buffer operations)
- Network/HTTP tasks: 24KB (proxy client with TLS)
- LVGL UI: 4KB

---

## Common Pitfalls & Solutions

### 1. No Audio Capture
**Symptom**: Silent recording, all zeros
**Cause**: Using 16-bit I2S width instead of 32-bit
**Fix**: Use `I2S_DATA_BIT_WIDTH_32BIT` and apply >>14 bit-shift conversion

### 2. Silent Speaker
**Symptom**: I2S writes succeed but no audio output
**Cause**: Using wrong macro or setting slot_mask
**Fix**:
- Use `I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG` (without 'S')
- Do NOT set `slot_cfg.slot_mask`
- Ensure `auto_clear = true`

### 3. Distorted/Clipped Audio
**Symptom**: Audio plays but sounds wrong
**Cause**: Missing or incorrect bit-shift conversion
**Fix**: Apply `>> 14` when converting 32-bit I2S to 16-bit PCM

### 4. I2S Read Failures (ESP_ERR_TIMEOUT)
**Symptom**: `i2s_channel_read()` returns error 263
**Cause**: Using timeout instead of blocking read
**Fix**: Use `portMAX_DELAY` for blocking reads (demo pattern)

### 5. UI Freezing During Playback
**Symptom**: Display unresponsive while audio plays
**Cause**: Synchronous I2S write in main task
**Fix**:
- Move playback to background FreeRTOS task
- Use callbacks for completion events
- Pin to core 1 (separate from UI/network on core 0)

### 6. Memory Allocation Failures
**Symptom**: malloc returns NULL for large buffers
**Cause**: Not using SPIRAM
**Fix**: Use `heap_caps_malloc(size, MALLOC_CAP_SPIRAM)` pattern above

---

## Demo Code Reference Locations

### Audio Playback (Speaker/DAC)
- **File**: `ESP-IDF/ESP32-S3-Touch-LCD-1.85C-Test/main/Audio_Driver/PCM5101.c`
- **Init**: Lines 78-100 (I2S configuration, GPIO setup)
- **Volume**: Lines 13-26 (in-place PCM scaling)
- **Reconfig**: Lines 27-39 (dynamic sample rate change)

### Audio Capture (MEMS Mic)
- **File**: `ESP-IDF/ESP32-S3-Touch-LCD-1.85C-Test/main/MIC_Driver/MIC_Speech.c`
- **Init**: Lines 26-41 (I2S setup with 32-bit width)
- **Conversion**: Lines 59-64 (32-bit → 16-bit PCM with amplification)
- **Read Loop**: Line 59 (portMAX_DELAY pattern)

### GPIO Definitions
- **File**: `ESP-IDF/ESP32-S3-Touch-LCD-1.85C-Test/main/Audio_Driver/PCM5101.h`
- **Pins**: Lines 15-19 (I2S GPIO mapping)
- **Macros**: Lines 36-41 (BSP_I2S_DUPLEX_MONO_CFG)

---

## Quick Start Checklist

When implementing audio on this board:

**Capture (Microphone)**:
- [ ] Use I2S_NUM_1, GPIO 15/2/39
- [ ] Configure 32-bit I2S width
- [ ] Use `I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG` (with 'S')
- [ ] Set `slot_mask = I2S_STD_SLOT_RIGHT`
- [ ] Apply `>> 14` bit-shift conversion to 16-bit
- [ ] Use `portMAX_DELAY` for reads
- [ ] Pin task to core 0

**Playback (Speaker)**:
- [ ] Use I2S_NUM_0, GPIO 48/38/47
- [ ] Configure 16-bit I2S width
- [ ] Use `I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG` (without 'S')
- [ ] Do NOT set slot_mask manually
- [ ] Set `auto_clear = true`
- [ ] Implement volume control (0-100%)
- [ ] Use callback-based async playback
- [ ] Pin task to core 1

**Memory**:
- [ ] Allocate large buffers from SPIRAM
- [ ] Add assert() for critical allocations
- [ ] Choose appropriate buffer duration (recommend 10-30 sec)

---

## Testing & Validation

### Microphone Test
```c
// Read samples and log first 10 values
int16_t samples[256];
// ... read and convert from I2S ...
ESP_LOGI(TAG, "Samples: %d %d %d %d %d %d %d %d %d %d",
         samples[0], samples[1], samples[2], samples[3], samples[4],
         samples[5], samples[6], samples[7], samples[8], samples[9]);
```
**Expected**: Non-zero values, range typically -3000 to +3000 for normal speech

### Speaker Test
```c
// Generate 440Hz sine wave (A note)
for (int i = 0; i < sample_count; i++) {
    samples[i] = (int16_t)(3000 * sin(2 * M_PI * 440 * i / 16000));
}
i2s_channel_write(tx_chan, samples, bytes, &written, portMAX_DELAY);
```
**Expected**: Clear tone from speaker

---

## ESP32-S3 SoC & SPIRAM Configuration

### SPIRAM/PSRAM Overview

The ESP32-S3 on this board includes **8MB of external PSRAM (SPIRAM)** which is essential for audio applications requiring large buffers.

**Key specifications**:
- Internal RAM: ~512KB (shared between code, data, heap)
- External PSRAM: 8MB (via Octal SPI interface)
- PSRAM speed: 80MHz (configurable)
- Access: Via `heap_caps_malloc(size, MALLOC_CAP_SPIRAM)`

### Critical Configuration: sdkconfig vs sdkconfig.defaults

**Problem**: After `idf.py fullclean`, SPIRAM was disabled despite being in `sdkconfig.defaults`

**Root cause**: The build system doesn't automatically regenerate `sdkconfig` from `sdkconfig.defaults` after a full clean.

**Solution**:
```bash
# After fullclean, always regenerate sdkconfig
rm sdkconfig
idf.py reconfigure
```

**Required settings in `sdkconfig.defaults`**:
```
# Enable SPIRAM (8MB PSRAM on ESP32-S3-Touch-LCD-1.85)
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_SPIRAM_USE_CAPS_ALLOC=y
```

**Verification after reconfigure**:
```bash
# Check that SPIRAM is enabled
grep -E "^CONFIG_SPIRAM" sdkconfig

# Expected output:
# CONFIG_SPIRAM=y
# CONFIG_SPIRAM_MODE_OCT=y
# CONFIG_SPIRAM_SPEED_80M=y
# CONFIG_SPIRAM_USE_CAPS_ALLOC=y
```

### SPIRAM Allocation Best Practices

**Critical lesson learned**: For large buffers (>100KB), **NEVER use internal RAM fallback** - it will always fail.

#### Anti-Pattern (DO NOT USE):
```c
// BAD: Fallback to internal RAM is pointless for large buffers
uint8_t *buffer = heap_caps_malloc(320000, MALLOC_CAP_SPIRAM);
if (!buffer) {
    buffer = heap_caps_malloc(320000, MALLOC_CAP_INTERNAL);  // ❌ Will ALWAYS fail!
}
```

#### Correct Pattern (SPIRAM-only):
```c
// GOOD: Explicitly require SPIRAM, fail clearly if unavailable
uint8_t *buffer = heap_caps_malloc(320000, MALLOC_CAP_SPIRAM);
if (!buffer) {
    ESP_LOGE(TAG, "Failed to allocate %u bytes from SPIRAM", size);
    return ESP_ERR_NO_MEM;
}
```

**Why this matters**:
- Internal RAM is limited (~512KB total, much less available for heap)
- Large allocations (320KB+) will **never** fit in internal RAM
- Failing fast with clear error messages is better than misleading fallback attempts

### When to Use SPIRAM vs Internal RAM

| Allocation Size | Memory Type | Rationale |
|----------------|-------------|-----------|
| < 4KB | Internal RAM | Fast access, low latency |
| 4KB - 32KB | Either (prefer internal) | Balance speed vs availability |
| 32KB - 100KB | SPIRAM | Too large for internal RAM fragmentation |
| > 100KB | **SPIRAM only** | Will not fit in internal RAM |

**Audio buffer examples**:
- Ring buffer (320KB): SPIRAM only
- Playback buffer copy: SPIRAM only
- Base64 encoding buffer: SPIRAM only
- JSON payload with audio: SPIRAM only
- Task control structures (<1KB): Internal RAM OK

### Ring Buffers in Current Implementation

**Current usage**: FreeRTOS ring buffer for audio **playback only** (not capture).

**Dynamic allocation** (used in `audio_playback.c`):
```c
// Create 96KB ring buffer for 2 seconds of 24kHz audio
RingbufHandle_t s_stream_buffer = xRingbufferCreate(96000, RINGBUF_TYPE_BYTEBUF);
```

**Why dynamic allocation works**:
- Playback buffer is modest size (96KB for 2 seconds)
- FreeRTOS allocates from SPIRAM when configured (`CONFIG_SPIRAM=y`)
- Simpler than static allocation for this use case

**Ring buffer behavior**:
- Producer: HTTP stream writes incoming audio chunks
- Consumer: Playback task reads for I2S output
- Pre-buffering: Waits for 500ms of audio before starting playback
- Prevents audio stuttering on network jitter

**No ring buffer for capture**: Audio chunks are sent immediately via HTTP as they're captured (100ms chunks). No need to buffer on device.

### SPIRAM Performance Characteristics

**Access speed**:
- Internal RAM: ~240 MHz (full CPU speed)
- SPIRAM (80MHz mode): ~80 MHz via Octal SPI
- Latency: SPIRAM has higher latency but sufficient for audio buffering

**When SPIRAM is fast enough**:
- Sequential reads/writes (streaming audio)
- Large buffer operations (memcpy, base64 encoding)
- Background processing (playback task)

**When to avoid SPIRAM**:
- Interrupt handlers (use internal RAM)
- Time-critical small allocations
- Frequently accessed control structures

### Debugging SPIRAM Issues

**Verify SPIRAM at runtime**:
```c
size_t spiram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
ESP_LOGI(TAG, "SPIRAM available: %zu bytes", spiram_free);
// Should show ~8MB available on this board
```

If SPIRAM shows 0 bytes:
1. Check `sdkconfig` has `CONFIG_SPIRAM=y`
2. If missing, run `rm sdkconfig && idf.py reconfigure`
3. Verify `sdkconfig.defaults` has SPIRAM settings
4. Rebuild: `idf.py build`

---

## Notes

- This guide is hardware-specific for the Waveshare ESP32-S3-Touch-LCD-1.85
- Current project uses ESP-IDF v5.5.1 (latest stable LTS)
- Hardware setup sections are current and validated
- Memory management sections reflect lessons learned during development
- Some SPIRAM examples reference historical `pcm_buffer` (removed Nov 2025) but principles remain valid
- Current implementation in `/smart_assistant_device/main/` uses streaming architecture
