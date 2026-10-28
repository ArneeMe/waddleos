#!/usr/bin/env bash
# Builds the screen simulator and draws the T5 mesh unit's screens to
# build/screens/*.png (or the directory given).
set -euo pipefail
cd "$(dirname "$0")/../.."
out=${1:-build/screens}
bash test/screens/build.sh build/screens-bin
build/screens-bin/screens "$out"
