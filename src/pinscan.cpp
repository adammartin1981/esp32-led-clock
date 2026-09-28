// Diagnostic-only build (env:pinscan). Uses Adafruit_NeoPixel (bit-banged,
// not FastLED's RMT driver) to rule out a driver-specific issue. Drives one
// candidate GPIO at a time, resetting all LEDs off and flashing just the
// first LED, so you can see which physical pin the strip's DIN is wired to.
#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <esp_system.h>

#include "config.h"

const char *resetReasonName(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON:   return "POWERON";
    case ESP_RST_SW:        return "SW (software restart)";
    case ESP_RST_PANIC:     return "PANIC (crash)";
    case ESP_RST_INT_WDT:   return "INT_WDT";
    case ESP_RST_TASK_WDT:  return "TASK_WDT";
    case ESP_RST_WDT:       return "WDT";
    case ESP_RST_BROWNOUT:  return "BROWNOUT (power/current issue)";
    default:                return "other";
  }
}

// Common ESP32 DevKit GPIOs usable for bit-bang output (skips input-only
// pins 34-39 and strapping pins 0/2/12/15 that can cause boot issues).
constexpr uint8_t CANDIDATE_PINS[] = {4, 5, 13, 14, 16, 17, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33};
constexpr size_t NUM_CANDIDATES = sizeof(CANDIDATE_PINS) / sizeof(CANDIDATE_PINS[0]);
constexpr unsigned long HOLD_MS = 3000;
constexpr unsigned long FLASH_INTERVAL_MS = 300;

Adafruit_NeoPixel strip(NUM_LEDS, CANDIDATE_PINS[0], NEO_GRB + NEO_KHZ800);

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== LED data-pin scanner (Adafruit_NeoPixel) ===");
  Serial.printf("Reset reason: %s\n", resetReasonName(esp_reset_reason()));
  Serial.println("Watch the strip: whichever pin flashes the first LED green is the real DIN pin.");

  strip.begin();
  strip.clear();
  strip.show();
}

void loop() {
  static size_t active = 0;

  strip.clear();
  strip.show();

  Serial.printf("Testing GPIO %d ...\n", CANDIDATE_PINS[active]);
  strip.setPin(CANDIDATE_PINS[active]);

  bool on = false;
  for (unsigned long elapsed = 0; elapsed < HOLD_MS; elapsed += FLASH_INTERVAL_MS) {
    on = !on;
    strip.setPixelColor(0, on ? strip.Color(0, 255, 0) : 0);
    strip.show();
    delay(FLASH_INTERVAL_MS);
  }

  active = (active + 1) % NUM_CANDIDATES;
}

