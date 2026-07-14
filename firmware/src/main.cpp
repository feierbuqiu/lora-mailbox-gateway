#include <Arduino.h>
#include <SPI.h>
#include <RadioLib.h>

#include "lora_mail_config.h"

#ifndef BATTERY_DIVIDER_HIGH_OHMS
#define BATTERY_DIVIDER_HIGH_OHMS 200000UL
#endif
#ifndef BATTERY_DIVIDER_LOW_OHMS
#define BATTERY_DIVIDER_LOW_OHMS 100000UL
#endif
#ifndef BATTERY_CALIBRATION_PERMILLE
#define BATTERY_CALIBRATION_PERMILLE 1000UL
#endif
#ifndef BATTERY_EMPTY_MV
#define BATTERY_EMPTY_MV 3300
#endif
#ifndef BATTERY_FULL_MV
#define BATTERY_FULL_MV 4200
#endif
#ifndef USB_POWER_MIN_MV
#define USB_POWER_MIN_MV 4400
#endif
#ifndef DEBUG_HEARTBEAT_SECS
#define DEBUG_HEARTBEAT_SECS 60
#endif
#ifndef HEARTBEAT_ACK_RETRIES
#define HEARTBEAT_ACK_RETRIES 3
#endif
#ifndef HEARTBEAT_PROBE_GRACE_SECS
#define HEARTBEAT_PROBE_GRACE_SECS 20
#endif
#ifndef HEARTBEAT_PROBE_INTERVAL_SECS
#define HEARTBEAT_PROBE_INTERVAL_SECS 30
#endif
#ifndef HEARTBEAT_PROBE_MAX_ATTEMPTS
#define HEARTBEAT_PROBE_MAX_ATTEMPTS 3
#endif
#ifndef HEARTBEAT_OFFLINE_AFTER_SECS
// Keep link flaps in MQTT/panel state. Healthchecks owns the email escalation.
#define HEARTBEAT_OFFLINE_AFTER_SECS 1800UL
#endif
#ifndef BATTERY_PRESENT_MIN_MV
#define BATTERY_PRESENT_MIN_MV 2500
#endif
#ifndef LOW_BATTERY_MV
#define LOW_BATTERY_MV 3500
#endif
#ifndef LOW_BATTERY_RECOVERY_MV
#define LOW_BATTERY_RECOVERY_MV 3650
#endif
#ifndef LOW_BATTERY_ALERT_REPEAT_SECS
#define LOW_BATTERY_ALERT_REPEAT_SECS 86400UL
#endif
#ifndef LOW_BATTERY_PCT
#define LOW_BATTERY_PCT 20
#endif
#ifndef CRITICAL_BATTERY_PCT
#define CRITICAL_BATTERY_PCT 10
#endif
#ifndef BATTERY_INVALID_ALERT_REPEAT_SECS
#define BATTERY_INVALID_ALERT_REPEAT_SECS 3600UL
#endif
#ifndef SENSOR_ACTIVE_LOW
#define SENSOR_ACTIVE_LOW 1
#endif
#ifndef SENSOR_SAMPLE_PERIOD_MS
#define SENSOR_SAMPLE_PERIOD_MS 40UL
#endif
#ifndef SENSOR_SETTLE_MS
#define SENSOR_SETTLE_MS 4UL
#endif
#ifndef SENSOR_DEBOUNCE_MS
#define SENSOR_DEBOUNCE_MS 35UL
#endif
#ifndef SENSOR_MIN_PULSE_MS
#define SENSOR_MIN_PULSE_MS 80UL
#endif
#ifndef SENSOR_DELIVERY_MAX_MS
#define SENSOR_DELIVERY_MAX_MS 1000UL
#endif
#ifndef SENSOR_EVENT_COOLDOWN_MS
#define SENSOR_EVENT_COOLDOWN_MS 5000UL
#endif
#ifndef ENABLE_DELIVERY_WINDOW
#define ENABLE_DELIVERY_WINDOW 1
#endif

#if ROLE_HOME
#include <HTTPClient.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <lwip/dns.h>
#include <time.h>
#endif

#include <mbedtls/aes.h>
#include <mbedtls/md.h>
#include <mbedtls/sha256.h>

#include "soc/rtc_cntl_reg.h"
#include "esp_sleep.h"

// ---------------------------------------------------------------------------
// Encrypted packet framing: | magic(4) | counter(4) | nonce(16) | len(2) |
//                           | AES-CTR ciphertext(len) | HMAC-SHA256 tag(16) |
// ---------------------------------------------------------------------------
static constexpr uint32_t PACKET_MAGIC = 0x484d4c31; // HML1
static constexpr size_t NONCE_LEN = 16;
static constexpr size_t TAG_LEN = 16;
static constexpr size_t HEADER_LEN = 4 + 4 + NONCE_LEN + 2;
static constexpr size_t MAX_PACKET_LEN = 240;
static constexpr size_t MAX_TEXT_LEN = MAX_PACKET_LEN - HEADER_LEN - TAG_LEN;

// Message vocabulary (plaintext inside the encrypted payload):
//   mail -> home:  "E|<seq>|<source>|<utc_ts>"   delivery event
//                  "H|<A/D>|<mv>|<pct>|<uptime_s>|<flags>"
//                                                  heartbeat; flags bit0=low battery,
//                                                  bit1=sensor currently blocked,
//                                                  bit2=debug mode,
//                                                  bit3=sampling active,
//                                                  bit4=sensor power on
//   home -> mail:  "A"                           ack
//                  "R"                           reset to ARMED
//                  "C|<cmd>"                     remote debug command
//                  "M|<D/N>"                     debug/normal mode

static constexpr float LORA_FREQ_MHZ = 915.0;
static constexpr float LORA_BW_KHZ = 125.0;
static constexpr uint8_t LORA_SF = 7;
static constexpr uint8_t LORA_CR = 5;
static constexpr uint8_t LORA_SYNC_WORD = 0x12;
static constexpr int8_t LORA_POWER_DBM = 17;
static constexpr uint16_t LORA_PREAMBLE = 8;
static constexpr float LORA_TCXO_V = 1.8;

// Seeed XIAO ESP32S3 + Wio-SX1262: pin map confirmed against Meshtastic boot
// log on this exact hardware (SPI SCK=7 MISO=8 MOSI=9, NSS=41, DIO1=39,
// RST=42, BUSY=40, RXEN=38, DIO2 drives the TX side of the RF switch).
static constexpr int PIN_LORA_SCK = 7;
static constexpr int PIN_LORA_MISO = 8;
static constexpr int PIN_LORA_MOSI = 9;
static constexpr int PIN_LORA_NSS = 41;
static constexpr int PIN_LORA_DIO1 = 39;
static constexpr int PIN_LORA_RST = 42;
static constexpr int PIN_LORA_BUSY = 40;
static constexpr int PIN_LORA_RXEN = 38;

SX1262 radio = new Module(PIN_LORA_NSS, PIN_LORA_DIO1, PIN_LORA_RST, PIN_LORA_BUSY);

static uint8_t aesKey[32];
static uint8_t macKey[32];
static uint32_t txCounter = 1;

static volatile bool rxFlag = false;
static bool radioSleeping = false;
static unsigned long radioRxWindowUntil = 0;

static void IRAM_ATTR onDio1() { rxFlag = true; }

static void putU32(uint8_t *p, uint32_t value) {
  p[0] = (value >> 24) & 0xff;
  p[1] = (value >> 16) & 0xff;
  p[2] = (value >> 8) & 0xff;
  p[3] = value & 0xff;
}

