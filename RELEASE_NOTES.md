# v1.1.0 Release Notes - WiFi Streaming & Web Interface

**Release Date:** May 2026  
**Changelog Commits:** v1.0.0...v1.1.0

---

## 🎉 Highlights

### 🌐 WiFi Streaming Mode
- **New WiFi connectivity** for remote music access
- Activated with **5 clicks on the main button** for seamless mode switching
- Stream music over HTTP from your ESP32 to any device on your network

### 🖥️ HTTP Web Server & Interface
- **Embedded HTTP server** hosted on the ESP32
- Modern web interface for music control and management
- Real-time web page for interaction with the device
- Infinite loading issue fixed for stable web experience
- HTML interface improvements and optimizations

### 🎵 Music Library Management
- **View music library** via web interface
- **Edit metadata** directly from the web UI
- Browse and organize your MP3 collection remotely
- Enhanced media library management functions

### 📊 Advanced Logging & Diagnostics
- **Comprehensive logging system** with configurable log levels
- Per-module logging tags for better debugging
- Real-time system status monitoring
- Buffer monitoring and stream stability tracking
- Detailed heap usage and memory diagnostics

### 🔧 Additional Enhancements
- WiFi music and config management system
- Partial streaming support for continuous playback
- Improved HTML rendering and web UI responsiveness
- Enhanced system bootstrap and initialization

---

## ✨ Features (Complete)

### Connectivity
- ✅ **Bluetooth A2DP** wireless streaming (from v1.0.0)
- ✅ **WiFi HTTP streaming** (NEW)
- ✅ **Dual-mode operation** - seamless Bluetooth/WiFi switching
- ✅ Auto-reconnection and discovery management

### Audio & Playback
- ✅ MP3 playback from microSD card
- ✅ Helix MP3 decoder for real-time decoding
- ✅ AVRCP control support for Bluetooth devices
- ✅ Volume control with configurable steps
- ✅ Random track selection
- ✅ Decode stall recovery mechanism
- ✅ Pre-buffer before stream initiation

### Management & Control
- ✅ Web-based music library viewer and editor (NEW)
- ✅ WiFi mode activation (5 clicks)
- ✅ Bluetooth/WiFi mode toggle
- ✅ Deep sleep with button wakeup
- ✅ Power LED status indicator
- ✅ Comprehensive logging system (NEW)

### Configuration
- ✅ `menuconfig` for Bluetooth device MAC address
- ✅ Hardware pin customization
- ✅ Buffer and timeout settings
- ✅ Volume and power parameters
- ✅ WiFi-specific configurations (NEW)

---

## 🔨 Technical Stack

| Component | Details |
|-----------|---------|
| **Language** | C (86.3%), Assembly (4.9%), C++ (2.0%) |
| **Framework** | ESP-IDF 5.4.2 |
| **RTOS** | FreeRTOS with multi-task architecture |
| **Audio Codec** | Helix MP3 (esp-libhelix-mp3) |
| **Connectivity** | Bluetooth Classic A2DP, WiFi HTTP |
| **Hardware** | ESP32 Dual-core 240MHz, 520KB RAM |

---

## 📦 What's New in v1.1.0

### New Modules & Features
1. **WiFi Mode System**
   - Activation via 5 button clicks
   - Separate WiFi streaming pipeline
   - Persistent WiFi configuration

2. **Web Interface**
   - HTTP server on ESP32
   - Music library browser
   - Metadata editor
   - System status dashboard
   - Real-time control interface

3. **Logging System**
   - Centralized `app_log.h/c` for logging
   - Per-module log level configuration
   - Memory and heap diagnostics
   - Stream stability monitoring

4. **Enhanced Media Library**
   - Improved file enumeration
   - Metadata extraction and display
   - Remote library management

### Bug Fixes & Improvements
- Fixed infinite loading in web interface
- Improved HTML rendering performance
- Enhanced system bootstrap initialization
- Better Bluetooth discovery and reconnection
- Optimized buffer management

---

## 🚀 Getting Started

### Installation
```bash
git clone https://github.com/GF-Silva/MusicPlayer.git
cd MusicPlayer
idf.py set-target esp32
idf.py menuconfig
idf.py build
idf.py -p /dev/YOUR_PORT flash monitor
```

