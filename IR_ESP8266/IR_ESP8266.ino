/*
 * IR_ESP8266 — Universal IR Controller
 *
 * Supports: NEC · LG · LG2 · Samsung · Sony · RC5 · RC6 · Panasonic
 *           JVC · Sharp · Denon · Coolix · Mitsubishi · Whynter
 *
 * Features:
 *   - WiFiManager: captive portal on first boot
 *   - Persistent config in LittleFS (/config.json)
 *   - mDNS: http://ir-remote.local  (name configurable)
 *   - NTP clock, IST by default
 *   - Web UI: protocol selector, quick presets, ON/OFF scheduler
 *   - Hold button D5 (GPIO14) for 10 s at boot → reset WiFi
 *
 * Hardware:
 *   - ESP8266 NodeMCU
 *   - IR LED on D2 (GPIO4) — NPN transistor driver recommended:
 *       D2 → 1 kΩ → Base (2N2222)   Emitter → GND
 *       Collector → IR LED cathode   IR LED anode → 100 Ω → 5 V
 *   - Momentary push-button: D5 (GPIO14) → GND  (uses internal pull-up)
 *
 * Required libraries (Arduino Library Manager):
 *   - IRremoteESP8266  by David Conran
 *   - WiFiManager      by tzapu (v2.x)
 *   - NTPClient        by Fabrice Weinberg
 *   - ArduinoJson      by Benoit Blanchon (v6.x)
 *
 * Board  : NodeMCU 1.0 (ESP-12E Module)
 * Flash  : 4MB (FS:2MB, OTA:~1MB)
 */

#include <Arduino.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <WiFiManager.h>
#include <WiFiUDP.h>
#include <NTPClient.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "html_page.h"

// ── Pins ───────────────────────────────────────────────────────────────────
static const uint16_t      IR_PIN     = 4;       // D2 = GPIO4
static const uint8_t       BUTTON_PIN = 14;      // D5 = GPIO14
static const unsigned long BTN_HOLD   = 10000UL; // 10 s hold to reset WiFi

// ── Config (persisted to /config.json) ────────────────────────────────────
struct Config {
  char    mdnsName[32];    // mDNS hostname (no ".local")
  int     utcOffsetMin;    // UTC offset in minutes  e.g. 330 = IST
  char    devName[32];     // friendly device label  e.g. "Living Room AC"
  char    protocol[16];    // IR protocol key        e.g. "LG", "NEC"
  // OFF command
  char    offCode[72];     // hex string, no 0x: 12 chars for simple, 70 for Daikin (35 bytes)
  uint8_t offBits;
  bool    offEnabled;
  char    offTime[6];      // "HH:MM"
  // ON command
  char    onCode[72];
  uint8_t onBits;
  bool    onEnabled;
  char    onTime[6];
};

static Config cfg;
static bool   acOffSentThisMinute = false;
static bool   acOnSentThisMinute  = false;

// ── Globals ────────────────────────────────────────────────────────────────
static IRsend            irsend(IR_PIN);
static ESP8266WebServer  server(80);
static WiFiUDP           ntpUDP;
static NTPClient         timeClient(ntpUDP, "pool.ntp.org", 0, 60000UL);

// ══════════════════════════════════════════════════════════════════════════
// Protocol dispatcher
// All common protocols that accept (code, bits) are handled here.
// Sony needs repeat >= 2. Panasonic splits into (address, data).
// ══════════════════════════════════════════════════════════════════════════
// Daikin sends a 35-byte state array — parse hex string directly into bytes.
static void dispatchDaikin(const char* hexStr) {
  uint8_t buf[35] = {0};
  uint8_t len = 0;
  const char* p = hexStr;
  while (p[0] && p[1] && len < 35) {
    char b[3] = { p[0], p[1], '\0' };
    buf[len++] = (uint8_t)strtoul(b, nullptr, 16);
    p += 2;
  }
  if (len > 0) irsend.sendDaikin(buf, len);
}

