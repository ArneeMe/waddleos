#!/usr/bin/env bash
# Builds the phone app's C++ (phone/app/src/main/cpp) on this computer as a
# shared library, with Java's jni.h and a stand-in for Android's log, so a
# compile error shows up here and in the tests job, not only in the Android build.
set -euo pipefail
cd "$(dirname "$0")/../.."
out=${1:-build/phone}
mkdir -p "$out/include/android"
printf '#pragma once\n#include <cstdio>\n#define ANDROID_LOG_ERROR 6\n#define ANDROID_LOG_INFO 4\n#define __android_log_print(p, tag, ...) std::fprintf(stderr, __VA_ARGS__)\n' \
  > "$out/include/android/log.h"
java_home=${JAVA_HOME:-$(dirname "$(dirname "$(readlink -f "$(command -v javac || command -v java)")")")}
json=$(ls -d .pio/libdeps/*/ArduinoJson/src 2>/dev/null | head -1 || true)
if [ -z "$json" ]; then
  json="$out/ArduinoJson/src"
  [ -d "$json" ] || git clone -q --depth 1 --branch v7.4.2 https://github.com/bblanchon/ArduinoJson "$out/ArduinoJson"
fi
gcc -O1 -fPIC -c lib/monocypher/src/monocypher.c -o "$out/monocypher.o"
gcc -O1 -fPIC -Ilib/monocypher/src -c lib/monocypher/src/monocypher-ed25519.c -o "$out/monocypher-ed25519.o"
g++ -std=c++20 -O1 -fPIC -shared -Wall -Wextra -Werror -Wno-maybe-uninitialized \
  -I"$out/include" -I"$java_home/include" -I"$java_home/include/linux" \
  -Iphone/app/src/main/cpp/platform -Itest/sim/stubs -Isrc -Ilib/WaddleMesh/src -Ilib/monocypher/src -isystem "$json" \
  phone/app/src/main/cpp/waddle_jni.cpp phone/app/src/main/cpp/platform_glue.cpp lib/WaddleMesh/src/*.cpp \
  src/services/mesh/MeshNode.cpp src/services/mesh/MeshReceive.cpp src/services/mesh/MeshKeys.cpp \
  src/services/mesh/MeshServerSync.cpp src/services/mesh/MeshPairing.cpp src/services/mesh/MessageStore.cpp \
  src/services/mesh/MeshConfig.cpp src/services/mesh/MeshGroups.cpp \
  "$out/monocypher.o" "$out/monocypher-ed25519.o" -o "$out/libwaddle.so"
echo "phone C++ builds: $out/libwaddle.so"
# Every native method once, from Java (the names must match Mesh.kt).
javac -d "$out/classes" test/phone/me/arnee/waddle/Mesh.java test/phone/Smoke.java
rm -rf "$out/files"
java -cp "$out/classes" Smoke "$PWD/$out/libwaddle.so" "$PWD/$out/files" 2>/dev/null
