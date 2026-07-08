$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$script = Join-Path $root "tools\meshtastic_mail_gateway.py"

$python = $env:PYTHON
if (-not $python) {
  $cmd = Get-Command python -ErrorAction SilentlyContinue
  if ($cmd) { $python = $cmd.Source }
}
if (-not $python) {
  throw "Python was not found. Install Python or set PYTHON to the interpreter path."
}
if (-not (Test-Path $script)) {
  throw "Gateway script not found at $script"
}

& $python $script
