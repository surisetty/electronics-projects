# IR_ESP8266 — Universal IR Remote Controller

A WiFi-enabled universal IR transmitter built on the ESP8266 NodeMCU.  
Control any AC, TV, or IR device from a browser — no app required.

> Developed by **Naveen Surisetty**

---

## Web Interface 

<img width="1390" height="1600" alt="WhatsApp Image 2026-05-01 at 5 58 12 PM" src="https://github.com/user-attachments/assets/60059a40-37c4-4b67-9bf5-e24778d94bec" />

<img width="1202" height="994" alt="image" src="https://github.com/user-attachments/assets/6b6f3f54-38e5-4f31-8645-91f2712802ff" />
<img width="678" height="936" alt="image" src="https://github.com/user-attachments/assets/3d4340de-ba8a-492f-b19b-a62d4f6bbf76" />

https://youtu.be/kJczzHXjUr4?si=K3VXQkY7WFXsv8iT

## Features

- **Universal IR protocol support** — NEC, LG, LG2, Samsung, Sony, RC5, RC6, Panasonic, JVC, Sharp, Denon, Coolix, Mitsubishi, Whynter, Daikin
- **Web UI** — beautiful dark-themed dashboard accessible from any browser on your local network
- **Quick presets** — one-tap load for LG AC, LG TV, Samsung TV, Sony TV, Panasonic TV, Daikin AC
- **ON / OFF scheduler** — set separate times to automatically send ON and OFF commands
- **Manual control** — send ON or OFF instantly from the UI
- **Custom IR codes** — enter any hex code + bit count for any device
- **WiFiManager** — captive portal on first boot, no hardcoded credentials
- **mDNS** — access at `http://ir-remote.local` (configurable name)
- **NTP clock** — auto-synced time, IST (UTC+5:30) by default, configurable
- **Persistent config** — all settings saved to flash (LittleFS), survive power cycles
- **WiFi reset** — hold button for 10 seconds at boot to clear saved WiFi

---

## Hardware

### Components

| Component | Details |
|---|---|
| Microcontroller | ESP8266 NodeMCU (ESP-12E) |
| IR LED | Any 940 nm IR LED |
| NPN Transistor | 2N2222 or BC547 (recommended for range) |
| Resistors | 1 kΩ (base), 100 Ω (LED) |
| Push button | Momentary, normally open |

### Wiring — Recommended (Transistor Driver)

```
                        5V (Vin)
                           │
                          100Ω
                           │
                        IR LED  (anode +)
                           │
                        IR LED  (cathode -)
                           │
                        Collector
                           │
                        2N2222 NPN
                           │
                        Emitter
                           │
                          GND
                   
         D2 (GPIO4) ──── 1kΩ ──── Base (2N2222)
```

```
NodeMCU          2N2222 NPN        IR LED
─────────        ──────────        ──────
D2 (GPIO4) ──┐
             1kΩ
             │
             └──► Base
                  Emitter ──────── GND
                  Collector ──────► Cathode (-)
                                    Anode (+) ──── 100Ω ──── 5V (Vin)

D5 (GPIO14) ────── Button ────── GND
                   (uses internal pull-up — no external resistor needed)
```

### Wiring — Simple (No Transistor, Short Range)

```
NodeMCU          IR LED
─────────        ──────
D2 (GPIO4) ──── 100Ω ──── Anode (+)
                           Cathode (-) ──── GND
```

> Range: ~1–2 m without transistor, 5+ m with transistor driver.

---

### Pin Reference

| NodeMCU Pin | GPIO | Function |
|---|---|---|
| D2 | GPIO4 | IR LED output |
| D5 | GPIO14 | WiFi reset button |

> Avoid D3, D4, D8 for IR output — these are boot-strapping pins and may cause boot issues.

---

## Software Setup

### Required Libraries

Install all via **Arduino IDE → Tools → Manage Libraries**:

| Library | Author |
|---|---|
| IRremoteESP8266 | David Conran |
| WiFiManager | tzapu (v2.x) |
| NTPClient | Fabrice Weinberg |
| ArduinoJson | Benoit Blanchon (v6.x) |

### Board Settings

| Setting | Value |
|---|---|
| Board | NodeMCU 1.0 (ESP-12E Module) |
| Flash Size | **4MB (FS:2MB, OTA:~1MB)** |
| Upload Speed | 115200 |

