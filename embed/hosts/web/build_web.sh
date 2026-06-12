#!/usr/bin/env bash
#	build_web.sh - build the ImGui-IDE web host to a single self-contained HTML
#	Part of ImGui-IDE (github.com/lobotomy-x/ImGuiColorTextEdit)
#	MIT license - see <https://opensource.org/licenses/MIT>.
#
#	Requirements:
#	  - emsdk activated in this shell (emcc on PATH) - https://emscripten.org
#	  - a Dear ImGui checkout; pass it in IMGUI_DIR (must match the tag the
#	    embed DLL uses, currently v1.92.7-docking)
#
#	Usage:
#	  IMGUI_DIR=/path/to/imgui ./build_web.sh [output.html]
#
#	Output is one .html with the wasm embedded (-sSINGLE_FILE) so it can be
#	opened straight from disk - no web server needed.

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$HERE/../../.."                       # repo root (TextEditor.cpp lives here)
EMBED="$HERE/../.."                         # embed/ (texteditor_embed.*)
IMGUI_DIR="${IMGUI_DIR:?set IMGUI_DIR to a Dear ImGui checkout (v1.92.7-docking)}"
OUT="${1:-$HERE/imgui_ide_web.html}"

emcc -std=c++17 -O2 \
	-I"$ROOT" -I"$EMBED" -I"$IMGUI_DIR" -I"$IMGUI_DIR/backends" \
	"$HERE/main.cpp" \
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
	-sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2 \
	-sALLOW_MEMORY_GROWTH=1 \
	-sSINGLE_FILE=1 \
	-sEXPORTED_FUNCTIONS=_main,_malloc,_free \
	-sEXPORTED_RUNTIME_METHODS=UTF8ToString,stringToUTF8,lengthBytesUTF8 \
	--shell-file "$HERE/shell.html" \
	-o "$OUT"

echo "built: $OUT"
