param(
  [string]$HostName = "meshtastic.local",
  [string]$EnvFile = ".env"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$envPath = Join-Path $root $EnvFile

$mesh = $env:MESHTASTIC_EXE
if (-not $mesh) {
  $cmd = Get-Command meshtastic -ErrorAction SilentlyContinue
  if ($cmd) { $mesh = $cmd.Source }
}
if (-not $mesh) {
  throw "Meshtastic CLI not found. Install it with 'python -m pip install meshtastic' or set MESHTASTIC_EXE."
}
if (-not (Test-Path $envPath)) {
  throw "Env file not found at $envPath"
}

$config = @{}
Get-Content -LiteralPath $envPath | ForEach-Object {
  $line = $_.Trim()
  if (-not $line -or $line.StartsWith("#") -or -not $line.Contains("=")) {
    return
  }
  $parts = $line.Split("=", 2)
  $config[$parts[0].Trim()] = $parts[1].Trim().Trim('"').Trim("'")
}

$mqttHost = $config["MQTT_HOST"]
$mqttUser = $config["MQTT_USERNAME"]
$mqttPassword = $config["MQTT_PASSWORD"]
$mqttRoot = $config["MQTT_ROOT"]

if (-not $mqttHost) { throw "MQTT_HOST is empty" }
if (-not $mqttUser -or -not $mqttPassword) { throw "MQTT_USERNAME/MQTT_PASSWORD are empty" }
if ($mqttPassword.Length -gt 31) {
  throw "MQTT_PASSWORD is $($mqttPassword.Length) chars; Meshtastic firmware limit is 31 (protobuf max_size:32 incl. NUL). Device silently rejects the whole config with 'string overflow'."
}
if (-not $mqttRoot) { $mqttRoot = "msh/home" }

function Set-MeshField($Field, $Value, [switch]$Secret) {
  Write-Host "Setting $Field"
  $oldPreference = $ErrorActionPreference
  $ErrorActionPreference = "Continue"
  $output = & $mesh --host $HostName --set $Field $Value 2>&1
  $code = $LASTEXITCODE
  $ErrorActionPreference = $oldPreference
  if ($Secret) {
    $output | ForEach-Object { ($_ -replace 'Set mqtt\.password to .+$', 'Set mqtt.password to [redacted]') }
  } else {
    $output | ForEach-Object { $_ }
  }
  if ($code -ne 0) {
    throw "Failed setting $Field"
  }
  Start-Sleep -Seconds 3
}

Set-MeshField "mqtt.enabled" "false"
Set-MeshField "mqtt.address" $mqttHost
Set-MeshField "mqtt.username" $mqttUser
Set-MeshField "mqtt.password" $mqttPassword -Secret
Set-MeshField "mqtt.tls_enabled" "true"
Set-MeshField "mqtt.encryption_enabled" "false"
Set-MeshField "mqtt.json_enabled" "true"
Set-MeshField "mqtt.root" $mqttRoot
Set-MeshField "mqtt.map_reporting_enabled" "false"
Set-MeshField "mqtt.proxy_to_client_enabled" "false"
Set-MeshField "lora.ignore_mqtt" "false"
Set-MeshField "lora.config_ok_to_mqtt" "true"

Write-Host "Enabling channel uplink"
& $mesh --host $HostName --ch-index 0 --ch-set uplink_enabled true --ch-set downlink_enabled false
if ($LASTEXITCODE -ne 0) {
  throw "Failed setting channel uplink"
}
Start-Sleep -Seconds 3

Set-MeshField "mqtt.enabled" "true"

Write-Host "Verifying MQTT fields"
& $mesh --host $HostName `
  --get mqtt.enabled `
  --get mqtt.address `
  --get mqtt.username `
  --get mqtt.tls_enabled `
  --get mqtt.json_enabled `
  --get mqtt.encryption_enabled `
  --get mqtt.root `
  --get lora.ignore_mqtt `
  --get lora.config_ok_to_mqtt `
  --no-nodes
