#!/usr/bin/env bash
# Builds and runs the mesh library tests on this computer (no device needed),
# and rewrites protocol/vectors.json from the C++ code.
set -euo pipefail
cd "$(dirname "$0")/../.."
out=$(mktemp -d)
san="-fsanitize=address,undefined"
gcc -O1 $san -c lib/monocypher/src/monocypher.c -o "$out/monocypher.o"
g++ -std=c++17 -O1 -Wall -Wextra -Werror $san -Ilib/WaddleMesh/src -Ilib/monocypher/src \
  test/mesh/test_mesh.cpp lib/WaddleMesh/src/*.cpp "$out/monocypher.o" -o "$out/test_mesh"
"$out/test_mesh" protocol/vectors.json
rm -rf "$out"
