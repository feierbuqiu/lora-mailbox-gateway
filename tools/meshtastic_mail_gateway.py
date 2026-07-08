#!/usr/bin/env python3
import argparse
import logging
import os
import signal
import smtplib
import sys
import time
from datetime import datetime, timezone
from email.message import EmailMessage
from pathlib import Path

import meshtastic.serial_interface
import meshtastic.tcp_interface
from pubsub import pub


ROOT = Path(__file__).resolve().parents[1]
LOG_DIR = ROOT / "logs"
LOG_FILE = LOG_DIR / "mail_gateway.log"


def load_dotenv(path: Path) -> None:
    if not path.exists():
        return
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        key = key.strip()
        value = value.strip().strip('"').strip("'")
        os.environ.setdefault(key, value)


def env_bool(name: str, default: bool) -> bool:
    value = os.environ.get(name)
    if value is None:
        return default
    return value.strip().lower() in {"1", "true", "yes", "on"}


def normalize_node_id(value: object) -> str:
    if value is None:
        return ""
    if isinstance(value, int):
        return f"!{value & 0xFFFFFFFF:08x}"
    text = str(value).strip()
    if not text:
        return ""
    if text.startswith("!"):
        return text.lower()
    if text.startswith("0x"):
        return f"!{int(text, 16) & 0xFFFFFFFF:08x}"
    if text.isdigit():
        return f"!{int(text) & 0xFFFFFFFF:08x}"
    return text.lower()


def packet_sender(packet: dict, interface) -> tuple[str, str, str]:
    node_id = normalize_node_id(packet.get("fromId") or packet.get("from"))
    node = None
    if node_id:
        node = interface.nodes.get(node_id)
    if node is None and packet.get("from") is not None:
        node = interface.nodesByNum.get(packet.get("from"))

    user = (node or {}).get("user", {}) if isinstance(node, dict) else {}
    long_name = str(user.get("longName") or "")
    short_name = str(user.get("shortName") or "")
    return node_id, long_name, short_name


def packet_text(packet: dict) -> str:
    decoded = packet.get("decoded") or {}
    if "text" in decoded:
        return str(decoded["text"])
    payload = decoded.get("payload")
    if isinstance(payload, bytes):
        return payload.decode("utf-8", errors="replace")
    return ""


def parse_trigger(text: str, prefix: str, token: str) -> tuple[str, str] | None:
    if not text.startswith(prefix):
        return None
    rest = text[len(prefix) :].strip()
    if token:
        token_prefix = f"{token}:"
        if not rest.startswith(token_prefix):
            return None
        rest = rest[len(token_prefix) :].strip()
    if "|" in rest:
        subject, body = rest.split("|", 1)
    else:
        subject, body = "Meshtastic mail trigger", rest
    subject = subject.strip() or "Meshtastic mail trigger"
    body = body.strip() or "(empty trigger body)"
    return subject, body


def smtp_configured() -> bool:
    required = ["SMTP_HOST", "SMTP_PORT", "SMTP_USER", "SMTP_PASSWORD", "EMAIL_TO"]
    return all(os.environ.get(name) for name in required)


def send_email(subject: str, body: str, sender_label: str) -> None:
    if not smtp_configured():
        logging.warning("SMTP not configured; dry-run email subject=%r body=%r", subject, body)
        return

    smtp_user = os.environ["SMTP_USER"]
    email_from = os.environ.get("EMAIL_FROM") or smtp_user
    email_to = os.environ["EMAIL_TO"]

    message = EmailMessage()
    message["From"] = email_from
    message["To"] = email_to
    message["Subject"] = subject
    message.set_content(
        "\n".join(
            [
                body,
                "",
                f"Triggered by: {sender_label}",
                f"Received at: {datetime.now(timezone.utc).isoformat()}",
            ]
        )
    )

    host = os.environ["SMTP_HOST"]
    port = int(os.environ["SMTP_PORT"])
    use_tls = env_bool("SMTP_TLS", True)
    with smtplib.SMTP(host, port, timeout=30) as smtp:
        smtp.ehlo()
        if use_tls:
            smtp.starttls()
            smtp.ehlo()
        smtp.login(smtp_user, os.environ["SMTP_PASSWORD"])
        smtp.send_message(message)

    logging.info("Email sent to %s subject=%r", email_to, subject)


