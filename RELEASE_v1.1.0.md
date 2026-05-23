# v1.1.0 - WiFi Streaming & Web Interface

ESP32 MP3 Player now with **WiFi HTTP streaming** and **web-based music management**! 🎵

## 🌐 What's New

### WiFi Streaming Mode
- Stream music over HTTP to any device on your network
- Activate by clicking the button **5 times**
- Seamless switching between Bluetooth A2DP and WiFi modes
- Persistent mode configuration across restarts

### 🖥️ Web Interface & HTTP Server
- Modern web dashboard hosted directly on the ESP32
- Real-time music library viewer
- Edit metadata and manage your collection
- Fixed infinite loading issues for stable experience
- Improved HTML rendering and responsiveness

### 📊 Advanced Logging System
- Comprehensive per-module logging configuration
- Real-time system diagnostics and monitoring
- Memory and heap usage tracking
- Stream stability and buffer monitoring
- Easy log level adjustment without recompilation

### 🎵 Enhanced Music Management
- View and browse MP3 library via web UI
- Edit track metadata remotely
- Improved media library enumeration
- Partial streaming support for continuous playback

---

## ✨ Complete Feature Set

### Connectivity
- ✅ **Bluetooth A2DP** wireless streaming
- ✅ **WiFi HTTP** streaming (NEW)
- ✅ Dual-mode operation with seamless switching
- ✅ Auto-reconnection and discovery management

### Audio & Playback
- ✅ MP3 playback from microSD card
- ✅ Real-time Helix MP3 decoding
- ✅ AVRCP control support
- ✅ Volume control with configurable steps
- ✅ Random track selection
- ✅ Decode stall recovery
- ✅ Pre-buffering for stable streaming

### Management & Control
- ✅ Web-based music library viewer (NEW)
- ✅ Metadata editor (NEW)
- ✅ WiFi/Bluetooth mode switching via button (NEW)
- ✅ Comprehensive logging system (NEW)
- ✅ Deep sleep with wakeup button
- ✅ Power LED status indicator

### Configuration
- ✅ WiFi settings (SSID, password, HTTP port)
- ✅ Bluetooth target device MAC address
- ✅ Hardware pin customization
- ✅ Buffer and timeout settings
- ✅ Volume and power parameters
- ✅ Per-module logging configuration

---

## 🔨 Technical Stack

| Component | Details |
|-----------|---------|
| **Languages** | C (86.3%), Assembly (4.9%), C++ (2.0%) |
| **Framework** | ESP-IDF 5.4.2 |
| **RTOS** | FreeRTOS with multi-task architecture |
| **Audio** | Helix MP3 codec (esp-libhelix-mp3) |
| **Connectivity** | Bluetooth Classic A2DP + WiFi HTTP |
| **Hardware** | ESP32 Dual-core 240MHz, 520KB RAM |

---

## 🚀 Getting Started

### Quick Installation
```bash
git clone https://github.com/GF-Silva/MusicPlayer.git
cd MusicPlayer
idf.py set-target esp32
idf.py menuconfig
idf.py build
idf.py -p /dev/YOUR_PORT flash monitor
```

### Setup Steps
1. **Add Music:** Place MP3 files in microSD card root
2. **Configure:** Use `idf.py menuconfig` → `MusicPlayer Configuration`
   - Set Bluetooth target MAC address
   - Configure WiFi SSID/password (optional)
   - Set hardware GPIO pins if needed
3. **Flash:** Upload firmware to ESP32
4. **Use:**
   - Press button once → Bluetooth mode (default)
   - Click 5 times → WiFi mode
   - Access web UI at `http://esp32.local`

---

## 📋 What's Changed

### Major Features
- `feat: add a wi-fi mode activated by clicking 5 times in the main btn`
- `feat: Partial creation of the add musics stream`
- `Add WiFi music and config management`
- `Improved html`

### Fixes & Improvements
- `fix (web): Corrigido o carregamento infinito da web`
- `feat: add configuration options for Bluetooth target, hardware pins, input, power, and audio pipeline`
- `docs: update README to clarify Bluetooth MAC address configuration`

---

## 🔄 Compatibility

