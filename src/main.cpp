#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>          // tzapu/WiFiManager - handles AP + captive portal SSID/password setup
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <FastLED.h>
#include <time.h>

#include "config.h"
#include "webpage.h"

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

CRGB leds[NUM_LEDS];
AsyncWebServer server(WEB_PORT);
Preferences prefs;

struct ClockConfig {
  uint8_t brightness = 120;
  uint8_t bgMode = 1;              // 0=off, 1=dim ticks, 2=rainbow sweep
  bool use24Hour = true;
  CRGB hourColor   = CRGB(255, 60, 0);
  CRGB minuteColor = CRGB(0, 200, 255);
  CRGB secondColor = CRGB(255, 0, 160);
  CRGB tickColor   = CRGB(20, 20, 20);
  String timezone  = DEFAULT_TZ;
} cfg;

struct Alarm {
  bool enabled = false;
  uint8_t hour = 7;
  uint8_t minute = 0;
  uint8_t days = 0b0111110;        // bit0=Sun..bit6=Sat, default Mon-Fri
  String label = "Alarm";
  CRGB color = CRGB(255, 0, 0);
};

Alarm alarms[MAX_ALARMS];
uint8_t alarmCount = 0;

bool alarmActive = false;
unsigned long alarmStartedAt = 0;
int8_t lastAlarmCheckedMinute = -1;

// ---------------------------------------------------------------------------
// Colour helpers
// ---------------------------------------------------------------------------

String colorToHex(const CRGB &c) {
  char buf[8];
  snprintf(buf, sizeof(buf), "#%02x%02x%02x", c.r, c.g, c.b);
  return String(buf);
}