static uint32_t getU32(const uint8_t *p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

static void putU16(uint8_t *p, uint16_t value) {
  p[0] = (value >> 8) & 0xff;
  p[1] = value & 0xff;
}

static uint16_t getU16(const uint8_t *p) {
  return (uint16_t(p[0]) << 8) | uint16_t(p[1]);
}

static String packetField(const String &text, uint8_t fieldIndex) {
  int start = 0;
  for (uint8_t i = 0; i < fieldIndex; i++) {
    start = text.indexOf('|', start);
    if (start < 0) {
      return "";
    }
    start++;
  }
  int end = text.indexOf('|', start);
  if (end < 0) {
    end = text.length();
  }
  return text.substring(start, end);
}

static int batteryPercentFromMv(long mv) {
  if (mv <= 0) {
    return -1;
  }

  struct Point {
    int mv;
    int pct;
  };
  static const Point curve[] = {
      {3300, 0}, {3500, 10}, {3600, 20}, {3700, 30}, {3750, 40},
      {3790, 50}, {3850, 60}, {3920, 70}, {4000, 80}, {4100, 90},
      {4200, 100},
  };

  if (mv <= curve[0].mv) {
    return 0;
  }
  const size_t count = sizeof(curve) / sizeof(curve[0]);
  if (mv >= curve[count - 1].mv) {
    return 100;
  }
  for (size_t i = 1; i < count; i++) {
    if (mv <= curve[i].mv) {
      const long spanMv = curve[i].mv - curve[i - 1].mv;
      const long spanPct = curve[i].pct - curve[i - 1].pct;
      return curve[i - 1].pct + int((mv - curve[i - 1].mv) * spanPct / spanMv);
    }
  }
  return 100;
}

static bool batteryIsLow(long mv) {
  const int pct = batteryPercentFromMv(mv);
  return mv >= BATTERY_PRESENT_MIN_MV && (mv <= LOW_BATTERY_MV || (pct >= 0 && pct <= LOW_BATTERY_PCT));
}

static const char *powerSourceFromMv(long mv) {
  if (mv <= 0) {
    return "unknown";
  }
  return mv >= USB_POWER_MIN_MV ? "usb" : "battery";
}

static void deriveKeys() {
  uint8_t buffer[sizeof(LORA_MAIL_KEY) + 4];
  memcpy(buffer, LORA_MAIL_KEY, sizeof(LORA_MAIL_KEY));

  memcpy(buffer + sizeof(LORA_MAIL_KEY), "aes1", 4);
  mbedtls_sha256(buffer, sizeof(buffer), aesKey, 0);

  memcpy(buffer + sizeof(LORA_MAIL_KEY), "mac1", 4);
  mbedtls_sha256(buffer, sizeof(buffer), macKey, 0);
}

static bool hmacSha256(const uint8_t *data, size_t len, uint8_t out[32]) {
  const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!info) {
    return false;
  }
  return mbedtls_md_hmac(info, macKey, sizeof(macKey), data, len, out) == 0;
}

static bool aesCtrCrypt(const uint8_t nonce[NONCE_LEN], const uint8_t *input, uint8_t *output, size_t len) {
  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  uint8_t nonceCounter[NONCE_LEN];
  uint8_t streamBlock[16] = {0};
  size_t ncOff = 0;
  memcpy(nonceCounter, nonce, NONCE_LEN);

  int rc = mbedtls_aes_setkey_enc(&aes, aesKey, 256);
  if (rc == 0) {
    rc = mbedtls_aes_crypt_ctr(&aes, len, &ncOff, nonceCounter, streamBlock, input, output);
  }
  mbedtls_aes_free(&aes);
  return rc == 0;
}

static size_t buildPacket(uint8_t *packet, size_t packetSize, uint32_t counter, const String &text) {
  String plain = text;
  if (plain.length() > MAX_TEXT_LEN) {
    plain = plain.substring(0, MAX_TEXT_LEN);
  }

  const size_t plainLen = plain.length();
  const size_t totalLen = HEADER_LEN + plainLen + TAG_LEN;
  if (totalLen > packetSize) {
    return 0;
  }

  putU32(packet, PACKET_MAGIC);
  putU32(packet + 4, counter);
  for (size_t i = 0; i < NONCE_LEN; i += 4) {
    putU32(packet + 8 + i, esp_random());
  }
  putU16(packet + 8 + NONCE_LEN, plainLen);

  if (!aesCtrCrypt(packet + 8, reinterpret_cast<const uint8_t *>(plain.c_str()), packet + HEADER_LEN, plainLen)) {
    return 0;
  }

  uint8_t mac[32];
  if (!hmacSha256(packet, HEADER_LEN + plainLen, mac)) {
    return 0;
  }
  memcpy(packet + HEADER_LEN + plainLen, mac, TAG_LEN);
  return totalLen;
}

static bool parsePacket(const uint8_t *packet, size_t packetLen, uint32_t &counter, String &text) {
  if (packetLen < HEADER_LEN + TAG_LEN) {
    return false;
  }
  if (getU32(packet) != PACKET_MAGIC) {
    return false;
  }

  const size_t cipherLen = getU16(packet + 8 + NONCE_LEN);
  if (HEADER_LEN + cipherLen + TAG_LEN != packetLen || cipherLen > MAX_TEXT_LEN) {
    return false;
  }

  uint8_t mac[32];
  if (!hmacSha256(packet, HEADER_LEN + cipherLen, mac)) {
    return false;
  }
  if (memcmp(mac, packet + HEADER_LEN + cipherLen, TAG_LEN) != 0) {
    Serial.println("RX reject: auth tag mismatch");
    return false;
  }

  uint8_t plain[MAX_TEXT_LEN + 1];
  if (!aesCtrCrypt(packet + 8, packet + HEADER_LEN, plain, cipherLen)) {
    return false;
  }
  plain[cipherLen] = 0;

  text = reinterpret_cast<char *>(plain);
  counter = getU32(packet + 4);
  return true;
}

// ---------------------------------------------------------------------------
// Radio helpers
// ---------------------------------------------------------------------------
static bool beginRadio() {
  SPI.begin(PIN_LORA_SCK, PIN_LORA_MISO, PIN_LORA_MOSI, PIN_LORA_NSS);
  Serial.println("Starting SX1262");
  int state = radio.begin(LORA_FREQ_MHZ, LORA_BW_KHZ, LORA_SF, LORA_CR, LORA_SYNC_WORD, LORA_POWER_DBM,
                          LORA_PREAMBLE, LORA_TCXO_V);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("Radio begin failed: %d\n", state);
    return false;
  }
  radio.setDio2AsRfSwitch(true);
  radio.setRfSwitchPins(PIN_LORA_RXEN, RADIOLIB_NC);
  radio.setRxBoostedGainMode(true);
  radio.setDio1Action(onDio1);
  state = radio.startReceive();
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("startReceive failed: %d\n", state);
    return false;
  }
  Serial.println("Radio ready (RX)");
  radioSleeping = false;
  radioRxWindowUntil = 0;
  return true;
}

static void wakeRadio() {
  if (!radioSleeping) {
    return;
  }
  radio.standby();
  radioSleeping = false;
  delay(2);
}

static void sleepRadio() {
  if (radioSleeping) {
    return;
  }
  rxFlag = false;
  radio.sleep();
  radioSleeping = true;
  radioRxWindowUntil = 0;
}

static void startRadioRxWindow(unsigned long windowMs) {
  wakeRadio();
  rxFlag = false;
  int state = radio.startReceive();
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("startReceive failed: %d\n", state);
    return;
  }
  radioRxWindowUntil = windowMs == 0 ? 0 : millis() + windowMs;
}

static bool sendText(const String &text) {
  wakeRadio();
  uint8_t packet[MAX_PACKET_LEN];
  size_t len = buildPacket(packet, sizeof(packet), txCounter++, text);
  if (len == 0) {
    Serial.println("Build packet failed");
    return false;
  }
  int state = radio.transmit(packet, len);
  startRadioRxWindow(ACK_WINDOW_MS);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("TX failed: %d\n", state);
    return false;
  }
  Serial.printf("TX ok: %s\n", text.c_str());
  return true;
}

// Returns true when a valid packet was decoded this poll.
static bool pollRadio(String &text, uint32_t &counter) {
  if (radioSleeping || !rxFlag) {
    return false;
  }
  rxFlag = false;
  uint8_t packet[MAX_PACKET_LEN];
  size_t len = radio.getPacketLength();
  int state = radio.readData(packet, len);
  bool ok = false;
  if (state == RADIOLIB_ERR_NONE && len > 0) {
    if (parsePacket(packet, len, counter, text)) {
      Serial.printf("RX ok: %s (counter=%lu RSSI=%.1f SNR=%.1f)\n", text.c_str(),
                    static_cast<unsigned long>(counter), radio.getRSSI(), radio.getSNR());
      ok = true;
    }
  } else if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("RX read error: %d\n", state);
  }
  int restartState = radio.startReceive();
  if (restartState != RADIOLIB_ERR_NONE) {
    Serial.printf("startReceive failed: %d\n", restartState);
  }
  return ok;
}

// ---------------------------------------------------------------------------
// Shared serial utilities
// ---------------------------------------------------------------------------
static void rebootToBootloader() {
  Serial.println("Rebooting into ROM download mode for flashing...");
  Serial.flush();
  delay(200);
  REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
  esp_restart();
}

// ===========================================================================
// HOME: gateway. LoRa RX -> Make webhook (email) + Healthchecks ping + MQTT
// retained status topics; MQTT reset button -> LoRa downlink to mail node.
// ===========================================================================
#if ROLE_HOME

WiFiClientSecure mqttTls;
PubSubClient mqtt(mqttTls);

