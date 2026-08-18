# Setup Guide

This guide provisions a reproducible open-source deployment without committing credentials.

## Choose a Starting Point

There are three supported entry points:

- **Release artifacts**: use [v0.2.2](https://github.com/feierbuqiu/lora-mailbox-gateway/releases/tag/v0.2.2) when you want the current pinned public baseline with checksums.
- **Source checkout**: clone the repository when provisioning real hardware or changing firmware/panel behavior.
- **Panel package**: install `@feierbuqiu/lora-mailbox-panel` from GitHub Packages when you only need the Cloudflare panel source.

Release binaries are built with the public example configuration. They do not contain production credentials and should not be treated as drop-in production firmware.

## Prerequisites

- PlatformIO CLI
- Node.js 22 or newer
- Python 3.11 or newer
- Optional Python packages for helper tools:

```powershell
python -m pip install -r requirements.txt
```

## Release Artifacts

Download the latest public release from:

```text
https://github.com/feierbuqiu/lora-mailbox-gateway/releases
```

For `v0.2.2`, the release contains:

- `lora-mailbox-gateway-v0.2.2-example-firmware.zip`
- `lora-mailbox-gateway-v0.2.2-web-panel.zip`
- `lora-mailbox-gateway-v0.2.2-cloud-battery-monitor.zip`
- `SHA256SUMS.txt`

Verify downloaded artifacts before inspecting or deploying them:

```powershell
Get-FileHash .\lora-mailbox-gateway-v0.2.2-web-panel.zip -Algorithm SHA256
Get-FileHash .\lora-mailbox-gateway-v0.2.2-example-firmware.zip -Algorithm SHA256
Get-FileHash .\lora-mailbox-gateway-v0.2.2-cloud-battery-monitor.zip -Algorithm SHA256
Get-Content .\SHA256SUMS.txt
```

The firmware archive is an example build produced from `lora_mail_config.example.h`. For real devices, rebuild from source after filling `firmware/include/lora_mail_config.h` with your private values.

## Local Configuration

Create local configuration files from the public templates:

```powershell
Copy-Item .env.example .env
Copy-Item firmware/include/lora_mail_config.example.h firmware/include/lora_mail_config.h
```

Fill `firmware/include/lora_mail_config.h`:

- `WIFI_SSID` and `WIFI_PASSWORD`
- `MAKE_WEBHOOK_URL`
- `LORA_MAIL_KEY`
- optional `HEALTHCHECKS_URL`
- `MQTT_HOST`, `MQTT_USER`, and `MQTT_PASS`

Generate a fresh 32-byte LoRa key:

```powershell
$bytes = [byte[]]::new(32)
[Security.Cryptography.RandomNumberGenerator]::Fill($bytes)
($bytes | ForEach-Object { "0x{0:x2}" -f $_ }) -join ", "
```

Paste the output into `LORA_MAIL_KEY`. Do not use the all-zero example key outside a bench test.

## Firmware Build

Build both production roles:

```powershell
platformio run -d firmware -e home
platformio run -d firmware -e mail
```

Optional LED sanity build:

```powershell
platformio run -d firmware -e hello
```

Upload with the serial port for each board:

```powershell
platformio run -d firmware -e home -t upload --upload-port COM6
platformio run -d firmware -e mail -t upload --upload-port COM4
```

If a XIAO ESP32S3 stops accepting uploads, enter bootloader mode manually: hold BOOT, tap or reconnect RESET/USB, keep BOOT held for a few seconds, then retry the upload.

## Web Panel Build

The panel is a Cloudflare Pages advanced-mode Worker plus a static HTML asset.

```powershell
cd web-panel
npm install
npm run build
```

The build output is written to `web-panel/deploy/` and is ignored by git.

Required Cloudflare bindings and variables:

```text
AUTH_KV                  KV namespace binding
RP_ID                    panel hostname, for example mailbox.example.com
PANEL_MQTT_URL           wss://<mqtt-host>:8084/mqtt
PANEL_MQTT_USERNAME      panel MQTT username
PANEL_MQTT_PASSWORD      panel MQTT password
PANEL_TOPIC_PREFIX       mailbox
PANEL_STALE_MINUTES      12
SETUP_TOKEN              temporary first-device registration token
HC_API_KEY               optional Healthchecks API key
HC_CHECK_UUID            optional Healthchecks check UUID
```

Deploy:

```powershell
npx wrangler pages deploy deploy --project-name <cloudflare-pages-project>
```

After registering your passkey, remove `SETUP_TOKEN` and redeploy or update the environment.

## Panel Package

The panel source is also published as a GitHub Packages npm package:

```text
@feierbuqiu/lora-mailbox-panel
```

Configure the GitHub Packages scope:

```ini
@feierbuqiu:registry=https://npm.pkg.github.com
```

Then install it in a separate workspace if you only need the panel:

```powershell
npm install @feierbuqiu/lora-mailbox-panel@0.2.2
```

GitHub Packages requires an authenticated npm client, even for public packages. Keep npm tokens in your user-level npm config or CI secrets, not in this repository.

## Cloud Battery Monitor

The optional scheduled Worker corrects battery telemetry without changing mailbox-node firmware. Use it only when you have a trustworthy meter comparison for the installed battery path.

```powershell
cd cloud-battery-monitor
npm.cmd ci
npm.cmd test
npm.cmd exec -- wrangler deploy --dry-run
npm.cmd run deploy
```

Configure these Worker secrets before relying on scheduled runs:

```text
MQTT_WSS_URL             wss://<mqtt-host>:8084/mqtt
MQTT_USERNAME            MQTT account permitted to read/write mailbox retained topics
MQTT_PASSWORD            matching MQTT password
MAKE_WEBHOOK_URL         Make-compatible HTTPS endpoint for calibrated alerts
MONITOR_ADMIN_TOKEN      random bearer token for the manual POST /run endpoint
```

Set the following deployment variables from measured behavior:

```text
BATTERY_OFFSET_MV        actual meter millivolts minus raw reported millivolts
BATTERY_EMPTY_MV         default 3300
BATTERY_FULL_MV          default 4200
BATTERY_MAX_MV           default 4220
```

The repository default for `BATTERY_OFFSET_MV` is `0`. A live installation must supply its own offset; a value measured on one ADC/divider combination is not portable to another. After deployment, confirm `/health`, run one authenticated `POST /run`, inspect retained `mailbox/battery-cloud-state`, and verify that `mailbox/heartbeat-calibrated` shows the corrected value while the raw gateway topic remains independent. The panel should label the displayed voltage as cloud calibrated.

## Make/Webhook Contract

The firmware POSTs JSON to `MAKE_WEBHOOK_URL`:

```json
{
  "subject": "Mailbox: new delivery detected",
  "body": "The mailbox node reported a delivery.",
  "node": "mail",
  "counter": 1,
  "source": "lora-home"
}
```

Make.com, n8n, a custom endpoint, or any HTTPS service can consume this shape.
