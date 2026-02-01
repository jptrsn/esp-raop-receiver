# ESP32 AirPlay Receiver

A high-quality AirPlay audio receiver implementation for ESP32-WROVER-B with external DAC support, featuring multi-room synchronization and robust audio buffering.

## Features

- **AirPlay Audio Streaming**: Full AirPlay 1 protocol support with authentication
- **Multi-room Sync**: Synchronized playback across multiple AirPlay devices
- **High-Quality Audio**: PCM5102A DAC support via I²S (44.1kHz, 16-bit stereo)
- **Software Volume Control**: Smooth volume adjustment
- **ALAC Decoding**: Apple Lossless audio codec support
- **Robust Buffering**: ~23 seconds of audio buffering with automatic drift correction
- **Web Configuration**: Simple web interface for WiFi setup and I²S pin configuration
- **WiFi Provisioning**: Easy setup via captive portal

## Hardware Requirements

- **ESP32-WROVER-B** (4MB PSRAM required)
- **PCM5102A DAC** or compatible I²S DAC
- Power supply (5V recommended)

### Default I²S Pin Configuration

- BCK (Bit Clock): GPIO 26
- WS (Word Select/LRCLK): GPIO 25
- DOUT (Data Out): GPIO 22

Pins can be reconfigured via the web interface.

## Setup

### Prerequisites

- [ESP-IDF v5.5.1](https://docs.espressif.com/projects/esp-idf/en/v5.5.1/esp32/get-started/index.html)
- Python 3.8+

### Building and Flashing

1. Clone the repository:
```bash
git clone https://codeberg.org/edu_coder/esp-airplay.git
cd esp-airplay
```

2. Set up ESP-IDF environment:
```bash
. ~/esp/esp-idf/export.sh
```

3. Configure (optional):
```bash
idf.py menuconfig
```

4. Build:
```bash
idf.py build
```

5. Flash to device:
```bash
idf.py -p /dev/ttyUSB0 flash monitor
```

### Initial Configuration

1. On first boot, the device creates a WiFi access point named `ESP32-AirPlay-Setup`
2. Connect to this network (password: `airplay123`)
3. Navigate to `192.168.4.1` in your browser
4. Enter your WiFi credentials and configure I²S pins if needed
5. Device will reboot and connect to your network

### Usage

Once configured:
1. Device appears as "ESP32-AirPlay" in your AirPlay device list
2. Select it from iTunes, Music.app, or any AirPlay-compatible application
3. Audio streams with automatic synchronization for multi-room playback

## Architecture

- **RTSP Server**: Handles AirPlay control protocol (Core 0)
- **RTP Receiver**: Manages audio packet reception and jitter buffering (Core 1)
- **Audio Buffer**: Large PSRAM-based ring buffer with timing-aware playback (Core 1)
- **I²S Output**: Hardware audio interface with software volume control
- **Timing Sync**: NTP-based synchronization for multi-room playback

## Credits and Acknowledgments

This project builds upon and incorporates code from:

### Squeezelite-ESP32
- **Project**: [https://github.com/sle118/squeezelite-esp32](https://github.com/sle118/squeezelite-esp32)
- **Contributors**: Philippe G., Sebastien, and contributors
- **Used for**: RTP packet handling, jitter buffer implementation, audio buffering architecture, and timing synchronization logic
- **License**: GPL v3 / MIT (components vary)

### Shairport-sync
- **Project**: [https://github.com/mikebrady/shairport-sync](https://github.com/mikebrady/shairport-sync)
- **Author**: Mike Brady and contributors
- **Used for**: RTSP protocol handling, RSA authentication with mbedtls v3, and AirPlay protocol reference
- **License**: GPL v2+

### ALAC Decoder
- Apple Lossless Audio Codec decoder
- **License**: Apache 2.0

Special thanks to the developers of these projects for their excellent work in reverse-engineering and implementing the AirPlay protocol.

## License

This project is licensed under the **GNU General Public License v3.0** - see the [LICENSE](LICENSE) file for details.

This license was chosen to maintain compatibility with the GPL-licensed components from squeezelite-esp32 and shairport-sync.

## Technical Details

### Memory Usage
- **PSRAM**: ~2.7MB for audio buffers (RTP jitter buffer + ring buffer)
- **DRAM**: ~180KB for code and data
- **Flash**: ~900KB application binary

### Network Requirements
- 2.4GHz WiFi (ESP32 limitation)
- Multicast DNS (mDNS) support on your network
- UDP ports for RTP, control, and timing packets (dynamically allocated)

## Troubleshooting

### Device not appearing in AirPlay list
- Ensure device and client are on the same network
- Check that mDNS/Bonjour is not blocked by firewall
- Verify device has successfully connected to WiFi (check serial monitor)

### Audio dropouts or stuttering
- Check WiFi signal strength
- Verify PSRAM is detected (should show 8MB PSRAM, 4MB mapped)
- Monitor for "Buffer full" or "Skipping late frame" warnings in logs

### Authentication failures
- RSA authentication errors indicate mbedtls issues
- Ensure ESP-IDF v5.5.1 is being used

## Development

### Project Structure
```
esp-airplay/
├── main/                   # Main application code
├── components/
│   ├── raop/              # RAOP protocol implementation
│   ├── audio_buffer/      # Timing-aware audio ring buffer
│   ├── i2s_output/        # I²S DAC interface
│   └── codecs/            # Audio codec libraries
└── partitions.csv         # Flash partition table
```

### Building with Debug Symbols
```bash
idf.py menuconfig
# Compiler options → Optimization Level → Debug (-Og)
idf.py build
```

## Contributing

Contributions are welcome! Please:
1. Fork the repository on Codeberg
2. Create a feature branch
3. Make your changes with clear commit messages
4. Submit a pull request

## Known Limitations

- AirPlay 1 only (AirPlay 2 not supported)
- No video support
- 2.4GHz WiFi only (ESP32 hardware limitation)
- Only 4MB of 8MB PSRAM usable (ESP32 address space limitation)

## Future Enhancements

- [ ] AirPlay 2 support (requires significant protocol work)
- [ ] Additional DAC support (I²S variants)
- [ ] Equalizer/DSP features
- [ ] Multiple endpoint support
- [ ] MQTT integration for home automation

## Support

For issues, questions, or contributions, please use the Codeberg issue tracker.

---

**Note**: AirPlay is a trademark of Apple Inc. This project is an independent implementation and is not affiliated with or endorsed by Apple Inc.