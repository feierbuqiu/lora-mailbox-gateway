# Cloud Battery Monitor

Requires Node.js 22 or newer for the pinned Wrangler version.

This Worker corrects the permanently installed mailbox node without changing or reflashing its firmware.

Every minute it:

1. reads the retained `mailbox/heartbeat`, `mailbox/status`, and cloud-state MQTT topics over WSS;
2. applies the deployment calibration offset (`0 mV` in public source; set `BATTERY_OFFSET_MV` from a real meter comparison);
3. takes the median of the latest five distinct heartbeats and rounds display voltage to `10 mV`;
4. calculates percentage linearly with `3.3 V = 0%` and `4.2 V = 100%`;
5. publishes calibrated heartbeat/status payloads on the dedicated retained topics `mailbox/heartbeat-calibrated` and `mailbox/status-calibrated`;
6. sends calibrated low/critical alerts through the existing Make webhook, with a 24-hour repeat cooldown.

The retained `mailbox/battery-cloud-state` topic stores filter and alert state, so no database or KV binding is required.

## Why this exists

Some mailbox nodes are permanently installed in a location where physical retrieval and reflashing are disproportionately expensive. This Worker corrects telemetry from the cloud side, so the installed firmware can remain untouched. Raw gateway topics remain gateway-owned, while the Worker owns dedicated calibrated topics and preserves the original values inside the calibrated payload for auditing. This single-writer split prevents the panel from alternating between raw and calibrated readings after each heartbeat.

The correction is an installation calibration, not a universal ESP32S3 constant. For example, if a meter reads `4215 mV` while the retained raw heartbeat reports `4051 mV` under comparable conditions, configure `BATTERY_OFFSET_MV=164`. Another divider, ADC, battery, or load path needs its own measurement.

## Required Worker secrets

- `MQTT_WSS_URL`
- `MQTT_USERNAME`
- `MQTT_PASSWORD`
- `MAKE_WEBHOOK_URL`
- `MONITOR_ADMIN_TOKEN`

Optional numeric variables are:

| Variable | Default | Purpose |
| --- | ---: | --- |
| `BATTERY_OFFSET_MV` | `0` | Deployment-specific meter minus raw voltage correction |
| `BATTERY_EMPTY_MV` | `3300` | Linear 0% endpoint |
| `BATTERY_FULL_MV` | `4200` | Linear 100% endpoint |
| `BATTERY_MAX_MV` | `4220` | Safety clamp for corrected/displayed voltage |

## Alert behavior

- `20%` enters low; `10%` enters critical.
- Recovery hysteresis is `25%` for low and `15%` for critical.
- The same level repeats at most once every 24 hours.
- Calibrated alerts use the existing Make-compatible JSON contract and include corrected voltage, linear percentage, raw voltage, and calibration metadata.
- If legacy firmware also generates low/critical mail from uncalibrated values, filter only those legacy subjects at the automation layer to avoid duplicate or premature alerts.

## Verification

Run `npm.cmd test` before deployment. Deploy with `npm.cmd run deploy`, then invoke `POST /run` once with the admin bearer token to perform the first correction immediately.

For a production check, confirm all of the following:

1. `GET /health` returns `200`.
2. unauthenticated `POST /run` returns `401`.
3. an authenticated run reports raw, corrected, displayed, percentage, level, and sample count fields.
4. `mailbox/battery-cloud-state` grows only on distinct raw heartbeat timestamps.
5. `mailbox/heartbeat` remains raw after the next gateway heartbeat, while `mailbox/heartbeat-calibrated` remains `calibrated: true` and keeps `raw_mv`;
6. the panel shows the filtered voltage/percentage and identifies the value as cloud calibrated.