static void dispatchIR(const char* proto, uint64_t code, uint8_t bits,
                       const char* rawHex = nullptr) {
  String p = String(proto);
  p.toUpperCase();

  if      (p == "DAIKIN")     { if (rawHex) dispatchDaikin(rawHex); }
  else if (p == "NEC")        irsend.sendNEC(code, bits);
  else if (p == "LG")         irsend.sendLG(code, bits);
  else if (p == "LG2")        irsend.sendLG2(code, bits);
  else if (p == "SAMSUNG")    irsend.sendSAMSUNG(code, bits);
  else if (p == "SONY")       irsend.sendSony(code, bits, 2);
  else if (p == "RC5")        irsend.sendRC5(code, bits);
  else if (p == "RC6")        irsend.sendRC6(code, bits);
  else if (p == "PANASONIC")  irsend.sendPanasonic((uint16_t)(code >> 32),
                                                    (uint32_t)(code & 0xFFFFFFFFUL));
  else if (p == "JVC")        irsend.sendJVC(code, bits, false);
  else if (p == "SHARP")      irsend.sendSharpRaw(code, bits);
  else if (p == "DENON")      irsend.sendDenon(code, bits);
  else if (p == "COOLIX")     irsend.sendCOOLIX(code, bits);
  else if (p == "MITSUBISHI") irsend.sendMitsubishi(code, bits);
  else if (p == "WHYNTER")    irsend.sendWhynter(code, bits);
  else                        irsend.sendNEC(code, bits); // safe fallback
}

// ══════════════════════════════════════════════════════════════════════════
// Config helpers
// ══════════════════════════════════════════════════════════════════════════
static void defaultConfig() {
  strlcpy(cfg.mdnsName, "ir-remote",  sizeof(cfg.mdnsName));
  cfg.utcOffsetMin = 330;
  strlcpy(cfg.devName,  "LG AC",      sizeof(cfg.devName));
  strlcpy(cfg.protocol, "LG",         sizeof(cfg.protocol));
  strlcpy(cfg.offCode,  "88C0051",    sizeof(cfg.offCode));
  cfg.offBits    = 28;
  cfg.offEnabled = false;
  strlcpy(cfg.offTime,  "22:00",      sizeof(cfg.offTime));
  strlcpy(cfg.onCode,   "88C0051",    sizeof(cfg.onCode));
  cfg.onBits     = 28;
  cfg.onEnabled  = false;
  strlcpy(cfg.onTime,   "08:00",      sizeof(cfg.onTime));
}

static bool loadConfig() {
  if (!LittleFS.exists("/config.json")) return false;
  File f = LittleFS.open("/config.json", "r");
  if (!f) return false;
  DynamicJsonDocument doc(768);
  bool ok = !deserializeJson(doc, f);
  f.close();
  if (!ok) return false;
  strlcpy(cfg.mdnsName, doc["mdnsName"] | "ir-remote", sizeof(cfg.mdnsName));
  cfg.utcOffsetMin = doc["utcOffsetMin"] | 330;
  strlcpy(cfg.devName,  doc["devName"]   | "LG AC",    sizeof(cfg.devName));
  strlcpy(cfg.protocol, doc["protocol"]  | "LG",       sizeof(cfg.protocol));
  strlcpy(cfg.offCode,  doc["offCode"]   | "88C0051",  sizeof(cfg.offCode));
  cfg.offBits    = doc["offBits"]    | 28;
  cfg.offEnabled = doc["offEnabled"] | false;
  strlcpy(cfg.offTime,  doc["offTime"]   | "22:00",    sizeof(cfg.offTime));
  strlcpy(cfg.onCode,   doc["onCode"]    | "88C0051",  sizeof(cfg.onCode));
  cfg.onBits     = doc["onBits"]     | 28;
  cfg.onEnabled  = doc["onEnabled"]  | false;
  strlcpy(cfg.onTime,   doc["onTime"]    | "08:00",    sizeof(cfg.onTime));
  return true;
}

static void saveConfig() {
  DynamicJsonDocument doc(768);
  doc["mdnsName"]    = cfg.mdnsName;
  doc["utcOffsetMin"]= cfg.utcOffsetMin;
  doc["devName"]     = cfg.devName;
  doc["protocol"]    = cfg.protocol;
  doc["offCode"]     = cfg.offCode;
  doc["offBits"]     = cfg.offBits;
  doc["offEnabled"]  = cfg.offEnabled;
  doc["offTime"]     = cfg.offTime;
  doc["onCode"]      = cfg.onCode;
  doc["onBits"]      = cfg.onBits;
  doc["onEnabled"]   = cfg.onEnabled;
  doc["onTime"]      = cfg.onTime;
  File f = LittleFS.open("/config.json", "w");
  if (f) { serializeJson(doc, f); f.close(); }
}

