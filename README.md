# LoRa Mailbox Gateway

An open-source mailbox notification system for two Seeed XIAO ESP32S3 + Wio-SX1262 boards. A low-power mailbox node detects delivery, sends an encrypted LoRa packet to a home gateway, and the gateway fans out to email/webhook alerts, Healthchecks monitoring, MQTT state, and an authenticated Cloudflare Pages status panel.

## Features

- Two firmware roles in one PlatformIO project: `home` gateway and `mail` sensor node.
- Private LoRa packets with AES-256-CTR payload encryption and HMAC-SHA256 authentication.
- Retained MQTT topics for panel state, heartbeat, gateway presence, reset, command, and mode.
- Cloudflare Pages panel gated by WebAuthn/passkeys.
- Healthchecks integration for offline monitoring and maintenance pause.
- Make.com-compatible webhook payload for email or other automations.
- Local helper scripts for EMQX, Meshtastic legacy workflows, MQTT tests, and bench triggers.

## Repository Layout

```text
firmware/              PlatformIO firmware for home/mail/hello environments
tools/                 Local setup, test, and legacy bridge utilities
web-panel/             Cloudflare Pages Worker and static panel source
docs/                  Architecture, setup, hardware, and operations guides
.github/workflows/     CI for firmware and panel builds
.env.example           Local environment template
```

Private credentials, operational exports, logs, photos, and historical snapshots are intentionally not tracked. Local copies belong in `.env`, `firmware/include/lora_mail_config.h`, and `.local/`, all of which are ignored.

## Quick Start

1. Install PlatformIO, Node.js 20+, and Python 3.11+.
2. Copy `.env.example` to `.env`.
3. Copy `firmware/include/lora_mail_config.example.h` to `firmware/include/lora_mail_config.h`.
4. Fill WiFi, LoRa key, webhook, MQTT, and optional Healthchecks values.
5. Build the firmware:

```powershell
platformio run -d firmware -e home
platformio run -d firmware -e mail
```

6. Build the panel:

```powershell
cd web-panel
npm install
npm run build
```

See [docs/setup.md](docs/setup.md) for full provisioning and deployment steps.

## Security Notes

This repository is designed so secrets are supplied at build/deploy time, not committed:

- Firmware secrets live in ignored `firmware/include/lora_mail_config.h`.
- Cloud service secrets live in ignored `.env` or Cloudflare environment variables.
- The web panel source does not embed MQTT credentials; it fetches them from the authenticated Worker API.
- Historical exports and backups should stay under ignored `.local/`.

Read [SECURITY.md](SECURITY.md) before deploying a public panel.

## Hardware Target

The firmware defaults target:

- Seeed Studio XIAO ESP32S3
- Seeed Wio-SX1262 LoRa module
- LoRa 915 MHz, SF7, BW125 kHz, CR4:5
- Active-low IR beam sensor on `D1`
- Sensor power gates on `D2` and `D3`
- Battery divider on `A0`

Adjust frequency, power, and sensor wiring for your region and hardware. Details are in [docs/hardware.md](docs/hardware.md).

## License

MIT. See [LICENSE](LICENSE).
