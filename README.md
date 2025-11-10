# Smart Assistant Device

ESP32-S3 voice assistant device using OpenAI Realtime API for natural voice conversations.

## What is this?

This firmware turns a Waveshare ESP32-S3 Touch LCD 1.85C development board into a walkie-talkie style voice assistant. Press the button, speak your question, and get an AI-generated voice response streamed back in real-time.

**Features:**
- Real-time voice input via I2S microphone
- Streaming audio playback with pre-buffering
- Touch LCD UI with large, finger-friendly buttons
- WiFi connectivity
- Integration with OpenAI Realtime API via proxy server

## Hardware Required

- [Waveshare ESP32-S3 Touch LCD 1.85C](https://www.waveshare.com/esp32-s3-touch-lcd-1.85c.htm)
  - ESP32-S3 with 8MB PSRAM
  - 1.85" ST77916 LCD (360x360)
  - Capacitive touch screen
  - I2S microphone and speaker

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

**Note:** Using a version-specific directory (`v5.5.1/`) allows you to maintain multiple ESP-IDF versions side-by-side.

### 2. Set up the Proxy Server

This device requires a companion proxy server to interface with OpenAI's Realtime API. See the [voice-assistant-proxy](https://github.com/pcomans/voice-assistant-proxy) repository for installation and setup instructions.

The proxy must be running and accessible on your local network before using the device.

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
```

**Note:** `wifi_credentials.h` is gitignored to protect your credentials.

### 2. Proxy Server IP Address

Edit `main/proxy_client.c` and update the proxy URL to match your proxy server's IP address:

```c
#define PROXY_DEFAULT_URL   "http://YOUR_PROXY_IP:8000/v1/audio"
```

For example, if your proxy is running on `192.168.1.100`:
```c
#define PROXY_DEFAULT_URL   "http://192.168.1.100:8000/v1/audio"
```

## Build and Flash

### 1. Set up ESP-IDF environment

```bash
. ~/esp/v5.5.1/esp-idf/export.sh  # Or your ESP-IDF installation path
```

### 2. Set target chip

```bash
idf.py set-target esp32s3
```

### 3. Build the firmware

```bash
idf.py build
```

### 4. Connect the device

Connect the Waveshare ESP32-S3 board to your computer via USB-C.

### 5. Flash to device

```bash
idf.py flash
```

The tool will automatically detect the serial port. If multiple devices are connected, specify the port:
```bash
idf.py -p /dev/ttyUSB0 flash  # Linux
idf.py -p /dev/cu.usbmodem101 flash  # macOS
idf.py -p COM3 flash  # Windows
```

## View Logs

To view real-time logs from the device:

```bash
idf.py -p /dev/cu.usbmodem101 monitor  # macOS
idf.py -p /dev/ttyUSB0 monitor         # Linux
idf.py -p COM3 monitor                 # Windows
```

Or let it auto-detect the port:
```bash
idf.py monitor
```

To flash and immediately start monitoring:
```bash
idf.py flash monitor
```

**Exit monitor:** Press `Ctrl+]`

### Useful monitor features

- Filter logs by tag: `idf.py monitor --print-filter "TAG_NAME"`
- Increase baud rate: `idf.py -p /dev/cu.usbmodem101 -b 460800 monitor`
- Save logs to file: `idf.py monitor | tee device.log`

## Usage

1. Ensure the proxy server is running on your local network
2. Power on the device - it will automatically connect to WiFi
3. Wait for the "Start Recording" button to appear on the screen
4. Press and hold the button, speak your question
5. Release the button when done speaking
6. The device will send audio to the proxy, which forwards to OpenAI
7. AI response will stream back and play through the speaker

## Project Structure

```
main/
├── app_main.c              # Application entry point, state machine, WiFi init
├── ui.c / ui.h             # LVGL touch UI and button controls
├── audio_capture.c / .h    # I2S microphone input and voice detection
├── audio_playback.c / .h   # I2S speaker output with streaming buffer
├── proxy_client.c / .h     # HTTP client for proxy communication
├── ST77916.c / .h          # LCD display driver
├── LVGL_Driver.c / .h      # LVGL integration
└── wifi_credentials.h      # WiFi configuration (gitignored)
```

## Troubleshooting

### WiFi connection fails
- Check WiFi credentials in `main/wifi_credentials.h`
- Ensure 2.4GHz network (ESP32 doesn't support 5GHz)
- Check monitor logs for detailed error messages

### Proxy connection fails
- Verify proxy server is running: `curl http://PROXY_IP:8000/healthz`
- Check proxy IP address in `main/proxy_client.c`
- Ensure device and proxy are on the same network
- Check firewall settings

### Audio issues
- Verify microphone is working (check logs for audio level)
- Check speaker connection
- Adjust volume if needed

### Build errors
- Ensure ESP-IDF 5.5.1 is installed: `idf.py --version`
- Clean and rebuild: `idf.py fullclean && idf.py build`
- Check that all managed components are downloaded

## Development

### Viewing detailed logs

```bash
# Set log level to DEBUG
idf.py menuconfig
# Navigate to: Component config → Log output → Default log verbosity → Debug
```

### Code formatting
Follow ESP-IDF coding style guidelines.

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

## Acknowledgments

- Built with [ESP-IDF](https://github.com/espressif/esp-idf)
- UI powered by [LVGL](https://lvgl.io/)
- Waveshare ESP32-S3 Touch LCD 1.85C hardware
- OpenAI Realtime API integration
