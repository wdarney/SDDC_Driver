# macOS ARM64 common-optimization baseline

This baseline is derived directly from the Windows reference commit `e3e5b151e8b968c5c92e5f5ea428158ec96bcc35` on branch `wd/rx888-n95-avx2`. The macOS branch is `wd/rx888-macos-arm64-baseline`. No Windows implementation was replaced or modified.

This phase intentionally stops before NEON intrinsics, FFT replacement, Accelerate/vDSP, Apple-specific scheduling, affinity, QoS, or other macOS performance tuning.

## Classification of the Windows-reference changes

### 1. Platform-independent optimizations inherited by macOS

- Bounded ring blocks now use explicit producer acquire/commit and consumer acquire/release ownership.
- The USB callback copies its completed libusb frame directly into a preallocated raw ring block, removing the temporary vector and its additional raw copy.
- R2IQ processes the raw ring block in place instead of copying it from `ringbuffer::pop()`.
- R2IQ writes directly into a preallocated IQ ring block instead of constructing and then copying an output vector.
- `RadioHandler` consumes the IQ block through its owned pointer instead of copying it out of the ring.
- The 2,048-sample overlap state is a fixed `std::array`, avoiding per-block vector construction.
- The unused `mutexR2iqControl` lock around the one-worker input pop was removed.
- Ring stop/wakeup behavior and counters use explicit atomics and ownership tests.
- Output blocks retain baseline zero initialization because some high-decimation offsets do not overwrite the complete block.
- Deterministic non-constant test input and output hashes provide comparison evidence, subject to the pre-existing ninth-window problem described below.

These changes compile unchanged in the ARM64 build and therefore transfer the calculated copy reduction automatically. At 128 MSPS, the raw side avoids about 1.024 GB/s of read-plus-write memory traffic. At full-band CF32 output, the direct IQ ring path avoids up to another 2.048 GB/s. These are traffic calculations, not measured M3 Max CPU savings.

### 2. Windows/x86-specific optimizations

- MSVC Release options `/O2`, `/Ob2`, and `/fp:precise`.
- CMake IPO/LTCG probing and Release enablement under `MSVC`.
- Explicit `-A x64` in the Windows CI configuration.
- The `SDDCSupport-Windows-x64` GitHub Actions artifact.
- The pre-existing x86 CPUID dispatcher and AVX/AVX2/AVX-512 translation-unit selection remain Windows/x86 mechanisms; the common-copy patch did not change them.

### 3. AVX2-specific optimizations

None were added. The AVX2 file still includes the common R2IQ implementation and is compiled with `/arch:AVX2`; its per-sample loops still depend on compiler auto-vectorization. There are no new AVX2 intrinsics to port to NEON.

### 4. Build changes requiring a macOS ARM64 equivalent

- Apple Clang already builds Release with `-O3 -DNDEBUG`; this baseline does not add a processor-specific `-mcpu` or unsafe floating-point option.
- `CMAKE_OSX_ARCHITECTURES=arm64` makes the module and benchmark thin ARM64 Mach-O artifacts.
- `scripts/build_macos_arm64_baseline.sh` installs only the macOS outputs under `artifacts/macos-arm64/` and enables the standalone benchmark target. The benchmark option defaults off, leaving the Windows reference build unchanged.
- The installed Soapy module is `libSDDCSupport.so`, not a `.dylib`. SoapySDR 0.8 discovers it under `lib/SoapySDR/modules0.8/`.
- The current module uses absolute Homebrew load paths and has no `LC_RPATH`. A portable bundle would need dependency copying and install-name rewriting, but this baseline deliberately remains a development artifact.
- The existing Windows DLL remains a separate GitHub Actions artifact. Nothing from this branch is placed in `artifacts/windows-x64/`.

## ARM64 and NEON audit

- Host: Apple M3 Max, native `arm64`, 16 reported logical CPUs.
- Apple reports `hw.optional.neon=1`.
- `fft_mt_r2iq_impl_neon.cpp` includes the same common R2IQ loop body used by the other architecture variants. ARM64 NEON is available by default, so Apple Clang receives no additional `-mfpu` option.
- Runtime dispatch checks `hw.optional.neon` and selects `r2iqThreadf_neon()`.
- Fine tuning in `pffft/pf_mixer.cpp` is compiled with `PFFFT_ENABLE_NEON` and uses the existing SSE-to-NEON compatibility implementation.
- No explicit NEON intrinsics were added in this phase, and no vectorization-performance claim is made.

## Dependency audit

### FFTW

