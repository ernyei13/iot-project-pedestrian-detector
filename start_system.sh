#!/usr/bin/env bash
set -Eeuo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BACKEND_DIR="$ROOT_DIR/backend"
FIRMWARE_DIR="$ROOT_DIR/firmware"
RUN_DIR="$ROOT_DIR/.run"
IDF_PATH="${IDF_PATH:-$HOME/.espressif/v5.5.2/esp-idf}"
MQTT_TOPIC="${MQTT_TOPIC:-}"
PUBLIC_MQTT_URI="${PUBLIC_MQTT_URI:-mqtt://test.mosquitto.org:1883}"

MOSQUITTO_PID=""
BACKEND_PID=""

usage() {
  cat <<EOF
Usage:
  ./start_system.sh [--no-flash] [--public-broker] [wifi_ssid] [wifi_password]

What it starts:
  1. MQTT broker connection
  2. Python MQTT-to-Telegram backend
  3. ESP-IDF build, flash, and serial monitor

Options:
  --no-flash    Start broker/backend and open serial monitor only.
                Does not build or flash the ESP32.
  --public-broker
                Use a public MQTT broker instead of this Mac as the broker.
                This helps on Wi-Fi networks that block device-to-device LAN traffic.
  --broker-uri URI
                Use a custom MQTT broker URI, for example mqtt://192.168.1.50:1883.

Optional environment variables:
  IDF_PATH=/path/to/esp-idf
  MQTT_HOST_IP=192.168.x.x
  MQTT_TOPIC=smart-visual-alarm/events
  PUBLIC_MQTT_URI=mqtt://test.mosquitto.org:1883
EOF
}

require_cmd() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "Missing command: $1"
    echo "$2"
    exit 1
  fi
}

cleanup() {
  if [[ -n "${BACKEND_PID:-}" ]] && kill -0 "$BACKEND_PID" >/dev/null 2>&1; then
    kill "$BACKEND_PID" >/dev/null 2>&1 || true
  fi
  if [[ -n "${MOSQUITTO_PID:-}" ]] && kill -0 "$MOSQUITTO_PID" >/dev/null 2>&1; then
    kill "$MOSQUITTO_PID" >/dev/null 2>&1 || true
  fi
}

shell_quote_for_kconfig() {
  python3 - "$1" <<'PY'
import sys
value = sys.argv[1]
print(value.replace("\\", "\\\\").replace('"', '\\"'))
PY
}

upsert_env() {
  local file="$1"
  local key="$2"
  local value="$3"
  python3 - "$file" "$key" "$value" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
key = sys.argv[2]
value = sys.argv[3]
lines = path.read_text().splitlines() if path.exists() else []
prefix = f"{key}="
updated = False
out = []
for line in lines:
    if line.startswith(prefix):
        out.append(f"{key}={value}")
        updated = True
    else:
        out.append(line)
if not updated:
    out.append(f"{key}={value}")
path.write_text("\n".join(out) + "\n")
PY
}

get_host_ip() {
  if [[ -n "${MQTT_HOST_IP:-}" ]]; then
    echo "$MQTT_HOST_IP"
    return
  fi

  if command -v route >/dev/null 2>&1 && command -v ipconfig >/dev/null 2>&1; then
    local iface
    iface="$(route get default 2>/dev/null | awk '/interface:/{print $2; exit}')"
    if [[ -n "$iface" ]]; then
      ipconfig getifaddr "$iface" 2>/dev/null && return
    fi
    ipconfig getifaddr en0 2>/dev/null && return
  fi

  if command -v ip >/dev/null 2>&1; then
    ip route get 1.1.1.1 2>/dev/null | awk '{for (i=1; i<=NF; i++) if ($i=="src") {print $(i+1); exit}}' && return
  fi

  echo "Could not determine this computer's LAN IP. Set MQTT_HOST_IP manually." >&2
  exit 1
}

uri_host() {
  python3 - "$1" <<'PY'
import sys
from urllib.parse import urlparse

uri = sys.argv[1]
parsed = urlparse(uri)
if not parsed.hostname:
    raise SystemExit(f"MQTT broker URI has no host: {uri}")
print(parsed.hostname)
PY
}

uri_port() {
  python3 - "$1" <<'PY'
import sys
from urllib.parse import urlparse

uri = sys.argv[1]
parsed = urlparse(uri)
print(parsed.port or 1883)
PY
}

