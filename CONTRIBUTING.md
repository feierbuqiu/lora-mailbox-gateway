# Contributing

Contributions are welcome when they keep the project reproducible and safe to publish.

## Ground Rules

- Do not commit credentials, real webhook URLs, private node IDs, personal email addresses, or local backups.
- Keep firmware changes compatible with the documented PlatformIO environments.
- Prefer configuration through `.env`, Cloudflare environment variables, or ignored firmware config files.
- Include documentation updates for wiring, protocol, or deployment changes.

## Local Checks

```powershell
Copy-Item firmware/include/lora_mail_config.example.h firmware/include/lora_mail_config.h
platformio run -d firmware -e home
platformio run -d firmware -e mail
cd web-panel
npm install
npm run build
npm run pack:inspect
```

Before opening a PR, run a secret scan over tracked files or at least:

```powershell
rg -n -i "token|secret|password|apikey|api_key|authorization|bearer|webhook" --glob "!web-panel/package-lock.json"
```

## Releases and Packages

Maintainers should keep public releases and packages reproducible:

- Sign release commits and public tags with the configured SSH signing key.
- Verify locally with `git verify-commit HEAD` and `git tag -v <tag>` before pushing.
- Build firmware from `firmware/include/lora_mail_config.example.h` when producing public example artifacts.
- Keep production firmware binaries and private Cloudflare/MQTT values out of release assets.
- Publish the web panel package through the `Publish Web Panel Package` workflow so package publication has an auditable GitHub Actions run.
- Update README and docs whenever release assets, package names, installation steps, or runtime variables change.
