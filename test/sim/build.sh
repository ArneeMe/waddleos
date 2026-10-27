#!/usr/bin/env bash
# Builds test/sim/sim_node (one simulated mesh unit) into the given directory.
set -euo pipefail
cd "$(dirname "$0")/../.."
out=${1:?output directory}
mkdir -p "$out"
json=$(ls -d .pio/libdeps/*/ArduinoJson/src 2>/dev/null | head -1 || true)
if [ -z "$json" ]; then
  # Without a PlatformIO build: the same ArduinoJson version, header only.
  json="$out/ArduinoJson/src"
  [ -d "$json" ] || git clone -q --depth 1 --branch v7.4.2 https://github.com/bblanchon/ArduinoJson "$out/ArduinoJson"
fi
san="-fsanitize=address,undefined"
gcc -O1 $san -c lib/monocypher/src/monocypher.c -o "$out/monocypher.o"
g++ -std=c++20 -O1 -g -Wall -Wextra -Werror -Wno-maybe-uninitialized $san \
  -Itest/sim/stubs -Isrc -Ilib/WaddleMesh/src -Ilib/monocypher/src -isystem "$json" \
  test/sim/sim_node.cpp src/services/mesh/MeshNode.cpp src/services/mesh/MessageStore.cpp \
  src/services/mesh/MeshConfig.cpp lib/WaddleMesh/src/*.cpp "$out/monocypher.o" -o "$out/sim_node"
