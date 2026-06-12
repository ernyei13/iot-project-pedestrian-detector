# IoT Project Pedestrian Detector

Smart visual alarm prototype for the ESP32-S3-EYE. The device captures camera frames, runs pedestrian detection locally on the ESP32-S3, confirms repeated positive detections, publishes a compact MQTT alarm event, and a Python backend forwards the event to Telegram.

## Repository Contents

- `firmware/` - ESP-IDF firmware for the ESP32-S3-EYE.
- `backend/` - Python MQTT subscriber and Telegram notification bridge.
- `report.pdf` - final technical report.
- `presentation.pptx` - final presentation deck.

## Requirements

- ESP32-S3-EYE board connected over USB.
- ESP-IDF v5.5.x configured for `esp32s3`.
- Python 3.10 or newer.
- Mosquitto MQTT broker.
- Telegram bot token and chat ID.
- Wi-Fi network reachable by the ESP32-S3-EYE and the computer running the backend.

## Firmware Setup

```bash
cd firmware
idf.py set-target esp32s3
idf.py menuconfig
```

In `menuconfig`, configure:

- Wi-Fi SSID and password.
- MQTT broker URI, for example `mqtt://<computer-ip>:1883`.
- MQTT topic, default `smart-visual-alarm/events`.
- Detection threshold, confirmation frames, and cooldown if needed.

Then build, flash, and monitor:

```bash
idf.py build
idf.py flash monitor
```

The firmware continues to run the detector and prints alarm JSON over serial even if Wi-Fi or MQTT is not configured.

## Backend Setup

```bash
cd backend
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
cp .env.example .env
```

Edit `backend/.env` with the local MQTT settings and Telegram credentials. Keep this file private.

Start a local MQTT broker:

```bash
mosquitto -c mosquitto-lan.conf
```

Run the Telegram bridge:

```bash
python mqtt_to_telegram.py
```

## Manual MQTT Test

With the backend running, publish the sample event:

```bash
mosquitto_pub -h localhost -t smart-visual-alarm/events -f sample_event.json
```

If Telegram credentials are correct, the configured chat receives a readable alarm message with device ID, event ID, detection label, confidence, confirmation frame count, timestamp, and bounding box.
