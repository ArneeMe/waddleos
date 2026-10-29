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
gcc -O1 $san -Ilib/monocypher/src -c lib/monocypher/src/monocypher-ed25519.c -o "$out/monocypher-ed25519.o"
g++ -std=c++20 -O1 -g -Wall -Wextra -Werror -Wno-maybe-uninitialized $san -DMESH_SIM \
  -Itest/sim/stubs -Isrc -Ilib/WaddleMesh/src -Ilib/monocypher/src -isystem "$json" \
  test/sim/sim_node.cpp src/services/mesh/MeshNode.cpp src/services/mesh/MeshReceive.cpp src/services/mesh/MeshKeys.cpp src/services/mesh/MeshServerSync.cpp src/services/mesh/MeshPairing.cpp src/services/mesh/MessageStore.cpp \
  src/services/mesh/MeshConfig.cpp src/services/mesh/MeshGroups.cpp lib/WaddleMesh/src/*.cpp "$out/monocypher.o" "$out/monocypher-ed25519.o" -o "$out/sim_node"