static bool pendingReset = false;
static String lastState = "UNKNOWN";
static uint32_t lastEventSeq = 0;
static unsigned long lastMqttAttempt = 0;
static long lastBatteryMv = 0;
static int lastBatteryPct = -1;
static bool lastBatteryLow = false;
static bool lastDebugMode = false;
static bool lastSamplingActive = false;
static bool lastSensorPower = false;
static const char *lastPowerSource = "unknown";
static uint32_t lastEventTs = 0;
static unsigned long lastHeartbeatAt = 0;
static bool heartbeatProbeActive = false;
static uint8_t heartbeatProbeCount = 0;
static unsigned long nextHeartbeatProbeAt = 0;
static bool heartbeatOfflineAlertActive = false;
static int batteryAlertLevel = 0;
static unsigned long lastLowBatteryAlertAt = 0;
static bool batteryInvalidAlertActive = false;
static unsigned long lastBatteryInvalidAlertAt = 0;
static bool queuedDownlink = false;
static String queuedDownlinkBase;
static bool queuedDownlinkIsReset = false;
static unsigned long lastDownlinkAttempt = 0;

static String jsonEscape(const String &input) {
  String out;
  out.reserve(input.length() + 8);
  for (size_t i = 0; i < input.length(); i++) {
    const char c = input[i];
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (uint8_t(c) < 0x20) {
          char buf[7];
          snprintf(buf, sizeof(buf), "\\u%04x", uint8_t(c));
          out += buf;
        } else {
          out += c;
        }
        break;
    }
  }
  return out;
}

static bool connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false); // gateway is mains powered: modem power-save only causes DNS/MQTT flaps
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("Connecting WiFi SSID=%s\n", WIFI_SSID);
  const unsigned long deadline = millis() + 30000;
  while (WiFi.status() != WL_CONNECTED && millis() < deadline) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi failed (will keep retrying in background)");
    return false;
  }
  Serial.print("WiFi IP: ");
  Serial.println(WiFi.localIP());
  return true;
}

static time_t epochNow() {
  time_t now = time(nullptr);
  return now > 1600000000 ? now : 0;
}

// Current UTC offset in minutes (DST-aware via TZ_SPEC); 0 when unsynced.
static int tzOffsetMinutes() {
  time_t now = time(nullptr);
  if (now < 1600000000) {
    return 0;
  }
  struct tm lt, gt;
  localtime_r(&now, &lt);
  gmtime_r(&now, &gt);
  int diff = (lt.tm_hour - gt.tm_hour) * 60 + (lt.tm_min - gt.tm_min);
  int dayDelta = lt.tm_yday - gt.tm_yday;
  if (dayDelta == 1 || dayDelta < -1) {
    diff += 1440;
  } else if (dayDelta == -1 || dayDelta > 1) {
    diff -= 1440;
  }
  return diff;
}

static bool sendWebhook(const String &subject, const String &body, uint32_t counter) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Webhook skipped: no WiFi");
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, MAKE_WEBHOOK_URL)) {
    Serial.println("HTTP begin failed");
    return false;
  }
  http.addHeader("Content-Type", "application/json");

  String payload = "{";
  payload += "\"subject\":\"" + jsonEscape(subject) + "\",";
  payload += "\"body\":\"" + jsonEscape(body) + "\",";
  payload += "\"node\":\"mail\",";
  payload += "\"counter\":" + String(counter) + ",";
  payload += "\"source\":\"lora-home\"";
  payload += "}";

  Serial.println("POST Make webhook");
  int code = http.POST(payload);
  String response = http.getString();
  http.end();
  Serial.printf("Webhook status=%d response=%s\n", code, response.c_str());
  return code >= 200 && code < 300;
}

static void pingHealthchecks() {
  if (strlen(HEALTHCHECKS_URL) == 0 || WiFi.status() != WL_CONNECTED) {
    return;
  }
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, HEALTHCHECKS_URL)) {
    return;
  }
  int code = http.GET();
  http.end();
  Serial.printf("Healthchecks ping status=%d\n", code);
}

static unsigned long expectedHeartbeatMs() {
  return (lastDebugMode ? DEBUG_HEARTBEAT_SECS : HEARTBEAT_SECS) * 1000UL;
}

static unsigned long heartbeatAgeSecs() {
  if (lastHeartbeatAt == 0) {
    return 0;
  }
  return (millis() - lastHeartbeatAt) / 1000UL;
}

static const char *heartbeatStatusName() {
  if (lastHeartbeatAt == 0) {
    return "unknown";
  }
  if (heartbeatOfflineAlertActive) {
    return "offline_confirmed";
  }
  if (heartbeatProbeActive) {
    return "probing";
  }
  return "ok";
}

static const char *batteryAlertName() {
  if (batteryInvalidAlertActive) {
    return "invalid";
  }
  if (batteryAlertLevel >= 2) {
    return "critical";
  }
  if (batteryAlertLevel == 1) {
    return "low";
  }
  return "ok";
}

static void publishStatus() {
  if (!mqtt.connected()) {
    return;
  }
  char buf[512];
  snprintf(buf, sizeof(buf),
           "{\"state\":\"%s\",\"ts\":%lu,\"reset_pending\":%s,\"last_event_seq\":%lu,\"last_event_ts\":%lu,"
           "\"battery_mv\":%ld,\"battery_pct\":%d,\"battery_low\":%s,\"power\":\"%s\","
           "\"debug\":%s,\"sampling\":%s,\"sensor_power\":%s,"
           "\"heartbeat_status\":\"%s\",\"heartbeat_age_s\":%lu,\"heartbeat_probe_count\":%u,"
           "\"battery_alert\":\"%s\"}",
           lastState.c_str(), static_cast<unsigned long>(epochNow()), pendingReset ? "true" : "false",
           static_cast<unsigned long>(lastEventSeq), static_cast<unsigned long>(lastEventTs), lastBatteryMv, lastBatteryPct,
           lastBatteryLow ? "true" : "false", lastPowerSource, lastDebugMode ? "true" : "false",
           lastSamplingActive ? "true" : "false", lastSensorPower ? "true" : "false", heartbeatStatusName(),
           heartbeatAgeSecs(), heartbeatProbeCount, batteryAlertName());
  mqtt.publish(TOPIC_STATUS, buf, true);
  Serial.printf("MQTT %s <- %s\n", TOPIC_STATUS, buf);
}

static void publishHeartbeat(long mv, int pct, bool low, bool debug, bool sampling, bool sensorPower,
                             unsigned long uptimeS, char stateChar) {
  if (!mqtt.connected()) {
    return;
  }
  char buf[384];
  snprintf(buf, sizeof(buf),
           "{\"ts\":%lu,\"mv\":%ld,\"pct\":%d,\"low\":%s,\"power\":\"%s\",\"debug\":%s,\"uptime\":%lu,"
           "\"state\":\"%s\",\"sampling\":%s,\"sensor_power\":%s,\"battery_alert\":\"%s\"}",
           static_cast<unsigned long>(epochNow()), mv, pct, low ? "true" : "false", powerSourceFromMv(mv),
           debug ? "true" : "false", uptimeS, stateChar == 'A' ? "ARMED" : "DELIVERED",
           sampling ? "true" : "false", sensorPower ? "true" : "false", batteryAlertName());
  mqtt.publish(TOPIC_HEARTBEAT, buf, true);
  Serial.printf("MQTT %s <- %s\n", TOPIC_HEARTBEAT, buf);
}

static void maybeSendBatteryAlert(long mv, int pct, bool telemetryValid) {
  const unsigned long now = millis();
  const bool invalid = !telemetryValid || mv <= 0 || pct < 0;
  if (invalid) {
    batteryInvalidAlertActive = true;
    const bool repeatDue = lastBatteryInvalidAlertAt == 0 ||
                           now - lastBatteryInvalidAlertAt >= BATTERY_INVALID_ALERT_REPEAT_SECS * 1000UL;
    if (repeatDue) {
      String body = "Battery telemetry is invalid.\n";
      body += "voltage_mv=" + String(mv) + "\n";
      body += "estimated_pct=" + String(pct) + "\n";
      body += "This may indicate a disconnected divider wire, short, or firmware telemetry fault.";
      if (sendWebhook("Mailbox: battery telemetry invalid", body, 0)) {
        lastBatteryInvalidAlertAt = now;
      }
    }
    return;
  }

  batteryInvalidAlertActive = false;
  int level = 0;
  if (pct <= CRITICAL_BATTERY_PCT) {
    level = 2;
  } else if (pct <= LOW_BATTERY_PCT || mv <= LOW_BATTERY_MV) {
    level = 1;
  }

  if (level == 0) {
    if (pct >= LOW_BATTERY_PCT + 5 && mv >= LOW_BATTERY_RECOVERY_MV) {
      batteryAlertLevel = 0;
    }
    return;
  }

  const bool repeatDue = lastLowBatteryAlertAt == 0 ||
                         now - lastLowBatteryAlertAt >= LOW_BATTERY_ALERT_REPEAT_SECS * 1000UL;
  if (level <= batteryAlertLevel && !repeatDue) {
    return;
  }

  String subject = level >= 2 ? "Mailbox: battery critical (<10%)" : "Mailbox: battery low (<20%)";
  String body = level >= 2 ? "Battery is below 10%; prepare to recharge soon.\n"
                           : "Battery is below 20%.\n";
  body += "voltage=" + String(mv / 1000.0f, 2) + "V\n";
  body += "estimated=" + String(pct) + "%\n";
  body += "low_threshold=" + String(LOW_BATTERY_PCT) + "%\n";
  body += "critical_threshold=" + String(CRITICAL_BATTERY_PCT) + "%";
  if (sendWebhook(subject, body, 0)) {
    batteryAlertLevel = level;
    lastLowBatteryAlertAt = now;
  }
}

