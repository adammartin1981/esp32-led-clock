#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>          // tzapu/WiFiManager - handles AP + captive portal SSID/password setup
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <Adafruit_NeoPixel.h>
#include <time.h>

#include "config.h"
#include "webpage.h"

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

struct RGB {
  uint8_t r = 0, g = 0, b = 0;
  constexpr RGB() {}
  constexpr RGB(uint8_t r_, uint8_t g_, uint8_t b_) : r(r_), g(g_), b(b_) {}
  RGB &operator+=(const RGB &o) {
    r = min(255, r + o.r);
    g = min(255, g + o.g);
    b = min(255, b + o.b);
    return *this;
  }
};
constexpr RGB COLOR_BLACK(0, 0, 0);
constexpr RGB COLOR_RED(255, 0, 0);
constexpr RGB COLOR_GREEN(0, 255, 0);

Adafruit_NeoPixel strip(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);
RGB leds[NUM_LEDS];
AsyncWebServer server(WEB_PORT);
Preferences prefs;

void fillSolid(RGB *arr, uint16_t count, const RGB &color) {
  for (uint16_t i = 0; i < count; i++) arr[i] = color;
}

void pushLeds() {
  for (uint16_t i = 0; i < NUM_LEDS; i++) {
    strip.setPixelColor(i, strip.Color(leds[i].r, leds[i].g, leds[i].b));
  }
  strip.show();
}

RGB hsvToRgb(uint8_t h, uint8_t s, uint8_t v) {
  if (s == 0) return RGB(v, v, v);
  uint8_t region = h / 43;
  uint8_t remainder = (h - (region * 43)) * 6;
  uint8_t p = (v * (255 - s)) >> 8;
  uint8_t q = (v * (255 - ((s * remainder) >> 8))) >> 8;
  uint8_t t = (v * (255 - ((s * (255 - remainder)) >> 8))) >> 8;
  switch (region) {
    case 0:  return RGB(v, t, p);
    case 1:  return RGB(q, v, p);
    case 2:  return RGB(p, v, t);
    case 3:  return RGB(p, q, v);
    case 4:  return RGB(t, p, v);
    default: return RGB(v, p, q);
  }
}

// Sine wave between lo and hi, cycling at the given beats-per-minute.
uint8_t beatsin8(uint8_t bpm, uint8_t lo, uint8_t hi) {
  float phase = (millis() * bpm) / 60000.0f;
  float wave = (sinf(phase * 2.0f * PI) + 1.0f) / 2.0f;
  return lo + static_cast<uint8_t>(wave * (hi - lo));
}

enum ClockStyle : uint8_t {
  STYLE_CLASSIC = 0,   // hour/minute/second hands as single dots
  STYLE_SWEEP   = 1,   // second hand rendered as a 5-LED fading comet trail
  STYLE_COUNT   = 2
};

struct ClockConfig {
  uint8_t brightness = 120;
  uint8_t bgMode = 1;              // 0=off, 1=dim ticks, 2=rainbow sweep
  uint8_t style = STYLE_CLASSIC;
  int8_t rotationOffset = 0;       // shifts the ring's "12 o'clock" position, -60..+60
  bool use24Hour = true;
  bool bstEnabled = true;          // true = auto UK GMT/BST rule, false = use `timezone` verbatim
  bool trailUnderHands = false;    // STYLE_SWEEP only: false = trail draws over hour/minute hands
  RGB hourColor   = RGB(255, 60, 0);
  RGB minuteColor = RGB(0, 200, 255);
  RGB secondColor = RGB(255, 0, 160);
  RGB tickColor   = RGB(20, 20, 20);
  RGB trailColor  = RGB(255, 0, 160);
  String timezone  = DEFAULT_TZ;
} cfg;

struct Alarm {
  bool enabled = false;
  uint8_t hour = 7;
  uint8_t minute = 0;
  uint8_t days = 0b0111110;        // bit0=Sun..bit6=Sat, default Mon-Fri
  String label = "Alarm";
  RGB color = RGB(255, 0, 0);
};

Alarm alarms[MAX_ALARMS];
uint8_t alarmCount = 0;

bool alarmActive = false;
unsigned long alarmStartedAt = 0;
int8_t lastAlarmCheckedMinute = -1;

// ---------------------------------------------------------------------------
// Colour helpers
// ---------------------------------------------------------------------------

String colorToHex(const RGB &c) {
  char buf[8];
  snprintf(buf, sizeof(buf), "#%02x%02x%02x", c.r, c.g, c.b);
  return String(buf);
}

