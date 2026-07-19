# Security Policy

Do not publish live credentials in issues, discussions, screenshots, or pull requests.

## Supported Scope

The current public project supports the latest `main` branch. Security fixes should target `main` first.

## Reporting

If you find a vulnerability, avoid posting exploitable details publicly. Open a minimal issue that says a private report is needed, or contact the project maintainer through the repository owner's preferred private channel.

## Deployment Checklist

- Keep `.env` out of git.
- Keep `firmware/include/lora_mail_config.h` out of git.
- Remove `SETUP_TOKEN` after passkey registration.
- Use separate MQTT users for firmware and panel access.
- Restrict MQTT ACLs to the configured topic prefix where possible.
- Rotate `LORA_MAIL_KEY`, webhooks, MQTT passwords, and Cloudflare variables after any suspected exposure.
- Do not commit `.local/`, logs, device backups, API exports, or screenshots that may show account data.
- Scope the cloud battery monitor's MQTT account to the mailbox topic prefix where possible.
- Keep `MQTT_WSS_URL`, `MQTT_USERNAME`, `MQTT_PASSWORD`, `MAKE_WEBHOOK_URL`, and `MONITOR_ADMIN_TOKEN` in Cloudflare secrets rather than tracked configuration.
- Treat `BATTERY_OFFSET_MV` as deployment data; do not publish a live installation's calibration as a universal default.
