#!/usr/bin/env bash
# Installs or updates the Waddle mesh server on the Raspberry Pi.
#   sudo bash server/install.sh
# Run it again after pulling a new version.
set -euo pipefail
cd "$(dirname "$0")"
id waddlemesh >/dev/null 2>&1 || useradd --system --home /var/lib/waddlemesh --groups dialout waddlemesh
install -d -o waddlemesh -g waddlemesh -m 700 /var/lib/waddlemesh /var/backups/waddlemesh
install -d /opt/waddlemesh
rm -rf /opt/waddlemesh/waddlemesh
cp -r waddlemesh /opt/waddlemesh/
if [ ! -f /etc/waddlemesh.json ]; then
  echo '{}' > /etc/waddlemesh.json
fi
chown waddlemesh /etc/waddlemesh.json
chmod 600 /etc/waddlemesh.json
cp systemd/*.service systemd/*.timer /etc/systemd/system/
systemctl daemon-reload
systemctl enable --now waddlemesh-backup.timer
if [ ! -f /var/lib/waddlemesh/cert.pem ]; then
  (cd /opt/waddlemesh && sudo -u waddlemesh python3 -m waddlemesh make-cert --config /etc/waddlemesh.json)
fi
systemctl enable waddlemesh
systemctl restart waddlemesh || true
echo "Installed. Status: systemctl status waddlemesh   Page: http://<pi>:8443 (https)"
