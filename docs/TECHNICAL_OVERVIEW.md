# Tuya BLE Breaker Bridge — Technical Overview

> ESP32-C3 firmware that bridges Tuya BLE breakers to MQTT/Home Assistant via the proprietary Tuya BLE protocol.

## The Challenge

Tuya BLE circuit breakers (MCBs) are cheap, widely available smart breakers — but they're locked into the Tuya ecosystem. There's no local API, no MQTT, no Home Assistant integration. The device communicates exclusively over Bluetooth Low Energy using Tuya's proprietary encrypted protocol.

The goal: build a standalone ESP32-C3 bridge that talks native Tuya BLE to the breaker and exposes it as a standard MQTT switch with Home Assistant auto-discovery. No cloud, no Tuya app, fully local.

### Why This Is Hard

1. **Proprietary encrypted protocol** — Every command is AES-128-CBC encrypted with session-derived keys. No replay attacks possible; each session negotiates a new key.

2. **Three-phase handshake** — Before you can send a simple ON/OFF, you must complete: DEVICE_INFO → PAIR → then DPS commands. Each phase uses different encryption keys.

3. **BLE packet fragmentation** — The 20-byte BLE MTU means every encrypted payload must be split into multiple BLE writes with varint framing, then reassembled on response.

4. **Undocumented GATT layout** — The device advertises service UUID `0xa201` but the actual GATT service is `0x1910`. Characteristics, handles, and security requirements vary by device and must be discovered dynamically.

5. **SMP pairing required** — The breaker requires Bluetooth Security Manager Protocol pairing (Just Works) before it will accept any GATT writes.

6. **No official documentation** — The Tuya BLE protocol is reverse-engineered from community projects, Android app captures, and the Tuya BLE Device SDK source code.

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│                    Home Assistant                         │
│              (MQTT auto-discovery switch)                 │
└──────────────────────┬──────────────────────────────────┘
                       │ MQTT (ON/OFF, state, availability)
                       │
┌──────────────────────▼──────────────────────────────────┐
│                   ESP32-C3 Bridge                        │
│                                                          │
│  ┌──────────┐  ┌──────────┐  ┌───────────┐              │
│  │ MQTT HA  │  │   Web    │  │  Tuya BLE │              │
│  │ Client   │  │ Dashboard│  │  Client   │              │
│  └────┬─────┘  └────┬─────┘  └─────┬─────┘              │
│       │              │              │                     │
│       └──────────────┴──────┬───────┘                    │
│                             │                             │
│              Connect-on-Demand Controller                 │
│         (connects BLE only when command received,         │
│          disconnects after 15s idle timeout)              │
└─────────────────────────────┬───────────────────────────┘
                              │ BLE (AES-128-CBC encrypted)
                              │