// ══════════════════════════════════════════════════════════════════════════
// IR send helpers
// ══════════════════════════════════════════════════════════════════════════

// TV remotes use a power-toggle: sending the same code multiple times
// turns the device on, then off, then on again — so send exactly once.
// AC protocols benefit from 3 repeats for reliability.
static uint8_t repeatCount(const char* proto) {
  String p = String(proto); p.toUpperCase();
  if (p == "NEC"      ) return 1;  // TV toggle
  if (p == "SAMSUNG"  ) return 1;  // TV toggle
  if (p == "SONY"     ) return 1;  // Sony handles its own repeats inside sendSony()
  if (p == "RC5"      ) return 1;  // TV toggle
  if (p == "RC6"      ) return 1;  // TV toggle
  if (p == "PANASONIC") return 1;  // TV toggle
  if (p == "JVC"      ) return 1;  // TV toggle
  if (p == "SHARP"    ) return 1;  // TV toggle
  if (p == "DENON"    ) return 1;  // AV receiver toggle
  if (p == "MITSUBISHI") return 1; // TV toggle
  if (p == "WHYNTER"  ) return 1;  // AC toggle (single-shot protocol)
  if (p == "DAIKIN"   ) return 1;  // Daikin handles framing internally
  return 3; // LG, LG2, COOLIX — AC protocols, repeat for reliability
}

static void sendOff() {
  uint64_t val = (uint64_t)strtoull(cfg.offCode, nullptr, 16);
  uint8_t  rpt = repeatCount(cfg.protocol);
  Serial.printf("[IR] OFF  proto=%s code=%s bits=%u x%u\n", cfg.protocol, cfg.offCode, cfg.offBits, rpt);
  for (uint8_t i = 0; i < rpt; i++) { dispatchIR(cfg.protocol, val, cfg.offBits, cfg.offCode); delay(150); }
}

static void sendOn() {
  uint64_t val = (uint64_t)strtoull(cfg.onCode, nullptr, 16);
  uint8_t  rpt = repeatCount(cfg.protocol);
  Serial.printf("[IR] ON   proto=%s code=%s bits=%u x%u\n", cfg.protocol, cfg.onCode, cfg.onBits, rpt);
  for (uint8_t i = 0; i < rpt; i++) { dispatchIR(cfg.protocol, val, cfg.onBits, cfg.onCode); delay(150); }
}

// ══════════════════════════════════════════════════════════════════════════
// Web handlers
// ══════════════════════════════════════════════════════════════════════════
static void handleRoot() {
  server.send_P(200, "text/html", HTML_PAGE);
}

static void handleStatus() {
  DynamicJsonDocument doc(768);
  doc["ip"]          = WiFi.localIP().toString();
  doc["mdnsName"]    = cfg.mdnsName;
  doc["utcOffsetMin"]= cfg.utcOffsetMin;
  doc["epochTime"]   = (uint32_t)timeClient.getEpochTime();
  doc["devName"]     = cfg.devName;
  doc["protocol"]    = cfg.protocol;
  doc["offCode"]     = cfg.offCode;
  doc["offBits"]     = cfg.offBits;
  doc["offEnabled"]  = cfg.offEnabled;
  doc["offTime"]     = cfg.offTime;
  doc["onCode"]      = cfg.onCode;
  doc["onBits"]      = cfg.onBits;
  doc["onEnabled"]   = cfg.onEnabled;
  doc["onTime"]      = cfg.onTime;
  String out; serializeJson(doc, out);
  server.send(200, "application/json", out);
}

