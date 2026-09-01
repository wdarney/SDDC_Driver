#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
app_path=${1:-/Applications/SDR++.app}
frameworks_dir="$app_path/Contents/Frameworks"
build_dir=${SDDC_BUILD_DIR:-"$repo_dir/build-macos-arm64-accelerate-sdrpp-bundle"}
artifact_dir=${SDDC_ARTIFACT_DIR:-"$repo_dir/artifacts/macos-arm64/accelerate-sdrpp-bundle"}

if [ ! -d "$frameworks_dir" ]; then
    echo "ERROR: SDR++ Frameworks directory not found: $frameworks_dir" >&2
    exit 1
fi

cmake -S "$repo_dir" -B "$build_dir" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_INSTALL_PREFIX="$artifact_dir" \
    -DSDDC_BUILD_SOAPY_BENCHMARK=OFF \
    -DSDDC_USE_ACCELERATE_FFT=ON \
    -DSDDC_MACOS_BUNDLE_FRAMEWORKS_DIR="$frameworks_dir"

cmake --build "$build_dir" --config Release --target SDDCSupport unittest
ctest --test-dir "$build_dir" --output-on-failure
cmake --install "$build_dir" --config Release

module="$artifact_dir/lib/SoapySDR/modules0.8/libSDDCSupport.so"

# Libraries copied into an SDR++ bundle can retain their original Homebrew
# install IDs. Normalize the module's references after installation so the
# bundle-relative rpath remains authoritative on machines without Homebrew.
otool -L "$module" | tail -n +2 | awk '{print $1}' | while IFS= read -r dependency; do
    dependency_name=$(basename "$dependency")
    case "$dependency_name" in
        libSoapySDR.0.8.dylib|libusb-1.0.0.dylib|libfftw3f.3.dylib)
            install_name_tool -change "$dependency" "@rpath/$dependency_name" "$module"
            ;;
    esac
done

if otool -L "$module" | grep -q '/opt/homebrew'; then
    echo "ERROR: Homebrew runtime dependency remains in $module" >&2
    exit 1
fi
if otool -L "$module" | grep -q 'libfftw'; then
    echo "ERROR: Accelerate build unexpectedly links FFTW: $module" >&2
    exit 1
fi

echo "SDR++ bundle-linked Accelerate ARM64 module: $module"
file "$module"
otool -L "$module"