┌─────────────────────────────▼───────────────────────────┐
│                  Tuya BLE Breaker                         │
│            (Circuit breaker / smart switch)               │
└─────────────────────────────────────────────────────────┘
```

## Tuya BLE Protocol Deep Dive

### Credentials

Four values are needed per device, obtained from the Tuya cloud via [tinytuya](https://github.com/jasonacox/tinytuya):

| Credential | Example | Purpose |
|------------|---------|---------|
| Device ID | `ebfe73c9f578843c9aaxob` | Identifies device in handshake |
| Local Key | `Rgu\|B+B(OB5x2QJ9` | 16-char key, basis for all encryption |
| UUID | `7e552d16d52d4d63` | 16-char hex, sent during PAIR |
| MAC Address | `C0:F8:53:C2:C0:EB` | BLE address for connection |

### Key Derivation

```
local_key = "Rgu|B+B(OB5x2QJ9"  (16 chars from Tuya cloud)

login_key = MD5(local_key[:6])
          = MD5("Rgu|B+")
          → 16 bytes, used for DEVICE_INFO encryption

session_key = MD5(local_key[:6] + srand)
            where srand = 6 bytes from DEVICE_INFO response
            → 16 bytes, used for PAIR and DPS encryption
```

### Connection Flow

```
    ESP32-C3                              Tuya Breaker
       │                                       │
       │──── BLE Connect ─────────────────────►│
       │◄─── Connection Established ───────────│
       │                                       │
       │──── SMP Pairing (Just Works) ────────►│
       │◄─── Pairing Confirmed ────────────────│
       │                                       │
       │──── GATT Service Discovery ──────────►│
       │◄─── Services: 0x1800, 0x1801, 0x1910 │
       │                                       │
       │──── Enable Notifications (0x2B10) ───►│
       │◄─── CCC Write Confirmed ──────────────│
       │                                       │
       │──── DEVICE_INFO (code 0x0000) ───────►│  ← encrypted with login_key
       │◄─── DEVICE_INFO Response ─────────────│  → contains srand for session_key
       │                                       │
       │──── PAIR (code 0x0001) ──────────────►│  ← encrypted with session_key
       │◄─── PAIR Response ────────────────────│  → device paired
       │                                       │
       │════ READY — can send DPS commands ════│
       │                                       │
       │──── DPS ON  (code 0x0002, DP1=1) ───►│  ← encrypted with session_key
       │◄─── DPS Response (DP1=1) ─────────────│
       │                                       │
       │──── DPS OFF (code 0x0002, DP1=0) ───►│
       │◄─── DPS Response (DP1=0) ─────────────│
       │                                       │
       │  ... 15s idle timeout ...             │
       │──── BLE Disconnect ──────────────────►│
```

### Packet Structure

Every command follows the same packet format:

```
Raw packet (before encryption):
┌──────────┬─────────────┬──────┬──────────┬──────┬───────┐
│ seq_num  │ response_to │ code │ data_len │ data │ CRC16 │
│ 4 bytes  │ 4 bytes     │ 2 B  │ 2 bytes  │ var  │ 2 B   │
└──────────┴─────────────┴──────┴──────────┴──────┴───────┘

Encrypted envelope:
┌───────────────┬────────┬─────────────────────────┐
│ security_flag │   IV   │ AES-128-CBC(raw_packet) │
│    1 byte     │ 16 B   │ padded to 16B multiple  │
└───────────────┴────────┴─────────────────────────┘

Security flags:
  0x04 = login_key encryption (DEVICE_INFO)
  0x05 = session_key encryption (PAIR, DPS)
```

### BLE Fragmentation

The encrypted envelope is split into 20-byte BLE MTU packets:

```
Packet 0 (first):
┌────────────┬─────────────┬─────────────────┬──────┐
│ pkt_num(v) │ total_len(v)│ proto_ver << 4  │ data │
│ varint     │ varint      │ 1 byte (0x30)   │ ...  │
└────────────┴─────────────┴─────────────────┴──────┘

Packet 1+ (continuation):
┌────────────┬──────────────────────────────────────┐
│ pkt_num(v) │ data (fills remaining MTU bytes)      │
│ varint     │                                       │
└────────────┴──────────────────────────────────────┘

(v) = varint encoded: 7 bits per byte, high bit = continuation
```

### DPS Command Format

The switch datapoint payload (inside the encrypted packet):

```
┌───────┬─────────┬───────────┬───────┐
│ dp_id │ dp_type │ value_len │ value │
│ 1 B   │ 1 B     │ 1 byte    │ 1 B   │
│ 0x01  │ 0x01    │ 0x01      │ 0/1   │
│       │ (BOOL)  │           │       │
└───────┴─────────┴───────────┴───────┘

dp_id: configurable (usually 1, some devices use 2+)
dp_type: 0x01=BOOL, 0x02=VALUE, 0x03=STRING, 0x04=ENUM
value: 0x00=OFF, 0x01=ON
```

### CRC-16

CRC-16/MODBUS calculated over the raw packet (before encryption):

```
Init:       0xFFFF
Polynomial: 0xA001 (reflected)
Input:      all bytes before CRC field
```

## GATT Layout

Discovered via dynamic service enumeration (handles vary by device):

| Service | UUID | Handles | Purpose |
|---------|------|---------|---------|
| Generic Access | 0x1800 | 0x0001–0x0007 | Device name, appearance |
| Generic Attribute | 0x1801 | 0x0008–0x000F | Service changed, DB hash |
| **Tuya BLE** | **0x1910** | **0x0010–0x0015** | **Command/response channel** |

Tuya service characteristics:

| Characteristic | UUID | Handle | Properties | Purpose |
|----------------|------|--------|------------|---------|
| Write | 0x2B11 | 0x0012 | Write, Write Without Response | Send commands |
| Notify | 0x2B10 | 0x0014 | Notify | Receive responses |
| CCC Descriptor | — | 0x0015 | — | Enable notifications |

## Implementation Details

### Connect-on-Demand Pattern

The bridge does **not** maintain a persistent BLE connection. Instead:

1. MQTT command or web button press triggers `send_switch_command()`
2. If BLE is disconnected, the command is queued and BLE connect initiated
3. Full handshake completes (SMP → GATT → DEVICE_INFO → PAIR)
4. Queued command is sent
5. 15-second idle timer starts
6. On timeout, BLE disconnects to free resources
7. Next command reconnects from scratch

This avoids the BLE stack consuming resources continuously and handles the breaker's tendency to disconnect idle connections.

### State Machine

```
DISCONNECTED → CONNECTING → CONNECTED → SMP_PAIRING
    → DISCOVERING_SERVICES → GETTING_DEVICE_INFO
    → PAIRING → PAIRED → READY
```

Each state transition is driven by BLE GATT callbacks. The state machine handles retries and error recovery at each phase.

### MQTT Home Assistant Integration

On MQTT connect, the bridge publishes a retained discovery message:

```
Topic: homeassistant/switch/{prefix}/switch/config
```

```json
{
  "name": "Tuya BLE Breaker",
  "unique_id": "tuya_ble_{device_id}",
  "command_topic": "{prefix}/command",
  "state_topic": "{prefix}/state",
  "availability_topic": "{prefix}/availability",
  "payload_on": "ON",
  "payload_off": "OFF",
  "device": {
    "identifiers": ["tuya_ble_{device_id}"],
    "name": "Tuya BLE Breaker",
    "manufacturer": "Tuya",
    "model": "BLE Breaker",
    "sw_version": "1.0.0+3.abc1234"
  }
}
```

The LWT (Last Will and Testament) publishes `offline` to the availability topic on unexpected disconnect.

### Web Dashboard

Built-in HTTP server on port 80 with:

- Real-time status API (`/api/status`) — BLE state, RSSI, heap, uptime, GATT handles
- Log ring buffer API (`/api/logs`) — last 80 log lines with ANSI stripping and JSON escaping
- Switch control API (`/api/switch/on`, `/api/switch/off`)
- BLE control API (`/api/connect`, `/api/disconnect`)
- Single-page dashboard with 2-second auto-refresh

## File Structure

```
hms-esp-tuya-ble/
├── main/
│   ├── main.c               WiFi, BLE init, connect-on-demand controller
│   ├── tuya_ble_client.c/h   BLE GATT client, state machine, SMP pairing
│   ├── tuya_crypto.c/h       MD5, AES-128-CBC, CRC-16/MODBUS
│   ├── tuya_packet.c/h       Packet building, encryption, BLE fragmentation
│   ├── mqtt_ha.c/h           MQTT client, HA auto-discovery, command handling
│   ├── web_server.c/h        HTTP dashboard, status/log/control APIs
│   ├── udp_logger.c/h        Optional UDP remote logging
│   ├── Kconfig.projbuild     All configuration options
│   └── CMakeLists.txt         Component registration
├── CMakeLists.txt             Project config, version system
├── partitions.csv             Dual OTA partition layout (4MB flash)
├── sdkconfig.defaults         ESP32-C3 BLE + WiFi defaults
├── version.txt                Semantic version (single source of truth)
├── CHANGELOG.md               Release history
├── build.sh                   Build helper script
├── LICENSE                    MIT
└── README.md                  User-facing documentation
```

## Lessons Learned

1. **Wrong credentials = silent failure.** The device accepts GATT writes but never responds. No error, no disconnect reason — just silence. Always verify Device ID + Local Key match via tinytuya.

2. **SMP pairing is mandatory** on these breakers, even though some community projects skip it. Without SMP, the device accepts writes but ignores them.

3. **Handle 0x003b doesn't exist.** Early analysis suggested a pre-flight security write was needed. Turns out that was a different device model. Dynamic GATT discovery is essential.

4. **va_list can only be consumed once.** The log hook that feeds both the web dashboard and UDP logger hit a subtle bug where the second consumer got garbage. Fixed with `va_copy()`.

5. **Connect-on-demand beats persistent connection.** The breaker disconnects idle connections after ~30 seconds anyway. Embracing this with an intentional pattern made the system more robust.

6. **The advertised UUID is a lie.** Device advertises `0xa201` but the actual GATT service UUID is `0x1910`. Trust discovery, not advertisements.

## References

- [PlusPlus-ua/ha_tuya_ble](https://github.com/PlusPlus-ua/ha_tuya_ble) — Python HA integration (protocol reference)
- [jasonacox/tinytuya](https://github.com/jasonacox/tinytuya) — Credential extraction tool
- [Tuya BLE Device SDK](https://github.com/tuya/tuya-ble-sdk) — Official SDK (partial protocol docs)
- [ShonP40/Tuya-BLE](https://github.com/ShonP40/Tuya-BLE) — Protocol documentation
