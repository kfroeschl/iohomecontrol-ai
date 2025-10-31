# AGENT.MD - iohomecontrol ESP32 Project Documentation

## Project Overview

This is an ESP32-based implementation of the **io-homecontrol®** protocol, designed to control smart home devices (primarily Somfy window coverings, blinds, awnings, etc.) using radio communication in the 868-870 MHz frequency band. The project supports both 1-Way (1W) and 2-Way (2W) communication modes.

**Primary Use Case**: Educational tool and alternative controller for io-homecontrol devices.

**Based on work by**:
- [Velocet/iown-homecontrol](https://github.com/Velocet/iown-homecontrol) - Protocol documentation and reverse engineering
- [cridp/iown-homecontrol-esp32sx1276](https://github.com/cridp/iown-homecontrol-esp32sx1276) - ESP32 implementation

**Current Focus**: 1W (OneWay) communication is stable; 2W (TwoWay) has been modified and may be unstable.

---

## Architecture Overview

### Core Technology Stack
- **Platform**: ESP32 (with Arduino framework via PlatformIO)
- **Radio Hardware**: SX1276 LoRa module (SX126X and CC1101 support exists but not guaranteed)
- **Build System**: PlatformIO
- **Filesystem**: LittleFS (for configuration files, web interface)
- **Network**: WiFi with WiFiManager, MQTT support, optional web server
- **Protocol**: io-homecontrol (FSK modulation, 38400 bps, 868 MHz band)

### Key Components

#### 1. Radio Layer (`iohcRadio`)
- **File**: `src/iohcRadio.cpp`, `include/iohcRadio.h`
- **Purpose**: Hardware abstraction for SX1276/SX126X radio modules
- **Features**:
  - Frequency hopping (FHSS) across 3 channels
  - Asynchronous TX/RX with interrupt-driven state machine
  - Preamble detection and payload handling
  - RSSI monitoring and timing analysis

#### 2. Packet Handling (`iohcPacket`)
- **File**: `src/iohcPacket.cpp`, `include/iohcPacket.h`
- **Purpose**: Frame encoding/decoding for io-homecontrol protocol
- **Frame Structure**:
  - Control Bytes (CB1, CB2) - Protocol metadata
  - Source/Target NodeID (3 bytes each)
  - Command ID
  - Payload (variable length, max 21 bytes)
  - CRC-16/KERMIT checksum

#### 3. Device Types

**1W Remote Controller** (`iohcRemote1W`)
- **Files**: `src/iohcRemote1W.cpp`, `include/iohcRemote1W.h`
- **Purpose**: Control 1-Way devices (window coverings, blinds)
- **Data Storage**: `extras/1W.json`
- **Capabilities**:
  - Pairing/unpairing devices
  - Position tracking with travel time calculation
  - Send commands: open, close, stop, position, vent, force
  - Mode switching

**2W Cozy Devices** (`iohcCozyDevice2W`)
- **Files**: `src/iohcCozyDevice2W.cpp`, `include/iohcCozyDevice2W.h`
- **Purpose**: Control Atlantic/Sauter/Thermor heating devices (2-Way)
- **Data Storage**: `extras/Cozy2W.json`
- **Capabilities**:
  - Temperature control (7.0-28.0°C)
  - Mode setting (auto, prog, manual, off)
  - Presence detection
  - Window status

**2W Other Devices** (`iohcOtherDevice2W`)
- **Files**: `src/iohcOtherDevice2W.cpp`, `include/iohcOtherDevice2W.h`
- **Purpose**: Generic 2-Way device controller
- **Data Storage**: `extras/Other2W.json`

**Remote Mapping** (`iohcRemoteMap`)
- **Files**: `src/iohcRemoteMap.cpp`, `include/iohcRemoteMap.h`
- **Purpose**: Map multiple devices to virtual remote controls
- **Data Storage**: `extras/RemoteMap.json`

**System Table** (`iohcSystemTable`)
- **Files**: `src/iohcSystemTable.cpp`, `include/iohcSystemTable.h`
- **Purpose**: Store discovered devices and network state
- **Data Storage**: `extras/sysTable.json`

#### 4. Cryptography

**AES Implementation** (`Aes.h`, multiple implementations)
- Original, OpenSSL-style, and TinyAES versions
- Used for 1W and 2W key exchange and encryption

**Crypto Utilities** (`iohcCryptoHelpers`)
- **Files**: `src/iohcCryptoHelpers.cpp`, `include/iohcCryptoHelpers.h`
- **Purpose**: Key derivation, HMAC calculation, challenge-response
- **Features**:
  - Transfer key generation
  - Rolling code verification
  - HMAC-based authentication

#### 5. Network Services

**WiFi** (`wifi_helper`)
- **Files**: `src/wifi_helper.cpp`, `include/wifi_helper.h`
- **Features**: WiFiManager for initial setup, mDNS support

**MQTT** (`mqtt_handler`)
- **Files**: `src/mqtt_handler.cpp`, `include/mqtt_handler.h`
- **Purpose**: Home Assistant integration via MQTT
- **Features**:
  - Auto-discovery for Home Assistant
  - Position reporting
  - Command reception
  - Configurable server/credentials

**Web Server** (`web_server_handler`)
- **Files**: `src/web_server_handler.cpp`, `include/web_server_handler.h`
- **Frontend**: `extras/web_interface_data/` (HTML, CSS, JS)
- **Features**:
  - RESTful API endpoints
  - WebSocket for real-time updates
  - Device control interface
  - Log streaming
  - OTA firmware updates

**Syslog** (`syslog_helper`)
- **Files**: `src/syslog_helper.cpp`, `include/syslog_helper.h`
- **Purpose**: Remote logging to syslog server

#### 6. User Interface

**OLED Display** (`oled_display`)
- **Files**: `src/oled_display.cpp`, `include/oled_display.h`
- **Hardware**: SSD1306 OLED (optional)
- **Display**: Status, RSSI, messages

**Command Line Interface** (`interact`)
- **Files**: `src/interact.cpp`, `include/interact.h`
- **Purpose**: Serial terminal command interface
- **Commands**: See COMMANDS.md for full list

---

## Protocol Details

### io-homecontrol Basics

**Frequency Bands**:
- Channel 1 (2W): 868.25 MHz (868.0-868.6 MHz)
- Channel 2 (1W/2W): 868.95 MHz (868.7-869.2 MHz)
- Channel 3 (2W): 869.85 MHz (869.7-870.0 MHz)

**Modulation**: FSK with 19.2 kHz deviation (NRZ encoding)
**Baud Rate**: 38400 bps (UART 8N1)
**Frequency Hopping**: 2.7ms per channel (FHSS)

**Modes**:
- **1W (OneWay)**: Unidirectional, uses 1 channel
- **2W (TwoWay)**: Bidirectional, uses 3 channels with frequency hopping

### Frame Format

```
┌─────────┬─────────┬────────┬────────┬─────────┬──────────┬─────────┬──────┬─────┐
│CtrlByte1│CtrlByte2│ Target │ Source │ CmdID   │ Payload  │ Rolling │ HMAC │ CRC │
│  (1B)   │  (1B)   │  (3B)  │  (3B)  │  (1B)   │ (0-21B)  │  Code   │      │(2B) │
└─────────┴─────────┴────────┴────────┴─────────┴──────────┴─────────┴──────┴─────┘
```

**CtrlByte1**:
- MsgLen (5 bits): Payload length
- Protocol (1 bit): 0=2W, 1=1W
- StartFrame/EndFrame (2 bits): Multi-frame indicators

**CtrlByte2**:
- Version, Priority, LPM, Routed, Beacon flags

**NodeID**: 3-byte device address (LSBF - Least Significant Byte First)
- Broadcast: 03:FF:FF (reversed and inverted from 00:00:3F)
- Manufacturer ranges identify device types

**CRC**: CRC-16/KERMIT (not CCITT!)

---

## File Structure

### Configuration Files (JSON in `extras/`)

**1W.json**: 1-Way devices
```json
{
  "b60d1a": {
    "key": "2d36bd8b4d4fb1e1a1b3099b394d3a9e",
    "sequence": "3410",
    "type": [0, 0],
    "manufacturer_id": 2,
    "description": "IZY1",
    "name": "Screen ZB",
    "travel_time": 25,
    "paired": false
  }
}
```

**Cozy2W.json**: Atlantic/Sauter/Thermor devices
**Other2W.json**: Generic 2-Way devices
**RemoteMap.json**: Virtual remote to device mappings
**sysTable.json**: Network discovery cache

### Source Organization

**`src/`**: Main application code
- `main.cpp`: Entry point, initialization
- Device implementations (1W, 2W variants)
- Radio and packet handling
- Network services (WiFi, MQTT, web server)
- UI components (OLED, serial interface)

**`include/`**: Header files
- Device interfaces
- Radio abstractions
- Crypto utilities
- Configuration headers (`user_config.h`, `board-config.h`)

**`iown-homecontrol/`**: Protocol library (submodule/dependency)
- Comprehensive protocol documentation
- Reference implementations
- Analysis tools (Python, Kaitai Struct, etc.)

**`extras/`**: Data files
- JSON device configurations
- Web interface assets (`web_interface_data/`)

---

## Development Workflow

### Build Process (PlatformIO)

**Initial Setup or JSON Changes**:
```bash
pio run --target buildfs    # Build filesystem image
pio run --target uploadfs   # Upload filesystem to ESP32
```

**Normal Development**:
```bash
pio run --target upload     # Upload firmware
pio run --target monitor    # Serial monitor
```

**Environment**: Set in `platformio.ini` (default: `HeltecLoraV2ESP32`)

### Configuration

**`platformio.ini`**:
- Board selection (Heltec, LilyGo, etc.)
- Build flags (debug levels, optimizations)
- Library dependencies
- Upload/monitor settings

**`sdkconfig`**:
- ESP32 SDK configuration
- **CRITICAL**: `CONFIG_ESP_TIMER_SUPPORTS_ISR_DISPATCH_METHOD=y` must be enabled

**`include/user_config.h`**:
- WiFi credentials (handled by WiFiManager)
- MQTT server settings
- Syslog configuration
- Feature flags (MQTT, WEBSERVER, SSD1306_DISPLAY)

**`include/board-config.h`**:
- Hardware pin definitions
- Radio module selection

### Command Interface

Serial terminal commands (115200 baud) - see `COMMANDS.md`:

**1W Commands**:
- `pair <device>`: Put device in pairing mode
- `add <device>`: Add controller to device
- `remove <device>`: Remove controller
- `open/close/stop <device>`: Movement commands
- `position <device> <0-100>`: Set position
- `new1W <addr> <key> <name>`: Add new device
- `list1W`: Show all 1W devices

**2W Commands**:
- `setTemp <device> <temp>`: Set temperature (7.0-28.0)
- `setMode <device> <mode>`: Set mode (auto/prog/manual/off)
- `discovery`: Send 2W discovery packet
- `associate`: Synchronize paired devices

**Common Commands**:
- `verbose`: Toggle detailed packet logging
- `lastAddr`: Show last received address
- `mqttIp/mqttUser/mqttPass`: Configure MQTT
- `ls/cat/rm`: Filesystem operations

### Web Interface

**Access**: 
- First boot: Connect to `iohc-setup` AP, configure WiFi
- Subsequent: `http://<ESP32_IP>` or `http://miopenio.local`

**Features**:
- Device control (open/close/position)
- Real-time position tracking
- Log viewing
- OTA firmware updates

**API Endpoints**:
- `GET /api/devices`: List all devices
- `POST /api/1w/command`: Execute 1W command
- `POST /api/mqtt/config`: Update MQTT settings
- `POST /update`: OTA firmware upload
- WebSocket `/ws`: Real-time updates

---

## Key Utilities and Helpers

### Filesystem Helpers (`fileSystemHelpers`)
- JSON file loading/saving
- LittleFS operations
- File listing and management

### NVS Helpers (`nvs_helpers`)
- Non-Volatile Storage for runtime configuration
- MQTT credentials, network settings

### Log Buffer (`log_buffer`)
- Circular buffer for system logs
- WebSocket broadcasting
- Syslog integration

### Ticker (`TickerUsESP32`)
- High-precision timing for radio state machine
- ISR-safe callbacks

### Utilities (`utils.h`)
- Address conversion (hex ↔ bytes)
- Endianness handling (LSBF)
- CRC calculations
- Broadcast address detection

### Blind Position Tracker (`blind_position`)
- Real-time position estimation based on travel time
- State machine (Idle, Opening, Closing)
- Auto-stop on completion

---

## Hardware Support

### Tested Boards
- **Heltec WiFi LoRa 32 V2/V3** (recommended)
- **LilyGo LoRa32**
- Generic ESP32 + SX1276 module

### Radio Modules
- **SX1276** (primary, well-tested)
- SX126X (supported, less tested)
- CC1101 (legacy, not maintained)

### Optional Components
- SSD1306 OLED display (128x64)
- Status LED (configurable pin)

---

## Protocol Tools (iown-homecontrol Library)

Located in `iown-homecontrol/scripts/`:

### Python Tools
- **`iohomecontrol.py`**: Kaitai Struct-generated parser
- **`Iown-AES.py`**: AES encryption utilities
- **`Iown-Bits.py`**: Bit manipulation helpers
- **`Iown-ioCrypto.py`**: Cryptographic operations
- **`Iown-IoHexFrameParser.py`**: Frame parser for hex dumps

### Other Tools
- **`io-homecontrol.ksy`**: Kaitai Struct definition for packet format
- **`kaitai-decoder.py`**: Decode captured packets
- **`GNURadio/`**: SDR signal processing
- **`rtl_433/`**: RTL-SDR decoder integration
- **`IDAPro/`**: Firmware reverse engineering scripts
- **`renode/`**: STM32 firmware emulation

### Documentation
- `iown-homecontrol/docs/`: Comprehensive protocol specs
  - `linklayer.md`: Frame format, Layer 2/3 details
  - `commands.md`: Command reference
  - `radio.md`: Physical layer, modulation
  - `devices.md`: Device types, NodeID ranges
  - `firmware.md`: Actuator/controller firmware analysis

---

## Integration Points

### Home Assistant (via MQTT)
- Auto-discovery enabled
- Cover entities with position support
- Commands: open, close, stop, set_position
- State updates via WebSocket → MQTT bridge

### RESTful API
- Language-agnostic HTTP interface
- JSON request/response
- Suitable for custom integrations

### Serial Console
- Direct command access
- Debugging and diagnostics
- Scanner mode for protocol analysis

---

## Security Considerations

**Encryption**:
- 128-bit AES for 1W devices
- Key exchange via challenge-response (2W)
- HMAC validation for message authenticity

**Best Practices**:
- Change default web UI credentials (`HTTP_USERNAME`, `HTTP_PASSWORD`)
- Use secure MQTT credentials
- Limit network exposure (use VLANs, firewalls)

**Educational Use Only**: This tool is for learning and testing. Respect privacy and only control devices you own.

---

## Troubleshooting

### Common Issues

**No devices detected**:
- Check radio module wiring (SPI, DIO pins)
- Verify frequency settings match region (868 MHz for EU)
- Ensure antenna is connected

**1W commands not working**:
- Device must be paired first (`pair` → `add`)
- Check `paired: true` in `1W.json`
- Verify key and sequence in JSON

**2W unstable**:
- Known issue; focus has been on 1W
- Check timing parameters in `iohcRadio.cpp`

**Web interface not loading**:
- Filesystem image must be uploaded (`uploadfs`)
- Check browser console for errors
- Verify ESP32 IP address

**MQTT not connecting**:
- Use serial commands to set server: `mqttIp 192.168.1.100`
- Check credentials: `mqttUser`, `mqttPass`
- Verify MQTT broker is accessible

### Debug Levels

In `platformio.ini`, adjust `CORE_DEBUG_LEVEL`:
```ini
-DCORE_DEBUG_LEVEL=5  ; 3=ERROR, 4=WARN, 5=DEBUG, 6=VERBOSE
```

Enable verbose packet logging:
```
> verbose
```

---

## Future Development

### Planned Features (from TODO/Issues)
- [ ] Stable 2W implementation
- [ ] EMS2 protocol support (long carrier sense)
- [ ] RTS protocol integration (Somfy legacy)
- [ ] ZigBee bridge for HA integration
- [ ] HomeKit support (HomeSpan)
- [ ] OTA updates via web UI (partially implemented)

### Known Limitations
- 2W code has regressions from 1W focus
- Limited actuator firmware analysis
- No support for Somfy TaHoma cloud

---

## Contributing

**Before contributing**:
1. Read protocol documentation in `iown-homecontrol/docs/`
2. Understand packet structure and timing
3. Test thoroughly with 1W devices first
4. Document any 2W changes carefully

**Key areas needing help**:
- 2W stability improvements
- Additional device type support
- Firmware reverse engineering
- Protocol edge cases

---

## Resources

### Official Specifications
- io-homecontrol® is a registered trademark
- Somfy's "So Open" program

### Community
- Telegram: [t.me/iownHomecontrol](https://t.me/iownHomecontrol)
- Discord: [discord.gg/MPEb7dTNdN](https://discord.gg/MPEb7dTNdN)

### External Tools
- [rtl_433](https://github.com/merbanan/rtl_433): RTL-SDR decoder
  ```bash
  rtl_433 -R 189 -f 868.9M -s 1000k -g 42.1
  ```
- [RadioLib](https://github.com/jgromes/RadioLib): Alternative radio abstraction (not used here)

### References
- [Velocet/iown-homecontrol](https://github.com/Velocet/iown-homecontrol): Original research
- [Documentation Site](https://velocet.github.io/iown-homecontrol)

---

## License

Licensed under the Apache License 2.0. See LICENSE file for details.

**Disclaimer**: Provided "as is" for educational purposes. Use at your own risk. Not affiliated with or endorsed by Somfy or the io-homecontrol® consortium.

---

## 2W Pairing and Control Protocol Sequence

Based on verbose log analysis (`analysis/pairing_log_2w_shutter_verbose.txt`), here's the typical 2W interaction sequence:

### Discovery Phase (Broadcast Scanning)
1. **Discovery Broadcast** - Remote (F153FA) sends CMD 0x39 to broadcast 00003F
   - Contains NodeID and MAC for identification
   - Repeated 2-3 times for reliability
   
2. **Key Transfer** - Remote sends CMD 0x30 (encrypted key transfer)
   - AES-encrypted 16-byte key
   - Includes sequence number and manufacturer ID
   
3. **Device Announcement** - Devices respond with CMD 0x20
   - Contains ACEI (capability info), Main Parameter, manufacturer
   - Examples: `Main E00` (roller shutter), `Main A01`, `Main 504`, `Main 505`

### Actuator Acknowledgement (CMD 0x2C/0x2D)
Controller (AA9BFA) sends CMD 0x2C to each discovered device, devices respond with CMD 0x2D:
```
> AA9BFA → CA5321 CMD 2C (Actuator Ack Asked)
< CA5321 → AA9BFA CMD 2D (Actuator Ack Response) ~74ms later
```
This happens for multiple devices in sequence (CA5321, 2287D7, 8B133C, BC7374, etc.)

### Learning/Pairing Mode (CMD 0x2E)
1. **Enter Learning Mode** - Controller sends CMD 0x2E with data byte:
   - `DATA(01) 00` - Generic learning mode broadcast (to 00003F)
   - `DATA(01) 02` - Specific device learning mode (to device address)
   
2. **Challenge Request** - Devices respond with:
   - CMD 0x3C (Challenge Request from device to controller)
   - CMD 0x3D (Challenge from controller to device)
   - Contains 6-byte challenge data for authentication

### Control Commands (Device 01C9F9 - Roller Shutter Example)

**Command 0x19** - Get Name/Info:
```
> AA9BFA → 01C9F9 CMD 19 DATA(1) 03/04/07
< Response varies by device
```

**Command 0x03** - Set Position/Function:
```
> AA9BFA → 01C9F9 CMD 03 DATA(3) 030001  (Open/Up)
> AA9BFA → 01C9F9 CMD 03 DATA(3) 030000  (Close/Down)
> AA9BFA → 01C9F9 CMD 03 DATA(4) 03200100 (Extended command)
```

**Command 0x0C** - Extended Control (unknown function):
```
> AA9BFA → 01C9F9 CMD 0C DATA(4) d8000000
> AA9BFA → 01C9F9 CMD 0C DATA(4) d80a0000
> AA9BFA → 01C9F9 CMD 0C DATA(6) d40020d80000
```

**Command 0x46** - Advanced Function:
```
> AA9BFA → 01C9F9 CMD 46 DATA(9) 080200a60300000000
```

**Command 0x4A** - Status/Position Control:
```
> AA9BFA → 01C9F9 CMD 4A DATA(3) 030001  (likely "move up")
> AA9BFA → 01C9F9 CMD 4A DATA(3) 030000  (likely "stop/close")
```

**Command 0x50** - Key Transfer Acknowledgement:
```
> AA9BFA → 01C9F9 CMD 50 DATA(0)
```

### Device E468A9 Control Sequence
```
> AA9BFA → E468A9 CMD 19 DATA(1) 03         (Get info)
< E468A9 → AA9BFA CMD FE DATA(1) 05         (Info response)

> AA9BFA → E468A9 CMD 03 DATA(3) 030000    (Control command)
< E468A9 → AA9BFA CMD 04 DATA(14) ...      (Status response with F153FA reference)

> AA9BFA → E468A9 CMD 0C DATA(4) d8000000  (Extended command)
< E468A9 → AA9BFA CMD 0D DATA(5) 05aa380000 (Response)
```

### Timing Observations
- Response times are typically 70-90ms between command and acknowledgement
- Multiple retransmissions with varying intervals (80-400ms)
- CMD 0x3D challenges appear ~70-76ms after CMD 0x2E/learning mode
- Command sequences often include CMD 0x3D authentication before/after control

### Key Findings
1. **Authentication is continuous** - CMD 0x3D challenges occur throughout, not just at pairing
2. **Multi-step control** - Commands like position setting involve CMD 0x19 → CMD 0x03 → CMD 0x50 → CMD 0x0C sequences
3. **Device types differ** - Different Main Parameter values (E00, A01, 504, 505) indicate different device capabilities
4. **Broadcast discovery still active** - 1W devices (F153FA) continue broadcasting even during 2W operations

---

## Current 2W Implementation Status

### Overview
The 2W implementation exists but is **unstable and incomplete**. The codebase has two separate 2W device classes targeting different device types, but neither is fully functional as a general-purpose 2W controller.

### Implemented Components

#### 1. **iohcCozyDevice2W** - Atlantic/Sauter/Thermor Heating Controller
**Status**: Partially functional for specific heating devices

**Implemented Features**:
- Temperature control (7.0-28.0°C) via CMD 0x20 (SEND_WRITE_PRIVATE)
- Mode setting: auto, manual, prog, off
- Presence detection
- Window status (open/close)
- Midnight sync command
- Basic association (CMD 0x31 - SEND_ASK_CHALLENGE)

**Data Format**:
```cpp
// Commands use 0x0C 0x61 0x01 <subcommand> <value> pattern
setTemp:     {0x0C, 0x61, 0x01, 0x03, temp_value}
setMode:     {0x0C, 0x61, 0x01, 0x00, mode_value}
setPresence: {0x0C, 0x61, 0x01, 0x10, presence_value}
setWindow:   {0x0C, 0x61, 0x01, 0x0E, window_value}
```

**Limitations**:
- Hardcoded gateway address: `{0xba, 0x11, 0xad}`
- Fixed target addresses in code, not from JSON
- No dynamic device discovery
- No proper challenge-response implementation
- Only works with Atlantic/Sauter/Thermor protocol variant

#### 2. **iohcOtherDevice2W** - Generic 2W Controller (Experimental)
**Status**: Highly experimental, mostly for protocol exploration

**Implemented Features**:
- Discovery broadcast (CMD 0x28, 0x2A)
- Device name query (CMD 0x50 - GET_NAME)
- Custom command testing framework
- ACEI scanning capabilities
- Pairing mode support
- Command validity mapping

**Test/Debug Commands**:
- `discovery`: Broadcast scan for devices
- `getName`: Query device name
- `custom`: Test arbitrary commands
- `custom60`: Test 0x60 command family
- `checkCmd`: Systematic command exploration
- `scanDump`: Display discovered command map

**Limitations**:
- No actual device control implemented
- Primarily a research/testing tool
- Hardcoded device lists
- No persistent state management
- Challenge-response incomplete

#### 3. **Command Handling in main.cpp**
**Status**: Partial implementation of pairing protocol

**Implemented Handlers**:
```cpp
RECEIVED_DISCOVER_0x28         → Send DISCOVER_ANSWER_0x29
RECEIVED_DISCOVER_ANSWER_0x29  → Send DISCOVER_ACTUATOR_0x2C
RECEIVED_DISCOVER_REMOTE_ANSWER_0x2B → Store in sysTable
RECEIVED_DISCOVER_ACTUATOR_0x2C → Send DISCOVER_ACTUATOR_ACK_0x2D
RECEIVED_LAUNCH_KEY_TRANSFERT_0x38 → Send KEY_TRANSFERT_0x32
RECEIVED_CHALLENGE_REQUEST_0x3C → Send CHALLENGE_ANSWER_0x3D
RECEIVED_WRITE_PRIVATE_0x20    → Memorize command
RECEIVED_PRIVATE_ACK_0x21      → (empty handler, no action)
```

**Crypto Implementation**:
- AES encryption for key transfer
- Initial value construction for challenges
- Transfer key XOR operations
- Challenge-response computation (partial)

### Missing Critical Components

#### 1. **Complete Challenge-Response System**
- CMD 0x3C/0x3D authentication works but not fully integrated
- No automatic challenge handling during normal operations
- Challenge data not properly validated
- Missing challenge timeout/retry logic

#### 2. **Device State Management**
- No tracking of paired devices
- No storage of session keys
- No sequence number management
- No device capability caching

#### 3. **Command Set**
**Implemented (partial/incomplete)**:
- CMD 0x00: **Activate/Execute Function** - Seen in logs, not implemented. Format: `Originator + ACEI + MainParam + FP1 + FP2`
- CMD 0x28: **Discover** - Implemented in OtherDevice2W (broadcast for discovery)
- CMD 0x29: **Discover Answer** - Handled, returns node type/subtype/address/manufacturer/multiinfo/timestamp
- CMD 0x2C: **Discover Actuator Confirmation** - No params, discover ack (implemented)
- CMD 0x2D: **Discover Confirmation Ack** - No params, answer to 0x2C (implemented)
- CMD 0x31: **Ask Challenge** - Implemented, triggers 0x3C response
- CMD 0x32: **Key Transfer** - Implemented, encrypted 2W key (16 bytes)
- CMD 0x33: **Key Transfer Ack** - Implemented, no params
- CMD 0x38: **Launch Key Transfer** - Implemented, contains 6-byte challenge
- CMD 0x3C: **Challenge Request** - Implemented, 6-byte challenge data
- CMD 0x3D: **Challenge Response** - Implemented, 6-byte response data
- CMD 0x50: **Get Name** - Implemented in OtherDevice2W
- CMD 0x51: **Get Name Answer** - Handled, 16 bytes ASCII
- CMD 0x54: **Get General Info 1** - Implemented
- CMD 0x55: **General Info 1 Answer** - Handled, 14 bytes
- CMD 0x56: **Get General Info 2** - Implemented
- CMD 0x57: **General Info 2 Answer** - Handled, 16 bytes

**Missing from logs but needed**:
- CMD 0x01: **Activate Mode** - Mode number + mode parameter + 2 unknown bytes (not implemented)
- CMD 0x02: **Direct Command/Manual Order** - Serial console only, ASCII format with OID+PID (not applicable via radio)
- CMD 0x03: **Private Command** - Seen in logs, 3-6 bytes data, purpose unclear (not implemented)
- CMD 0x04: **Private Command Answer** - Seen in logs, 6-20 bytes data, first byte is error code (not implemented)
- CMD 0x0C: **Unknown (IoPrepareDataSession?)** - Seen in logs with 4 bytes data (not implemented)
- CMD 0x0D: **Answer to 0x0C** - Seen in logs, 5 bytes data (not implemented)
- CMD 0x19: **Unknown (Followed by 0xFE)** - Seen in logs, 1 byte data (02/03/04/07) (not implemented)
- CMD 0x20: **Write Private** - Manufacturer-specific private commands, can brick devices (not implemented)
- CMD 0x21: **Private Protocol Response** - Handler exists in main.cpp but empty (not implemented)
- CMD 0x2A: **Discover Remote** - 12-byte challenge key, requires authentication (not implemented)
- CMD 0x2B: **Discover Remote Answer** - Similar to 0x29 format (not implemented)
- CMD 0x30: **Send 1W Key** - 16-byte encrypted key + manufacturer + sequence (not implemented)
- CMD 0x46: **Script: Register/Upload** - 9 bytes config data (not implemented)
- CMD 0x47: **Download Config (Answer to 46)** - 5 bytes config (not implemented)
- CMD 0x4A: **Delete File / Large Data Transfer Request** - 3-13 bytes filename (not implemented)
- CMD 0xFE: **Status/Confirmation** - 1 byte error code. 0x05=NO ERROR, 0x08=ERROR_DURING_EXECUTION (minimal handling)

**Command Originators** (byte 2 in CMD 0x00):
Used to specify who/what fired the command:
- `0x00`: User (local button on actuator)
- `0x01`: **User** - Remote control (most common)
- `0x02`: Rain sensor
- `0x03`: Timer sensor
- `0x04`: Security controller (SCD)
- `0x08`: **SAAC** - Stand Alone Automatic Controls (common)
- `0x09`: Wind sensor
- `0x0C`: Local light sensor
- `0x0D`: Unspecified environment sensor
- `0x10`: Myself (actuator self-initiated)
- `0xFE`: Automatic cycle
- `0xFF`: Emergency (never disabled)

**ACEI Format** (byte 3 in CMD 0x00):
Action Control Extended Info - controls priority and session validity:
- Bits 7-5: Priority Level
  - `0b000` (0): Personal/Human - Most secure, disables all other levels
  - `0b001` (1): Environment/Goods Protection - Local sensors
  - `0b010` (2): User Level 1 - High priority controllers
  - `0b011` (3): User Level 2 - Default for remote controllers
  - `0b100-111` (4-7): Comfort/Automatic control levels
- Bits 4-3: Service Number (0-3)
- Bits 2-1: Extended Info (TBD)
- Bit 0: Is Valid Frame (always 1)

**Standard Parameter Values** (for MainParam and FP):
- `0x0000`: Min / On / 1W Button Up
- `0x0001`: 1W Button Down
- `0x0002`: 1W Button Stop
- `0x0003`: 1W Button Prog (My?)
- `0x00FE`: Button Released
- `0x00FF`: Button Stop
- `0xC800`: Max / Off / Down
- `0x0000-0xC800`: Relative Percentage (0-100%)
- `0xD100`: Target (Execution Parameter Buffer)
- `0xD200`: Current (Execution Parameter Buffer)
- `0xD300`: Default
- `0xD400`: MP: Ignore / FP: ReadOnly
- `0xF7FF`: Unknown Feedback Value

#### 4. **Status/Error Codes** (for CMD 0xFE and CMD 0x04)
**Common Status Values**:
- `0x00`: UNKNOWN_STATUS_REPLY
- `0x01`: COMMAND_COMPLETED_OK - No errors detected
- `0x02`: NO_CONTACT - No communication to node
- `0x03`: MANUALLY_OPERATED - Manually operated by a user
- `0x04`: BLOCKED - Node has been blocked by an object
- `0x05`: **WRONG_SYSTEMKEY** - The node contains a wrong system key (often used as success code in some contexts)
- `0x06`: PRIORITY_LEVEL_LOCKED - Node is locked on this priority level
- `0x07`: REACHED_WRONG_POSITION - Node stopped in another position than expected
- `0x08`: **ERROR_DURING_EXECUTION** - An error occurred during command execution
- `0x09`: NO_EXECUTION - No movement of the node parameter
- `0x0A`: CALIBRATING - Node is calibrating the parameters
- `0x0B`: POWER_CONSUMPTION_TOO_HIGH
- `0x0C`: POWER_CONSUMPTION_TOO_LOW
- `0x0D`: LOCK_POSITION_OPEN - Door open during lock command
- `0x0E`: MOTION_TIME_TOO_LONG__COMMUNICATION_ENDED - Target not reached in time
- `0x0F`: THERMAL_PROTECTION - Node has gone into thermal protection mode
- `0x10`: PRODUCT_NOT_OPERATIONAL
- `0x11`: FILTER_MAINTENANCE_NEEDED
- `0x12`: BATTERY_LEVEL - Battery level is low
- `0x13`: TARGET_MODIFIED - Node modified the target value
- `0x14`: MODE_NOT_IMPLEMENTED - Node doesn't support this mode
- `0x1B`: TARGET_NOT_REACHABLE - Node unable to reach target position
- `0x1D`: COMMAND_OVERRULED - Command was overruled by a new command
- `0xDF-0xEE`: LIMITATION codes (by user, rain, timer, SAAC, wind, emergency, etc.)

#### 5. **Multi-Frame Support**
- StartFrame/EndFrame handling exists but untested
- No frame reassembly logic
- No fragmentation for large payloads

#### 6. **Frequency Hopping**
- Radio layer supports 3-channel FHSS
- 2W packet creation sets `frequency = CHANNEL2` only
- No dynamic channel switching based on protocol state
- Timing parameters may need adjustment

#### 7. **Device Types**
Based on log analysis, need support for:
- Roller shutters (Main E00)
- Unknown type A01
- Unknown type 504, 505
- Different manufacturer protocols

### Code Quality Issues

**Hardcoded Values**:
```cpp
// From iohcCozyDevice2W.h
address gateway = {0xba, 0x11, 0xad};
address master_from = {0x47, 0x77, 0x06};
address master_to = {0x48, 0x79, 0x02};
// These should be in JSON configuration
```

**Inconsistent Patterns**:
- CozyDevice2W uses `cmd(DeviceButton, Tokens*)`
- OtherDevice2W uses `cmd(Other2WButton, Tokens*)`
- No unified device interface
- Each class has its own packet forging logic

**Memory Management**:
- Extensive use of `new iohcPacket` without clear cleanup
- Vectors of pointers without RAII wrappers
- Potential memory leaks in send paths

**Error Handling**:
- Minimal validation of received packets
- No timeout handling for expected responses
- No retry logic for failed commands

### What's Needed for Full 2W Controller

#### High Priority:
1. **Unified device database** - JSON-based like 1W.json
   - Device address, type, capabilities
   - Session keys, sequence numbers
   - Pairing status, last seen timestamp

2. **Complete pairing workflow**
   - Discovery → Challenge → Key Exchange → Association
   - Persistent storage of paired devices
   - Re-pairing after power loss

3. **Generic command framework**
   - Command factory based on device type
   - Automatic challenge injection
   - Response validation and parsing

4. **State machine per device**
   - Track pairing state
   - Manage pending commands
   - Handle async responses

5. **Proper ACEI/capability handling**
   - Parse and store device capabilities
   - Validate commands against capabilities
   - Map high-level commands to device-specific payloads

#### Medium Priority:
6. **Multi-channel coordination**
   - Implement proper channel hopping
   - Timing synchronization
   - Collision avoidance

7. **Robust crypto layer**
   - Automatic challenge generation
   - Key rotation support
   - Secure key storage (NVS)

8. **Device type abstraction**
   - Base class for 2W devices
   - Type-specific command builders
   - Polymorphic device handling

9. **Testing framework**
   - Packet simulation
   - Mock devices for testing
   - Command validation tools

#### Low Priority:
10. **MQTT/Web integration**
    - Similar to 1W implementation
    - Device discovery via MQTT
    - Status updates

11. **Multi-frame support**
    - Large payload fragmentation
    - Frame reassembly
    - Out-of-order handling

12. **Advanced features**
    - Scenes/groups
    - Scheduled commands
    - Status polling

### Recommended Development Path

**Phase 1: Stabilization** (Current → MVP)
1. Create unified `Other2W.json` format based on 1W.json
2. Implement basic device CRUD (add/remove/list)
3. Fix memory leaks in packet sending
4. Add proper error handling

**Phase 2: Core Protocol** (MVP → Functional)
5. Complete challenge-response automation
6. Implement full pairing workflow
7. Add sequence number management
8. Store session keys in NVS

**Phase 3: Device Support** (Functional → Usable)
9. Reverse-engineer common command patterns from logs
10. Add support for roller shutters (most common use case)
11. Implement position tracking
12. Add MQTT integration

**Phase 4: Polish** (Usable → Production)
13. Multi-channel support
14. Web UI for 2W devices
15. Home Assistant auto-discovery
16. Comprehensive testing

### Current Blockers

1. **Incomplete protocol understanding** - Many commands (0x0C, 0x46, 0x4A) have unknown purposes
2. **Device-specific behavior** - Different manufacturers use different command patterns
3. **Timing sensitivity** - 2W requires precise timing that may conflict with 1W optimizations
4. **Testing difficulty** - Requires real 2W hardware to validate
5. **Documentation gaps** - Less community knowledge compared to 1W

### Conclusion

The 2W implementation is a **work-in-progress research project**, not a production-ready controller. It demonstrates protocol knowledge but lacks the robustness and completeness of the 1W implementation. Significant development effort is required to achieve feature parity with 1W.

**For developers**: Focus on understanding the protocol through log analysis before attempting major refactoring. The existing code provides valuable insights even if incomplete.

**For users**: Stick with 1W devices or use official Somfy gateways for 2W. The current 2W code is primarily educational.

## Quick Start for AI Agents

When assisting with this project:

1. **Understand the context**: This is a protocol implementation, not just device control
2. **1W vs 2W**: Know which mode the user is working with (1W is stable, 2W is WIP)
3. **JSON configs**: Device state is in `extras/*.json` files
4. **Timing critical**: Radio operations use ISR-based state machines
5. **Check sdkconfig**: ESP-IDF timer settings are crucial
6. **Read protocol docs**: `iown-homecontrol/docs/` has definitive specs
7. **Serial debugging**: Most issues can be diagnosed via serial output with verbose mode

**Common tasks**:
- Adding a new 1W device → `new1W` command or edit `1W.json`
- Modifying commands → `src/interact.cpp` and respective device classes
- Web UI changes → `extras/web_interface_data/` + rebuild filesystem
- MQTT integration → `src/mqtt_handler.cpp`
- Timing issues → `src/iohcRadio.cpp`, check ticker settings

**Code style**:
- Classes use singleton pattern
- Delegates for callbacks
- Extensive use of namespaces (`IOHC::`)
- Bitfield structs for protocol parsing
