#!/usr/bin/env bash
# syntax_check.sh - compile-check (no link) Overture / HPL1 C++ sources on Linux.
#
# Runs  $CXX -std=c++14 -fsyntax-only -w  over each given .cpp with
#   -DPENUMBRA_MULTIPLAYER=1 -DPENUMBRA_VOICE=1 -D__stdcall=
#   -I<stubs> -IOverture -IOverture/multiplayer -IHPL1Engine/include
# where <stubs> = Overture/multiplayer/tools/syntax_stubs/ holds minimal fake
# third-party headers (angelscript, Newton, GLee/gl/glu, SDL, SDL_ttf, theora,
# enet, Cg, opus + AL/al.h + AL/alc.h for the v16 voice chat). _WIN32 is NOT defined by default: files are checked in their
# non-Windows branch, except the ones in WIN32_FILES (see below). The stubs dir
# is put FIRST on the include path so stubs/GL/GLee.h shadows
# HPL1Engine/include/GL/GLee.h (which needs a real glx.h).
#
# Usage:  syntax_check.sh [-v] [--win32] [file.cpp ...]
#   no args     -> checks DEFAULT_FILES below
#   -v          -> print full compiler diagnostics (default: first 20 lines)
#   --win32     -> also -D_WIN32 -DWIN32 and add syntax_stubs/win32/ (fake
#                  windows.h / winsock2.h / iphlpapi.h) for every file given
#   --no-voice  -> leave PENUMBRA_VOICE undefined (the no-opus CMake
#                  configuration: VoiceChat.cpp compiles its stub half)
#   --no-mp     -> leave PENUMBRA_MULTIPLAYER undefined too (single-player
#                  build: every multiplayer file compiles its stub half)
#   CXX=clang++ -> use clang instead of g++
# Exit status: 0 if every file passed, 1 otherwise.
#
# Per-file win32 mode: files listed in WIN32_FILES are always checked in
# --win32 mode. Overture/multiplayer/NetworkManager.cpp is there because its
# LAN-discovery code is NOT behind #ifdef _WIN32: SOCKET / INVALID_SOCKET
# (NetworkManager.cpp:264-265, 486-505), ioctlsocket/WSAIoctl/closesocket
# (:491-503), _snprintf (:512) and GetAdaptersInfo/IP_ADAPTER_INFO/ULONG
# (:2350-2360) are used unguarded, so there is no POSIX branch to check.
# Only NetworkManager.cpp:1-4 (windows.h), :247-255 (iphlpapi.h,
# SIO_UDP_CONNRESET) and :1636-1656 (clipboard) are _WIN32-guarded.
#
# -include cstdint is passed because Overture/GameEnemy.h:368/375/419/421 use
# uint32_t/uint8_t without including <cstdint> (MSVC gets it transitively).
#
# Known limitations: this is a parser-level check only. Missing symbols,
# MSVC-only behaviour and link errors are not detected. Stub functions are
# mostly declared variadic (ret f(...)) so any argument list parses.
#
# Coverage (as of writing, g++ 13 and clang++ both): all 8 DEFAULT_FILES pass;
# 58/59 Overture/*.cpp pass (MainMenu.cpp needs OALWrapper/OAL_Init.h, not
# stubbed); 207/231 HPL1Engine/sources/**/*.cpp pass, including
# scene/MeshEntity.cpp and impl/MeshLoaderCollada.cpp. Engine files that do
# not pass either need bigger third-party stubs (GL extension entry points
# such as glGenQueriesARB/glDrawElements, SDL_BYTEORDER, SDL_image, OALWrapper,
# FLTK FL/fl_ask.H, X11 Display in impl/PBuffer.h:49, AngelScript add-on
# macros BEGIN_AS_NAMESPACE/asOBJ_CLASS_CDA, MSVC io.h) or are MSVC-permissive
# code that a conforming compiler rejects (graphics/Renderer2D.cpp:898 and
# impl/PhysicsBodyNewton.cpp:104 take the address of an rvalue,
# sound/SoundHandler.cpp:289 returns bool as iSoundChannel*). -fpermissive is
# deliberately not used so real errors in game code are not masked.

set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
STUBS="$ROOT/Overture/multiplayer/tools/syntax_stubs"
CXX="${CXX:-g++}"

DEFAULT_FILES=(
  Overture/multiplayer/GhostPlayer.cpp
  Overture/multiplayer/NetworkManager.cpp
  Overture/multiplayer/BodySync.cpp
  Overture/multiplayer/VoiceChat.cpp
  Overture/Player.cpp
  Overture/GameEnemy.cpp
  Overture/GameEnemy_Dog.cpp
  Overture/GameEnemy_Spider.cpp
  Overture/GameScripts.cpp
  Overture/Init.cpp
)

WIN32_FILES=(
  Overture/multiplayer/NetworkManager.cpp
)

VERBOSE=0
WIN32ALL=0
VOICE=1
MP=1
FILES=()
for a in "$@"; do
  case "$a" in
    -v|--verbose) VERBOSE=1 ;;
    --win32) WIN32ALL=1 ;;
    --no-voice) VOICE=0 ;;
    --no-mp) MP=0; VOICE=0 ;;
    -h|--help) sed -n '2,25p' "$0"; exit 0 ;;
    *) FILES+=("$a") ;;
  esac
done
[ ${#FILES[@]} -eq 0 ] && FILES=("${DEFAULT_FILES[@]}")

FLAGS=(-std=c++14 -fsyntax-only -w
  "-D__stdcall=" -include cstdint
  -I"$STUBS" -I"$ROOT/Overture" -I"$ROOT/Overture/multiplayer" -I"$ROOT/HPL1Engine/include")
[ $MP -eq 1 ] && FLAGS+=(-DPENUMBRA_MULTIPLAYER=1)
[ $VOICE -eq 1 ] && FLAGS+=(-DPENUMBRA_VOICE=1)
WIN32_FLAGS=(-D_WIN32 -DWIN32 -I"$STUBS/win32")

is_win32_file() {
  [ $WIN32ALL -eq 1 ] && return 0
  local f="$1" w
  for w in "${WIN32_FILES[@]}"; do
    case "$f" in "$w"|*/"$w") return 0 ;; esac
  done
  return 1
}

pass=0; fail=0; failed=()
for f in "${FILES[@]}"; do
  case "$f" in /*) path="$f" ;; *) path="$ROOT/$f" ;; esac
  extra=(); tag=""
  if is_win32_file "$f"; then extra=("${WIN32_FLAGS[@]}"); tag=" (win32 mode)"; fi
  out="$("$CXX" "${FLAGS[@]}" "${extra[@]}" "$path" 2>&1)"; rc=$?
  if [ $rc -eq 0 ]; then
    echo "PASS  $f$tag"; pass=$((pass+1))
  else
    echo "FAIL  $f$tag"; fail=$((fail+1)); failed+=("$f")
    if [ $VERBOSE -eq 1 ]; then echo "$out"; else echo "$out" | grep -E 'error' | head -20; fi
  fi
done
echo "----"
echo "$pass passed, $fail failed"
[ $fail -eq 0 ]
