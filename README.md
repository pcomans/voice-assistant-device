# Smart Assistant Device

ESP32-S3 voice assistant device using OpenAI Realtime API for natural voice conversations.

## What is this?

This firmware turns a Waveshare ESP32-S3 Touch LCD 1.85C development board into a voice assistant with continuous conversation support. Click to unmute, speak naturally, and get AI-generated voice responses streamed back in real-time.

**Key Features:**
- **Continuous streaming**: Audio streams continuously with OpenAI Server VAD detecting speech
- **Auto-muting**: Mic automatically mutes during AI responses to prevent feedback
- **Natural conversations**: Server VAD handles turn detection - no manual "end of turn" signaling
- **Real-time I2S audio**: 16kHz microphone input, 24kHz speaker output
- **Touch LCD UI**: Large finger-friendly unmute/mute button
- **WebSocket streaming**: Persistent connection to proxy for low-latency responses

## Architecture

```
┌─────────────────┐         WebSocket          ┌──────────────┐         WebSocket         ┌─────────────┐
│   ESP32-S3      │◄──────────────────────────►│    Proxy     │◄────────────────────────►│   OpenAI    │
│   Device        │  Binary PCM Audio (16kHz)  │    Server    │  JSON + PCM (24kHz)      │  Realtime   │
│                 │                             │              │                          │     API     │
│  • I2S Mic      │                             │  • Resamples │                          │             │
│  • I2S Speaker  │                             │  • Routes    │                          │ • Server    │
│  • Touch UI     │                             │  • Logs      │                          │   VAD       │
│  • Auto-mute    │                             │              │                          │ • TTS       │
└─────────────────┘                             └──────────────┘                          └─────────────┘

Flow:
1. User clicks "Unmute" → mic enabled
2. Device continuously streams audio → Proxy → OpenAI
3. Server VAD detects speech start/end automatically
4. OpenAI generates response → streams back to device
5. Device detects incoming audio → auto-mutes mic (prevents feedback)
6. Response plays through speaker
7. After 2s silence → mic auto-unmutes if user hasn't clicked "Mute"
```

### Continuous Streaming with Auto-Mute

Unlike traditional push-to-talk or VAD-gated systems, this device streams audio continuously:

- **Mic enabled**: Streams real microphone audio to OpenAI
- **Mic disabled**: Streams silence (prevents accidental triggering)
- **Auto-mute during AI speech**: When audio is received from OpenAI, mic automatically mutes for 2 seconds past the last audio chunk (accounts for buffering and safety margin)

This architecture leverages OpenAI's Server VAD which is designed for continuous streaming and handles:
- Speech detection
- Background noise filtering
- Automatic turn detection
- Response generation

## Hardware Required

