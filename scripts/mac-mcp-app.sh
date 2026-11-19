#!/bin/bash
# Builds QIDI Studio with its MCP server on an Apple silicon Mac and runs it as
# ~/Applications/QIDIStudio-MCP.app, apart from an official QIDI Studio install.
# Each step calls BuildMac.sh with the settings that work with current Xcode and CMake 4.
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
app="$HOME/Applications/QIDIStudio-MCP.app"
exe="$app/Contents/MacOS/QIDIStudio"
port=${QIDI_MCP_PORT:-13140}
# At the default 10.15, TBB in the deps does not build against the current libc++ (commit 68765694).
target=14.0
# CMake 4 refuses the old cmake_minimum_required() of some deps.
export CMAKE_POLICY_VERSION_MINIMUM=3.5
# BuildMac.sh passes --parallel with no number, which Makefiles turn into "make -j" with no limit.
export CMAKE_BUILD_PARALLEL_LEVEL=${CMAKE_BUILD_PARALLEL_LEVEL:-$(sysctl -n hw.ncpu)}

usage() {
  cat <<EOF
Usage: scripts/mac-mcp-app.sh <step>
  deps     Build the libraries into deps/build/arm64. Do this one time. It is the longest
           step, uses all cores and about 7 GB of disk.
  build    Build the app into build/arm64. The first build also configures CMake.
  install  Copy the build to $app, give it its own bundle ID and
           a stable signature, then restart.
  restart  Quit the app, start it in the background, and wait until the MCP server answers.
           'restart <file.3mf>' opens that file as a project, with its plates.
  all      build, then install.
EOF
}

die() { echo "$*" >&2; exit 1; }

deps() { cd "$root" && nice ./BuildMac.sh -d -a arm64 -t "$target"; }

build() {
  [ -d "$root/deps/build/arm64/QIDIStudio_deps/usr/local" ] || die "No libraries in deps/build/arm64: run 'scripts/mac-mcp-app.sh deps' first."
  if [ -f "$root/build/arm64/build.ninja" ]; then
    nice cmake --build "$root/build/arm64" --target QIDIStudio
  else
    cd "$root" && nice ./BuildMac.sh -s -x -a arm64 -t "$target"
  fi
}

restart() {
  local file=${1:-}
  [ -z "$file" ] || [ -f "$file" ] || die "No file $file"
  if pgrep -f "$exe" >/dev/null; then
    pkill -TERM -f "$exe"
    for _ in $(seq 30); do pgrep -f "$exe" >/dev/null || break; sleep 1; done
    pgrep -f "$exe" >/dev/null && die "QIDI Studio did not quit in 30 s. Look for an open dialog in the app."
  fi
  # -n: a copy installed before the sign step has the official app's bundle ID. Without -n,
  # macOS can bring that app to the front.
  open -n -g -a "$app" ${file:+"$file"}
  for i in $(seq 120); do
    code=$(curl -s -o /dev/null -w '%{http_code}' -m 2 -X POST "http://127.0.0.1:$port/mcp" || true)
    # 401 is the answer to a request with no token: the server runs.
    [ "$code" != 000 ] && { echo "MCP server answers on port $port (HTTP $code) after ${i} s"; return; }
    sleep 1
  done
  die "No MCP server on port $port after 120 s. Is it on in Preferences > AI Assistant (MCP)? Does a macOS prompt wait for an answer?"
}

install() {
  local src="$root/build/arm64/src/QIDIStudio.app"
  [ -x "$src/Contents/MacOS/QIDIStudio" ] || die "No app in build/arm64: run 'scripts/mac-mcp-app.sh build' first."
  mkdir -p "$app"
  # -L copies the build's Resources link as a folder. rsync writes each changed file as a new
  # file, so macOS does not keep the code signature of the old binary for the new one.
  rsync -aL --delete "$src/" "$app/"
  sign
  restart
}

# macOS keeps a permission, such as access to files on /Volumes, for each bundle ID and signature.
# The build has the official app's bundle ID and a linker signature that changes with each build,
# so each new build, and each start of the official app, asks again.
sign() {
  plutil -replace CFBundleIdentifier -string com.qiditech.qidi-studio.mcp "$app/Contents/Info.plist"
  # The lowest hash, so that each install uses the same identity.
  local id
  id=$(security find-identity -v -p codesigning | awk '/"Apple Development/ {print $2}' | sort | head -1 || true)
  if [ -z "$id" ]; then
    echo "No Apple Development identity: ad hoc signature, so macOS asks again after each build." >&2
    id=-
  fi
  codesign --force --sign "$id" "$app"
}

case "${1:-}" in
  deps) deps ;;
  build) build ;;
  install) install ;;
  restart) restart "${2:-}" ;;
  all) build && install ;;
  -h | --help) usage ;;
  *) usage >&2; exit 1 ;;
esac
