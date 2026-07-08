# Hardware Guide

## Tested Target

- Seeed Studio XIAO ESP32S3
- Seeed Wio-SX1262 LoRa module
- Active-low IR beam sensor
- Optional LiPo battery on the mailbox node
- Optional resistor divider for battery telemetry

The default LoRa settings are 915 MHz, SF7, BW125 kHz, CR4:5, sync word `0x12`, and 17 dBm transmit power. Change these constants in `firmware/src/main.cpp` if your region or hardware requires different radio settings.

## Pin Map

| Function | XIAO pin | Notes |
| --- | --- | --- |
| IR beam input | `D1` / GPIO2 | Active-low by default, `INPUT_PULLUP` |
| Sensor TX power gate | `D2` | Pulsed by firmware to reduce idle current |
| Sensor RX power gate | `D3` | Pulsed by firmware to reduce idle current |
| Battery divider | `A0` / GPIO1 | Default divider is 200k high, 100k low |
| LoRa SPI SCK | GPIO7 | Wio-SX1262 wiring |
| LoRa SPI MISO | GPIO8 | Wio-SX1262 wiring |
| LoRa SPI MOSI | GPIO9 | Wio-SX1262 wiring |
| LoRa NSS | GPIO41 | Wio-SX1262 wiring |
| LoRa DIO1 | GPIO39 | Wio-SX1262 wiring |
| LoRa RESET | GPIO42 | Wio-SX1262 wiring |
| LoRa BUSY | GPIO40 | Wio-SX1262 wiring |
| LoRa RX enable | GPIO38 | Wio-SX1262 wiring |

## Sensor Input Safety

Many IR beam sensors pull their output up to their supply voltage. If the sensor is powered at 5 V, do not connect the output directly to an ESP32S3 GPIO. Either power the sensor at 3.3 V or use a divider/level shifter that keeps the GPIO below the board's safe input voltage.

The firmware assumes an active-low beam:

- unblocked beam: input high
- blocked beam: input low

Invert `SENSOR_ACTIVE_LOW` in `lora_mail_config.h` if your sensor behaves differently.

## Battery Telemetry

The default divider is:

```text
BAT+ -- 200k -- A0 -- 100k -- GND
```

Adjust these macros if your divider differs:

```c
#define BATTERY_DIVIDER_HIGH_OHMS 200000UL
#define BATTERY_DIVIDER_LOW_OHMS 100000UL
#define BATTERY_CALIBRATION_PERMILLE 1000UL
```

The mailbox firmware can calibrate battery reporting from the serial console with `cal <actual_mv>`.
