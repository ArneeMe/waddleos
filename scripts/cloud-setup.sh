#!/usr/bin/env bash
# Sets up PlatformIO in a Claude Code cloud session so `pio run -e x4pro-waddle`
# works there. Run once per session (the container starts fresh):
#   bash scripts/cloud-setup.sh
# Needs api.registry.platformio.org and dl.registry.platformio.org in the
# environment's allowed domains. Takes a few minutes the first time.
set -euo pipefail

# Same PlatformIO as the CI build (waddle-build.yml).
pip install -q "pioarduino==6.1.19" "littlefs-python>=0.16.0" "fatfs-ng>=0.1.14" "pyyaml>=6.0.2" \
  "rich-click>=1.8.6" "zopfli>=0.2.2" "intelhex>=2.3.0" "rich>=14.0.0" "urllib3<2" \
  "cryptography>=45.0.3" "certifi>=2025.8.3" "ecdsa>=0.19.1" "bitstring>=4.3.1" \
  "reedsolo>=1.5.3,<1.8" "esp-idf-size>=2.0.0" "esp-coredump>=1.14.0" 2>&1 | grep -v "Running pip as" || true

# The session's HTTPS proxy re-signs traffic with its own certificate. PlatformIO
# downloads through Python's certifi list, so add the proxy's certificate there.
add_proxy_ca() {
  for f in $(find / -name cacert.pem -path "*certifi*" 2>/dev/null); do
    grep -q "ccr agent proxy" "$f" || { echo "# ccr agent proxy" >> "$f"; cat /root/.ccr/ca-bundle.crt >> "$f"; }
  done
}
[ -f /root/.ccr/ca-bundle.crt ] && add_proxy_ca

cd "$(dirname "$0")/.."
git submodule update --init --recursive -q
# PlatformIO makes its own Python environment partway through the install, and
# that one needs the certificate too: patch it and run the install again.
if ! pio pkg install -e x4pro-waddle -e t5s3-waddle -e heltec-gateway; then
  [ -f /root/.ccr/ca-bundle.crt ] && add_proxy_ca
  pio pkg install -e x4pro-waddle -e t5s3-waddle -e heltec-gateway
fi
echo "Ready: pio run -e x4pro-waddle (or t5s3-waddle, heltec-gateway)"
