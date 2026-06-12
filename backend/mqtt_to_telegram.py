"""Forward ESP32-S3-EYE MQTT alarm events to Telegram."""

from __future__ import annotations

import json
import logging
import os
import time
from typing import Any

import requests
from dotenv import load_dotenv
from paho.mqtt import client as mqtt


LOG_FORMAT = "%(asctime)s %(levelname)s %(message)s"
REQUIRED_EVENT_FIELDS = (
    "event_id",
    "device_id",
    "timestamp",
    "label",
    "confidence",
    "confirmation_frames",
)


def required_env(name: str) -> str:
    value = os.getenv(name)
    if not value:
        raise RuntimeError(f"Missing required environment variable: {name}")
    return value


def env_flag(name: str, default: bool = False) -> bool:
    value = os.getenv(name)
    if value is None:
        return default

    return value.strip().lower() in {"1", "true", "yes", "on"}


def parse_payload(raw_payload: bytes) -> dict[str, Any]:
    try:
        payload = json.loads(raw_payload.decode("utf-8"))
    except json.JSONDecodeError as exc:
        raise ValueError("MQTT payload is not valid JSON") from exc

    if not isinstance(payload, dict):
        raise ValueError("MQTT payload must be a JSON object")

    validate_event(payload)

    return payload


def validate_event(event: dict[str, Any]) -> None:
    missing_fields = [field for field in REQUIRED_EVENT_FIELDS if field not in event]
    if missing_fields:
        raise ValueError(f"MQTT payload missing required fields: {', '.join(missing_fields)}")

    for field in ("event_id", "device_id", "timestamp", "label"):
        if not isinstance(event[field], str) or not event[field].strip():
            raise ValueError(f"MQTT payload field {field} must be a non-empty string")

    confidence = event["confidence"]
    if not isinstance(confidence, (float, int)) or isinstance(confidence, bool):
        raise ValueError("MQTT payload field confidence must be a number")

    if confidence < 0 or confidence > 1:
        raise ValueError("MQTT payload field confidence must be between 0 and 1")

    confirmation_frames = event.get("confirmation_frames")
    if confirmation_frames is not None and (
        not isinstance(confirmation_frames, int) or isinstance(confirmation_frames, bool) or confirmation_frames < 1
    ):
        raise ValueError("MQTT payload field confirmation_frames must be a positive integer")


def format_message(event: dict[str, Any]) -> str:
    event_id = event.get("event_id", "unknown-event")
    device_id = event.get("device_id", "unknown-device")
    label = event.get("label", "person")
    confidence = event.get("confidence", "unknown")
    timestamp = event.get("timestamp", "unknown-time")
    confirmation_frames = event.get("confirmation_frames", "unknown")

    if isinstance(confidence, (float, int)):
        confidence_text = f"{confidence:.3f}"
    else:
        confidence_text = str(confidence)

    lines = [
        "Smart Visual Alarm",
        f"Device: {device_id}",
        f"Event: {event_id}",
        f"Detection: {label}",
        f"Confidence: {confidence_text}",
        f"Confirmation frames: {confirmation_frames}",
        f"Timestamp: {timestamp}",
    ]
    box = event.get("box")
    if isinstance(box, dict):
        lines.append(
            "Box: "
            f"{box.get('x_min', '?')},"
            f"{box.get('y_min', '?')},"
            f"{box.get('x_max', '?')},"
            f"{box.get('y_max', '?')}"
        )

    return "\n".join(lines)


def send_telegram_message(token: str, chat_id: str, text: str) -> None:
    response = requests.post(
        f"https://api.telegram.org/bot{token}/sendMessage",
        json={"chat_id": chat_id, "text": text},
        timeout=10,
    )
    response.raise_for_status()


def deliver_alert(token: str, chat_id: str, text: str, dry_run: bool) -> None:
    if dry_run:
        logging.info("Dry-run Telegram alert:\n%s", text)
        return

    send_telegram_message(token, chat_id, text)


def make_client(client_id: str) -> mqtt.Client:
    try:
        return mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=client_id)
    except AttributeError:
        return mqtt.Client(client_id=client_id)


def main() -> None:
    load_dotenv()
    logging.basicConfig(level=logging.INFO, format=LOG_FORMAT)

    mqtt_host = required_env("MQTT_HOST")
    mqtt_port = int(os.getenv("MQTT_PORT", "1883"))
    mqtt_topic = os.getenv("MQTT_TOPIC", "smart-visual-alarm/events")
    mqtt_username = os.getenv("MQTT_USERNAME")
    mqtt_password = os.getenv("MQTT_PASSWORD")
    dry_run = env_flag("TELEGRAM_DRY_RUN", default=False)
    telegram_token = os.getenv("TELEGRAM_BOT_TOKEN", "")
    telegram_chat_id = os.getenv("TELEGRAM_CHAT_ID", "")
    cooldown_seconds = float(os.getenv("ALERT_COOLDOWN_SECONDS", "30"))

    if not dry_run:
        telegram_token = required_env("TELEGRAM_BOT_TOKEN")
        telegram_chat_id = required_env("TELEGRAM_CHAT_ID")

    state = {"last_alert_at": 0.0}
    client = make_client("smart-visual-alarm-backend")

    if mqtt_username:
        client.username_pw_set(mqtt_username, mqtt_password)

    def on_connect(client: mqtt.Client, userdata: Any, flags: Any, reason_code: Any, properties: Any = None) -> None:
        logging.info("Connected to MQTT broker %s:%s with result %s", mqtt_host, mqtt_port, reason_code)
        client.subscribe(mqtt_topic)
        logging.info("Subscribed to %s", mqtt_topic)

    def on_message(client: mqtt.Client, userdata: Any, message: mqtt.MQTTMessage) -> None:
        now = time.monotonic()
        if now - state["last_alert_at"] < cooldown_seconds:
            logging.info("Skipping alert during cooldown window")
            return

        try:
            event = parse_payload(message.payload)
            text = format_message(event)
            deliver_alert(telegram_token, telegram_chat_id, text, dry_run)
            state["last_alert_at"] = now
            if dry_run:
                logging.info("Processed event %s in dry-run mode", event["event_id"])
            else:
                logging.info("Forwarded event %s to Telegram", event["event_id"])
        except Exception:
            logging.exception("Failed to process MQTT alarm event")

    client.on_connect = on_connect
    client.on_message = on_message
    client.connect(mqtt_host, mqtt_port, keepalive=60)
    try:
        client.loop_forever()
    except KeyboardInterrupt:
        logging.info("Stopping backend")


if __name__ == "__main__":
    main()
