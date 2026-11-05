#!/usr/bin/env bash
set -e

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$PROJECT_DIR/build/arm64"
WEB_DIST="$PROJECT_DIR/resources/web/device_page/dist"
APP_WEB_DIST="$BUILD_DIR/BambuStudio/BambuStudio.app/Contents/Resources/web/device_page/dist"

if [ ! -d "$BUILD_DIR" ] || [ ! -d "$APP_WEB_DIST" ]; then
    printf '%s\n' "Existing arm64 build or packaged app not found. Run ./mybuild.sh first." >&2
    exit 1
fi

export DEVELOPER_DIR="/Applications/Xcode.app/Contents/Developer"
export SDKROOT="$(xcrun --sdk macosx --show-sdk-path)"
export PATH="/Applications/CMake.app/Contents/bin:$PATH"

cmake --build "$BUILD_DIR" --config Release --target device_page_build
ditto "$WEB_DIST" "$APP_WEB_DIST"