static void handleSettings() {
  if (server.method() != HTTP_POST) { server.send(405); return; }
  DynamicJsonDocument doc(768);
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "text/plain", "Bad JSON"); return;
  }

  // Hex validator: 1-70 hex chars (12 for simple protocols, 70 for Daikin 35-byte state)
  auto validHex = [](const String& h) -> bool {
    if (h.length() == 0 || h.length() > 70) return false;
    for (unsigned int i = 0; i < h.length(); i++) if (!isxdigit(h[i])) return false;
    return true;
  };

  bool mdnsChanged = false, offsetChanged = false;

  if (doc.containsKey("devName")) {
    String n = doc["devName"].as<String>(); n.trim();
    if (n.length() > 0 && n.length() < 32) strlcpy(cfg.devName, n.c_str(), sizeof(cfg.devName));
  }
  if (doc.containsKey("protocol")) {
    String p = doc["protocol"].as<String>(); p.trim(); p.toUpperCase();
    if (p.length() > 0 && p.length() < 16) strlcpy(cfg.protocol, p.c_str(), sizeof(cfg.protocol));
  }
  if (doc.containsKey("offCode")) {
    String h = doc["offCode"].as<String>(); h.trim();
    if (validHex(h)) strlcpy(cfg.offCode, h.c_str(), sizeof(cfg.offCode));
  }
  if (doc.containsKey("offBits"))    { uint8_t b = doc["offBits"];    if (b>=8&&b<=64) cfg.offBits = b; }
  if (doc.containsKey("offEnabled")) cfg.offEnabled = doc["offEnabled"].as<bool>();
  if (doc.containsKey("offTime"))    strlcpy(cfg.offTime, doc["offTime"]|cfg.offTime, sizeof(cfg.offTime));

  if (doc.containsKey("onCode")) {
    String h = doc["onCode"].as<String>(); h.trim();
    if (validHex(h)) strlcpy(cfg.onCode, h.c_str(), sizeof(cfg.onCode));
  }
  if (doc.containsKey("onBits"))    { uint8_t b = doc["onBits"];    if (b>=8&&b<=64) cfg.onBits = b; }
  if (doc.containsKey("onEnabled")) cfg.onEnabled = doc["onEnabled"].as<bool>();
  if (doc.containsKey("onTime"))    strlcpy(cfg.onTime, doc["onTime"]|cfg.onTime, sizeof(cfg.onTime));

  if (doc.containsKey("mdnsName")) {
    String n = doc["mdnsName"].as<String>(); n.trim();
    bool v = (n.length() > 0 && n.length() < 32);
    for (unsigned int i = 0; i < n.length() && v; i++) if (!isalnum(n[i]) && n[i] != '-') v = false;
    if (v) { strlcpy(cfg.mdnsName, n.c_str(), sizeof(cfg.mdnsName)); mdnsChanged = true; }
  }
  if (doc.containsKey("utcOffsetMin")) {
    cfg.utcOffsetMin = doc["utcOffsetMin"].as<int>(); offsetChanged = true;
  }

  saveConfig();
  if (offsetChanged) { timeClient.setTimeOffset(cfg.utcOffsetMin * 60); timeClient.forceUpdate(); }
  if (mdnsChanged)   { MDNS.end(); if (MDNS.begin(cfg.mdnsName)) MDNS.addService("http","tcp",80); }

  server.send(200, "application/json", "{\"ok\":true}");
}

static void handleSendOff() {
  if (server.method() != HTTP_POST) { server.send(405); return; }
  sendOff();
  server.send(200, "application/json", "{\"ok\":true}");
}

static void handleSendOn() {
  if (server.method() != HTTP_POST) { server.send(405); return; }
  sendOn();
  server.send(200, "application/json", "{\"ok\":true}");
}

// Test any code + protocol without changing saved config
static void handleSendTest() {
  if (server.method() != HTTP_POST) { server.send(405); return; }
  StaticJsonDocument<128> doc;
  if (deserializeJson(doc, server.arg("plain"))) { server.send(400); return; }
  const char* proto = doc["protocol"] | cfg.protocol;
  const char* code  = doc["code"]     | "";
  uint8_t     bits  = doc["bits"]     | 28;
  if (strlen(code) == 0) { server.send(400, "text/plain", "Missing code"); return; }
  uint64_t val = (uint64_t)strtoull(code, nullptr, 16);
  uint8_t  rpt = repeatCount(proto);
  Serial.printf("[IR] Test proto=%s code=%s bits=%u x%u\n", proto, code, bits, rpt);
  for (uint8_t i = 0; i < rpt; i++) { dispatchIR(proto, val, bits, code); delay(150); }
  server.send(200, "application/json", "{\"ok\":true}");
}

