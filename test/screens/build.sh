#!/usr/bin/env bash
# Builds test/screens/screens (the T5 mesh unit's screens, drawn to PNG) into
# the given directory. Needs only g++, git and python3 (no PlatformIO); fetches
# the freeink-sdk submodule and ArduinoJson when they are missing.
set -euo pipefail
cd "$(dirname "$0")/../.."
out=${1:?output directory}
mkdir -p "$out/obj"

if [ ! -f freeink-sdk/libs/ui/FreeInkUI/src/FreeInkUI.cpp ]; then
  git submodule update --init --depth 1 freeink-sdk
fi
json=$(ls -d .pio/libdeps/*/ArduinoJson/src 2>/dev/null | head -1 || true)
if [ -z "$json" ]; then
  # Without a PlatformIO build: the same ArduinoJson version, header only.
  json="$out/ArduinoJson/src"
  [ -d "$json" ] || git -c advice.detachedHead=false clone -q --depth 1 --branch v7.4.2 \
    https://github.com/bblanchon/ArduinoJson "$out/ArduinoJson"
fi
# Translations (tr(STR_*)), generated as in the firmware build.
[ -f lib/I18n/I18nKeys.h ] || python3 scripts/gen_i18n.py lib/I18n/translations lib/I18n >/dev/null

sdk=freeink-sdk/libs
# The t5s3-waddle build's defines. test/screens/stubs comes first: it stands in
# for the hardware (display, buttons, clock, FreeRTOS) and the SD-card fonts.
flags=(-std=gnu++2a -O1 -g -w
  -DFREEINK_DEVICE_LILYGO=1 -DFREEINK_CAP_TOUCH=1 -DWADDLE=1 -DWADDLE_PRIVATE=1 '-DWADDLE_OWNER_NAME="Arne"' -DLOG_LEVEL=1
  -Itest/screens/stubs -Itest/sim/stubs -Isrc
  -Ilib/GfxRenderer -Ilib/EpdFont -Ilib/Utf8 -Ilib/MiniBidi -Ilib/Memory -Ilib/InflateReader -Ilib/uzlib/src
  -Ilib -Ilib/Epub -Ilib/FsHelpers -Ilib/BlePageTurner/include -Ilib/I18n -Ilib/Serialization
  -Ilib/WaddleMesh/src -Ilib/monocypher/src
  -I$sdk/hardware/BoardConfig/include -I$sdk/display/FreeInkDisplay/include -I$sdk/ui/FreeInkUI/include
  -I$sdk/assets/Icons/include -I$sdk/network/BleKeyboardHost/include
  -isystem "$json")

sources=(
  test/screens/screens.cpp test/screens/stubs/stubs.cpp
  # Drawing: the renderer, the built-in fonts, FreeInkUI and the Lyra theme.
  lib/GfxRenderer/GfxRenderer.cpp lib/GfxRenderer/FontCacheManager.cpp
  lib/GfxRenderer/Bitmap.cpp lib/GfxRenderer/BitmapHelpers.cpp
  lib/EpdFont/EpdFont.cpp lib/EpdFont/EpdFontFamily.cpp lib/EpdFont/FontDecompressor.cpp
  lib/Utf8/Utf8.cpp lib/MiniBidi/BidiUtils.cpp lib/Memory/BuildScratch.cpp lib/InflateReader/InflateReader.cpp
  lib/FsHelpers/FsHelpers.cpp lib/I18n/I18n.cpp lib/I18n/I18nStrings.cpp
  $sdk/ui/FreeInkUI/src/FreeInkUI.cpp
  src/core/components/UITheme.cpp src/core/components/themes/BaseTheme.cpp
  src/core/components/themes/lyra/LyraTheme.cpp src/core/waddle/WaddleDraw.cpp
  # The screens, and the screens they open.
  src/core/activities/Activity.cpp src/core/util/ButtonNavigator.cpp
  src/core/activities/util/KeyboardEntryActivity.cpp src/core/activities/util/NorwegianKeyboard.cpp
  src/products/t5s3/T5HomeActivity.cpp src/products/t5s3/T5SettingsActivity.cpp src/products/t5s3/T5AppsActivity.cpp src/apps/notes/NotesActivity.cpp
  src/apps/messages/MeshUi.cpp src/apps/messages/MessagesActivity.cpp src/apps/messages/PlayUi.cpp
  src/apps/map/MapActivity.cpp src/apps/sos/SosAlertActivity.cpp
  # The mesh: the real node, store and crypto.
  src/services/mesh/MeshNode.cpp src/services/mesh/MeshReceive.cpp src/services/mesh/MeshKeys.cpp src/services/mesh/MeshServerSync.cpp src/services/mesh/MeshPairing.cpp src/services/mesh/MessageStore.cpp src/services/mesh/MeshConfig.cpp src/services/mesh/MeshGroups.cpp
  lib/WaddleMesh/src/*.cpp
)
csources=(lib/monocypher/src/monocypher.c lib/monocypher/src/monocypher-ed25519.c lib/MiniBidi/minibidi.c lib/uzlib/src/*.c)

# Every file at once, then link. time() is wrapped so the clock is the program's.
objs=()
pids=()
for src in "${sources[@]}" "${csources[@]}"; do
  obj="$out/obj/$(echo "$src" | tr '/' '_').o"
  objs+=("$obj")
  case "$src" in
    *.c) gcc -O1 -w -Ilib/uzlib/src -c "$src" -o "$obj" & ;;
    *) g++ "${flags[@]}" -c "$src" -o "$obj" & ;;
  esac
  pids+=($!)
done
failed=0
for pid in "${pids[@]}"; do wait "$pid" || failed=1; done
[ "$failed" = 0 ] || exit 1
g++ "${objs[@]}" -Wl,--wrap=time -o "$out/screens"
