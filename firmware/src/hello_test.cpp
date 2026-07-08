#if TEST_HELLO
#include <Arduino.h>

// XIAO ESP32S3 user LED on GPIO21, active low.
static constexpr int PIN_LED = 21;

void setup() {
  pinMode(PIN_LED, OUTPUT);
  Serial.begin(115200);
}

void loop() {
  static uint32_t n = 0;
  digitalWrite(PIN_LED, n % 2);
  Serial.printf("hello %lu uptime_ms=%lu\n", n++, millis());
  delay(500);
}
#endif
