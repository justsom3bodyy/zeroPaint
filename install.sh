#!/bin/sh
# install.sh - quick build helper script for 0paint on linux
set -eu

# === sanity checks ===
has_cmd() {
    command -v "$1" >/dev/null 2>&1
}

if ! has_cmd gcc && ! has_cmd clang; then
    echo "error: need gcc or clang installed first"
    echo "  debian/ubuntu: sudo apt install build-essential libsdl2-dev pkg-config"
    echo "  fedora:        sudo dnf install gcc make SDL2-devel pkgconf-pkg-config"
    echo "  arch:          sudo pacman -S base-devel sdl2 pkgconf"
    echo "  alpine:        sudo apk add build-base sdl2-dev pkgconf"
    echo "  void:          sudo xbps-install -S base-devel SDL2-devel pkg-config"
    exit 1
fi

if ! pkg-config --exists sdl2 2>/dev/null && ! has_cmd sdl2-config; then
    echo "error: sdl2 dev headers not found (install libsdl2-dev / SDL2-devel)"
    exit 1
fi

# === build and strip ===
CC_BIN="gcc"
if ! has_cmd gcc; then
    CC_BIN="clang"
fi

echo "building 0paint with $CC_BIN..."
make CC="$CC_BIN" clean
make CC="$CC_BIN"

# strip symbols if avialable to keep the binary tiny
if has_cmd strip; then
    strip --strip-unneeded ./0paint || true
fi

SIZE=$(wc -c < ./0paint)
echo "done: ./0paint (${SIZE} bytes)"
echo "run:     ./0paint [file]"
echo "install: sudo make install"
