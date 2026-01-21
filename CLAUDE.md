# CLAUDE.md - AI Assistant Guide for Cypher Jammer

## Project Overview

**Cypher Jammer** is an open-source wireless jamming and penetration-testing device for educational and security research purposes. It generates noise signals in the 2.4 GHz frequency band to simulate interference with Bluetooth and WiFi communications.

**CRITICAL LEGAL WARNING**: Jamming wireless signals is illegal in many jurisdictions without explicit authorization. This project is for educational and authorized security research only.

### Key Technologies
- **Microcontroller**: ESP32-WROOM-32D
- **RF Modules**: 1-2x NRF24L01+PA+LNA transceivers
- **Platform**: Arduino/ESP32 ecosystem
- **License**: GNU GPL v3

## Repository Structure

```
cypher-jammer/
├── README.md                    # Main documentation with wiring diagrams
├── BEGINNER.md                  # Beginner's guide with safety notes
├── LICENSE                      # GNU GPLv3 license
├── flash1.html                  # Browser-based firmware flasher
│
├── cypher-jammer/
│   └── cypher-jammer.ino        # Main dual NRF24 firmware (primary)
│
├── smoochie_version/            # Alternative firmware variants
│   ├── FOR DUAL PINS.ino        # Dual NRF24 with toggle switch
│   ├── FOR HSPI PIN.ino         # Single NRF24 on HSPI
│   ├── FOR VSPI PIN.ino         # Single NRF24 on VSPI
│   └── User_control_channel.ino # User-selectable channel hopping
│
├── hardware/                    # Hardware design files
│   ├── Gerber_cypher_jammer.zip # PCB manufacturing files
│   ├── Schematic_cypher_jammer.pdf
│   └── cypher-jammer.svg        # PCB artwork
│
├── web/                         # Precompiled binaries & manifests
│   ├── *dual*.ino.*             # Dual NRF24 binaries
│   ├── *single*.ino.*           # Single VSPI binaries
│   ├── buttonhspi.ino.*         # Single HSPI binaries
│   └── manifest_Ble*.json       # Web flasher configurations
│
└── image/                       # Product photos
```

## Build & Development

### Dependencies

**Required Arduino Libraries:**
1. **RF24** - NRF24L01 driver (https://github.com/nRF24/RF24)
2. **ezButton** - Button handling library
3. **ESP32 Arduino Core** - Board support package

**Built-in Libraries (no installation needed):**
- `SPI.h` - SPI communication
- `esp_bt.h` - Bluetooth stack control
- `esp_wifi.h` - WiFi stack control

### Compilation

**Arduino IDE Method:**
1. Install Arduino IDE with ESP32 board package
2. Install RF24 and ezButton via Library Manager
3. Select board: "ESP32 Dev Module"
4. Open desired .ino file and compile/upload

**Web Flasher Method (No compilation):**
1. Open `flash1.html` in Chrome/Chromium
2. Select hardware configuration (Ble1/Ble2/Ble3)
3. Connect ESP32 and flash directly

### Binary Offsets (for manual flashing)
- Bootloader: 0x1000 (4096)
- Partitions: 0x8000 (32768)
- Firmware: 0x10000 (65536)

## Hardware Configuration

### Pin Mapping

```
HSPI (SPI Bus 1):
  SCK=GPIO14, MISO=GPIO12, MOSI=GPIO13, CS=GPIO15, CE=GPIO16

VSPI (SPI Bus 2):
  SCK=GPIO18, MISO=GPIO19, MOSI=GPIO23, CS=GPIO21, CE=GPIO22

Toggle Switch (optional): GPIO33
```

### Firmware Variants

| Variant | File | NRF24 Count | SPI Bus | Switch |
|---------|------|-------------|---------|--------|
| Main | cypher-jammer.ino | 2 | HSPI+VSPI | No |
| Dual Control | FOR DUAL PINS.ino | 2 | HSPI+VSPI | Yes |
| HSPI Single | FOR HSPI PIN.ino | 1 | HSPI | Yes |
| VSPI Single | FOR VSPI PIN.ino | 1 | VSPI | Yes |
| User Channels | User_control_channel.ino | 1 | VSPI | No |

## Code Conventions

### RF Configuration Constants
```cpp
RF24_PA_MAX      // Maximum transmit power
2MBPS            // Data rate
CRC disabled     // No error checking
AutoAck disabled // No acknowledgments
0 retries        // No retry logic
```

### Function Naming
- `one()`, `two()` - Channel hopping pattern functions
- `initHP()` - Initialize HSPI radio
- `initSP()` - Initialize VSPI radio

### Serial Debug
- Baud rate: 115200
- Status messages during initialization

## Key Files to Understand

| File | Purpose |
|------|---------|
| `cypher-jammer/cypher-jammer.ino` | Primary firmware, understand this first |
| `flash1.html` | Web flasher UI, uses esp-web-tools |
| `hardware/Schematic_cypher_jammer.pdf` | Circuit design reference |
| `web/manifest_Ble*.json` | Flasher configuration files |

## Common Tasks

### Adding a New Channel Hopping Pattern
1. Create a new function in the .ino file
2. Define channel array or selection logic
3. Call `setChannel()` and `startConstCarrier()` in loop
4. Add switch case if using toggle switch control

### Modifying RF Parameters
1. Edit RF24 configuration in `setup()` or init functions
2. For payload/carrier modifications, edit RF24.cpp (line ~1972)
3. Recompile and flash

### Creating New Firmware Variant
1. Copy closest existing .ino file
2. Modify pin definitions if needed
3. Adjust channel hopping logic
4. Create matching manifest_*.json for web flasher
5. Compile and add binaries to web/ folder

## Important Warnings

1. **Legal Compliance**: Jamming is illegal without authorization in most countries
2. **Frequency Range**: 2.4 GHz affects Bluetooth, WiFi, drones, and other devices
3. **Test Range**: ~10 meters effective range reported
4. **Capacitors Required**: Add 10-100µF decoupling capacitors to NRF24 modules
5. **No .gitignore**: Repository tracks all files including binaries

## Git Workflow

- Main branch contains stable releases
- Commit messages use imperative format: "docs: add...", "Update..."
- PRs used for significant changes (see PR #1 for example)
- Current development branch: `claude/add-claude-documentation-CFKEe`

## Contributors

- **smoochiee** (Smoochie) - Primary author
- **David Kyazze / dkyazzentwatwa** - Repository maintainer
- Based on "Noisy Boy" BLE jammer project

## Resources

- [RF24 Library Documentation](https://github.com/nRF24/RF24)
- [ESP32 Arduino Core](https://github.com/espressif/arduino-esp32)
- [ezButton Library](https://arduinogetstarted.com/tutorials/arduino-button-library)
