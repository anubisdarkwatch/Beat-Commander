#!/usr/bin/env bash
# Beat Hub installer for Raspberry Pi OS (any Pi; Ableton Link needs 64-bit, i.e. Zero 2W / 3B / 4 / 5 on a 64-bit OS).
# Usage:  sudo bash install.sh
set -euo pipefail
if [ "$(id -u)" -ne 0 ]; then echo "Run with sudo:  sudo bash install.sh"; exit 1; fi

APP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
USER_NAME="${SUDO_USER:-pi}"
echo "==> Beat Hub install for $USER_NAME ($APP_DIR)"

echo "==> Packages"
apt-get update
apt-get install -y python3-serial python3-venv

echo "==> Python env (Ableton Link)"
sudo -u "$USER_NAME" python3 -m venv --system-site-packages "$APP_DIR/.venv"
if sudo -u "$USER_NAME" "$APP_DIR/.venv/bin/pip" install --only-binary=:all: aalink; then
  echo "    Ableton Link ready"
else
  echo "    No Ableton Link build for this system ($(uname -m)). OSC, MIDI and the web page still work."
  echo "    For Link use a 64-bit Raspberry Pi OS."
fi

echo "==> Config"
[ -f "$APP_DIR/config.ini" ] || cp "$APP_DIR/config.example.ini" "$APP_DIR/config.ini"
usermod -aG dialout "$USER_NAME"
chown -R "$USER_NAME:$USER_NAME" "$APP_DIR"

echo "==> Service"
sed -e "s#__USER__#$USER_NAME#g" -e "s#__APP_DIR__#$APP_DIR#g" -e "s#__PY__#$APP_DIR/.venv/bin/python#g" \
  "$APP_DIR/beat-hub.service.in" > /etc/systemd/system/beat-hub.service
systemctl daemon-reload
systemctl enable beat-hub.service
systemctl restart beat-hub.service

echo
echo "Beat Hub is running. Plug in the Beat Gateway, then open  http://$(hostname).local:8090"
echo "Logs:  journalctl -u beat-hub -f"
