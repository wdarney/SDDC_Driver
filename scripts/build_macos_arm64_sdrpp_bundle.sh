#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
app_path=${1:-/Applications/SDR++.app}
frameworks_dir="$app_path/Contents/Frameworks"
build_dir="$repo_dir/build-macos-arm64-sdrpp-bundle"
artifact_dir="$repo_dir/artifacts/macos-arm64/sdrpp-bundle"

cmake -S "$repo_dir" -B "$build_dir" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_INSTALL_PREFIX="$artifact_dir" \
    -DSDDC_BUILD_SOAPY_BENCHMARK=OFF \
    -DSDDC_MACOS_BUNDLE_FRAMEWORKS_DIR="$frameworks_dir"

cmake --build "$build_dir" --config Release --target SDDCSupport
cmake --install "$build_dir" --config Release

module="$artifact_dir/lib/SoapySDR/modules0.8/libSDDCSupport.so"
if otool -L "$module" | grep -q '/opt/homebrew'; then
    echo "ERROR: Homebrew runtime dependency remains in $module" >&2
    exit 1
fi

echo "SDR++ bundle-linked ARM64 module: $module"
file "$module"
otool -L "$module"
