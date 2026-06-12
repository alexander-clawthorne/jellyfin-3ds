#!/bin/bash
# Build jellyfin-3ds using devkitPro Docker image
# Usage: ./build.sh [clean]

set -e

IMAGE="devkitpro/devkitarm:latest"
PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"
MOUNT="/src/jellyfin-3ds"

# Bootstrap FFmpeg static libs on first build (~15 min, one-time)
if [ ! -f "$PROJECT_DIR/lib/ffmpeg/libavformat.a" ]; then
    echo "FFmpeg libs not found — cross-compiling (one-time, ~15 min)..."
    "$PROJECT_DIR/lib/ffmpeg/build-ffmpeg.sh" docker
fi

if [ "$1" = "clean" ]; then
    docker run --rm -v "$PROJECT_DIR:$MOUNT" -w "$MOUNT" "$IMAGE" make clean
fi

docker run --rm -v "$PROJECT_DIR:$MOUNT" -w "$MOUNT" "$IMAGE" make

echo ""
echo "Output: $PROJECT_DIR/jellyfin-3ds.3dsx"
ls -lh "$PROJECT_DIR/jellyfin-3ds.3dsx"
