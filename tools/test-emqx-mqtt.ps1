param(
  [string]$EnvFile = ".env",
  [string]$Topic = "xiao/codex/test"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$envPath = Join-Path $root $EnvFile

$python = $env:PYTHON
if (-not $python) {
  $cmd = Get-Command python -ErrorAction SilentlyContinue
  if ($cmd) { $python = $cmd.Source }
}
if (-not $python) {
  throw "Python was not found. Install Python or set PYTHON to the interpreter path."
}
if (-not (Test-Path $envPath)) {
  throw "Env file not found at $envPath"
}

$script = @'
import json
import os
import ssl
import sys
import time
from pathlib import Path

import paho.mqtt.client as mqtt


def load_env(path):
    data = {}
    for raw in Path(path).read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        data[key.strip()] = value.strip().strip('"').strip("'")
    return data


env = load_env(sys.argv[1])
topic = sys.argv[2]
host = env.get("MQTT_HOST", "")
port = int(env.get("MQTT_PORT") or "8883")
username = env.get("MQTT_USERNAME", "")
password = env.get("MQTT_PASSWORD", "")

if not host:
    raise SystemExit("MQTT_HOST is empty")
if not username or not password:
    raise SystemExit("MQTT_USERNAME/MQTT_PASSWORD are empty; create Client Authentication in EMQX first")

result = {
    "host": host,
    "port": port,
    "topic": topic,
    "connected": False,
    "reason_code": None,
    "disconnect_code": None,
    "error": None,
}

try:
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION1, client_id=f"codex-emqx-test-{int(time.time())}", protocol=mqtt.MQTTv311)
except Exception:
    client = mqtt.Client(client_id=f"codex-emqx-test-{int(time.time())}", protocol=mqtt.MQTTv311)
client.username_pw_set(username, password)
client.tls_set(cert_reqs=ssl.CERT_REQUIRED)
client.tls_insecure_set(False)


def on_connect(client, userdata, flags, rc, properties=None):
    result["reason_code"] = int(rc)
    result["connected"] = int(rc) == 0
    if int(rc) == 0:
        client.publish(topic, payload="codex mqtt tls test", qos=0)
        client.disconnect()


def on_disconnect(client, userdata, rc, properties=None):
    try:
        result["disconnect_code"] = int(rc)
    except Exception:
        result["disconnect_code"] = str(rc)


client.on_connect = on_connect
client.on_disconnect = on_disconnect

try:
    client.connect(host, port, keepalive=20)
    client.loop_start()
    deadline = time.time() + 8
    while time.time() < deadline and result["reason_code"] is None and result["disconnect_code"] is None:
        time.sleep(0.1)
    client.loop_stop()
    try:
        client.disconnect()
    except Exception:
        pass
except Exception as exc:
    result["error"] = type(exc).__name__ + ": " + str(exc)

print(json.dumps(result, ensure_ascii=False, indent=2))
if not result["connected"]:
    raise SystemExit(2)
'@

$script | & $python - $envPath $Topic
if ($LASTEXITCODE -ne 0) {
  exit $LASTEXITCODE
}
