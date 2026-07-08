param(
  [Parameter(Mandatory = $true)]
  [string]$Port,

  [string]$ChannelUrl = "",

  [string]$ChannelFile = ".local\private\home-private-channel.secret.txt"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$secretPath = Join-Path $root $ChannelFile

$mesh = $env:MESHTASTIC_EXE
if (-not $mesh) {
  $cmd = Get-Command meshtastic -ErrorAction SilentlyContinue
  if ($cmd) { $mesh = $cmd.Source }
}
if (-not $mesh) {
  throw "Meshtastic CLI not found. Install it with 'python -m pip install meshtastic' or set MESHTASTIC_EXE."
}

if (-not $ChannelUrl) {
  if (-not (Test-Path $secretPath)) {
    throw "Provide -ChannelUrl or create a private channel file at $secretPath"
  }
  $raw = Get-Content -Raw -LiteralPath $secretPath
  $match = [regex]::Match($raw, "https://meshtastic\.org/e/#\S+")
  if (-not $match.Success) {
    throw "Could not find a Meshtastic channel URL in $secretPath"
  }
  $ChannelUrl = $match.Value.Trim()
}

Write-Host "Configuring mail device on $Port"
& $mesh --port $Port --set-owner "mail" --set-owner-short "mail"
Start-Sleep -Seconds 4
& $mesh --port $Port --ch-set-url $ChannelUrl
Start-Sleep -Seconds 8
& $mesh --port $Port --set device.node_info_broadcast_secs 86400 --set device.rebroadcast_mode LOCAL_ONLY
Start-Sleep -Seconds 4
& $mesh --port $Port --set bluetooth.enabled false
Start-Sleep -Seconds 4
& $mesh --port $Port --set lora.hop_limit 1 --set lora.ignore_mqtt true --set lora.config_ok_to_mqtt false
Start-Sleep -Seconds 4
& $mesh --port $Port --set mqtt.enabled false --set mqtt.map_reporting_enabled false --set mqtt.proxy_to_client_enabled false --set mqtt.json_enabled false --set mqtt.tls_enabled false
Start-Sleep -Seconds 4
& $mesh --port $Port --remove-position
Start-Sleep -Seconds 3
& $mesh --port $Port --set position.gps_enabled false --set position.gps_mode DISABLED --set position.position_broadcast_smart_enabled false --set position.position_flags UNSET
Start-Sleep -Seconds 4
& $mesh --port $Port --reset-nodedb
Start-Sleep -Seconds 8
& $mesh --port $Port --info