RGB hexToColor(const String &hex) {
  if (hex.length() < 7 || hex[0] != '#') return COLOR_BLACK;
  long v = strtol(hex.substring(1).c_str(), nullptr, 16);
  return RGB((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
}

// ---------------------------------------------------------------------------
// Persistence (NVS via Preferences, config/alarms stored as JSON blobs)
// ---------------------------------------------------------------------------

void saveConfig() {
  JsonDocument doc;
  doc["brightness"] = cfg.brightness;
  doc["bgMode"] = cfg.bgMode;
  doc["style"] = cfg.style;
  doc["rotationOffset"] = cfg.rotationOffset;
  doc["use24Hour"] = cfg.use24Hour;
  doc["bstEnabled"] = cfg.bstEnabled;
  doc["trailUnderHands"] = cfg.trailUnderHands;
  doc["hourColor"] = colorToHex(cfg.hourColor);
  doc["minuteColor"] = colorToHex(cfg.minuteColor);
  doc["secondColor"] = colorToHex(cfg.secondColor);
  doc["tickColor"] = colorToHex(cfg.tickColor);
  doc["trailColor"] = colorToHex(cfg.trailColor);
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
  cfg.style = doc["style"] | cfg.style;
  cfg.rotationOffset = doc["rotationOffset"] | cfg.rotationOffset;
  cfg.use24Hour = doc["use24Hour"] | cfg.use24Hour;
  cfg.bstEnabled = doc["bstEnabled"] | cfg.bstEnabled;
  cfg.trailUnderHands = doc["trailUnderHands"] | cfg.trailUnderHands;
  if (doc["hourColor"].is<const char*>()) cfg.hourColor = hexToColor(doc["hourColor"].as<String>());
  if (doc["minuteColor"].is<const char*>()) cfg.minuteColor = hexToColor(doc["minuteColor"].as<String>());
  if (doc["secondColor"].is<const char*>()) cfg.secondColor = hexToColor(doc["secondColor"].as<String>());
  if (doc["tickColor"].is<const char*>()) cfg.tickColor = hexToColor(doc["tickColor"].as<String>());
  if (doc["trailColor"].is<const char*>()) cfg.trailColor = hexToColor(doc["trailColor"].as<String>());
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
  RGB flashColor = COLOR_RED;
  for (uint8_t i = 0; i < alarmCount; i++) {
    if (alarms[i].enabled) { flashColor = alarms[i].color; break; }
  }
  uint8_t pulse = beatsin8(60, 40, 255);
  fillSolid(leds, NUM_LEDS, flashColor);
  strip.setBrightness(pulse);
}

// Wraps a signed ring position into 0..NUM_LEDS-1, applying the configured
// rotation offset so the ring's "12 o'clock" can be shifted physically.
uint8_t pixelIndex(int pos) {
  int idx = (pos + cfg.rotationOffset) % NUM_LEDS;
  if (idx < 0) idx += NUM_LEDS;
  return static_cast<uint8_t>(idx);
}

RGB scaleColor(const RGB &c, uint8_t scale) {
  return RGB((c.r * scale) / 255, (c.g * scale) / 255, (c.b * scale) / 255);
}

// Cheap integer hash used to give each tick its own twinkle phase.
uint8_t hash8(uint32_t x) {
  x ^= x >> 16; x *= 0x7feb352dU;
  x ^= x >> 15; x *= 0x846ca68bU;
  x ^= x >> 16;
  return static_cast<uint8_t>(x & 0xFF);
}

void renderClock(const struct tm &timeinfo) {
  strip.setBrightness(cfg.brightness);
  fillSolid(leds, NUM_LEDS, COLOR_BLACK);

  // Background
  if (cfg.bgMode == 1) {
    for (uint8_t i = 0; i < NUM_LEDS; i += 5) leds[pixelIndex(i)] = cfg.tickColor;
  } else if (cfg.bgMode == 2 || cfg.bgMode == 3) {
    uint8_t hueBase = (millis() / 40) % 255;
    for (uint8_t i = 0; i < NUM_LEDS; i++) {
      leds[pixelIndex(i)] = hsvToRgb(hueBase + (i * 255 / NUM_LEDS), 255, 25);
    }
    if (cfg.bgMode == 3) {
      // Twinkling white sparkle at each hour tick, each with its own phase.
      unsigned long t = millis();
      for (uint8_t i = 0; i < NUM_LEDS; i += 5) {
        float phase = (t + hash8(i) * 37UL) / 300.0f;
        float wave = (sinf(phase) + 1.0f) / 2.0f;
        uint8_t twinkle = 30 + static_cast<uint8_t>(wave * 225);
        leds[pixelIndex(i)] = scaleColor(RGB(255, 255, 255), twinkle);
      }
    }
  }

  int hourPos = ((timeinfo.tm_hour % 12) * 5) + (timeinfo.tm_min / 12);
  int minutePos = timeinfo.tm_min;
  int secondPos = timeinfo.tm_sec;

  // Hands are assigned (not blended) so they always show cleanly over ticks.
  auto drawHourMinute = [&]() {
    leds[pixelIndex(hourPos)] = cfg.hourColor;
    leds[pixelIndex(minutePos)] = cfg.minuteColor;
  };

  if (cfg.style == STYLE_SWEEP) {
    // 5-LED fading comet trail that does one full lap of the ring per
    // second, in cfg.trailColor. The second hand itself stays fixed in
    // place at secondPos the whole time - only the trail moves.
    constexpr uint8_t TRAIL_LEN = 5;
    float fracSecond = (millis() % 1000) / 1000.0f;
    int headPos = secondPos + static_cast<int>(fracSecond * NUM_LEDS);

    auto drawTrail = [&]() {
      for (uint8_t k = 0; k < TRAIL_LEN; k++) {
        uint8_t scale = 255 - (k * (255 / TRAIL_LEN));
        leds[pixelIndex(headPos - k)] = scaleColor(cfg.trailColor, scale);
      }
    };

    if (cfg.trailUnderHands) {
      drawTrail();
      drawHourMinute();
    } else {
      drawHourMinute();
      drawTrail();
    }
    leds[pixelIndex(secondPos)] = cfg.secondColor; // always visible, drawn last
  } else {
    drawHourMinute();
    leds[pixelIndex(secondPos)] = cfg.secondColor;
  }
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

  strip.setBrightness(cfg.brightness);
  fillSolid(leds, NUM_LEDS, COLOR_BLACK);

  uint8_t pos = 0;
  for (uint8_t i = 0; i < 3 && pos < NUM_LEDS; i++) {
    for (uint8_t j = 0; j < digits[i] && pos < NUM_LEDS; j++, pos++) {
      leds[pos] = COLOR_GREEN;
    }
    pos += gap;
  }

  pushLeds();
  delay(5000);
  fillSolid(leds, NUM_LEDS, COLOR_BLACK);
  pushLeds();
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
  doc["style"] = cfg.style;
  doc["rotationOffset"] = cfg.rotationOffset;
  doc["use24Hour"] = cfg.use24Hour;
  doc["bstEnabled"] = cfg.bstEnabled;
  doc["trailUnderHands"] = cfg.trailUnderHands;
  doc["hourColor"] = colorToHex(cfg.hourColor);
  doc["minuteColor"] = colorToHex(cfg.minuteColor);
  doc["secondColor"] = colorToHex(cfg.secondColor);
  doc["tickColor"] = colorToHex(cfg.tickColor);
  doc["trailColor"] = colorToHex(cfg.trailColor);
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

// When bstEnabled, applies the UK's auto GMT/BST daylight-saving rule
// regardless of `timezone`; otherwise uses `timezone` verbatim.
void applyTimezone() {
  const char *tz = cfg.bstEnabled ? "GMT0BST,M3.5.0/1,M10.5.0" : cfg.timezone.c_str();
  setenv("TZ", tz, 1);
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
      cfg.style = doc["style"] | cfg.style;
      cfg.rotationOffset = doc["rotationOffset"] | cfg.rotationOffset;
      cfg.use24Hour = doc["use24Hour"] | cfg.use24Hour;
      cfg.bstEnabled = doc["bstEnabled"] | cfg.bstEnabled;
      cfg.trailUnderHands = doc["trailUnderHands"] | cfg.trailUnderHands;
      if (doc["hourColor"].is<const char*>()) cfg.hourColor = hexToColor(doc["hourColor"].as<String>());
      if (doc["minuteColor"].is<const char*>()) cfg.minuteColor = hexToColor(doc["minuteColor"].as<String>());
      if (doc["secondColor"].is<const char*>()) cfg.secondColor = hexToColor(doc["secondColor"].as<String>());
      if (doc["tickColor"].is<const char*>()) cfg.tickColor = hexToColor(doc["tickColor"].as<String>());
      if (doc["trailColor"].is<const char*>()) cfg.trailColor = hexToColor(doc["trailColor"].as<String>());
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

  strip.begin();
  strip.setBrightness(cfg.brightness);
  fillSolid(leds, NUM_LEDS, COLOR_BLACK);
  pushLeds();

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
    pushLeds();
  }

  delay(20); // ~50fps, plenty smooth for a second hand, keeps AsyncWebServer responsive
}