- Homebrew `fftw3f` version 3.3.11, native ARM64.
- The module links `/opt/homebrew/opt/fftw/lib/libfftw3f.3.dylib`.
- Homebrew enables FFTW threads/OpenMP generally and the ARMv8 counter option, but the driver links the serial `fftw3f` library and calls no FFTW threading APIs.
- The driver uses single-precision FFTW plans with `FFTW_MEASURE` and a relative `wisdom` file.
- No Accelerate or vDSP substitution was attempted.

### libusb

- Homebrew libusb 1.0.29, native ARM64.
- The module links `/opt/homebrew/opt/libusb/lib/libusb-1.0.0.dylib`.
- macOS allocates 16 USB frames with `malloc`; Linux-only `libusb_dev_mem_alloc` zero-copy allocation is not available on this path.
- One poll thread calls `libusb_handle_events_completed`. The libusb completion callback copies each completed frame into an acquired raw ring block and resubmits the transfer.

### SoapySDR

- Homebrew SoapySDR 0.8.1, API 0.8.0 and ABI 0.8, native ARM64.
- With `SOAPY_SDR_PLUGIN_PATH` set to the artifact module directory, Soapy reports module version `1.0.1-e3e5b15` and factory `SDDC`.
- No RX888 was attached during this baseline, so enumeration correctly returned zero devices.

## Thread behavior on Apple Silicon

Steady-state driver threads are:

1. One libusb event/poll thread.
2. One R2IQ worker (`N_MAX_R2IQ_THREADS` is 1 even though the host reports 16 CPUs).
3. One submit/callback thread moving completed IQ blocks into the Soapy buffer ring.
4. One statistics thread, sleeping for one second per iteration in Release.
5. The client benchmark thread calling `readStream()`.

There is no Apple QoS, affinity, performance-core selection, or platform-specific worker-count change. Multiple R2IQ workers remain unsafe without ordered overlap and output handling.

## Baseline build and validation

The unmodified common optimization was first built and installed before the benchmark harness was added.

- Native ARM64 Release module build: passed.
- Focused R2IQ, tuning, ring ownership, and stop-unblock tests: passed.
- Three timed R2IQ test runs took 7.46, 7.06, and 8.07 seconds wall time, but the test contains fixed sleeps and is not a driver-throughput benchmark.
- Higher-decimation first-output hashes varied in one repeated run. This is consistent with the documented ninth FFT window extending beyond initialized/allocated input geometry. That algorithm was not changed here.
- Module load and factory registration: passed.
- Live RX888 sample throughput, CPU percentage, overflows, and real-time ratio: not measured because no SDDC device was attached.

Therefore the macOS baseline result is: the common optimized code builds, loads, and passes focused non-hardware tests on Apple Silicon, while the hardware performance baseline remains pending an attached RX888 MkII.

## Artifacts and benchmark

Build with:

```sh
./scripts/build_macos_arm64_baseline.sh
```

Artifacts:

```text
artifacts/macos-arm64/lib/SoapySDR/modules0.8/libSDDCSupport.so
artifacts/macos-arm64/bin/sddc-soapy-benchmark
```

List devices:

```sh
export SOAPY_SDR_PLUGIN_PATH="$PWD/artifacts/macos-arm64/lib/SoapySDR/modules0.8"
artifacts/macos-arm64/bin/sddc-soapy-benchmark --list
```

Example 128 MSPS ADC / 64 MSPS CF32 benchmark:

```sh
artifacts/macos-arm64/bin/sddc-soapy-benchmark \
  --sample-rate 64000000 \
  --frequency 10000000 \
  --warmup 2 \
  --duration 15
```

The benchmark reports actual sample rate, wall and process CPU time, samples per second, real-time ratio, read calls, timeouts, Soapy overflows, other errors, and read batch sizes. It runs through the Soapy module and requires no SDR++ installation.

## Stop boundary

Do not begin NEON intrinsics, Accelerate/vDSP, alternate FFTW builds, IPO/LTO changes, QoS, affinity, or Apple-specific thread tuning until the standalone benchmark is run with an attached RX888 MkII and this common-code baseline is recorded.

## SDR++ bundle linkage

The standalone benchmark build intentionally uses development libraries found
by Homebrew. A module installed in `SDR++.app` must not introduce a second
SoapySDR, FFTW, or libusb runtime into the process.

Build against the libraries shipped in the target app bundle:

```sh
./scripts/build_macos_arm64_sdrpp_bundle.sh /Applications/SDR++.app
```

The module is staged under
`artifacts/macos-arm64/sdrpp-bundle/lib/SoapySDR/modules0.8/`. Its external
dependencies use `@rpath`, with `LC_RPATH` set to
`@loader_path/../../Frameworks`, so installation under
`Contents/SoapySDR/modules0.8` resolves only against the app's bundled runtime.
