param(
  [Parameter(Mandatory = $true)]
  [string]$Port,

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
$mqttPort = $config["MQTT_PORT"]
$mqttUser = $config["MQTT_USERNAME"]
$mqttPassword = $config["MQTT_PASSWORD"]
$mqttRoot = $config["MQTT_ROOT"]

if (-not $mqttHost) { throw "MQTT_HOST is empty" }
if (-not $mqttPort) { $mqttPort = "8883" }
if (-not $mqttUser -or -not $mqttPassword) {
  throw "MQTT_USERNAME/MQTT_PASSWORD are empty; create Client Authentication in EMQX first"
}
if ($mqttPassword.Length -gt 31) {
  throw "MQTT_PASSWORD is $($mqttPassword.Length) chars; Meshtastic firmware limit is 31 (protobuf max_size:32 incl. NUL). Device silently rejects the whole config with 'string overflow'."
}
if (-not $mqttRoot) { $mqttRoot = "msh/home" }

$mqttAddress = $mqttHost
if ($mqttPort -ne "8883") {
  $mqttAddress = "${mqttHost}:${mqttPort}"
}

Write-Host "Configuring home MQTT on $Port -> $mqttAddress, root $mqttRoot"
& $mesh --port $Port `
  --set mqtt.address $mqttAddress `
  --set mqtt.username $mqttUser `
  --set mqtt.password $mqttPassword `
  --set mqtt.tls_enabled true `
  --set mqtt.json_enabled true `
  --set mqtt.encryption_enabled false `
  --set mqtt.root $mqttRoot `
  --set mqtt.map_reporting_enabled false `
  --set mqtt.proxy_to_client_enabled false `
  --set lora.ignore_mqtt false `
  --set lora.config_ok_to_mqtt true `
  --set mqtt.enabled true `
  --ch-index 0 `
  --ch-set uplink_enabled true `
  --ch-set downlink_enabled false

Write-Host "Verifying applied MQTT fields"
& $mesh --port $Port `
  --get mqtt.enabled `
  --get mqtt.address `
  --get mqtt.username `
  --get mqtt.tls_enabled `
  --get mqtt.json_enabled `
  --get mqtt.encryption_enabled `
  --get mqtt.root `
  --get mqtt.map_reporting_enabled `
  --get mqtt.proxy_to_client_enabled `
  --get lora.ignore_mqtt `
  --get lora.config_ok_to_mqtt `
  --no-nodes
