# LoRa Mailbox Gateway

An open-source mailbox notification system for two Seeed XIAO ESP32S3 + Wio-SX1262 boards. A low-power mailbox node detects delivery, sends an encrypted LoRa packet to a home gateway, and the gateway fans out to email/webhook alerts, Healthchecks monitoring, MQTT state, and an authenticated Cloudflare Pages status panel. An optional Cloudflare Worker corrects and smooths battery telemetry for permanently installed nodes without requiring a firmware reflash.

## Features

- Two firmware roles in one PlatformIO project: `home` gateway and `mail` sensor node.
- Private LoRa packets with AES-256-CTR payload encryption and HMAC-SHA256 authentication.
- Retained MQTT topics for panel state, heartbeat, gateway presence, reset, command, and mode.
- Cloudflare Pages panel gated by WebAuthn/passkeys.
- No-reflash cloud battery calibration with retained MQTT state, five-sample median filtering, linear percentage calculation, and calibrated low/critical alerts.
- Healthchecks integration for offline monitoring and maintenance pause.
- Make.com-compatible webhook payload for actionable delivery and battery alerts; Healthchecks owns sustained-link email escalation.
- Local helper scripts for EMQX, Meshtastic legacy workflows, MQTT tests, and bench triggers.

## Repository Layout

```text
firmware/              PlatformIO firmware for home/mail/hello environments
tools/                 Local setup, test, and legacy bridge utilities
web-panel/             Cloudflare Pages Worker and static panel source
cloud-battery-monitor/ Scheduled Cloudflare Worker for battery calibration and alerts
docs/                  Architecture, setup, hardware, and operations guides
.github/workflows/     CI and package publication workflows
.env.example           Local environment template
```

Private credentials, operational exports, logs, photos, and historical snapshots are intentionally not tracked. Local copies belong in `.env`, `firmware/include/lora_mail_config.h`, and `.local/`, all of which are ignored.

## Published Artifacts

- Latest public release: [v0.2.2](https://github.com/feierbuqiu/lora-mailbox-gateway/releases/tag/v0.2.2)
- GitHub Packages npm package: `@feierbuqiu/lora-mailbox-panel@0.2.2`
- Signed source tags:
  - `v0.2.2` for the current repository release
  - `lora-mailbox-panel-v0.2.2` for the current panel package publication

The release includes example firmware builds, a built Cloudflare panel bundle, the deployable cloud battery monitor source, and `SHA256SUMS.txt`. Example firmware binaries are for inspection and smoke testing only; production firmware should be rebuilt locally with your own ignored configuration.

## Quick Start

Choose the path that matches your goal:

- **Use the release** when you want a pinned, checksumed baseline.
- **Build from source** when you are provisioning real hardware.
- **Install the package** when you only need the Cloudflare panel source.

### Build From Source

1. Install PlatformIO, Node.js 22+, and Python 3.11+.
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

## Package

The Cloudflare panel is published separately through GitHub Packages:

```text
@feierbuqiu/lora-mailbox-panel
```

The current package version is `0.2.2`. It keeps raw gateway state separate from calibrated battery state and prefers the stable calibrated retained topics produced by `cloud-battery-monitor/`.

Use it when you want to inspect or rebuild the panel without cloning the full firmware repository. GitHub Packages requires npm authentication, so configure the `@feierbuqiu` scope for `https://npm.pkg.github.com` before installing:

```ini
@feierbuqiu:registry=https://npm.pkg.github.com
```

## Cloud Battery Monitor

`cloud-battery-monitor/` is an optional scheduled Worker for installations where retrieving the mailbox node for a firmware update is impractical. It reads the raw retained heartbeat/status messages over MQTT WSS, applies a deployment-specific voltage offset, keeps the latest five distinct samples, publishes a 10 mV-rounded median on dedicated calibrated topics, and recalculates percentage linearly from 3.3 V to 4.2 V. Separating raw and calibrated topics prevents the gateway and Worker from overwriting each other's retained values.

The repository default offset is `0 mV`. Set `BATTERY_OFFSET_MV` in the Cloudflare deployment from a real meter comparison; do not copy another installation's calibration value. See [cloud-battery-monitor/README.md](cloud-battery-monitor/README.md) for deployment, secrets, alert behavior, and verification.

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
