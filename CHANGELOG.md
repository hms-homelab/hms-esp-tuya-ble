# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.0.0] - 2025-03-02

### Added
- Full Tuya BLE protocol: SMP pairing, GATT discovery, AES-CBC encrypted commands
- Connect-on-demand pattern: BLE connects only when a command is sent, auto-disconnects after 15s idle
- MQTT with Home Assistant auto-discovery (switch entity)
- Web dashboard with live BLE/WiFi status, GATT details, log viewer, and ON/OFF controls
- All settings configurable via `idf.py menuconfig` (WiFi, Tuya credentials, MQTT, static IP, UDP logger)
- Configurable switch datapoint ID (default DP 1)
- Configurable MQTT topic prefix and HA device name
- DHCP (default) or static IP
- Optional UDP remote logger for debugging
- OTA-ready dual partition table
- Build helper script (`build.sh`)
