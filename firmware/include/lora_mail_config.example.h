#pragma once

#define WIFI_SSID "your-wifi"
#define WIFI_PASSWORD "your-wifi-password"
#define MAKE_WEBHOOK_URL "https://example.invalid/mailbox-event"

// Generate a unique 32-byte key for your pair of nodes. Do not reuse the
// all-zero example key below outside a bench test.
static const uint8_t LORA_MAIL_KEY[32] = {
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

#define DEFAULT_MAIL_SUBJECT "LoRa mail trigger"
#define DEFAULT_MAIL_BODY "Triggered by the private mail LoRa device."

#define HEALTHCHECKS_URL ""

#define MQTT_HOST "your-mqtt-host.example.com"
#define MQTT_PORT 8883
#define MQTT_USER "home"
#define MQTT_PASS "change-this-password"

#define TOPIC_STATUS "mailbox/status"
#define TOPIC_HEARTBEAT "mailbox/heartbeat"
#define TOPIC_RESET "mailbox/reset"
#define TOPIC_COMMAND "mailbox/command"
#define TOPIC_MODE "mailbox/mode"
#define TOPIC_GATEWAY "mailbox/gateway"

#define HEARTBEAT_SECS 300
#define DEBUG_HEARTBEAT_SECS 60
#define HEARTBEAT_ACK_RETRIES 3
#define HEARTBEAT_PROBE_GRACE_SECS 20
#define HEARTBEAT_PROBE_INTERVAL_SECS 30
#define HEARTBEAT_PROBE_MAX_ATTEMPTS 3
// Panel marks sustained loss as offline after 30 minutes. Configure
// Healthchecks separately for the only availability email escalation.
#define HEARTBEAT_OFFLINE_AFTER_SECS 1800UL
#define EVENT_MAX_RETRIES 5
#define ACK_WINDOW_MS 4000

#define BATTERY_DIVIDER_HIGH_OHMS 200000UL
#define BATTERY_DIVIDER_LOW_OHMS 100000UL
#define BATTERY_CALIBRATION_PERMILLE 1000UL
#define BATTERY_EMPTY_MV 3300
#define BATTERY_FULL_MV 4200
#define BATTERY_PRESENT_MIN_MV 2500
#define LOW_BATTERY_MV 3500
#define LOW_BATTERY_RECOVERY_MV 3650
#define LOW_BATTERY_PCT 20
#define CRITICAL_BATTERY_PCT 10
#define LOW_BATTERY_ALERT_REPEAT_SECS 86400UL
#define BATTERY_INVALID_ALERT_REPEAT_SECS 3600UL

#define SENSOR_ACTIVE_LOW 1
#define SENSOR_SAMPLE_PERIOD_MS 40UL
#define SENSOR_SETTLE_MS 4UL
#define SENSOR_DEBOUNCE_MS 2UL
#define SENSOR_MIN_PULSE_MS 3UL
#define SENSOR_DELIVERY_MAX_MS 5000UL
#define SENSOR_EVENT_COOLDOWN_MS 5000UL
#define SENSOR_TX_POWER_PIN D2
#define SENSOR_RX_POWER_PIN D3

// Normal mode samples only during the weekday delivery window. Debug mode
// bypasses the window but still uses the same 40 ms pulsed sensor power.
#define ENABLE_DELIVERY_WINDOW 1
#define WINDOW_START_MINUTES (7 * 60 + 30)
#define WINDOW_END_MINUTES (17 * 60)
#define TZ_SPEC "AEST-10AEDT,M10.1.0,M4.1.0/3"
