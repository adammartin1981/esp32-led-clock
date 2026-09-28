#pragma once

// ---- Hardware ----
// Change these two to match your wiring.
#define LED_PIN        16
#define NUM_LEDS       60

// ---- Networking ----
#define AP_NAME        "ESP32-Clock-Setup"   // hotspot name shown during first-time config
#define WEB_PORT       80

// ---- Time ----
// POSIX TZ string. Default = Europe/London (handles GMT/BST automatically).
// Can be overridden at runtime from the web UI; this is only the first-boot default.
#define DEFAULT_TZ     "GMT0BST,M3.5.0/1,M10.5.0"
#define NTP_SERVER_1   "pool.ntp.org"
#define NTP_SERVER_2   "time.nist.gov"

// ---- Alarms ----
#define MAX_ALARMS         10
#define ALARM_DURATION_MS  60000UL   // auto-stop an alarm after 60s if not dismissed
