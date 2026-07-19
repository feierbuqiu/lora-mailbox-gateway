# Operations

## Serial Commands

Home gateway:

| Command | Effect |
| --- | --- |
| `test` | POST a webhook self-test |
| `status` | Print WiFi, MQTT, battery, heartbeat, and queue state |
| `reset-now` | Queue a reset downlink |
| `cmd hb` | Request a mailbox heartbeat |
| `cmd test` | Request a remote test delivery |
| `cmd debug` | Put mailbox node in debug mode |
| `cmd normal` | Return mailbox node to normal mode |
| `flash` | Reboot into ESP32S3 ROM download mode |

Mailbox node:

| Command | Effect |
| --- | --- |
| `t` | Simulate delivery inside the delivery window |
| `t!` | Force simulated delivery |
| `hb` | Send heartbeat |
| `reset` | Return to `ARMED` |
| `state` | Print current state |
| `sensor` | Print sensor state |
| `bat` | Print battery reading |
| `cal <mv>` | Calibrate battery reporting |
| `debug` | Enable 24-hour debug sampling |
| `normal` | Restore normal sampling window |
| `flash` | Reboot into ESP32S3 ROM download mode |

## MQTT Operations

Use retained MQTT topics to recover the panel state after reload. The gateway publishes `mailbox/status`, `mailbox/heartbeat`, and `mailbox/gateway`. The panel publishes `mailbox/reset`, `mailbox/command`, and `mailbox/mode`.

Use a separate MQTT username/password for the panel. Keep the panel account limited to the mailbox topic prefix where your broker supports ACLs.

## Availability alert policy

The normal mailbox heartbeat is every 5 minutes. A missing heartbeat first appears as `probing` in the MQTT/panel state, and becomes `offline_confirmed` in the panel only after `HEARTBEAT_OFFLINE_AFTER_SECS` (30 minutes by default). The firmware deliberately does not POST heartbeat loss to Make, so transient LoRa loss cannot consume webhook operations or send repeated email.

Configure Healthchecks as the single availability-email path. For the default 5-minute heartbeat, use a 55-minute Healthchecks grace period (a 60-minute total alert window). This preserves delivery and battery email alerts while suppressing normal long-range LoRa flaps.

## Secret Rotation

Rotate these independently:

- `LORA_MAIL_KEY`: update and reflash both boards.
- WiFi credentials: update and reflash the home gateway.
- Make/webhook URL: update and reflash the home gateway.
- MQTT passwords: update broker credentials, firmware config, and Cloudflare panel env vars.
- `PANEL_SETUP_TOKEN`: remove after registration; set a new temporary value only to add a new passkey.
- Cloud battery monitor secrets: rotate the MQTT account, webhook, and `MONITOR_ADMIN_TOKEN` in Cloudflare without changing firmware.

## Cloud Battery Monitor Operations

The scheduled Worker runs every minute but only adds a filter sample when it sees a new raw heartbeat timestamp. It republishes corrected retained heartbeat/status payloads and stores its filter/alert state in `mailbox/battery-cloud-state`.

Operational checks:

```powershell
npm.cmd --prefix cloud-battery-monitor test
npm.cmd --prefix cloud-battery-monitor exec -- wrangler deploy --dry-run
Invoke-RestMethod https://<worker>.workers.dev/health
```

The manual `POST /run` route requires `Authorization: Bearer <MONITOR_ADMIN_TOKEN>` and should return `401` without it. A successful result reports raw millivolts, corrected sample millivolts, displayed millivolts, percentage, alert level, and sample count.

Calibration policy:

- Keep the source default at `0 mV`; store the installed node's `BATTERY_OFFSET_MV` in Cloudflare.
- Calculate the offset from a meter reading taken under comparable battery/load conditions.
- Preserve `3.3 V = 0%` and `4.2 V = 100%` unless the battery chemistry or protected-pack limits require a different range.
- Change calibration incrementally and inspect retained raw/calibrated fields after each adjustment.
- If legacy firmware also emits low/critical emails from uncalibrated values, filter only those legacy subjects in the automation layer; preserve delivery, invalid-telemetry, and sustained-offline alerts.

## Release and Package Operations

Public releases and packages are part of the operational surface:

- Repository releases are attached to signed annotated tags such as `v0.2.1`.
- Panel package publications are attached to signed tags such as `lora-mailbox-panel-v0.2.1`.
- Release assets include checksum files so downstream users can verify downloads.
- The panel package is published by the `Publish Web Panel Package` workflow with `GITHUB_TOKEN` and `packages: write`.

Before publishing a new release or package:

```powershell
git status -sb --ignored
git log --show-signature --oneline -1
git verify-commit HEAD
npm.cmd --prefix web-panel run build
npm.cmd --prefix web-panel run pack:inspect
npm.cmd --prefix cloud-battery-monitor test
npm.cmd --prefix cloud-battery-monitor exec -- wrangler deploy --dry-run
```

Use `git tag -s` for public release tags and verify the tag with `git tag -v <tag>` before pushing. After publishing, confirm GitHub reports `verified: true` for the commit/tag and that CI completes successfully.

## Local Meshtastic Proxy

`tools/meshtastic-web-local-proxy.mjs` is retained for legacy Meshtastic setup and recovery. It proxies the hosted Meshtastic web client through localhost and stretches a short reachability timeout. It is not required for the custom firmware path.

```powershell
node tools/meshtastic-web-local-proxy.mjs
```

Then open `http://127.0.0.1:8099/`.

## Troubleshooting

- If upload fails but a COM port is visible, force ESP32S3 bootloader mode with BOOT/RESET timing and retry.
- If MQTT connects from a laptop but not from the gateway, check WiFi RSSI, DNS, TLS port, and broker ACLs.
- If the panel loads but shows no data, check retained MQTT messages and Cloudflare `PANEL_MQTT_*` environment variables.
- If offline alerts are noisy during planned maintenance, use the panel maintenance button or pause the Healthchecks check directly.
- If delivery events duplicate, verify the node remains latched in `DELIVERED` until reset and that the gateway sees monotonic event sequence values.