static String downlinkWithTime(const String &base) {
  return base + "|" + String(static_cast<unsigned long>(epochNow())) + "|" + String(tzOffsetMinutes());
}

static void queueMailDownlink(const String &base, bool isReset) {
  queuedDownlinkBase = base;
  queuedDownlinkIsReset = isReset;
  queuedDownlink = true;
  lastDownlinkAttempt = 0;
  Serial.printf("Queued mail downlink: %s\n", base.c_str());
}

static void processQueuedDownlink() {
  // In normal power-saving mode the mail node only listens right after it sends
  // a heartbeat/event. Keep queued commands until that uplink arrives. When the
  // last known mode is DEBUG, the node is continuously listening and immediate
  // pushes are useful for bench work.
  if (!lastDebugMode) {
    return;
  }
  if (!queuedDownlink || millis() - lastDownlinkAttempt < 750) {
    return;
  }
  lastDownlinkAttempt = millis();
  String packet = downlinkWithTime(queuedDownlinkBase);
  if (!sendText(packet)) {
    Serial.println("Queued downlink TX failed; will retry");
    return;
  }
  Serial.printf("Queued downlink sent: %s\n", packet.c_str());
  if (queuedDownlinkIsReset) {
    pendingReset = false;
    lastState = "ARMED"; // mail sends a heartbeat immediately after applying R; this is quickly confirmed.
    publishStatus();
  }
  queuedDownlink = false;
}

static void handleCloudCommand(const String &rawCommand) {
  String cmd = rawCommand;
  cmd.trim();
  cmd.toLowerCase();
  if (cmd.length() == 0) {
    return;
  }
  if (cmd == "reset" || cmd == "collected") {
    pendingReset = true;
    publishStatus();
    queueMailDownlink("R", true);
  } else if (cmd == "hb" || cmd == "heartbeat" || cmd == "status" || cmd == "bat" || cmd == "battery") {
    queueMailDownlink("C|HB", false);
  } else if (cmd == "test" || cmd == "test-delivery" || cmd == "trigger") {
    queueMailDownlink("C|TEST", false);
  } else if (cmd == "debug" || cmd == "debug-on") {
    queueMailDownlink("M|D", false);
  } else if (cmd == "normal" || cmd == "debug-off") {
    queueMailDownlink("M|N", false);
  } else {
    Serial.printf("Unknown cloud command: %s\n", rawCommand.c_str());
  }
}

static void noteHeartbeatReceived() {
  lastHeartbeatAt = millis();
  heartbeatProbeActive = false;
  heartbeatProbeCount = 0;
  nextHeartbeatProbeAt = 0;
  heartbeatOfflineAlertActive = false;
}

static void sendHeartbeatProbe() {
  heartbeatProbeCount++;
  if (lastDebugMode) {
    String packet = downlinkWithTime("C|HB");
    if (sendText(packet)) {
      Serial.printf("Heartbeat probe %u/%u sent: %s\n", heartbeatProbeCount, HEARTBEAT_PROBE_MAX_ATTEMPTS,
                    packet.c_str());
    } else {
      Serial.printf("Heartbeat probe %u/%u TX failed\n", heartbeatProbeCount, HEARTBEAT_PROBE_MAX_ATTEMPTS);
    }
  } else {
    Serial.printf("Heartbeat check %u/%u: node is in NORMAL, waiting for its own heartbeat retries\n",
                  heartbeatProbeCount, HEARTBEAT_PROBE_MAX_ATTEMPTS);
  }
  nextHeartbeatProbeAt = millis() + HEARTBEAT_PROBE_INTERVAL_SECS * 1000UL;
  publishStatus();
}

static void markHeartbeatOffline() {
  if (heartbeatOfflineAlertActive) {
    return;
  }
  heartbeatOfflineAlertActive = true;
  Serial.printf("Heartbeat offline in panel: age=%lus probes=%u; Healthchecks owns email escalation\n",
                heartbeatAgeSecs(), heartbeatProbeCount);
  publishStatus();
}

static void checkHeartbeatTimeout() {
  if (lastHeartbeatAt == 0) {
    return;
  }
  const unsigned long now = millis();
  const unsigned long staleAfter = expectedHeartbeatMs() + HEARTBEAT_PROBE_GRACE_SECS * 1000UL;
  const unsigned long configuredOfflineAfter = HEARTBEAT_OFFLINE_AFTER_SECS * 1000UL;
  const unsigned long offlineAfter = configuredOfflineAfter > staleAfter ? configuredOfflineAfter : staleAfter;
  if (now - lastHeartbeatAt <= staleAfter) {
    if (heartbeatProbeActive || heartbeatOfflineAlertActive) {
      heartbeatProbeActive = false;
      heartbeatProbeCount = 0;
      heartbeatOfflineAlertActive = false;
      publishStatus();
    }
    return;
  }

  if (!heartbeatProbeActive && !heartbeatOfflineAlertActive) {
    heartbeatProbeActive = true;
    heartbeatProbeCount = 0;
    nextHeartbeatProbeAt = 0;
    Serial.printf("Heartbeat stale: age=%lus expected=%lus grace=%lus; starting probes\n", heartbeatAgeSecs(),
                  expectedHeartbeatMs() / 1000UL, static_cast<unsigned long>(HEARTBEAT_PROBE_GRACE_SECS));
    publishStatus();
  }

  // The mail node sleeps between its own heartbeats in normal mode. A single
  // missed interval is expected with a long, obstructed LoRa path, so it is
  // panel-only until the sustained-offline threshold. Healthchecks is the
  // single email escalation path and has a longer independent grace window.
  if (now - lastHeartbeatAt < offlineAfter) {
    if (heartbeatProbeCount < HEARTBEAT_PROBE_MAX_ATTEMPTS &&
        (nextHeartbeatProbeAt == 0 || static_cast<long>(now - nextHeartbeatProbeAt) >= 0)) {
      sendHeartbeatProbe();
    }
    return;
  }

  markHeartbeatOffline();
}

static void onMqttMessage(char *topic, byte *payload, unsigned int length) {
  String body;
  body.reserve(length + 1);
  for (unsigned int i = 0; i < length; i++) {
    body += char(payload[i]);
  }
  if (strcmp(topic, TOPIC_RESET) == 0) {
    Serial.println("MQTT reset request received");
    handleCloudCommand("reset");
  } else if (strcmp(topic, TOPIC_COMMAND) == 0) {
    Serial.printf("MQTT command received: %s\n", body.c_str());
    handleCloudCommand(body);
  } else if (strcmp(topic, TOPIC_MODE) == 0) {
    Serial.printf("MQTT mode received: %s\n", body.c_str());
    String mode = body;
    mode.trim();
    mode.toLowerCase();
    if (mode == "debug" || mode == "1" || mode == "on") {
      queueMailDownlink("M|D", false);
    } else if (mode == "normal" || mode == "0" || mode == "off") {
      queueMailDownlink("M|N", false);
    }
  } else {
    return;
  }
}

static void ensureMqtt() {
  if (mqtt.connected() || WiFi.status() != WL_CONNECTED) {
    return;
  }
  if (millis() - lastMqttAttempt < 5000) {
    return;
  }
  lastMqttAttempt = millis();
  Serial.println("Connecting MQTT...");
  if (mqtt.connect("lora-home-gw", MQTT_USER, MQTT_PASS, TOPIC_GATEWAY, 0, true, "offline")) {
    mqtt.publish(TOPIC_GATEWAY, "online", true);
    mqtt.subscribe(TOPIC_RESET);
    mqtt.subscribe(TOPIC_COMMAND);
    mqtt.subscribe(TOPIC_MODE);
    Serial.println("MQTT connected");
  } else {
    Serial.printf("MQTT connect failed rc=%d\n", mqtt.state());
  }
}

// Reply to any uplink from the mail node. Queued commands get priority because
// the power-saving mail node only opens a RX window after its own uplinks.
// Downlinks carry "<type>|<utc_epoch>|<tz_offset_min>" so the mail node can keep
// local time for the delivery window.
static void replyToMail() {
  delay(250); // give the mail node time to re-enter RX after its TX
  String suffix = "|" + String(static_cast<unsigned long>(epochNow())) + "|" + String(tzOffsetMinutes());
  if (queuedDownlink) {
    String packet = downlinkWithTime(queuedDownlinkBase);
    if (sendText(packet)) {
      Serial.printf("Queued downlink sent in RX window: %s\n", packet.c_str());
      if (queuedDownlinkIsReset) {
        pendingReset = false;
        lastState = "ARMED"; // mail sends a heartbeat immediately after applying R; this is quickly confirmed.
        publishStatus();
      }
      queuedDownlink = false;
    }
  } else if (pendingReset) {
    if (sendText("R" + suffix)) {
      pendingReset = false;
      if (queuedDownlinkIsReset) {
        queuedDownlink = false;
      }
      Serial.println("Reset downlink sent");
    }
  } else {
    sendText("A" + suffix);
  }
}