CRGB hexToColor(const String &hex) {
  if (hex.length() < 7 || hex[0] != '#') return CRGB::Black;
  long v = strtol(hex.substring(1).c_str(), nullptr, 16);
  return CRGB((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
}

// ---------------------------------------------------------------------------
// Persistence (NVS via Preferences, config/alarms stored as JSON blobs)
// ---------------------------------------------------------------------------

void saveConfig() {
  JsonDocument doc;
  doc["brightness"] = cfg.brightness;
  doc["bgMode"] = cfg.bgMode;
  doc["use24Hour"] = cfg.use24Hour;
  doc["hourColor"] = colorToHex(cfg.hourColor);
  doc["minuteColor"] = colorToHex(cfg.minuteColor);
  doc["secondColor"] = colorToHex(cfg.secondColor);
  doc["tickColor"] = colorToHex(cfg.tickColor);
  doc["timezone"] = cfg.timezone;

  String out;
  serializeJson(doc, out);
  prefs.putString("cfg", out);
}

void loadConfig() {
  String raw = prefs.getString("cfg", "");
  if (raw.length() == 0) return;

  JsonDocument doc;
  if (deserializeJson(doc, raw) != DeserializationError::Ok) return;

  cfg.brightness = doc["brightness"] | cfg.brightness;
  cfg.bgMode = doc["bgMode"] | cfg.bgMode;
  cfg.use24Hour = doc["use24Hour"] | cfg.use24Hour;
  if (doc["hourColor"].is<const char*>()) cfg.hourColor = hexToColor(doc["hourColor"].as<String>());
  if (doc["minuteColor"].is<const char*>()) cfg.minuteColor = hexToColor(doc["minuteColor"].as<String>());
  if (doc["secondColor"].is<const char*>()) cfg.secondColor = hexToColor(doc["secondColor"].as<String>());
  if (doc["tickColor"].is<const char*>()) cfg.tickColor = hexToColor(doc["tickColor"].as<String>());
  if (doc["timezone"].is<const char*>()) cfg.timezone = doc["timezone"].as<String>();
}

void saveAlarms() {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (uint8_t i = 0; i < alarmCount; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["enabled"] = alarms[i].enabled;
    o["hour"] = alarms[i].hour;
    o["minute"] = alarms[i].minute;
    o["days"] = alarms[i].days;
    o["label"] = alarms[i].label;
    o["color"] = colorToHex(alarms[i].color);
  }
  String out;
  serializeJson(doc, out);
  prefs.putString("alarms", out);
}

void loadAlarms() {
  String raw = prefs.getString("alarms", "");
  alarmCount = 0;
  if (raw.length() == 0) return;

  JsonDocument doc;
  if (deserializeJson(doc, raw) != DeserializationError::Ok) return;

  for (JsonObject o : doc.as<JsonArray>()) {
    if (alarmCount >= MAX_ALARMS) break;
    Alarm &a = alarms[alarmCount];
    a.enabled = o["enabled"] | false;
    a.hour = o["hour"] | 7;
    a.minute = o["minute"] | 0;
    a.days = o["days"] | 0;
    a.label = o["label"] | "Alarm";
    a.color = hexToColor(o["color"] | "#ff0000");
    alarmCount++;
  }
}

// ---------------------------------------------------------------------------
// Time / alarm checking
// ---------------------------------------------------------------------------

bool getTime(struct tm &timeinfo) {
  return getLocalTime(&timeinfo, 200);
}

void checkAlarms(const struct tm &timeinfo) {
  if (timeinfo.tm_sec != 0) return;                 // only check once per minute
  if (timeinfo.tm_min == lastAlarmCheckedMinute) return;
  lastAlarmCheckedMinute = timeinfo.tm_min;

  for (uint8_t i = 0; i < alarmCount; i++) {
    Alarm &a = alarms[i];
    if (!a.enabled) continue;
    if (a.hour != timeinfo.tm_hour || a.minute != timeinfo.tm_min) continue;
    if (!(a.days & (1 << timeinfo.tm_wday))) continue;

    alarmActive = true;
    alarmStartedAt = millis();
    break;
  }
}

// ---------------------------------------------------------------------------
// LED rendering
// ---------------------------------------------------------------------------

void renderAlarm() {
  // Pulsing flash across the whole ring using the triggered alarm's colour
  // (falls back to red if somehow no alarm is flagged as the active one).
  CRGB flashColor = CRGB::Red;
  for (uint8_t i = 0; i < alarmCount; i++) {
    if (alarms[i].enabled) { flashColor = alarms[i].color; break; }
  }
  uint8_t pulse = beatsin8(60, 40, 255);
  fill_solid(leds, NUM_LEDS, flashColor);
  FastLED.setBrightness(pulse);
}

void renderClock(const struct tm &timeinfo) {
  FastLED.setBrightness(cfg.brightness);
  fill_solid(leds, NUM_LEDS, CRGB::Black);

  // Background
  if (cfg.bgMode == 1) {
    for (uint8_t i = 0; i < NUM_LEDS; i += 5) leds[i] = cfg.tickColor;
  } else if (cfg.bgMode == 2) {
    uint8_t hueBase = (millis() / 40) % 255;
    for (uint8_t i = 0; i < NUM_LEDS; i++) {
      leds[i] = CHSV(hueBase + (i * 255 / NUM_LEDS), 255, 25);
    }
  }

  int hourPos = ((timeinfo.tm_hour % 12) * 5) + (timeinfo.tm_min / 12);
  int minutePos = timeinfo.tm_min;
  int secondPos = timeinfo.tm_sec;

  leds[hourPos % NUM_LEDS]   += cfg.hourColor;
  leds[minutePos % NUM_LEDS] += cfg.minuteColor;
  leds[secondPos % NUM_LEDS] += cfg.secondColor;
}

// Shows the last octet of the IP as three digit-groups of lit LEDs (separated
// by a 5-LED dark gap) so the address can be read off the ring at boot,
// e.g. 192.168.0.145 -> 1 lit, gap, 4 lit, gap, 5 lit.
void showIpOnLeds(const IPAddress &ip) {
  uint8_t lastOctet = ip[3];
  uint8_t digits[3] = {
    static_cast<uint8_t>(lastOctet / 100),
    static_cast<uint8_t>((lastOctet / 10) % 10),
    static_cast<uint8_t>(lastOctet % 10)
  };
  const uint8_t gap = 5;

  FastLED.setBrightness(cfg.brightness);
  fill_solid(leds, NUM_LEDS, CRGB::Black);

  uint8_t pos = 0;
  for (uint8_t i = 0; i < 3 && pos < NUM_LEDS; i++) {
    for (uint8_t j = 0; j < digits[i] && pos < NUM_LEDS; j++, pos++) {
      leds[pos] = CRGB::Green;
    }
    pos += gap;
  }

  FastLED.show();
  delay(5000);
  fill_solid(leds, NUM_LEDS, CRGB::Black);
  FastLED.show();
}

// ---------------------------------------------------------------------------
// Web server
// ---------------------------------------------------------------------------

void handleGetStatus(AsyncWebServerRequest *request) {
  struct tm timeinfo;
  bool haveTime = getTime(timeinfo);

  char timeBuf[16] = "--:--:--";
  char dateBuf[16] = "-";
  if (haveTime) {
    strftime(timeBuf, sizeof(timeBuf), cfg.use24Hour ? "%H:%M:%S" : "%I:%M:%S %p", &timeinfo);
    strftime(dateBuf, sizeof(dateBuf), "%Y-%m-%d", &timeinfo);
  }

  JsonDocument doc;
  doc["connected"] = WiFi.status() == WL_CONNECTED;
  doc["ip"] = WiFi.localIP().toString();
  doc["time"] = timeBuf;
  doc["date"] = dateBuf;
  doc["alarmActive"] = alarmActive;

  String out;
  serializeJson(doc, out);
  request->send(200, "application/json", out);
}

void handleGetConfig(AsyncWebServerRequest *request) {
  JsonDocument doc;
  doc["brightness"] = cfg.brightness;
  doc["bgMode"] = cfg.bgMode;
  doc["use24Hour"] = cfg.use24Hour;
  doc["hourColor"] = colorToHex(cfg.hourColor);
  doc["minuteColor"] = colorToHex(cfg.minuteColor);
  doc["secondColor"] = colorToHex(cfg.secondColor);
  doc["tickColor"] = colorToHex(cfg.tickColor);
  doc["timezone"] = cfg.timezone;

  String out;
  serializeJson(doc, out);
  request->send(200, "application/json", out);
}

void handleGetAlarms(AsyncWebServerRequest *request) {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (uint8_t i = 0; i < alarmCount; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["enabled"] = alarms[i].enabled;
    o["hour"] = alarms[i].hour;
    o["minute"] = alarms[i].minute;
    o["days"] = alarms[i].days;
    o["label"] = alarms[i].label;
    o["color"] = colorToHex(alarms[i].color);
  }
  String out;
  serializeJson(doc, out);
  request->send(200, "application/json", out);
}

void applyTimezone() {
  setenv("TZ", cfg.timezone.c_str(), 1);
  tzset();
}

void setupWebServer() {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send_P(200, "text/html", INDEX_HTML);
  });

  server.on("/api/status", HTTP_GET, handleGetStatus);
  server.on("/api/config", HTTP_GET, handleGetConfig);
  server.on("/api/alarms", HTTP_GET, handleGetAlarms);

  server.on("/api/config", HTTP_POST,
    [](AsyncWebServerRequest *request) {},
    nullptr,
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
      JsonDocument doc;
      if (deserializeJson(doc, data, len) != DeserializationError::Ok) {
        request->send(400, "application/json", "{\"error\":\"bad json\"}");
        return;
      }
      cfg.brightness = doc["brightness"] | cfg.brightness;
      cfg.bgMode = doc["bgMode"] | cfg.bgMode;
      cfg.use24Hour = doc["use24Hour"] | cfg.use24Hour;
      if (doc["hourColor"].is<const char*>()) cfg.hourColor = hexToColor(doc["hourColor"].as<String>());
      if (doc["minuteColor"].is<const char*>()) cfg.minuteColor = hexToColor(doc["minuteColor"].as<String>());
      if (doc["secondColor"].is<const char*>()) cfg.secondColor = hexToColor(doc["secondColor"].as<String>());
      if (doc["tickColor"].is<const char*>()) cfg.tickColor = hexToColor(doc["tickColor"].as<String>());
      if (doc["timezone"].is<const char*>()) cfg.timezone = doc["timezone"].as<String>();

      saveConfig();
      applyTimezone();
      request->send(200, "application/json", "{\"ok\":true}");
    });

  server.on("/api/alarms", HTTP_POST,
    [](AsyncWebServerRequest *request) {},
    nullptr,
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
      JsonDocument doc;
      if (deserializeJson(doc, data, len) != DeserializationError::Ok) {
        request->send(400, "application/json", "{\"error\":\"bad json\"}");
        return;
      }
      alarmCount = 0;
      for (JsonObject o : doc.as<JsonArray>()) {
        if (alarmCount >= MAX_ALARMS) break;
        Alarm &a = alarms[alarmCount];
        a.enabled = o["enabled"] | false;
        a.hour = o["hour"] | 7;
        a.minute = o["minute"] | 0;
        a.days = o["days"] | 0;
        a.label = o["label"] | "Alarm";
        a.color = hexToColor(o["color"] | "#ff0000");
        alarmCount++;
      }
      saveAlarms();
      request->send(200, "application/json", "{\"ok\":true}");
    });

  server.on("/api/alarm/stop", HTTP_POST, [](AsyncWebServerRequest *request) {
    alarmActive = false;
    request->send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/api/reset-wifi", HTTP_POST, [](AsyncWebServerRequest *request) {
    request->send(200, "application/json", "{\"ok\":true}");
    delay(300);
    WiFiManager wm;
    wm.resetSettings();
    ESP.restart();
  });

  server.begin();
}