static void handleResetWifi() {
  if (server.method() != HTTP_POST) { server.send(405); return; }
  server.send(200, "application/json", "{\"ok\":true}");
  delay(300);
  WiFiManager wm; wm.resetSettings(); ESP.restart();
}

// ══════════════════════════════════════════════════════════════════════════
// setup()
// ══════════════════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println(F("\n=== Universal IR Controller ==="));

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  irsend.begin();

  if (!LittleFS.begin()) { LittleFS.format(); LittleFS.begin(); }

  defaultConfig();
  if (loadConfig()) Serial.println(F("[CFG] Loaded"));
  else { Serial.println(F("[CFG] Defaults")); saveConfig(); }

  // Button hold → WiFi reset
  if (digitalRead(BUTTON_PIN) == LOW) {
    Serial.println(F("[BTN] Held — keep 10 s to reset WiFi..."));
    unsigned long t0 = millis();
    while (digitalRead(BUTTON_PIN) == LOW) {
      if (millis() - t0 >= BTN_HOLD) {
        Serial.println(F("[BTN] WiFi reset"));
        WiFiManager wm; wm.resetSettings(); delay(300); ESP.restart();
      }
      delay(50);
    }
  }

  // WiFiManager
  WiFiManagerParameter mdnsParam("mdns", "Device Name (e.g. ir-remote)", cfg.mdnsName, 31);
  WiFiManager wm;
  wm.addParameter(&mdnsParam);
  wm.setSaveParamsCallback([&]() {
    const char* v = mdnsParam.getValue();
    if (v && strlen(v) > 0 && strlen(v) < 32) {
      strlcpy(cfg.mdnsName, v, sizeof(cfg.mdnsName)); saveConfig();
    }
  });
  wm.setConfigPortalTimeout(180);
  if (!wm.autoConnect("IRREMOTE")) { delay(1000); ESP.restart(); }

  Serial.print(F("[WiFi] IP: ")); Serial.println(WiFi.localIP());

  if (MDNS.begin(cfg.mdnsName)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("[mDNS] http://%s.local\n", cfg.mdnsName);
  }

  timeClient.setTimeOffset(cfg.utcOffsetMin * 60);
  timeClient.begin();
  timeClient.forceUpdate();
  Serial.print(F("[NTP] ")); Serial.println(timeClient.getFormattedTime());

  server.on("/",               HTTP_GET,  handleRoot);
  server.on("/api/status",     HTTP_GET,  handleStatus);
  server.on("/api/settings",   HTTP_POST, handleSettings);
  server.on("/api/send-off",   HTTP_POST, handleSendOff);
  server.on("/api/send-on",    HTTP_POST, handleSendOn);
  server.on("/api/send-test",  HTTP_POST, handleSendTest);
  server.on("/api/reset-wifi", HTTP_POST, handleResetWifi);
  server.begin();
  Serial.println(F("[HTTP] Ready on port 80"));
}

// ══════════════════════════════════════════════════════════════════════════
// loop()
// ══════════════════════════════════════════════════════════════════════════
void loop() {
  MDNS.update();
  server.handleClient();
  timeClient.update();

  if (cfg.offEnabled || cfg.onEnabled) {
    String now5 = timeClient.getFormattedTime().substring(0, 5);
    if (cfg.offEnabled) {
      if (now5 == String(cfg.offTime)) {
        if (!acOffSentThisMinute) { Serial.printf("[SCHED] OFF %s\n", now5.c_str()); sendOff(); acOffSentThisMinute = true; }
      } else { acOffSentThisMinute = false; }
    }
    if (cfg.onEnabled) {
      if (now5 == String(cfg.onTime)) {
        if (!acOnSentThisMinute)  { Serial.printf("[SCHED] ON  %s\n", now5.c_str()); sendOn();  acOnSentThisMinute  = true; }
      } else { acOnSentThisMinute = false; }
    }
  }

  delay(200);
}