static void handleMailText(const String &text) {
  if (text.length() == 0) {
    return;
  }
  const char type = text[0];

  if (type == 'E') {
    uint32_t seq = packetField(text, 1).toInt();
    String source = packetField(text, 2);
    uint32_t eventTs = strtoul(packetField(text, 3).c_str(), nullptr, 10);
    if (eventTs < 1600000000UL) {
      eventTs = epochNow();
    }
    if (source.length() == 0) {
      source = "sensor";
    }
    replyToMail();
    if (seq != lastEventSeq) {
      lastEventSeq = seq;
      lastEventTs = eventTs;
      lastState = "DELIVERED";
      publishStatus();
      String subject = source == "sensor" ? "Mailbox: new delivery detected" : "Mailbox: remote test delivery";
      sendWebhook(subject,
                  "The mailbox node reported a delivery.\nseq=" + String(seq) + "\nsource=" + source +
                      "\nevent_ts=" + String(static_cast<unsigned long>(eventTs)),
                  seq);
    } else {
      if (lastEventTs == 0) {
        lastEventTs = eventTs;
        publishStatus();
      }
      Serial.println("Duplicate event seq, ack only");
    }
    return;
  }

  if (type == 'H') {
    // H|<A/D>|<mv>|<pct>|<uptime>|<flags>; older H|<A/D>|<mv>|<uptime> is still accepted.
    noteHeartbeatReceived();
    char stateChar = 'A';
    long mv = 0;
    int pct = -1;
    unsigned long uptimeS = 0;
    int flags = 0;
    String f1 = packetField(text, 1);
    String f2 = packetField(text, 2);
    String f3 = packetField(text, 3);
    String f4 = packetField(text, 4);
    String f5 = packetField(text, 5);
    bool hasVoltage = false;
    if (f1.length() > 0) {
      stateChar = f1[0];
    }
    if (f2.length() > 0) {
      mv = f2.toInt();
      hasVoltage = true;
    }
    if (f4.length() > 0) {
      pct = f3.toInt();
      uptimeS = strtoul(f4.c_str(), nullptr, 10);
      flags = f5.toInt();
    } else if (f3.length() > 0) {
      uptimeS = strtoul(f3.c_str(), nullptr, 10);
      pct = batteryPercentFromMv(mv);
    }
    const bool low = (flags & 0x01) || batteryIsLow(mv);
    const bool debug = flags & 0x04;
    const bool sampling = flags & 0x08;
    const bool sensorPower = flags & 0x10;
    lastBatteryMv = mv;
    lastBatteryPct = pct;
    lastBatteryLow = low;
    lastDebugMode = debug;
    lastSamplingActive = sampling;
    lastSensorPower = sensorPower;
    lastPowerSource = powerSourceFromMv(mv);
    maybeSendBatteryAlert(mv, pct, hasVoltage && mv > 0 && pct >= 0);
    replyToMail();

    if (stateChar == 'D' && lastState != "DELIVERED") {
      // Self-heal: the event packet was lost but the heartbeat says DELIVERED.
      lastState = "DELIVERED";
      if (lastEventTs == 0) {
        lastEventTs = epochNow();
      }
      sendWebhook("Mailbox: delivery detected (recovered from heartbeat)",
                  "The mailbox node is in DELIVERED state but no event packet was received.", 0);
    }
    if (stateChar == 'A') {
      lastState = "ARMED";
    }
    publishStatus();
    publishHeartbeat(mv, pct, low, debug, sampling, sensorPower, uptimeS, stateChar);
    pingHealthchecks();
    return;
  }

  Serial.printf("Unknown message type: %c\n", type);
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("home gateway firmware v1");
  deriveKeys();
  connectWifi();
  configTime(0, 0, "pool.ntp.org", "time.cloudflare.com");
  setenv("TZ", TZ_SPEC, 1);
  tzset();
  // Fallback DNS: the router's resolver drops queries now and then.
  ip_addr_t fallbackDns;
  IP_ADDR4(&fallbackDns, 1, 1, 1, 1);
  dns_setserver(1, &fallbackDns);
  mqttTls.setInsecure();
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setKeepAlive(60);
  mqtt.setCallback(onMqttMessage);
  mqtt.setBufferSize(512);
  beginRadio();
  Serial.println("Commands: test | status | reset-now | cmd <hb|test|debug|normal> | flash");
}

void loop() {
  String text;
  uint32_t counter = 0;
  if (pollRadio(text, counter)) {
    handleMailText(text);
  }

  ensureMqtt();
  mqtt.loop();
  processQueuedDownlink();
  checkHeartbeatTimeout();

  if (Serial.available()) {
    String line = Serial.readStringUntil('\n');
    line.trim();
    if (line == "test") {
      sendWebhook("LoRa home webhook self-test", "Triggered from the home gateway serial test command.", 0);
    } else if (line == "status") {
      Serial.printf("wifi=%d rssi=%d mqtt=%d state=%s reset_pending=%d last_seq=%lu epoch=%lu uptime=%lus "
                    "battery=%ldmV pct=%d low=%d alert=%s power=%s debug=%d sampling=%d sensor_power=%d "
                    "hb=%s age=%lus probes=%u queued=%d\n",
                    WiFi.status() == WL_CONNECTED, WiFi.RSSI(), mqtt.connected(), lastState.c_str(), pendingReset,
                    static_cast<unsigned long>(lastEventSeq), static_cast<unsigned long>(epochNow()),
                    millis() / 1000, lastBatteryMv, lastBatteryPct, lastBatteryLow, batteryAlertName(),
                    lastPowerSource, lastDebugMode, lastSamplingActive, lastSensorPower, heartbeatStatusName(),
                    heartbeatAgeSecs(), heartbeatProbeCount, queuedDownlink);
    } else if (line == "reset-now") {
      handleCloudCommand("reset");
    } else if (line.startsWith("cmd ")) {
      handleCloudCommand(line.substring(4));
    } else if (line == "flash") {
      rebootToBootloader();
    } else if (line.length() > 0) {
      Serial.println("Commands: test | status | reset-now | cmd <hb|test|debug|normal> | flash");
    }
  }
}
#endif // ROLE_HOME

// ===========================================================================
// MAIL: mailbox sensor node. D1 is the active-low IR beam input, D2/D3 gate
// sensor power, A0 reads the battery divider, serial commands remain as a
// debug backdoor.
// ===========================================================================
#if ROLE_MAIL

#include <Preferences.h>

enum class MailState : uint8_t { ARMED, DELIVERED };

#ifdef D1
static constexpr int PIN_SENSOR = D1;
#else
static constexpr int PIN_SENSOR = 2;
#endif

#ifdef A0
static constexpr int PIN_BATTERY_ADC = A0;
#else
static constexpr int PIN_BATTERY_ADC = 1;
#endif

#ifdef SENSOR_TX_POWER_PIN
static constexpr int PIN_SENSOR_TX_POWER = SENSOR_TX_POWER_PIN;
#elif defined(D2)
static constexpr int PIN_SENSOR_TX_POWER = D2;
#else
static constexpr int PIN_SENSOR_TX_POWER = 3;
#endif

#ifdef SENSOR_RX_POWER_PIN
static constexpr int PIN_SENSOR_RX_POWER = SENSOR_RX_POWER_PIN;
#elif defined(D3)
static constexpr int PIN_SENSOR_RX_POWER = D3;
#else
static constexpr int PIN_SENSOR_RX_POWER = 4;
#endif

static Preferences prefs;
static MailState state = MailState::ARMED;
static uint32_t eventSeq = 0;
static String lastEventMsg;
static String lastHeartbeatMsg;
static bool awaitingAck = false;
static bool awaitingHeartbeatAck = false;
static unsigned long ackDeadline = 0;
static unsigned long heartbeatAckDeadline = 0;
static int retriesLeft = 0;
static int heartbeatRetriesLeft = 0;
static unsigned long nextHeartbeatAt = 0;

// Clock synced from home's downlinks: UTC epoch reference + local offset.
static uint32_t utcRef = 0;
static unsigned long utcRefMillis = 0;
static int tzOffsetMin = 0;

