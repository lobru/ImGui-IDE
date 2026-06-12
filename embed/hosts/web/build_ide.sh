#!/usr/bin/env bash
#	build_ide.sh - build the GitHub-backed cloud IDE to one self-contained HTML
#	Part of ImGui-IDE (github.com/lobotomy-x/ImGuiColorTextEdit)
#	MIT license - see <https://opensource.org/licenses/MIT>.
#
#	Requirements:
#	  - emsdk activated (emcc on PATH)
#	  - IMGUI_DIR: Dear ImGui checkout (v1.92.7-docking)
#	  - JSON_DIR: directory containing nlohmann/json.hpp (single-header)
#
#	Usage:
#	  IMGUI_DIR=/path/imgui JSON_DIR=/path/json ./build_ide.sh [output.html]

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$HERE/../../.."
EMBED="$HERE/../.."
IMGUI_DIR="${IMGUI_DIR:?set IMGUI_DIR to a Dear ImGui checkout (v1.92.7-docking)}"
JSON_DIR="${JSON_DIR:?set JSON_DIR to a dir containing nlohmann/json.hpp}"
OUT="${1:-$HERE/imgui_ide_cloud.html}"

emcc -std=c++17 -O2 \
	-I"$ROOT" -I"$EMBED" -I"$HERE" -I"$JSON_DIR" -I"$IMGUI_DIR" -I"$IMGUI_DIR/backends" \
	"$HERE/ide_main.cpp" \
	"$EMBED/texteditor_embed.cpp" \
	"$ROOT/TextEditor.cpp" \
	"$ROOT/TextDiff.cpp" \
	"$IMGUI_DIR/imgui.cpp" \
	"$IMGUI_DIR/imgui_draw.cpp" \
	"$IMGUI_DIR/imgui_tables.cpp" \
	"$IMGUI_DIR/imgui_widgets.cpp" \
	"$IMGUI_DIR/backends/imgui_impl_glfw.cpp" \
	"$IMGUI_DIR/backends/imgui_impl_opengl3.cpp" \
	-sUSE_GLFW=3 \
	-sFETCH=1 \
	-sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2 \
	-sALLOW_MEMORY_GROWTH=1 \
	-sSINGLE_FILE=1 \
	-sEXPORTED_FUNCTIONS=_main,_malloc,_free \
	-sEXPORTED_RUNTIME_METHODS=UTF8ToString,stringToUTF8,lengthBytesUTF8 \
	--shell-file "$HERE/shell.html" \
	-o "$OUT"

echo "built: $OUT"
