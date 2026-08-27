#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
build_dir="$repo_dir/build-macos-arm64-baseline"
artifact_dir="$repo_dir/artifacts/macos-arm64"

cmake -S "$repo_dir" -B "$build_dir" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_INSTALL_PREFIX="$artifact_dir" \
    -DSDDC_BUILD_SOAPY_BENCHMARK=ON

cmake --build "$build_dir" --target SDDCSupport sddc-soapy-benchmark -j8
cmake --install "$build_dir"

module_dir="$artifact_dir/lib/SoapySDR/modules0.8"
echo "macOS ARM64 artifacts installed in: $artifact_dir"
echo "List devices with:"
echo "SOAPY_SDR_PLUGIN_PATH='$module_dir' '$artifact_dir/bin/sddc-soapy-benchmark' --list"