static int sensorRaw = HIGH;
static int sensorStable = HIGH;
static unsigned long sensorRawChangedAt = 0;
static unsigned long sensorBlockedSince = 0;
static bool sensorTriggeredThisBlock = false;
static unsigned long lastSensorEventAt = 0;
static unsigned long lastSensorPulseMs = 0;
static uint32_t sensorDeliveryPulses = 0;
static uint32_t sensorShortRejects = 0;
static uint32_t sensorLongRejects = 0;
static uint16_t batteryCalPermille = BATTERY_CALIBRATION_PERMILLE;
static long lastBatteryRawMv = 0;
static long lastBatteryMv = 0;
static bool debugMode = false;
static bool sensorPowerOn = false;
static unsigned long nextSensorSampleAt = 0;

static const char *stateName() { return state == MailState::ARMED ? "ARMED" : "DELIVERED"; }

static void setSensorPower(bool on) {
  if (sensorPowerOn == on) {
    return;
  }
  digitalWrite(PIN_SENSOR_TX_POWER, on ? HIGH : LOW);
  digitalWrite(PIN_SENSOR_RX_POWER, on ? HIGH : LOW);
  sensorPowerOn = on;
  if (!on) {
    sensorRaw = HIGH;
    sensorStable = HIGH;
    sensorRawChangedAt = millis();
    sensorBlockedSince = 0;
    sensorTriggeredThisBlock = false;
  }
}

static void persistState() {
  prefs.putUChar("state", static_cast<uint8_t>(state));
  prefs.putUInt("seq", eventSeq);
}

// Seconds of local epoch, or 0 when never synced.
static uint32_t localEpoch() {
  if (utcRef == 0) {
    return 0;
  }
  return utcRef + (millis() - utcRefMillis) / 1000 + int32_t(tzOffsetMin) * 60;
}

static uint32_t utcEpochNow() {
  if (utcRef == 0) {
    return 0;
  }
  return utcRef + (millis() - utcRefMillis) / 1000;
}

// ISO weekday 1=Mon..7=Sun plus minutes-of-day; window check fails OPEN when
// the clock was never synced (missing a real delivery is worse than noise).
static bool inDeliveryWindow(String *why = nullptr) {
#if !ENABLE_DELIVERY_WINDOW
  if (why) *why = "delivery window disabled (24/7 armed)";
  return true;
#else
  uint32_t le = localEpoch();
  if (le == 0) {
    if (why) *why = "clock unsynced (fail-open)";
    return true;
  }
  uint32_t days = le / 86400;
  int isoDow = int((days + 3) % 7) + 1;
  int minutesOfDay = int((le % 86400) / 60);
  bool ok = isoDow <= 5 && minutesOfDay >= WINDOW_START_MINUTES && minutesOfDay < WINDOW_END_MINUTES;
  if (why) {
    char buf[64];
    snprintf(buf, sizeof(buf), "local dow=%d %02d:%02d -> %s", isoDow, minutesOfDay / 60, minutesOfDay % 60,
             ok ? "in window" : "outside window");
    *why = buf;
  }
  return ok;
#endif
}

static bool normalSamplingWindowActive(String *why = nullptr) {
  if (state != MailState::ARMED) {
    if (why) *why = "latched delivered";
    return false;
  }
  return inDeliveryWindow(why);
}

static bool samplingActiveNow(String *why = nullptr) {
  if (state != MailState::ARMED) {
    if (why) *why = "latched delivered";
    return false;
  }
  if (debugMode) {
    if (why) *why = "debug 24/7 periodic sampling";
    return true;
  }
  return inDeliveryWindow(why);
}

static const char *powerModeName() {
  return debugMode ? "DEBUG_24H_PERIODIC_SAMPLE" : "NORMAL_POWER_SAVE";
}

static unsigned long heartbeatIntervalMs() {
  return (debugMode ? DEBUG_HEARTBEAT_SECS : HEARTBEAT_SECS) * 1000UL;
}

// Downlinks carry UTC epoch and local offset as their last two fields; older
// gateways may send a bare type char, so both extras are optional.
static void syncClockFromDownlink(const String &text) {
  int last = text.lastIndexOf('|');
  if (last < 0) {
    return;
  }
  int prev = text.lastIndexOf('|', last - 1);
  if (prev < 0) {
    return;
  }
  uint32_t epoch = strtoul(text.substring(prev + 1, last).c_str(), nullptr, 10);
  if (epoch < 1600000000UL) {
    return; // gateway itself not NTP-synced yet
  }
  utcRef = epoch;
  utcRefMillis = millis();
  tzOffsetMin = text.substring(last + 1).toInt();
}

static long readBatteryMvRaw() {
  uint32_t pinMvSum = 0;
  uint32_t rawSum = 0;
  static constexpr uint8_t samples = 24;
  for (uint8_t i = 0; i < samples; i++) {
    rawSum += analogRead(PIN_BATTERY_ADC);
    pinMvSum += analogReadMilliVolts(PIN_BATTERY_ADC);
    delay(2);
  }
  const uint32_t pinMv = pinMvSum / samples;
  const uint32_t dividerPermille =
      ((BATTERY_DIVIDER_HIGH_OHMS + BATTERY_DIVIDER_LOW_OHMS) * 1000UL + BATTERY_DIVIDER_LOW_OHMS / 2) /
      BATTERY_DIVIDER_LOW_OHMS;
  (void)rawSum; // raw is printed by readBatteryAndPrint(); calibrated mV uses ESP32 ADC calibration.
  return long((uint64_t(pinMv) * dividerPermille + 500) / 1000);
}

static long batteryMv() {
  lastBatteryRawMv = readBatteryMvRaw();
  long calibrated = long((uint64_t(lastBatteryRawMv) * batteryCalPermille + 500) / 1000);
  lastBatteryMv = calibrated >= BATTERY_PRESENT_MIN_MV ? calibrated : 0;
  return lastBatteryMv;
}

