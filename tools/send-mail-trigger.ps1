param(
  [Parameter(Mandatory = $true)]
  [string]$Port,

  [Parameter(Mandatory = $true)]
  [string]$Destination,

  [string]$Subject = "Meshtastic trigger test",
  [string]$Body = "Triggered from mail over the private LoRa channel."
)

$ErrorActionPreference = "Stop"
$mesh = $env:MESHTASTIC_EXE
if (-not $mesh) {
  $cmd = Get-Command meshtastic -ErrorAction SilentlyContinue
  if ($cmd) { $mesh = $cmd.Source }
}
if (-not $mesh) {
  throw "Meshtastic CLI not found. Install it with 'python -m pip install meshtastic' or set MESHTASTIC_EXE."
}

$message = "EMAIL: $Subject | $Body"
Write-Host "Sending trigger from $Port to $Destination"
& $mesh --port $Port --dest $Destination --ch-index 0 --sendtext $message --ack