### Quick Start
1. **Prepare Files:** Add MP3 files to the root of your microSD card
2. **Configure:** Use `idf.py menuconfig` to set:
   - Bluetooth target device MAC address
   - WiFi SSID and password (optional)
   - Hardware GPIO pins
3. **Flash:** Upload firmware to ESP32
4. **Connect:** 
   - Press button to activate Bluetooth mode (default)
   - Click 5× to switch to WiFi mode
   - Access web interface at `http://esp32.local`

### Mode Switching
- **Bluetooth A2DP:** Device starts in this mode by default
- **WiFi HTTP:** Press the button 5 times to activate
- Mode preference persists across restarts

---

## 🎛️ Configuration Menu

Access via `idf.py menuconfig` → `MusicPlayer Configuration`:

```
┌─ MusicPlayer Configuration
├─ WiFi Settings (NEW)
│  ├─ SSID
│  ├─ Password
│  └─ HTTP Server Port
├─ Bluetooth Settings
│  ├─ Target Device MAC
│  └─ Target Device Name
├─ Hardware Settings
│  ├─ SD Card Pins
│  ├─ Power/Wake GPIO
│  └─ LED GPIO
├─ Audio Pipeline
│  ├─ Buffer Sizes
│  ├─ Volume Settings
│  └─ Prebuffer Frames
└─ Logging (NEW)
   ├─ Log Level
   └─ Per-Module Settings
```

---

## ⚙️ System Requirements

- **Hardware:** ESP32 with 520KB RAM minimum
- **Storage:** microSD Card (Class 10+ recommended)
- **Build:** ESP-IDF 5.4.2+
- **Dependencies:** FreeRTOS, Helix MP3 (included)

---

## 🐛 Known Limitations

- MP3 files limited to root directory of microSD
- Random track selection (next/previous not implemented yet)
- WiFi streaming requires network configuration
- No folder hierarchies in library view
- Log output dependent on UART baud rate (115200)

---

## 📋 Commits in this Release

**Major Features:**
- `feat: add a wi-fi mode activated by clicking 5 times in the main btn`
- `feat: Partial creation of the add musics stream`
- `Add WiFi music and config management`
- `Improved html`

**Fixes & Improvements:**
- `fix (web): Corrigido o carregamento infinito da web`
- `feat: add configuration options for Bluetooth target, hardware pins, input, power, and audio pipeline`
- `docs: update README to clarify Bluetooth MAC address configuration in menuconfig`

---

## 🔄 Migration from v1.0.0

**Backward Compatible:** ✅ Yes

Your existing Bluetooth A2DP setup continues to work without any changes. WiFi mode is **optional** and activated only when triggered.

**What to Update:**
1. Rebuild with new ESP-IDF (v5.4.2 recommended)
2. Optional: Configure WiFi settings in menuconfig
3. Optional: Configure web server port (default: 80)
4. Flash as normal

No breaking changes to existing hardware configurations.

---

## 💡 Tips & Tricks

### Optimize Performance
- Use a Class 10 microSD card for better read speeds
- Position ESP32 in WiFi range for stable streaming
- Keep buffer sizes at defaults unless experiencing issues

### Troubleshooting
- **WiFi not connecting?** Check SSID/password in menuconfig
- **Web page loads forever?** Issue fixed in this release; reflash
- **Bluetooth disconnects?** Verify target MAC address in menuconfig
- **Low memory?** Reduce buffer sizes in Logging menu

### Development
- Check logs in `main/app_log.c` for debugging
- Modify logging levels per-module without recompilation
- Use monitor (`idf.py monitor`) for real-time diagnostics

---

## 📝 Credits & Acknowledgments

**Project Built With:**
- ESP-IDF by Espressif
- FreeRTOS RTOS
- Helix MP3 Decoder
- Community feedback and contributions

**Author:** GF-Silva  
**License:** MIT (see LICENSE file)

---

## 📞 Support & Feedback

Found a bug? Have a feature request?  
Please open an issue on [GitHub Issues](https://github.com/GF-Silva/MusicPlayer/issues)

---

**Thank you for using ESP32 MusicPlayer! 🎵**