- [Waveshare ESP32-S3 Touch LCD 1.85C](https://www.waveshare.com/esp32-s3-touch-lcd-1.85c.htm)
  - ESP32-S3-WROOM-1-N16R8 module
  - 16MB Flash, 8MB PSRAM
  - 1.85" ST77916 LCD (360x360 RGB)
  - CST816 capacitive touch controller
  - MAX98357A I2S audio amplifier
  - MSM261S4030H0 I2S MEMS microphone
  - Built-in speaker

## Prerequisites

### 1. Install ESP-IDF 5.5.1

This project requires **ESP-IDF v5.5.1** (the latest stable LTS release). Do not use v6.x as it contains breaking API changes.

Follow the [ESP-IDF Getting Started Guide](https://docs.espressif.com/projects/esp-idf/en/v5.5.1/esp32s3/get-started/index.html) to install ESP-IDF v5.5.1.

**Quick setup:**
```bash
# Clone ESP-IDF into version-specific directory
mkdir -p ~/esp/v5.5.1
cd ~/esp/v5.5.1
git clone -b v5.5.1 --recursive https://github.com/espressif/esp-idf.git esp-idf

# Install tools
cd esp-idf
./install.sh esp32s3

# Set up environment (add to your .bashrc/.zshrc)
. $HOME/esp/v5.5.1/esp-idf/export.sh
```

**Verify installation:**
```bash
idf.py --version
# Should show: ESP-IDF v5.5.1
```

**Note:** Using a version-specific directory (`v5.5.1/`) allows you to maintain multiple ESP-IDF versions side-by-side.

### 2. Set up the Proxy Server

This device requires a companion proxy server to interface with OpenAI's Realtime API. The proxy:
- Maintains persistent WebSocket connection to OpenAI
- Handles authentication and API key management
- Resamples audio (16kHz → 24kHz for OpenAI)
- Routes audio streams between device and OpenAI

See the [smart_assistant_proxy](../smart_assistant_proxy/) directory for installation and setup instructions.

**The proxy must be running and accessible on your local network before using the device.**

## Configuration

### 1. WiFi Credentials

Copy the WiFi credentials template and add your network details:

```bash
cp main/wifi_credentials.template.h main/wifi_credentials.h
```

Edit `main/wifi_credentials.h`:
```c
#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"
#define WEBSOCKET_URL "ws://YOUR_SERVER_IP:8000/ws"
```

**Important:**
- ESP32-S3 only supports 2.4GHz WiFi networks
- `wifi_credentials.h` is gitignored to protect your credentials
- `WEBSOCKET_URL` is required and replaces the old hardcoded proxy configuration

**Finding your proxy IP:**
- **macOS/Linux**: `ifconfig | grep "inet " | grep -v 127.0.0.1`
- **Windows**: `ipconfig`

Look for your local network IP (usually starts with `192.168.x.x` or `10.x.x.x`).

### 3. Authentication Token (Optional)

The device uses a shared secret token to authenticate with the proxy. The default token is already configured in `main/proxy_client.c`:

```c
#define PROXY_DEFAULT_TOKEN "498b1b65-26a3-49e8-a55e-46a0b47365e2"
```

**For first-time setup:** This default token should already be configured in your proxy's `.env` file.

**For enhanced security:**
1. Generate a new UUID or secure random string
2. Update `PROXY_DEFAULT_TOKEN` in `main/proxy_client.c`
3. Update `ASSISTANT_SHARED_SECRET` in the proxy's `.env` file
4. Rebuild and reflash the device firmware

## Build and Flash

### 1. Set up ESP-IDF environment

```bash
. ~/esp/v5.5.1/esp-idf/export.sh  # Or your ESP-IDF installation path
```

Add this to your `.bashrc` or `.zshrc` for automatic activation.

### 2. Set target chip (first time only)

```bash
idf.py set-target esp32s3
```

This configures the build system for ESP32-S3 and generates `sdkconfig`.

### 3. Build the firmware

```bash
idf.py build
```

Build output will be in `build/` directory:
- `build/smart_assistant_device.bin` - Main application
- `build/bootloader/bootloader.bin` - Bootloader
- `build/partition_table/partition-table.bin` - Partition table

### 4. Connect the device

Connect the Waveshare ESP32-S3 board to your computer via USB-C. The board will appear as a serial device.

### 5. Flash to device

**Using idf.py:**
```bash
idf.py flash
```

The tool will automatically detect the serial port. If multiple devices are connected, specify the port:
```bash
idf.py -p /dev/ttyUSB0 flash  # Linux
idf.py -p /dev/cu.usbmodem101 flash  # macOS
idf.py -p COM3 flash  # Windows
```

**Using the flash script (macOS/Linux):**
```bash
./flash.sh
```

This script automatically:
- Finds the USB serial port
- Kills any processes using the port
- Flashes the firmware
- Shows completion status

### 6. Flash and monitor in one command

```bash
idf.py flash monitor
```

This flashes the firmware and immediately starts showing logs.

## View Logs

To view real-time logs from the device:

```bash
idf.py monitor
```

Or specify the port:
```bash
idf.py -p /dev/cu.usbmodem101 monitor  # macOS
idf.py -p /dev/ttyUSB0 monitor         # Linux
idf.py -p COM3 monitor                 # Windows
```

**Exit monitor:** Press `Ctrl+]`

### Useful monitor features

```bash
# Filter logs by tag
idf.py monitor --print-filter "smart_assistant"

# Increase baud rate for faster output
idf.py -p /dev/cu.usbmodem101 -b 460800 monitor

# Save logs to file
idf.py monitor | tee device.log

# Monitor with timestamps
idf.py monitor | ts '[%Y-%m-%d %H:%M:%S]'
```

### Key log tags to watch

- `smart_assistant`: Main application state and events
- `audio_ctrl`: Microphone capture and audio levels
- `audio_playback`: Speaker output and buffering
- `ws_client`: WebSocket connection status
- `proxy_client`: Proxy communication
- `ui`: Touch button events

## Usage

### Basic Operation

1. **Start the proxy server** on your local network (see proxy documentation)
2. **Power on the device** - it will automatically:
   - Connect to WiFi (configured in `wifi_credentials.h`)
   - Connect to proxy via WebSocket
   - Initialize audio and display
3. **Wait for the "Unmute" button** to appear on the touchscreen
4. **Click "Unmute"** to enable the microphone
5. **Speak naturally** - Server VAD will detect when you start/stop speaking
6. **AI response streams back** automatically - mic mutes during playback
7. **Continue conversation** or click "Mute" when done

### Button Behavior

- **Unmute → Mute**: Toggle button that enables/disables the microphone
- **Not push-to-talk**: You don't need to hold the button while speaking
- **Auto-mute during AI**: Mic automatically mutes when AI is speaking (prevents acoustic feedback)
- **Auto-unmute after AI**: Mic auto-unmutes 2 seconds after AI finishes (if you haven't clicked "Mute")

### Expected Boot Sequence

```
I (0) cpu_start: Starting scheduler on APP CPU.
I (123) smart_assistant: Starting smart assistant device
I (456) wifi_station: WiFi connecting...
I (2341) wifi_station: WiFi connected
I (2341) wifi_station: Got IP: 192.168.1.42
I (2456) ws_client: WebSocket connected
I (2456) proxy_client: WebSocket connected to proxy
I (2456) smart_assistant: WebSocket connected - starting continuous audio streaming
I (2567) audio_ctrl: Streaming capture task started
I (2567) audio_ctrl: Using raw microphone (no processing)
I (2567) ui: UI initialised
```

### Understanding the Logs

**Normal operation logs:**
```
I (7248) smart_assistant: Button pressed - enabling microphone
I (7348) smart_assistant: Mic active, audio level: avg=95, samples=1600
I (11288) smart_assistant: AI audio received (3840 bytes, chunk #1)
I (11298) smart_assistant: Auto-muting mic (AI speaking, 12 ms since last audio)
I (11298) audio_playback: Pre-buffer complete (24000 bytes), playback task will start consuming
I (13456) smart_assistant: Auto-unmuting mic (AI finished, 2034 ms since last audio)
```

**Troubleshooting logs:**
```
W (5000) smart_assistant: Cannot enable mic: WebSocket not connected
E (12345) ws_client: WebSocket error occurred
W (12345) proxy_client: WebSocket disconnected from proxy
```

## Project Structure

```
smart_assistant_device/
├── main/
│   ├── app_main.c              # Application entry point and state machine
│   ├── smart_assistant.h       # Global state and data structures
│   │
│   ├── audio_controller.c/h    # I2S microphone capture (16kHz, raw PCM)
│   ├── audio_playback.c/h      # I2S speaker output (24kHz) with ring buffer
│   ├── audio_resampler.c/h     # Audio resampling utilities
│   │
│   ├── websocket_client.c/h    # WebSocket client (binary PCM streaming)
│   ├── proxy_client.c/h        # Proxy connection management
│   │
│   ├── ui.c/h                  # LVGL touch UI and button controls
│   │
│   ├── drivers/
│   │   ├── lcd/
│   │   │   ├── ST77916.c/h             # LCD hardware initialization
│   │   │   └── esp_lcd_st77916/        # ST77916 display driver
│   │   ├── touch/
│   │   │   ├── CST816.c/h              # Touch controller
│   │   │   └── esp_lcd_touch/          # Touch interface
│   │   ├── i2c/I2C_Driver.c/h          # I2C bus management
│   │   ├── exio/TCA9554PWR.c/h         # GPIO expander
│   │   └── lvgl/LVGL_Driver.c/h        # LVGL display/touch integration
│   │
│   ├── wifi_credentials.h      # WiFi config (gitignored, copy from template)
│   ├── wifi_credentials.template.h  # WiFi config template
│   │
│   ├── CMakeLists.txt          # Build configuration
│   └── idf_component.yml       # Component dependencies
│
├── docs/
│   └── hypotheses.md           # Technical debugging notes
│
├── build/                      # Build output (generated)
├── managed_components/         # Downloaded component dependencies
│
├── flash.sh                    # Convenience flash script
├── sdkconfig                   # ESP-IDF configuration
├── sdkconfig.defaults          # Default configuration overrides
├── partitions.csv              # Flash partition table
│
├── README.md                   # This file
└── LICENSE                     # MIT License
```

## Audio Pipeline

### Capture Path (Microphone → OpenAI)

```
I2S Microphone (32-bit)
    ↓ (>> 14 bit shift)
Raw 16-bit PCM @ 16kHz
    ↓ (100ms chunks, 1600 samples)
Auto-mute logic (silence if muted)
    ↓ (binary WebSocket frames)
Proxy Server
    ↓ (resample 16kHz → 24kHz)
OpenAI Realtime API (Server VAD)
```

**Key components:**
- **I2S channel**: RIGHT slot, 16kHz sample rate
- **Chunk size**: 100ms (1600 samples @ 16kHz = 3200 bytes)
- **Auto-mute**: Sends pre-allocated silence buffer when muted
- **No processing**: Raw microphone audio, no AEC/AGC/VAD on device

### Playback Path (OpenAI → Speaker)

```
OpenAI Realtime API (PCM @ 24kHz)
    ↓ (binary WebSocket frames)
Proxy Server (forwards as-is)
    ↓ (binary WebSocket frames)
Device WebSocket Client
    ↓ (ring buffer)
Audio Playback Task
    ↓ (pre-buffer 24000 bytes = 0.5s)
I2S Speaker @ 24kHz
```

**Key components:**
- **Ring buffer**: 32KB in PSRAM
- **Pre-buffer**: 24000 bytes (500ms) before playback starts
- **I2S format**: 16-bit PCM, 24kHz, mono
- **Timestamp tracking**: Auto-mute logic monitors last audio received

### Auto-Mute Behavior

The device tracks when audio is received from OpenAI and automatically mutes the microphone:

```c
#define AI_SPEAKING_TIMEOUT_MS 2000  // 2s after last audio chunk

// Timeline:
// t=0ms:    AI audio chunk received → update timestamp
// t=100ms:  Another chunk → update timestamp
// t=200ms:  Another chunk → update timestamp
// ...
// t=5000ms: Last chunk received → update timestamp
// t=5100ms: Mic still muted (only 100ms since last audio)
// t=7000ms: Mic auto-unmutes (2000ms+ since last audio, pre-buffer + safety margin)
```

This prevents acoustic feedback while allowing natural conversation flow.

## Memory Usage

- **Flash**: ~1.2 MB (60% free of 3 MB app partition)
- **Internal RAM**: ~150 KB (for tasks, stacks, buffers)
- **PSRAM (8 MB)**:
  - LVGL display buffers: ~520 KB (2× 360×360×2 bytes)
  - Audio ring buffer: 32 KB
  - Silence buffer: 4 KB
  - Audio task stacks: ~8 KB
  - Remaining: ~7.4 MB free

## Troubleshooting

### WiFi Connection Issues

**Symptoms:**
```
W (5000) wifi_station: WiFi connecting...
E (30000) wifi_station: WiFi connection failed
```

**Solutions:**
- Verify WiFi credentials in `main/wifi_credentials.h`
- Ensure 2.4GHz network (ESP32 doesn't support 5GHz)
- Check WiFi signal strength
- Try disabling WiFi encryption temporarily to test
- Check router allows new device connections

### Proxy Connection Fails

**Symptoms:**
```
E (5000) ws_client: WebSocket connection failed
W (5000) proxy_client: WebSocket disconnected from proxy
```

**Solutions:**
- Verify proxy server is running: `curl http://PROXY_IP:8000/healthz`
- Check proxy URL in `main/proxy_client.c` matches proxy IP
- Ensure device and proxy are on same network (check IP subnet)
- Check firewall settings allow port 8000
- Verify authentication token matches between device and proxy
- Test WebSocket from another device: `wscat -c ws://PROXY_IP:8000/ws`

### Audio Quality Issues

**No microphone input:**
```
I (7248) smart_assistant: Mic active, audio level: avg=0, samples=1600
```
- Microphone may be faulty
- Check I2S wiring/configuration
- Test with known working firmware

**Distorted/buzzing audio:**
- Check speaker connections
- Reduce volume if clipping
- Verify sample rate configuration (16kHz mic, 24kHz speaker)

**Audio cutting out:**
- Check WiFi signal strength
- Monitor proxy logs for dropped connections
- Check for memory issues: `idf.py monitor | grep "heap"`

### Self-Interruption (AI interrupts itself)

**Symptoms:** AI starts responding to its own voice during playback

**Cause:** Auto-mute not working properly

**Check logs for:**
```
I (11298) smart_assistant: Auto-muting mic (AI speaking, 12 ms since last audio)
I (13456) smart_assistant: Auto-unmuting mic (AI finished, 2034 ms since last audio)
```

**If missing these logs:**
- Verify firmware is latest version
- Check `s_last_audio_received_us` is being updated
- Increase `AI_SPEAKING_TIMEOUT_MS` if needed

**If still occurring:**
- Reduce speaker volume (acoustic feedback through case)
- Increase timeout: edit `AI_SPEAKING_TIMEOUT_MS` in `app_main.c`
- Check for hardware issues (poor isolation between mic and speaker)

### Display Issues

**Screen distortion or SPI errors:**
```
E (5497) lcd_panel.io.spi: spi transmit (queue) color failed
```

**Cause:** Memory allocation conflicts between audio and display

**Solution:** Verify audio buffers use PSRAM, not internal RAM
```c
// In audio_controller.c and app_main.c:
buffer = heap_caps_calloc(1, size, MALLOC_CAP_SPIRAM);
```

### Build Errors

**ESP-IDF version mismatch:**
```bash
idf.py --version
# Must show v5.5.1
```

**Component not found:**
```
ERROR: Component 'esp_websocket_client' not found
```
Solution: `idf.py reconfigure` to download managed components

**Clean build:**
```bash
idf.py fullclean
idf.py build
```

**Permission denied on serial port (Linux):**
```bash
sudo usermod -a -G dialout $USER
# Log out and back in
```

## Development

### Changing Log Levels

**Via menuconfig:**
```bash
idf.py menuconfig
# Navigate to: Component config → Log output → Default log verbosity → Debug
```

**Per-component in code:**
```c
// In component source file:
static const char *TAG = "my_component";
esp_log_level_set(TAG, ESP_LOG_DEBUG);
```

### Adding Debug Logging

```c
ESP_LOGD(TAG, "Debug message: value=%d", value);    // Debug (not shown by default)
ESP_LOGI(TAG, "Info message: status=%s", status);   // Info
ESP_LOGW(TAG, "Warning: issue detected");           // Warning
ESP_LOGE(TAG, "Error: failed with code %d", err);   // Error
```

### Memory Debugging

**Check heap usage:**
```c
ESP_LOGI(TAG, "Free heap: %lu bytes", esp_get_free_heap_size());
ESP_LOGI(TAG, "Free PSRAM: %lu bytes", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
```

**Monitor in logs:**
```bash
idf.py monitor | grep "heap"
```

### Performance Profiling

**Enable task statistics:**
```bash
idf.py menuconfig
# Component config → FreeRTOS → Enable FreeRTOS trace facility
# Component config → FreeRTOS → Enable FreeRTOS stats formatting functions
```

**Print task info:**
```c
char buf[2048];
vTaskGetRunTimeStats(buf);
ESP_LOGI(TAG, "Task stats:\n%s", buf);
```

## Advanced Configuration

### Adjusting Auto-Mute Timing

Edit `main/app_main.c`:
```c
#define AI_SPEAKING_TIMEOUT_MS 2000  // Default: 2 seconds

// Increase for more safety margin (slower response):
#define AI_SPEAKING_TIMEOUT_MS 3000  // 3 seconds

// Decrease for faster response (risk of feedback):
#define AI_SPEAKING_TIMEOUT_MS 1500  // 1.5 seconds
```

### Changing Audio Chunk Size

Edit `main/audio_controller.c`:
```c
const size_t chunk_samples = 1600;  // 100ms @ 16kHz

// Larger chunks = less overhead, more latency
// Smaller chunks = more overhead, less latency
```

### Modifying Pre-Buffer Size

Edit `main/audio_playback.c`:
```c
#define PREBUFFER_SIZE 24000  // 500ms @ 24kHz

// Increase for choppy networks:
#define PREBUFFER_SIZE 48000  // 1 second

// Decrease for lower latency:
#define PREBUFFER_SIZE 12000  // 250ms
```

## Known Issues

1. **WebSocket disconnect during transmission** - Under investigation
   - Symptom: `E (29398) transport_ws: Error transport_poll_write(0)`
   - Workaround: Connection auto-reconnects, may cause brief audio interruption

2. **Display artifacts during heavy audio processing** - Resolved
   - Fixed by using PSRAM for audio buffers instead of internal RAM

3. **Acoustic feedback in noisy environments**
   - Mitigated by 2-second auto-mute timeout
   - Consider increasing timeout for very reverberant spaces

## Future Enhancements

- [ ] Direct OpenAI connection (eliminate proxy dependency)
- [ ] On-device VAD (reduce network traffic)
- [ ] Persistent sessions (maintain conversation context across reboots)
- [ ] Multi-device support (multiple devices sharing proxy)
- [ ] Configuration UI (WiFi/proxy setup via touch screen)
- [ ] Battery power support
- [ ] Wake word detection

## Contributing

Contributions welcome! Areas of interest:
- Performance optimization
- Audio quality improvements
- UI enhancements
- Power management
- Alternative hardware support

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

## Acknowledgments

- Built with [ESP-IDF v5.5.1](https://github.com/espressif/esp-idf)
- UI powered by [LVGL v8](https://lvgl.io/)
- Audio codec via [esp_audio_codec](https://components.espressif.com/components/espressif/esp_audio_codec)
- WebSocket client via [esp_websocket_client](https://components.espressif.com/components/espressif/esp_websocket_client)
- Hardware: Waveshare ESP32-S3 Touch LCD 1.85C
- OpenAI Realtime API integration

## Support

For issues, questions, or contributions:
- Check existing issues and documentation first
- Provide detailed logs when reporting problems
- Include hardware revision and ESP-IDF version
- Describe steps to reproduce any bugs

**Getting logs:**
```bash
idf.py monitor | tee issue-log.txt
# Reproduce the issue
# Ctrl+] to exit
# Attach issue-log.txt to bug report
```