> The Flash Size setting is important — LittleFS needs the filesystem partition.

### Flash

1. Open `IR_ESP8266.ino` in Arduino IDE
2. Select the correct board and port
3. Click **Upload**

---

## First Boot

1. Device powers on and creates a WiFi hotspot: **`IRREMOTE`** (open, no password)
2. Connect your phone or PC to `IRREMOTE`
3. A captive portal opens automatically — or browse to `192.168.4.1`
4. Enter your home WiFi credentials and optionally set the device hostname
5. Device connects and is accessible at `http://ir-remote.local` or its IP address

---

## Web Interface

### Scheduler
Set separate ON and OFF times. Each can be enabled/disabled independently.  
Changes apply immediately — no restart needed.

### Manual Control
Send ON or OFF command instantly with a single tap.

### IR Device Card
- **Quick Presets** — tap a chip to load known codes for common devices
- **Protocol selector** — choose from 15 supported protocols; bit count auto-fills
- **ON / OFF code fields** — enter hex codes manually for any device
- **Test buttons** — send a code immediately to verify it works before saving

### Settings
- Change mDNS hostname (applies after restart)
- Set UTC offset in minutes (applies immediately)
- Reset WiFi credentials

---

## Supported Protocols

| Protocol | Typical Devices | Bits | Repeat |
|---|---|---|---|
| NEC | LG TV, many brands | 32 | 1× (toggle) |
| LG | LG AC (split units) | 28 | 3× |
| LG2 | LG AC (newer models) | 28 | 3× |
| Samsung | Samsung TV / AC | 32 | 1× (toggle) |
| Sony | Sony TV / AV | 12–20 | 1× |
| RC5 | Philips devices | 13 | 1× |
| RC6 | Philips newer | 20 | 1× |
| Panasonic | Panasonic TV | 48 | 1× |
| JVC | JVC devices | 16 | 1× |
| Sharp | Sharp TV | 15 | 1× |
| Denon | Denon AV receivers | 14 | 1× |
| Coolix | Coolix / Midea AC | 24 | 3× |
| Mitsubishi | Mitsubishi TV | 16 | 1× |
| Whynter | Whynter AC | 32 | 1× |
| Daikin | Daikin split AC | 35-byte state | 1× |

> **TV remotes** use a single power-toggle code — the same hex value turns the device ON and OFF.  
> **AC remotes** typically have separate ON and OFF codes.

### Daikin Note
Daikin does not use a simple hex code. It sends a **35-byte full AC state** (70 hex characters) on every transmission, encoding mode, temperature, fan speed, etc.  
The built-in preset is a generic default state (Auto mode, 25°C, fan auto).  
For exact codes matching your model, capture them with a **TSOP1838 IR receiver** and the `IRrecvDumpV2` example from the IRremoteESP8266 library.

---

## Finding IR Codes

**Option 1 — Capture from your remote (most reliable)**  
Wire a TSOP1838 receiver to any GPIO, upload the `IRrecvDumpV2` example, open Serial Monitor, and press the button on your real remote. The protocol, hex code, and bit count will be printed.

**Option 2 — Online databases**  
- [IRDB](https://irdb.tk) — searchable IR code database by brand and model
- [Remote Central](https://www.remotecentral.com) — large community IR code archive
- [IRremoteESP8266 source](https://github.com/crankyoldgit/IRremoteESP8266) — `src/ir_*.h` files contain known codes

---

## WiFi Reset

Hold the **D5 button** for **10 seconds** while powering on (or while pressing RST).  
The device clears saved WiFi credentials and restarts into the `IRREMOTE` hotspot.

Can also be triggered from the web UI → **Settings → Reset WiFi & Restart**.

---

## Project Structure

```
IR_ESP8266/
├── IR_ESP8266.ino    — Main firmware (WiFi, NTP, IR dispatch, web routes)
└── html_page.h       — Web UI (HTML/CSS/JS, stored in PROGMEM)
```

The HTML is kept in a separate `.h` file to avoid a ctags parsing bug in Arduino IDE 1.x on Windows that causes `exit status 1` compilation errors with large C++11 raw string literals.

---

## License

MIT License — free to use, modify, and distribute.