static uint32_t batteryFlags(long mv) {
  uint32_t flags = 0;
  if (batteryIsLow(mv)) {
    flags |= 0x01;
  }
#if SENSOR_ACTIVE_LOW
  if (sensorStable == LOW) {
#else
  if (sensorStable == HIGH) {
#endif
    flags |= 0x02;
  }
  if (debugMode) {
    flags |= 0x04;
  }
  if (samplingActiveNow()) {
    flags |= 0x08;
  }
  if (sensorPowerOn) {
    flags |= 0x10;
  }
  return flags;
}

static void printBattery() {
  uint32_t rawSum = 0;
  uint32_t pinMvSum = 0;
  static constexpr uint8_t samples = 24;
  for (uint8_t i = 0; i < samples; i++) {
    rawSum += analogRead(PIN_BATTERY_ADC);
    pinMvSum += analogReadMilliVolts(PIN_BATTERY_ADC);
    delay(2);
  }
  long rawMv = readBatteryMvRaw();
  long calibratedMv = long((uint64_t(rawMv) * batteryCalPermille + 500) / 1000);
  long mv = calibratedMv >= BATTERY_PRESENT_MIN_MV ? calibratedMv : 0;
  Serial.printf("battery: adc_raw=%lu pin=%lumV raw=%ldmV cal=%ldmV reported=%ldmV pct=%d low=%d power=%s cal=%u/1000 divider=%lu/%lu\n",
                static_cast<unsigned long>(rawSum / samples), static_cast<unsigned long>(pinMvSum / samples), rawMv,
                calibratedMv, mv, batteryPercentFromMv(mv), batteryIsLow(mv), powerSourceFromMv(mv), batteryCalPermille,
                static_cast<unsigned long>(BATTERY_DIVIDER_HIGH_OHMS),
                static_cast<unsigned long>(BATTERY_DIVIDER_LOW_OHMS));
}

static void calibrateBattery(long measuredMv) {
  if (measuredMv < 3000 || measuredMv > 5500) {
    Serial.println("Calibration ignored: expected measured rail mV, e.g. cal 3800 or cal 4800");
    return;
  }
  long rawMv = readBatteryMvRaw();
  if (rawMv < 2500 || rawMv > 5500) {
    Serial.printf("Calibration ignored: raw reading looks wrong (%ldmV)\n", rawMv);
    return;
  }
  batteryCalPermille = uint16_t((uint64_t(measuredMv) * 1000 + rawMv / 2) / rawMv);
  prefs.putUShort("batcal", batteryCalPermille);
  Serial.printf("Battery calibration saved: measured=%ldmV raw=%ldmV cal=%u/1000\n", measuredMv, rawMv,
                batteryCalPermille);
}

static void sendHeartbeat() {
  long mv = batteryMv();
  int pct = batteryPercentFromMv(mv);
  uint32_t flags = batteryFlags(mv);
  String msg = "H|";
  msg += (state == MailState::ARMED) ? 'A' : 'D';
  msg += "|" + String(mv);
  msg += "|" + String(pct);
  msg += "|" + String(millis() / 1000);
  msg += "|" + String(flags);
  lastHeartbeatMsg = msg;
  awaitingHeartbeatAck = true;
  heartbeatRetriesLeft = HEARTBEAT_ACK_RETRIES;
  heartbeatAckDeadline = millis() + ACK_WINDOW_MS;
  sendText(msg);
  Serial.printf("Heartbeat state=%s battery=%ldmV pct=%d flags=0x%lx\n", stateName(), mv, pct,
                static_cast<unsigned long>(flags));
  nextHeartbeatAt = millis() + heartbeatIntervalMs();
}

static void triggerDelivery(bool force, const char *source = "sensor") {
  if (state == MailState::DELIVERED) {
    Serial.println("Latched in DELIVERED; trigger ignored (send 'reset' or use the panel button)");
    return;
  }
  String why;
  if (!force && !debugMode && !inDeliveryWindow(&why)) {
    Serial.printf("Trigger ignored: %s (use 't!' to force)\n", why.c_str());
    return;
  }
  eventSeq++;
  state = MailState::DELIVERED; // latch immediately: one email per delivery
  persistState();
  setSensorPower(false);
  nextSensorSampleAt = millis() + 1000UL;
  lastEventMsg = "E|" + String(eventSeq) + "|" + source + "|" + String(static_cast<unsigned long>(utcEpochNow()));
  retriesLeft = EVENT_MAX_RETRIES - 1;
  awaitingAck = true;
  ackDeadline = millis() + ACK_WINDOW_MS;
  Serial.printf("Delivery! seq=%lu -> DELIVERED (latched, persisted)\n", static_cast<unsigned long>(eventSeq));
  sendText(lastEventMsg);
}

static bool sensorIsBlockedLevel(int level) {
#if SENSOR_ACTIVE_LOW
  return level == LOW;
#else
  return level == HIGH;
#endif
}

static void acceptSensorBlock(unsigned long blockMs) {
  if (millis() - lastSensorEventAt < SENSOR_EVENT_COOLDOWN_MS) {
    Serial.printf("Sensor block ignored during cooldown: %lums\n", blockMs);
    return;
  }
  lastSensorEventAt = millis();
  sensorDeliveryPulses++;
  lastSensorPulseMs = blockMs;
  Serial.printf("Sensor delivery block accepted: %lums\n", blockMs);
  triggerDelivery(false, "sensor");
}

static void handleSensorPulse(unsigned long pulseMs) {
  lastSensorPulseMs = pulseMs;
  if (pulseMs < SENSOR_MIN_PULSE_MS) {
    sensorShortRejects++;
    Serial.printf("Sensor pulse ignored: %lums < %lums min\n", pulseMs,
                  static_cast<unsigned long>(SENSOR_MIN_PULSE_MS));
    return;
  }
  if (pulseMs > SENSOR_DELIVERY_MAX_MS) {
    sensorLongRejects++;
    Serial.printf("Sensor long block seen: %lums > %lums; accepting to avoid missed delivery\n", pulseMs,
                  static_cast<unsigned long>(SENSOR_DELIVERY_MAX_MS));
  }
  acceptSensorBlock(pulseMs);
}

static void pollSensor() {
  const unsigned long now = millis();
  int raw = digitalRead(PIN_SENSOR);
  if (raw != sensorRaw) {
    sensorRaw = raw;
    sensorRawChangedAt = now;
  }

  if (raw == sensorStable || now - sensorRawChangedAt < SENSOR_DEBOUNCE_MS) {
    if (sensorIsBlockedLevel(sensorStable) && !sensorTriggeredThisBlock && sensorBlockedSince != 0 &&
        now - sensorBlockedSince >= SENSOR_MIN_PULSE_MS) {
      sensorTriggeredThisBlock = true;
      acceptSensorBlock(now - sensorBlockedSince);
    }
    return;
  }

  const int previous = sensorStable;
  sensorStable = raw;
  const bool wasBlocked = sensorIsBlockedLevel(previous);
  const bool blocked = sensorIsBlockedLevel(sensorStable);

  if (!wasBlocked && blocked) {
    sensorBlockedSince = now;
    sensorTriggeredThisBlock = false;
    Serial.printf("Sensor blocked on D1 (level=%d)\n", sensorStable);
  } else if (wasBlocked && !blocked) {
    unsigned long pulseMs = sensorBlockedSince == 0 ? 0 : now - sensorBlockedSince;
    sensorBlockedSince = 0;
    Serial.printf("Sensor clear on D1 pulse=%lums\n", pulseMs);
    if (sensorTriggeredThisBlock) {
      lastSensorPulseMs = pulseMs;
      sensorTriggeredThisBlock = false;
    } else {
      handleSensorPulse(pulseMs);
    }
  } else {
    Serial.printf("Sensor stable level changed to %d\n", sensorStable);
  }
}

static void sampleSensorLowPower() {
  const unsigned long startedAt = millis();
  String why;
  if (!samplingActiveNow(&why)) {
    setSensorPower(false);
    nextSensorSampleAt = startedAt + 1000UL;
    return;
  }

  setSensorPower(true);
  delay(SENSOR_SETTLE_MS);
  const int raw = digitalRead(PIN_SENSOR);
  sensorRaw = raw;
  sensorStable = raw;
  sensorRawChangedAt = millis();

  if (sensorIsBlockedLevel(raw)) {
    if (!sensorTriggeredThisBlock) {
      sensorBlockedSince = millis();
      sensorTriggeredThisBlock = true;
      acceptSensorBlock(SENSOR_SAMPLE_PERIOD_MS);
    }
  } else {
    sensorBlockedSince = 0;
    sensorTriggeredThisBlock = false;
  }

  setSensorPower(false);
  nextSensorSampleAt = startedAt + SENSOR_SAMPLE_PERIOD_MS;
}

static void lightSleepFor(unsigned long sleepMs) {
  if (sleepMs < 2) {
    return;
  }
  Serial.flush();
  esp_sleep_enable_timer_wakeup(uint64_t(sleepMs) * 1000ULL);
  esp_light_sleep_start();
}

static void printSensor() {
  Serial.printf("sensor: pin=D1/GPIO2 raw=%d stable=%d blocked=%d sensor_power=%d sample_active=%d mode=%s "
                "last_block=%lums accepted=%lu short=%lu long_seen=%lu debounce=%lums min=%lums max=%lums "
                "sample_period=%lums settle=%lums power_pins=D2/%d,D3/%d\n",
                digitalRead(PIN_SENSOR), sensorStable, sensorIsBlockedLevel(sensorStable), sensorPowerOn,
                samplingActiveNow(), powerModeName(), lastSensorPulseMs, static_cast<unsigned long>(sensorDeliveryPulses),
                static_cast<unsigned long>(sensorShortRejects),
                static_cast<unsigned long>(sensorLongRejects), static_cast<unsigned long>(SENSOR_DEBOUNCE_MS),
                static_cast<unsigned long>(SENSOR_MIN_PULSE_MS),
                static_cast<unsigned long>(SENSOR_DELIVERY_MAX_MS), static_cast<unsigned long>(SENSOR_SAMPLE_PERIOD_MS),
                static_cast<unsigned long>(SENSOR_SETTLE_MS), PIN_SENSOR_TX_POWER, PIN_SENSOR_RX_POWER);
}

static void applyReset(const char *via) {
  state = MailState::ARMED;
  awaitingAck = false;
  awaitingHeartbeatAck = false;
  sensorBlockedSince = 0;
  sensorTriggeredThisBlock = false;
  nextSensorSampleAt = millis();
  persistState();
  Serial.printf("Reset via %s -> ARMED (persisted)\n", via);
  sendHeartbeat(); // confirm the new state to the gateway right away
}

static void handleDownlink(const String &text) {
  if (text.length() == 0) {
    return;
  }
  const char type = text[0];
  syncClockFromDownlink(text);
  if (type == 'A') {
    bool clearedSomething = false;
    if (awaitingAck) {
      awaitingAck = false;
      clearedSomething = true;
      Serial.println("Ack received");
    }
    if (awaitingHeartbeatAck) {
      awaitingHeartbeatAck = false;
      clearedSomething = true;
      Serial.println("Heartbeat ack received");
    }
    if (clearedSomething && !debugMode && !awaitingAck && !awaitingHeartbeatAck) {
      sleepRadio();
    }
    return;
  }
  if (type == 'R') {
    applyReset("downlink");
    return;
  }
  if (type == 'C') {
    String cmd = packetField(text, 1);
    cmd.trim();
    cmd.toUpperCase();
    Serial.printf("Remote command: %s\n", cmd.c_str());
    if (cmd == "HB" || cmd == "STATUS" || cmd == "BAT" || cmd == "BATTERY") {
      sendHeartbeat();
    } else if (cmd == "TEST" || cmd == "TRIGGER") {
      triggerDelivery(true, "remote-test");
    } else if (cmd == "RESET") {
      applyReset("remote-command");
    } else {
      Serial.printf("Unknown remote command: %s\n", cmd.c_str());
    }
    return;
  }
  if (type == 'M') {
    String mode = packetField(text, 1);
    mode.trim();
    mode.toUpperCase();
    bool newDebugMode = debugMode;
    if (mode == "D" || mode == "DEBUG" || mode == "1" || mode == "ON") {
      newDebugMode = true;
    } else if (mode == "N" || mode == "NORMAL" || mode == "0" || mode == "OFF") {
      newDebugMode = false;
    } else {
      Serial.printf("Unknown mode command: %s\n", mode.c_str());
      return;
    }
    debugMode = newDebugMode;
    prefs.putBool("debug", debugMode);
    if (debugMode) {
      setSensorPower(false);
      nextSensorSampleAt = millis();
      startRadioRxWindow(0);
    } else {
      setSensorPower(false);
      sleepRadio();
      nextSensorSampleAt = millis();
    }
    nextHeartbeatAt = millis() + 1000;
    Serial.printf("Mode changed -> %s heartbeat=%lus\n", debugMode ? "DEBUG" : "NORMAL",
                  debugMode ? DEBUG_HEARTBEAT_SECS : HEARTBEAT_SECS);
    sendHeartbeat();
    return;
  }
  Serial.printf("Unknown downlink type: %c\n", type);
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("mail node firmware v1.3 (fast remote reset + debug mode)");
  prefs.begin("mail", false);
  state = static_cast<MailState>(prefs.getUChar("state", 0));
  eventSeq = prefs.getUInt("seq", 0);
  batteryCalPermille = prefs.getUShort("batcal", BATTERY_CALIBRATION_PERMILLE);
  debugMode = prefs.getBool("debug", false);
  if (batteryCalPermille < 700 || batteryCalPermille > 1400) {
    batteryCalPermille = BATTERY_CALIBRATION_PERMILLE;
  }
  pinMode(PIN_SENSOR_TX_POWER, OUTPUT);
  pinMode(PIN_SENSOR_RX_POWER, OUTPUT);
  digitalWrite(PIN_SENSOR_TX_POWER, LOW);
  digitalWrite(PIN_SENSOR_RX_POWER, LOW);
  sensorPowerOn = false;
  pinMode(PIN_SENSOR, INPUT_PULLUP);
  if (samplingActiveNow(nullptr)) {
    setSensorPower(true);
    delay(SENSOR_SETTLE_MS);
  }
  sensorRaw = digitalRead(PIN_SENSOR);
  sensorStable = sensorRaw;
  sensorRawChangedAt = millis();
  sensorBlockedSince = sensorIsBlockedLevel(sensorStable) ? millis() : 0;
  setSensorPower(false);
  nextSensorSampleAt = millis();
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_BATTERY_ADC, ADC_11db);
  Serial.printf("Restored state=%s seq=%lu\n", stateName(), static_cast<unsigned long>(eventSeq));
  Serial.printf("Pins: sensor D1/GPIO2=%d active_low=%d tx_power D2/GPIO%d=%d rx_power D3/GPIO%d=%d "
                "battery D0/A0/GPIO1=%d cal=%u/1000 mode=%s\n",
                PIN_SENSOR, SENSOR_ACTIVE_LOW, PIN_SENSOR_TX_POWER, PIN_SENSOR_TX_POWER, PIN_SENSOR_RX_POWER,
                PIN_SENSOR_RX_POWER, PIN_BATTERY_ADC, batteryCalPermille, powerModeName());
  deriveKeys();
  beginRadio();
  if (debugMode) {
    startRadioRxWindow(0);
  } else {
    sleepRadio();
  }
  nextHeartbeatAt = millis() + 10000; // first heartbeat shortly after boot
  printSensor();
  printBattery();
  Serial.println("Commands: t (trigger) | t! (force) | hb | reset | state | sensor | bat | cal <mv> | debug | normal | flash");
}

