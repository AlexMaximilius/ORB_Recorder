#!/usr/bin/env bash
# build.sh -- Linux build for ORB_Recorder Orb.
# Alex Maz -- first program (2026) -- Linux port.
#
# Requires (Debian/Ubuntu):
#   sudo apt install build-essential libglfw3-dev libx11-dev libxext-dev \
#                    libxrandr-dev libgl1-mesa-dev libglu1-mesa-dev
set -euo pipefail
cd "$(dirname "$0")"

CC=${CC:-gcc}
CFLAGS="-O2 -std=c11 -Wall -Wextra -Wno-unused-parameter"
# Static libgcc/pthread so the binary does not need the toolchain's
# runtime present on the target machine.
CFLAGS="$CFLAGS -static-libgcc"
INCS="$(pkg-config --cflags glfw3 gl glu x11 xext 2>/dev/null || true)"
LIBS="$(pkg-config --libs   glfw3 gl glu x11 xext 2>/dev/null || echo '-lglfw -lGL -lGLU -lX11 -lXext')"
# Xrandr + Xshm are pulled in via libxext / libxrandr:
# -ldl: gdk-pixbuf is dlopen'd at runtime, never linked, so the
# binary still runs on a box that does not have it.
LIBS="$LIBS -lXrandr -lpthread -lm -ldl"

echo "Compiling orb_recorder (Linux/X11)..."
$CC $CFLAGS $INCS -o orb_recorder \
    orb_recorder.c platform_x11.c \
    $LIBS

echo "BUILD OK -- ./orb_recorder"
echo "Log at:   \$XDG_STATE_HOME/ORB_Recorder/log.txt (or ~/.local/state/ORB_Recorder/log.txt)"
echo "Output:   \$XDG_PICTURES_DIR/GIF (or ~/Pictures/GIF)"
