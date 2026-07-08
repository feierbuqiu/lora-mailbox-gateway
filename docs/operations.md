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

## Secret Rotation

Rotate these independently:

- `LORA_MAIL_KEY`: update and reflash both boards.
- WiFi credentials: update and reflash the home gateway.
- Make/webhook URL: update and reflash the home gateway.
- MQTT passwords: update broker credentials, firmware config, and Cloudflare panel env vars.
- `PANEL_SETUP_TOKEN`: remove after registration; set a new temporary value only to add a new passkey.

## Release and Package Operations

Public releases and packages are part of the operational surface:

- Repository releases are attached to signed annotated tags such as `v0.1.0`.
- Panel package publications are attached to signed tags such as `lora-mailbox-panel-v0.1.0`.
- Release assets include checksum files so downstream users can verify downloads.
- The panel package is published by the `Publish Web Panel Package` workflow with `GITHUB_TOKEN` and `packages: write`.

Before publishing a new release or package:

```powershell
git status -sb --ignored
git log --show-signature --oneline -1
git verify-commit HEAD
npm.cmd --prefix web-panel run build
npm.cmd --prefix web-panel run pack:inspect
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