void loop() {
  if (millis() >= nextSensorSampleAt) {
    sampleSensorLowPower();
  }

  String text;
  uint32_t counter = 0;
  if (!radioSleeping && pollRadio(text, counter)) {
    handleDownlink(text);
  }

  if (awaitingAck && millis() > ackDeadline) {
    if (retriesLeft > 0) {
      retriesLeft--;
      ackDeadline = millis() + ACK_WINDOW_MS;
      Serial.printf("No ack, retrying event (%d left)\n", retriesLeft);
      sendText(lastEventMsg);
    } else {
      awaitingAck = false;
      Serial.println("Event unacked after all retries; staying DELIVERED (heartbeat will self-heal)");
    }
  }

  if (awaitingHeartbeatAck && millis() > heartbeatAckDeadline) {
    if (heartbeatRetriesLeft > 0 && lastHeartbeatMsg.length() > 0) {
      heartbeatRetriesLeft--;
      heartbeatAckDeadline = millis() + ACK_WINDOW_MS;
      Serial.printf("No heartbeat ack, retrying heartbeat (%d left)\n", heartbeatRetriesLeft);
      sendText(lastHeartbeatMsg);
    } else {
      awaitingHeartbeatAck = false;
      Serial.println("Heartbeat unacked after retries; next scheduled heartbeat will try again");
    }
  }

  if (millis() > nextHeartbeatAt) {
    sendHeartbeat();
  }

  if (Serial.available()) {
    String line = Serial.readStringUntil('\n');
    line.trim();
    if (line == "t" || line == "send" || line == "trigger") {
      triggerDelivery(false, "serial");
    } else if (line == "t!" || line == "force") {
      triggerDelivery(true, "serial-force");
    } else if (line == "hb") {
      sendHeartbeat();
    } else if (line == "reset") {
      applyReset("serial");
    } else if (line == "state") {
      String why;
      inDeliveryWindow(&why);
      long mv = batteryMv();
      Serial.printf("state=%s seq=%lu awaiting_ack=%d awaiting_hb_ack=%d mode=%s sampling=%d sensor_power=%d "
                    "radio_sleep=%d uptime=%lus next_hb_in=%lds "
                    "battery=%ldmV pct=%d low=%d power=%s\n",
                    stateName(),
                    static_cast<unsigned long>(eventSeq), awaitingAck, awaitingHeartbeatAck, powerModeName(),
                    samplingActiveNow(), sensorPowerOn, radioSleeping, millis() / 1000,
                    static_cast<long>((static_cast<long long>(nextHeartbeatAt) - static_cast<long long>(millis())) / 1000),
                    mv, batteryPercentFromMv(mv), batteryIsLow(mv), powerSourceFromMv(mv));
      Serial.printf("clock: synced=%d tz_offset_min=%d window: %s\n", utcRef != 0, tzOffsetMin, why.c_str());
      printSensor();
    } else if (line == "sensor") {
      printSensor();
    } else if (line == "bat" || line == "battery") {
      printBattery();
    } else if (line.startsWith("cal ")) {
      calibrateBattery(line.substring(4).toInt());
      printBattery();
    } else if (line == "debug") {
      debugMode = true;
      prefs.putBool("debug", debugMode);
      setSensorPower(false);
      nextSensorSampleAt = millis();
      startRadioRxWindow(0);
      nextHeartbeatAt = millis() + 1000;
      Serial.printf("Mode changed -> DEBUG heartbeat=%lus\n", static_cast<unsigned long>(DEBUG_HEARTBEAT_SECS));
      sendHeartbeat();
    } else if (line == "normal") {
      debugMode = false;
      prefs.putBool("debug", debugMode);
      setSensorPower(false);
      sleepRadio();
      nextSensorSampleAt = millis();
      nextHeartbeatAt = millis() + 1000;
      Serial.printf("Mode changed -> NORMAL heartbeat=%lus\n", static_cast<unsigned long>(HEARTBEAT_SECS));
      sendHeartbeat();
    } else if (line == "flash") {
      rebootToBootloader();
    } else if (line.length() > 0) {
      Serial.println("Commands: t (trigger) | t! (force) | hb | reset | state | sensor | bat | cal <mv> | debug | normal | flash");
    }
  }

  if (!debugMode && !awaitingAck && !awaitingHeartbeatAck && !radioSleeping && radioRxWindowUntil > 0 &&
      static_cast<long>(millis() - radioRxWindowUntil) >= 0) {
    sleepRadio();
  }

  if (!debugMode && !awaitingAck && !awaitingHeartbeatAck && radioSleeping) {
    const unsigned long now = millis();
    unsigned long sleepUntil = now + 1000UL;
    if (samplingActiveNow(nullptr) && static_cast<long>(nextSensorSampleAt - now) > 0) {
      sleepUntil = min(sleepUntil, nextSensorSampleAt);
    }
    if (static_cast<long>(nextHeartbeatAt - now) > 0) {
      sleepUntil = min(sleepUntil, nextHeartbeatAt);
    } else {
      sleepUntil = now + 1UL;
    }
    lightSleepFor(min(1000UL, sleepUntil - now));
  }
}
#endif // ROLE_MAIL
