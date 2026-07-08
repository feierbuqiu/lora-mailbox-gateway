param(
  [string]$EnvFile = ".env"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$envPath = Join-Path $root $EnvFile

if (-not (Test-Path $envPath)) {
  throw "Env file not found at $envPath"
}

function Read-DotEnv($Path) {
  $data = [ordered]@{}
  Get-Content -LiteralPath $Path | ForEach-Object {
    $line = $_.Trim()
    if (-not $line -or $line.StartsWith("#") -or -not $line.Contains("=")) {
      return
    }
    $parts = $line.Split("=", 2)
    $data[$parts[0].Trim()] = $parts[1].Trim().Trim('"').Trim("'")
  }
  $data
}

function Set-DotEnvValue($Path, $Key, $Value) {
  $lines = [System.Collections.Generic.List[string]]::new()
  $found = $false
  if (Test-Path $Path) {
    foreach ($line in Get-Content -LiteralPath $Path) {
      if ($line -match "^\s*$([regex]::Escape($Key))\s*=") {
        $lines.Add("$Key=$Value")
        $found = $true
      } else {
        $lines.Add($line)
      }
    }
  }
  if (-not $found) {
    $lines.Add("$Key=$Value")
  }
  Set-Content -LiteralPath $Path -Value $lines -Encoding utf8
}

$config = Read-DotEnv $envPath
$apiBase = [string]$config["EMQX_DEPLOYMENT_API_BASE"]
$apiBase = $apiBase.TrimEnd("/")
$appId = $config["EMQX_APP_ID"]
$appSecret = $config["EMQX_APP_SECRET"]
$mqttUser = $config["MQTT_USERNAME"]
$mqttPassword = $config["MQTT_PASSWORD"]

if (-not $apiBase) { throw "EMQX_DEPLOYMENT_API_BASE is empty" }
if (-not $appId -or -not $appSecret) { throw "EMQX_APP_ID/EMQX_APP_SECRET are empty" }

if (-not $mqttUser) {
  $mqttUser = "home"
  Set-DotEnvValue $envPath "MQTT_USERNAME" $mqttUser
}
if (-not $mqttPassword) {
  $bytes = [byte[]]::new(24)
  $rng = [Security.Cryptography.RNGCryptoServiceProvider]::Create()
  try {
    $rng.GetBytes($bytes)
  } finally {
    $rng.Dispose()
  }
  $mqttPassword = [Convert]::ToBase64String($bytes).TrimEnd("=")
  Set-DotEnvValue $envPath "MQTT_PASSWORD" $mqttPassword
}

$basic = [Convert]::ToBase64String([Text.Encoding]::ASCII.GetBytes(("{0}:{1}" -f $appId, $appSecret)))
$headers = @{
  Authorization = "Basic $basic"
  Accept = "application/json"
}
$body = @{
  user_id = $mqttUser
  password = $mqttPassword
  is_superuser = $false
} | ConvertTo-Json -Compress
$userPath = "/authentication/password_based:built_in_database/users"
$userUri = "$apiBase$userPath"

try {
  $response = Invoke-WebRequest -Headers $headers -Method Post -Uri $userUri -Body $body -ContentType "application/json" -UseBasicParsing -ErrorAction Stop
  Write-Host "Created EMQX MQTT user '$mqttUser' (status $($response.StatusCode))"
} catch {
  $status = $null
  if ($_.Exception.Response) { $status = [int]$_.Exception.Response.StatusCode }
  if ($status -eq 400 -or $status -eq 409) {
    $encodedUser = [Uri]::EscapeDataString($mqttUser)
    $updateUri = "$apiBase$userPath/$encodedUser"
    $response = Invoke-WebRequest -Headers $headers -Method Put -Uri $updateUri -Body $body -ContentType "application/json" -UseBasicParsing -ErrorAction Stop
    Write-Host "Updated EMQX MQTT user '$mqttUser' (status $($response.StatusCode))"
  } else {
    throw
  }
}

$testScript = Join-Path $root "tools\test-emqx-mqtt.ps1"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File $testScript -EnvFile $EnvFile
if ($LASTEXITCODE -ne 0) {
  exit $LASTEXITCODE
}