def allowed_sender(node_id: str, long_name: str, short_name: str) -> bool:
    allowed_id = normalize_node_id(os.environ.get("MAIL_NODE_ID", ""))
    if allowed_id:
        return node_id == allowed_id
    expected = (os.environ.get("MAIL_NODE_NAME") or "mail").casefold()
    return long_name.casefold() == expected or short_name.casefold() == expected


def main() -> int:
    load_dotenv(ROOT / ".env")
    parser = argparse.ArgumentParser(description="Meshtastic mail trigger gateway")
    parser.add_argument("--host", default=os.environ.get("MESHTASTIC_HOST", "meshtastic.local"))
    parser.add_argument("--tcp-port", type=int, default=int(os.environ.get("MESHTASTIC_TCP_PORT", "4403")))
    parser.add_argument("--serial-port", default=os.environ.get("MESHTASTIC_SERIAL_PORT", ""))
    parser.add_argument("--listen-seconds", type=int, default=0)
    parser.add_argument("--simulate-trigger", default="")
    parser.add_argument("--simulate-node-id", default="")
    parser.add_argument("--simulate-node-name", default="mail")
    args = parser.parse_args()

    LOG_DIR.mkdir(exist_ok=True)
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s %(levelname)s %(message)s",
        handlers=[logging.StreamHandler(sys.stdout), logging.FileHandler(LOG_FILE, encoding="utf-8")],
    )

    prefix = os.environ.get("TRIGGER_PREFIX", "EMAIL:")
    token = os.environ.get("TRIGGER_TOKEN", "")

    if args.simulate_trigger:
        node_id = normalize_node_id(args.simulate_node_id)
        if not allowed_sender(node_id, args.simulate_node_name, args.simulate_node_name[:4]):
            logging.error("Simulated sender is not authorized")
            return 2
        parsed = parse_trigger(args.simulate_trigger, prefix, token)
        if parsed is None:
            logging.error("Simulated trigger did not match prefix/token")
            return 2
        subject, body = parsed
        send_email(subject, body, f"{args.simulate_node_name} {node_id}".strip())
        return 0

    stop = False
    interface_holder = {"interface": None}

    def on_receive(packet, interface):
        text = packet_text(packet)
        node_id, long_name, short_name = packet_sender(packet, interface)
        sender_label = f"{long_name or short_name or 'unknown'} {node_id}".strip()
        logging.info("RX text from %s: %r", sender_label, text)

        if not allowed_sender(node_id, long_name, short_name):
            logging.warning("Ignored packet from unauthorized sender %s", sender_label)
            return

        parsed = parse_trigger(text, prefix, token)
        if parsed is None:
            logging.info("Ignored authorized packet because it did not match trigger prefix")
            return

        subject, body = parsed
        send_email(subject, body, sender_label)

    def on_connection(interface, topic=pub.AUTO_TOPIC):
        local = getattr(interface, "myInfo", None)
        logging.info("Connected to Meshtastic radio: %s", local)

    def on_lost(interface=None, topic=pub.AUTO_TOPIC):
        logging.warning("Meshtastic connection lost")

    def handle_signal(signum, frame):
        nonlocal stop
        stop = True

    signal.signal(signal.SIGINT, handle_signal)
    signal.signal(signal.SIGTERM, handle_signal)
    pub.subscribe(on_receive, "meshtastic.receive.text")
    pub.subscribe(on_connection, "meshtastic.connection.established")
    pub.subscribe(on_lost, "meshtastic.connection.lost")

    if args.serial_port:
        interface = meshtastic.serial_interface.SerialInterface(args.serial_port)
        logging.info("Listening via serial %s", args.serial_port)
    else:
        interface = meshtastic.tcp_interface.TCPInterface(args.host, portNumber=args.tcp_port)
        logging.info("Listening via TCP %s:%s", args.host, args.tcp_port)
    interface_holder["interface"] = interface

    deadline = time.time() + args.listen_seconds if args.listen_seconds > 0 else None
    try:
        while not stop:
            if deadline and time.time() >= deadline:
                break
            time.sleep(0.5)
    finally:
        interface.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