make_idf_python_shim() {
  local shim_dir="$RUN_DIR/python311-shim"
  local py=""

  if [[ -x "$HOME/anaconda3/bin/python3.11" ]]; then
    py="$HOME/anaconda3/bin/python3.11"
  elif command -v python3.11 >/dev/null 2>&1; then
    py="$(command -v python3.11)"
  else
    echo "Python 3.11 was not found. ESP-IDF v5.5 works best with Python 3.11 here."
    echo "Install it with Homebrew or Anaconda, then run this script again."
    exit 1
  fi

  rm -rf "$shim_dir"
  mkdir -p "$shim_dir"
  ln -s "$py" "$shim_dir/python3"
  ln -s "$py" "$shim_dir/python"
  echo "$shim_dir"
}

ensure_idf() {
  if [[ ! -f "$IDF_PATH/export.sh" || ! -f "$IDF_PATH/install.sh" ]]; then
    echo "ESP-IDF was not found at: $IDF_PATH"
    echo "Set IDF_PATH to the ESP-IDF directory or install ESP-IDF v5.5.x."
    exit 1
  fi

  local python_shim
  python_shim="$(make_idf_python_shim)"
  export PATH="$python_shim:$PATH"

  local export_log="$RUN_DIR/idf_export.log"
  if ! bash -c "source '$IDF_PATH/export.sh' >/dev/null" >"$export_log" 2>&1; then
    echo "ESP-IDF Python environment is missing or incomplete."
    echo "Running ESP-IDF installer for esp32s3. This can take a few minutes..."
    (cd "$IDF_PATH" && ./install.sh esp32s3)
  fi

  # shellcheck disable=SC1091
  source "$IDF_PATH/export.sh"
}

prepare_backend_env() {
  local mqtt_host="$1"
  local mqtt_port="$2"
  local env_file="$BACKEND_DIR/.env"
  if [[ ! -f "$env_file" ]]; then
    echo "No backend/.env found."
    read -r -p "Telegram bot token (Enter for dry-run only): " telegram_token
    local telegram_chat_id=""
    local dry_run="true"
    if [[ -n "$telegram_token" ]]; then
      read -r -p "Telegram chat ID: " telegram_chat_id
      dry_run="false"
    fi

    cat > "$env_file" <<EOF
MQTT_HOST=$mqtt_host
MQTT_PORT=$mqtt_port
MQTT_TOPIC=$MQTT_TOPIC
MQTT_USERNAME=
MQTT_PASSWORD=
TELEGRAM_DRY_RUN=$dry_run
TELEGRAM_BOT_TOKEN=$telegram_token
TELEGRAM_CHAT_ID=$telegram_chat_id
ALERT_COOLDOWN_SECONDS=30
EOF
    chmod 600 "$env_file"
  else
    upsert_env "$env_file" "MQTT_HOST" "$mqtt_host"
    upsert_env "$env_file" "MQTT_PORT" "$mqtt_port"
    upsert_env "$env_file" "MQTT_TOPIC" "$MQTT_TOPIC"
  fi
}

write_firmware_local_defaults() {
  local ssid="$1"
  local password="$2"
  local broker_uri="$3"
  local local_defaults="$FIRMWARE_DIR/sdkconfig.local.defaults"
  local ssid_escaped
  local password_escaped
  ssid_escaped="$(shell_quote_for_kconfig "$ssid")"
  password_escaped="$(shell_quote_for_kconfig "$password")"

  cat > "$local_defaults" <<EOF
# Generated by start_system.sh. This file is ignored by git.
CONFIG_SVA_WIFI_SSID="$ssid_escaped"
CONFIG_SVA_WIFI_PASSWORD="$password_escaped"
CONFIG_SVA_MQTT_URI="$broker_uri"
CONFIG_SVA_MQTT_TOPIC="$MQTT_TOPIC"
EOF
  chmod 600 "$local_defaults"
}

start_mosquitto() {
  if nc -z localhost 1883 >/dev/null 2>&1; then
    echo "MQTT broker already reachable on localhost:1883"
    return
  fi

  echo "Starting Mosquitto..."
  mosquitto -c "$BACKEND_DIR/mosquitto-lan.conf" > "$RUN_DIR/mosquitto.log" 2>&1 &
  MOSQUITTO_PID=$!
  sleep 1
  if ! kill -0 "$MOSQUITTO_PID" >/dev/null 2>&1; then
    echo "Mosquitto failed to start. Log:"
    cat "$RUN_DIR/mosquitto.log"
    exit 1
  fi
}

