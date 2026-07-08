# Setup Guide

This guide provisions a reproducible open-source deployment without committing credentials.

## Prerequisites

- PlatformIO CLI
- Node.js 20 or newer
- Python 3.11 or newer
- Optional Python packages for helper tools:

```powershell
python -m pip install -r requirements.txt
```

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
