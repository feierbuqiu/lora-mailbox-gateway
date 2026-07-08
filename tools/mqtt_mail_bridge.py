"""EMQX -> Make webhook mail bridge.

Subscribes to the home node's Meshtastic JSON uplink topic on EMQX and, for
text messages that start with TRIGGER_PREFIX ("EMAIL:"), posts
{subject, body} to the Make "LoRa Mail Trigger" webhook which sends the email.

Message format on the mesh:  EMAIL: <subject> | <body>
(without "|" the whole text becomes the body and a default subject is used)

Run:  python tools/mqtt_mail_bridge.py            (uses ../.env)
"""

import json
import ssl
import sys
import time
import urllib.request
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


ROOT = Path(__file__).resolve().parent.parent
env = load_env(ROOT / ".env")

HOST = env.get("MQTT_HOST", "")
PORT = int(env.get("MQTT_PORT") or "8883")
USERNAME = env.get("MQTT_USERNAME", "")
PASSWORD = env.get("MQTT_PASSWORD", "")
TOPIC = (env.get("MQTT_ROOT") or "msh/home") + "/2/json/#"
PREFIX = env.get("TRIGGER_PREFIX") or "EMAIL:"
HOOK = env.get("MAKE_LORA_WEBHOOK_URL", "")

if not HOST or not USERNAME or not PASSWORD:
    raise SystemExit("MQTT_HOST/MQTT_USERNAME/MQTT_PASSWORD missing in .env")
if not HOOK:
    raise SystemExit("MAKE_LORA_WEBHOOK_URL missing in .env")

seen_ids = set()


def post_hook(subject, body, counter):
    payload = json.dumps({
        "subject": subject,
        "body": body,
        "node": "home",
        "counter": counter,
        "source": "emqx-mqtt-bridge",
    }).encode("utf-8")
    req = urllib.request.Request(HOOK, data=payload, headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=15) as resp:
        print("HOOK POST status=%s body=%s" % (resp.status, resp.read()[:100]), flush=True)


def on_connect(c, u, f, rc, p=None):
    print("BRIDGE CONNECTED rc=%s, subscribing %s" % (rc, TOPIC), flush=True)
    c.subscribe(TOPIC, qos=0)


def on_message(c, u, m):
    try:
        data = json.loads(m.payload.decode("utf-8"))
    except Exception:
        return
    if data.get("type") != "text":
        return
    msg_id = data.get("id")
    if msg_id in seen_ids:
        return
    seen_ids.add(msg_id)
    text = (data.get("payload") or {}).get("text", "")
    print("TEXT id=%s from=%s: %r" % (msg_id, data.get("sender"), text), flush=True)
    if not text.startswith(PREFIX):
        return
    rest = text[len(PREFIX):].strip()
    if "|" in rest:
        subject, body = [s.strip() for s in rest.split("|", 1)]
    else:
        subject, body = "LoRa mail trigger", rest
    print("TRIGGER subject=%r" % subject, flush=True)
    try:
        post_hook(subject, body, msg_id)
    except Exception as e:
        print("HOOK ERROR:", e, flush=True)


while True:
    try:
        c = mqtt.Client(client_id="mqtt-mail-bridge-%d" % int(time.time()), protocol=mqtt.MQTTv311)
        c.username_pw_set(USERNAME, PASSWORD)
        c.tls_set(cert_reqs=ssl.CERT_REQUIRED)
        c.on_connect = on_connect
        c.on_message = on_message
        c.connect(HOST, PORT, keepalive=30)
        c.loop_forever()
    except KeyboardInterrupt:
        sys.exit(0)
    except Exception as e:
        print("RECONNECT after error:", e, flush=True)
        time.sleep(10)
