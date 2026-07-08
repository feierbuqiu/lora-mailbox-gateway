# @feierbuqiu/lora-mailbox-panel

Cloudflare Pages Worker and static status panel for LoRa Mailbox Gateway. The package is published to GitHub Packages so the panel source can be consumed, inspected, and rebuilt independently from the full firmware repository.

## Install

Create or update an `.npmrc` file for the GitHub Packages scope:

```ini
@feierbuqiu:registry=https://npm.pkg.github.com
```

Then install the package:

```powershell
npm install @feierbuqiu/lora-mailbox-panel
```

GitHub Packages requires an authenticated npm client, even for public packages. Use a token through your user-level `.npmrc` or through `NODE_AUTH_TOKEN` in automation.

## Build

From this package directory:

```powershell
npm install
npm run build
```

The build writes a Cloudflare Pages-compatible `deploy/` directory containing:

- `_worker.js`
- `index.html`

## Runtime Configuration

Secrets are not embedded in the package. Supply runtime values through Cloudflare environment variables:

- `RP_ID`
- `PANEL_MQTT_URL`
- `PANEL_MQTT_USERNAME`
- `PANEL_MQTT_PASSWORD`
- optional `SETUP_TOKEN`
- optional `PANEL_TOPIC_PREFIX`
- optional `PANEL_STALE_MINUTES`
- optional `HC_API_KEY`
- optional `HC_CHECK_UUID`

See the repository setup and operations guides before deploying this panel publicly.

## License

MIT.
