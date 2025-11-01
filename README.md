# Smart Assistant Device Firmware (ESP-IDF)

Prototype firmware for the Waveshare ESP32-S3 Touch LCD 1.85C smart assistant.

## Layout
- `main/app_main.c` – application entry, state machine, Wi-Fi init.
- `main/ui.c` – LVGL button UI and state updates.
- `main/audio_controller.c` – stubs for I2S capture/playback control.
- `main/proxy_client.c` – stubbed network client for the local proxy.

## Build
```bash
idf.py set-target esp32s3
idf.py build
idf.py -p <PORT> flash monitor
```

Copy `main/wifi_credentials.template.h` to `main/wifi_credentials.h` and populate your SSID/password before building.

Populate Wi-Fi credentials and proxy configuration via NVS or a provisioning flow before flashing.
