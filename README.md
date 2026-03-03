# hms-esp-tuya-ble

[![Buy Me A Coffee](https://img.shields.io/badge/Buy%20Me%20A%20Coffee-support-%23FFDD00.svg?logo=buy-me-a-coffee)](https://www.buymeacoffee.com/aamat09)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

ESP32-C3 BLE-to-MQTT bridge for Tuya BLE breakers — Home Assistant auto-discovery.

Connects to any Tuya BLE breaker/switch over Bluetooth, translates commands to the Tuya BLE protocol, and publishes state via MQTT. Home Assistant discovers the switch automatically — no YAML configuration required.

## Features

- Full Tuya BLE protocol: SMP pairing, GATT discovery, AES-CBC encrypted commands
- Connect-on-demand: BLE connects only when a command is sent, disconnects after idle timeout
- MQTT with Home Assistant auto-discovery (switch entity)
- Web dashboard with live BLE/WiFi status, GATT details, log viewer, and ON/OFF controls
- All device credentials and network settings configurable via `idf.py menuconfig`
- Optional UDP remote logger for debugging
- DHCP (default) or static IP
- OTA-ready partition table

## Supported Hardware

- **Microcontroller**: Any ESP32-C3 board (e.g., ESP32-C3-DevKitM, Seeed XIAO ESP32C3, M5Stamp C3)
- **Device**: Any Tuya BLE breaker or switch (tested with WiFi Breaker 3 / MCB)
- **Requirements**: The device must use the Tuya BLE protocol (advertises service UUID `0xa201`)

## Prerequisites

- [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c3/get-started/) v5.3 or later
- An MQTT broker (e.g., Mosquitto)
- Home Assistant with MQTT integration enabled
- Tuya device credentials (Device ID, Local Key, UUID, MAC address)

## Getting Tuya Credentials

You need four values from the Tuya cloud for your device. The easiest way is [tinytuya](https://github.com/jasonacox/tinytuya):

```bash
pip install tinytuya
python -m tinytuya wizard
```

Follow the prompts to link your Tuya developer account. The wizard outputs a `devices.json` with:

| Field | Kconfig Option |
|-------|----------------|
| `id` | `TUYA_DEVICE_ID` |
| `key` | `TUYA_LOCAL_KEY` |
| `uuid` | `TUYA_UUID` |
| `mac` | `TUYA_DEVICE_MAC` |

> **Tip**: If the MAC address is not in `devices.json`, use a BLE scanner app (e.g., nRF Connect) to find the device advertising Tuya service UUID `0xa201`.

## Build & Flash

```bash
# Clone the repository
git clone https://github.com/hms-homelab/hms-esp-tuya-ble.git
cd hms-esp-tuya-ble

# Configure your device credentials and network settings
idf.py menuconfig
# Navigate to: Tuya BLE Breaker Configuration
#   WiFi        → Set SSID and Password
#   Tuya Device → Set MAC, Device ID, Local Key, UUID
#   MQTT        → Set Broker URI (and username/password if needed)

# Build
idf.py build

# Flash (replace PORT with your serial port)
idf.py -p PORT flash

# Monitor serial output
idf.py -p PORT monitor
```

## Configuration

All settings are configured via `idf.py menuconfig` under **Tuya BLE Breaker Configuration**:

### WiFi

| Setting | Default | Description |
|---------|---------|-------------|
| WiFi SSID | `myssid` | Your WiFi network name |
| WiFi Password | `mypassword` | Your WiFi password |
| Use Static IP | `n` | Enable for static IP instead of DHCP |
| Static IP | `192.168.1.50` | Static IP address (if enabled) |
| Gateway | `192.168.1.1` | Gateway address (if enabled) |
| Netmask | `255.255.255.0` | Subnet mask (if enabled) |

### Tuya Device

| Setting | Default | Description |
|---------|---------|-------------|
| Device MAC | `00:00:00:00:00:00` | BLE MAC address of the Tuya device |
| Device ID | `your_device_id` | Tuya device ID (from tinytuya) |
| Local Key | `your_local_key` | Tuya local key (from tinytuya) |
| UUID | `your_uuid` | Tuya UUID (from tinytuya) |
| Switch DP ID | `1` | Datapoint ID for the switch (most devices use 1) |

### MQTT

| Setting | Default | Description |
|---------|---------|-------------|
| Broker URI | `mqtt://192.168.1.100:1883` | MQTT broker address |
| Username | *(empty)* | MQTT username (optional) |
| Password | *(empty)* | MQTT password (optional) |
| Topic Prefix | `tuya_ble_breaker` | Prefix for MQTT topics |
| HA Device Name | `Tuya BLE Breaker` | Device name shown in Home Assistant |

### Debug

| Setting | Default | Description |
|---------|---------|-------------|
| UDP Logger | `n` | Enable UDP log output for remote debugging |
| UDP Logger IP | `192.168.1.100` | Destination IP for UDP logs |
| UDP Logger Port | `9999` | Destination port for UDP logs |

## How It Works

The bridge uses a **connect-on-demand** pattern to minimize BLE resource usage:

```
Home Assistant / Web UI
        |
        | MQTT command (ON/OFF) or HTTP POST
        v
  ESP32-C3 Bridge
        |
        |  1. Receive command
        |  2. BLE connect (if not already connected)
        |  3. SMP pair → GATT discover → Tuya handshake
        |  4. Send encrypted switch command (DP ID)
        |  5. Receive state confirmation
        |  6. Publish state to MQTT
        |  7. Start idle timer (15s)
        |  8. Disconnect BLE on timeout
        v
  Tuya BLE Breaker
```

### MQTT Topics

| Topic | Direction | Description |
|-------|-----------|-------------|
| `{prefix}/command` | HA → Bridge | Send `ON` or `OFF` |
| `{prefix}/state` | Bridge → HA | Current state (`ON`/`OFF`) |
| `{prefix}/availability` | Bridge → HA | `online` when BLE ready, `offline` otherwise |

## Home Assistant

Once running, a switch entity appears automatically in Home Assistant via MQTT discovery. The entity name matches the configured **HA Device Name**.

The device shows:
- **Switch control** — toggle ON/OFF
- **Availability** — online when BLE is connected, offline when disconnected

> **Note**: Since BLE connects on demand, the device may briefly show as "unavailable" between commands. It will reconnect automatically when the next command is sent.

## Web Dashboard

The built-in web dashboard is available at `http://<bridge-ip>/` and provides:

- **Switch control** — ON/OFF buttons
- **BLE state** — connection state, RSSI, target found
- **WiFi RSSI** — signal strength
- **System info** — uptime, free heap
- **GATT details** — SMP pairing status, service/characteristic handles
- **Live log viewer** — scrolling ESP-IDF log output

## Troubleshooting

| Problem | Solution |
|---------|----------|
| BLE never finds device | Verify MAC address is correct. Use nRF Connect to confirm the device advertises `0xa201`. |
| SMP pairing fails | Some devices need to be reset or re-paired. Power cycle the breaker. |
| Handshake/pairing fails | Double-check Device ID, Local Key, and UUID from tinytuya. Keys rotate on re-pairing in the Tuya app. |
| MQTT not connecting | Verify broker URI, username, and password. Check broker logs. |
| Wrong DP ID | Some devices use DP 2 or others. Check your device's datapoints in the Tuya IoT Platform. |
| Static IP not working | Ensure the IP is not already in use and the gateway/netmask are correct. |

## Contributing

Contributions are welcome! Please open an issue or pull request.

1. Fork the repository
2. Create a feature branch (`git checkout -b feature/my-change`)
3. Commit your changes
4. Push to the branch and open a Pull Request

## License

This project is licensed under the MIT License — see the [LICENSE](LICENSE) file for details.