start_backend() {
  echo "Preparing Python backend..."
  if [[ ! -x "$BACKEND_DIR/.venv/bin/python" ]]; then
    python3 -m venv "$BACKEND_DIR/.venv"
  fi
  "$BACKEND_DIR/.venv/bin/python" -m pip install -q -r "$BACKEND_DIR/requirements.txt"

  echo "Starting Telegram backend..."
  (cd "$BACKEND_DIR" && .venv/bin/python mqtt_to_telegram.py) > "$RUN_DIR/backend.log" 2>&1 &
  BACKEND_PID=$!
  sleep 2
  if ! kill -0 "$BACKEND_PID" >/dev/null 2>&1; then
    echo "Backend failed to start. Log:"
    cat "$RUN_DIR/backend.log"
    exit 1
  fi
}

main() {
  local no_flash="false"
  local broker_mode="local"
  local broker_uri=""
  local positional=()
  while [[ $# -gt 0 ]]; do
    case "$1" in
      -h|--help)
        usage
        exit 0
        ;;
      --no-flash)
        no_flash="true"
        shift
        ;;
      --public-broker)
        broker_mode="public"
        broker_uri="$PUBLIC_MQTT_URI"
        shift
        ;;
      --broker-uri)
        if [[ $# -lt 2 ]]; then
          echo "--broker-uri requires an MQTT URI"
          usage
          exit 1
        fi
        broker_mode="custom"
        broker_uri="$2"
        shift 2
        ;;
      --)
        shift
        while [[ $# -gt 0 ]]; do
          positional+=("$1")
          shift
        done
        ;;
      -*)
        echo "Unknown option: $1"
        usage
        exit 1
        ;;
      *)
        positional+=("$1")
        shift
        ;;
    esac
  done

  mkdir -p "$RUN_DIR"
  trap cleanup EXIT INT TERM

  require_cmd python3 "Install Python 3 first."
  require_cmd mosquitto "Install Mosquitto with: brew install mosquitto"
  require_cmd nc "The nc command is needed to check whether port 1883 is already open."

  local wifi_ssid="${positional[0]:-}"
  local wifi_password="${positional[1]:-}"
  if [[ -z "$wifi_ssid" ]]; then
    read -r -p "Wi-Fi SSID: " wifi_ssid
  fi
  if [[ -z "$wifi_password" ]]; then
    read -r -s -p "Wi-Fi password: " wifi_password
    echo
  fi

  if [[ -z "$MQTT_TOPIC" ]]; then
    if [[ "$broker_mode" == "local" ]]; then
      MQTT_TOPIC="smart-visual-alarm/events"
    else
      MQTT_TOPIC="smart-visual-alarm/ernyei13/esp32s3-eye-01/events"
    fi
  fi

  local backend_mqtt_host
  local backend_mqtt_port
  if [[ "$broker_mode" == "local" ]]; then
    local host_ip
    host_ip="$(get_host_ip)"
    broker_uri="mqtt://$host_ip:1883"
    backend_mqtt_host="localhost"
    backend_mqtt_port="1883"
  else
    backend_mqtt_host="$(uri_host "$broker_uri")"
    backend_mqtt_port="$(uri_port "$broker_uri")"
  fi

  echo "Using MQTT broker address for ESP32: $broker_uri"
  echo "Backend subscribes to: $backend_mqtt_host:$backend_mqtt_port"
  echo "MQTT topic: $MQTT_TOPIC"

  prepare_backend_env "$backend_mqtt_host" "$backend_mqtt_port"
  write_firmware_local_defaults "$wifi_ssid" "$wifi_password" "$broker_uri"
  if [[ "$broker_mode" == "local" ]]; then
    start_mosquitto
  else
    echo "Skipping local Mosquitto; using external MQTT broker."
  fi
  start_backend
  ensure_idf

  echo
  echo "Logs:"
  echo "  $RUN_DIR/mosquitto.log"
  echo "  $RUN_DIR/backend.log"
  echo
  if [[ "$no_flash" == "true" ]]; then
    echo "No-flash mode: opening serial monitor only."
    echo "The already-flashed firmware stays unchanged."
    echo "If you changed broker mode, run once without --no-flash so the ESP32 receives the new broker URI."
  else
    echo "Building, flashing, and opening serial monitor..."
  fi
  echo "Stop the monitor with Ctrl+] or Ctrl+C. Background services stop when this script exits."

  cd "$FIRMWARE_DIR"
  if [[ "$no_flash" == "true" ]]; then
    idf.py monitor
  else
    rm -f sdkconfig
    idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.local.defaults" set-target esp32s3
    idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.local.defaults" build
    idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.local.defaults" flash monitor
  fi
}

main "$@"
