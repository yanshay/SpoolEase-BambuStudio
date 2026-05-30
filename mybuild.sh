#!/usr/bin/env bash
set -e

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

export DEVELOPER_DIR="/Applications/Xcode.app/Contents/Developer"
export SDKROOT="$(xcrun --sdk macosx --show-sdk-path)"
export PATH="/Applications/CMake.app/Contents/bin:$PATH"

which cmake
cmake --version
xcrun --find clang
xcrun --find clang++

./BuildMac.sh