**✅ Backward Compatible** - Existing v1.0.0 Bluetooth setups work without changes!

### Migration Guide
1. Rebuild with ESP-IDF 5.4.2+
2. Optional: Configure WiFi settings in menuconfig
3. Flash normally - all existing features remain unchanged

**No breaking changes to hardware configuration.**

---

## ⚙️ Requirements

- **Hardware:** ESP32 with 520KB RAM minimum
- **Storage:** microSD Card (Class 10+ recommended)
- **Build:** ESP-IDF 5.4.2 or newer
- **Dependencies:** FreeRTOS, Helix MP3 (included in repo)

---

## 🐛 Known Limitations

- MP3 files limited to microSD root directory
- Random track selection only (next/previous not implemented)
- WiFi streaming requires network configuration
- No folder hierarchies in library view
- Logging requires 115200 baud UART connection

---

## 📚 Documentation

See `RELEASE_NOTES.md` for:
- Detailed feature descriptions
- Configuration menu reference
- Troubleshooting guide
- Performance optimization tips
- Development guidelines

---

## 📦 Download

Firmware binaries (`.bin`, `.elf`) can be added to assets after building:

```bash
# After successful build, binaries located at:
# build/musicPlayer.bin       # Firmware binary
# build/musicPlayer.elf       # ELF for debugging
```

For detailed instructions, visit the [GitHub repository](https://github.com/GF-Silva/MusicPlayer).

---

## 🎛️ Configuration Menu Reference

Access via `idf.py menuconfig` → `MusicPlayer Configuration`:

```
┌─ MusicPlayer Configuration
├─ WiFi Settings (NEW)
│  ├─ WiFi SSID
│  ├─ WiFi Password
│  └─ HTTP Server Port (default: 80)
├─ Bluetooth Settings
│  ├─ Target Device MAC (required)
│  └─ Target Device Name
├─ Hardware Settings
│  ├─ SD MOSI GPIO
│  ├─ SD MISO GPIO
│  ├─ SD CLK GPIO
│  ├─ SD CS GPIO
│  ├─ Power/Wake GPIO
│  └─ LED GPIO
├─ Audio Pipeline
│  ├─ Stream Buffer Size
│  ├─ MP3 Input Buffer Size
│  ├─ PCM Output Buffer Size
│  ├─ Prebuffer Frames
│  └─ Volume Settings
└─ Logging (NEW)
   ├─ Default Log Level
   ├─ Per-Module Levels
   └─ Stream Diagnostics
```

---

## 💡 Quick Tips

### Mode Switching
- **Default:** Starts in Bluetooth A2DP mode
- **WiFi Mode:** Click button 5 times to activate
- **Switch Back:** Click 5 times again to return to Bluetooth
- **Web UI:** Access at `http://esp32.local` (or device IP)

### Performance Optimization
- Use Class 10 microSD card for better read speeds
- Keep buffer sizes at defaults unless experiencing issues
- Position ESP32 in WiFi range for stable streaming
- Monitor logs with `idf.py monitor` at 115200 baud

### Troubleshooting
- **WiFi not connecting?** Verify SSID/password in menuconfig
- **Web page loads forever?** Fixed in this version; reflash device
- **Bluetooth disconnects?** Check target MAC address
- **Low memory?** Reduce buffer sizes in Audio Pipeline menu

---

## 📝 Credits & Acknowledgments

**Built With:**
- ESP-IDF by Espressif Systems
- FreeRTOS Real-Time Operating System
- Helix MP3 Decoder Library
- Community feedback and contributions

**Author:** GF-Silva  
**License:** MIT (see LICENSE file)  
**Repository:** https://github.com/GF-Silva/MusicPlayer

---

## 📞 Support & Feedback

Found a bug? Have a feature request? Suggestions?

Please open an issue on [GitHub Issues](https://github.com/GF-Silva/MusicPlayer/issues) with:
- Clear description of the problem
- Reproduction steps (if applicable)
- Device configuration (GPIO pins, WiFi/BT MAC, etc.)
- Log output from serial monitor

---

**🎵 Thank you for using ESP32 MusicPlayer v1.1.0!**

*Dual-mode streaming, web management, and advanced logging - all in your pocket!*
