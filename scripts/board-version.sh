#!/usr/bin/env bash
# Prints the version of one device's firmware: "<number> <short hash>".
#
# Each device has its own version, made only from the commits that change its
# firmware, so a change to the mesh unit does not offer the reader an update.
# The number counts those commits (not merges) since WaddleOS 2 began; the hash is the newest
# of them. Needs the full git history (actions/checkout with fetch-depth: 0).
#
#   bash scripts/board-version.sh x4pro|t5s3|gateway
set -euo pipefail

board="${1:?board: x4pro, t5s3 or gateway}"
# The first commit of WaddleOS 2 (two devices, one version per device).
since=29ade4a

# Never part of any firmware.
common=(':!docs' ':!*.md' ':!server' ':!tools' ':!test' ':!protocol' ':!.github'
        ':!scripts/cloud-setup.sh' ':!scripts/board-version.sh')
case "$board" in
  x4pro) paths=(. "${common[@]}" ':!src/products/t5s3' ':!src/products/gateway' ':!src/services') ;;
  t5s3) paths=(. "${common[@]}" ':!src/products/x4pro' ':!src/products/gateway' ':!src/apps/bluesound') ;;
  gateway) paths=(src/products/gateway lib/WaddleMesh platformio.ini) ;;
  *) echo "unknown board: $board" >&2; exit 1 ;;
esac

count=$(git rev-list --count --no-merges "$since..HEAD" -- "${paths[@]}")
hash=$(git log -1 --no-merges --format=%h --abbrev=7 -- "${paths[@]}")
echo "$count $hash"