// ---------------------------------------------------------------------------
// Setup / loop
// ---------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);

  FastLED.addLeds<LED_TYPE, LED_PIN, COLOR_ORDER>(leds, NUM_LEDS);
  FastLED.setBrightness(cfg.brightness);
  fill_solid(leds, NUM_LEDS, CRGB::Black);
  FastLED.show();

  prefs.begin("clockcfg", false);
  loadConfig();
  loadAlarms();

  // First boot (or forgotten Wi-Fi): opens a hotspot named AP_NAME with a
  // captive portal where the user picks their SSID/password. Once saved it
  // reconnects automatically on every future boot without the portal.
  WiFiManager wm;
  wm.setConfigPortalTimeout(180); // give up on the portal after 3 min and retry later
  bool connected = wm.autoConnect(AP_NAME);

  if (connected) {
    showIpOnLeds(WiFi.localIP());

    applyTimezone();
    configTime(0, 0, NTP_SERVER_1, NTP_SERVER_2);

    struct tm timeinfo;
    int attempts = 0;
    while (!getLocalTime(&timeinfo, 500) && attempts < 20) attempts++;

    setupWebServer();
  }
}

void loop() {
  struct tm timeinfo;
  bool haveTime = getTime(timeinfo);

  if (haveTime) {
    checkAlarms(timeinfo);

    if (alarmActive && millis() - alarmStartedAt > ALARM_DURATION_MS) {
      alarmActive = false;
    }

    if (alarmActive) {
      renderAlarm();
    } else {
      renderClock(timeinfo);
    }
    FastLED.show();
  }

  delay(20); // ~50fps, plenty smooth for a second hand, keeps AsyncWebServer responsive
}
