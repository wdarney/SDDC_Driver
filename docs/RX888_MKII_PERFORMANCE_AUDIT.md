# RX888 MkII / SoapySDR steady-state performance audit

Target: Windows 10/11 x64, Intel N95 (4 cores, AVX2), RX888 MkII, 128 MSPS ADC, CF32 output through SoapySDR 0.8.

Audit baseline: upstream `e540d6c2df41e629e9a487090411e311493d49ca`.

## Streaming path and ownership

| Stage | Allocation / ownership | Baseline transition | Synchronization |
|---|---|---|---|
| FX3 / Windows CyAPI | Four fixed 128 KiB overlapped-transfer buffers are allocated once per stream | FX3 DMA/USB fills a CyAPI-owned frame | Cypress driver overlapped I/O and one completion thread |
| USB callback | `PacketRead()` constructed a 65,536-element `vector<int16_t>` per transfer | full 128 KiB copy from USB frame to temporary vector | callback blocks when the input ring is full |
| Raw input ring | 64 vectors, resized once to 65,536 `int16_t` values each | `push(vector by value)` copied the temporary into the ring; `pop()` copied it out again | atomics plus one mutex/CV pair; 100-iteration spin before sleeping |
| R2IQ input preparation | one FFTW-aligned float work area plus FFT workspaces, allocated in `Init()` | complete raw block converted from int16 to float; 2,048-sample overlap copied into a newly constructed vector | one R2IQ worker in current upstream |
| Forward FFT | FFTW single-precision real-to-complex, size 10,240 | overlapping windows read the converted float work area | no FFTW internal threading API is used |
| Shift / filter | preallocated `inFreqTmp` and filter spectra | complex multiply and bin shift, plus zero fill | per-bin scalar source loop compiled in the selected SIMD translation unit |
| Inverse FFT | FFTW single-precision complex transform, sizes 5,120 down to 80 | one inverse transform per processed forward window | no FFTW internal threading API is used |
| IQ postprocess | baseline allocated a 65,536-float vector in each R2IQ thread start | copy I/Q, optionally negate Q for LSB | output ring backpressure |
| IQ ring | 64 preallocated 256 KiB blocks | baseline `push()` and `pop()` each made a full-buffer copy | same ring mutex/CV design |
| Soapy callback ring | 16 preallocated byte vectors | callback copies IQ into its own bounded buffer | atomic count plus mutex/CV |
| `readStream()` | caller owns SDR++ buffer | mandatory Soapy copy into the caller's CF32 buffer | acquire/release buffer protocol |

No heap allocation is required in the optimized raw or IQ ring steady-state paths. FFTW plans, filter spectra, USB frames, ring blocks, and Soapy buffers are created before or at stream setup. The remaining Soapy callback-buffer copy and `readStream()` copy are still steady-state full-buffer copies.

## Copy and bandwidth accounting

At 128 MSPS, raw ADC payload is 256 MB/s (decimal). One full logical raw copy therefore causes about 256 MB/s of reads plus 256 MB/s of writes, or roughly 512 MB/s of memory traffic.

Baseline raw path:

1. USB frame to temporary vector: 256 MB/s payload, about 512 MB/s read+write traffic.
2. Temporary vector to raw ring: 256 MB/s payload, about 512 MB/s traffic.
3. Raw ring to R2IQ vector: 256 MB/s payload, about 512 MB/s traffic.

The three baseline raw copies account for about 768 MB/s of copied payload or 1.536 GB/s of read+write traffic. The ownership API reduces this to the one required USB-frame-to-ring copy: about 256 MB/s payload or 512 MB/s traffic. Avoided raw traffic is therefore about 1.024 GB/s.

The int16-to-float pass additionally reads about 256 MB/s and writes about 512 MB/s. The fixed 2,048-sample overlap copy is about 8 MB/s payload at 128 MSPS.

CF32 payload bandwidth is `ADC rate / (2 * decimation ratio) * 8 bytes`. At full output bandwidth this is 512 MB/s. The optimized ownership path removes the R2IQ-vector-to-IQ-ring copy and the IQ-ring-to-submit-thread copy, avoiding up to 1.024 GB/s of copied payload, or about 2.048 GB/s of read+write memory traffic. The two Soapy-facing copies remain.

## FFTW and SIMD findings

- `FFTN_R_ADC` is 8,192, but the actual forward FFT is 10,240 samples because it includes 2,048 overlap samples. The useful step is 8,192 samples.
- At 128 MSPS, there are 1,953.125 USB/input blocks per second. The current formula sets nine forward/inverse transforms per block, or about 17,578 transform pairs per second. Eight windows exactly cover the 65,536-sample block. The ninth passes FFTW a 10,240-float window beginning at offset 65,536, while the allocation contains only 69,632 floats; its requested end is offset 75,776. This is an out-of-allocation read by geometry. An AddressSanitizer run did not report it because the actual read occurs inside the uninstrumented FFTW binary. The ninth transform's full-band output length is zero, but its lower-decimation output/offset accounting is non-obvious. It was not changed in the copy-elimination patch because removing it without a full response/order comparison could alter DSP output.
- Inverse FFT sizes by decimation index are 5,120, 2,560, 1,280, 640, 320, 160, and 80 complex samples.
- Only `fftw3f` is called. There are no `fftwf_init_threads`, `fftwf_plan_with_nthreads`, or FFTW thread-library references, so each transform is internally single-threaded.
- Windows downloads the official prebuilt FFTW 3.3.5 single-precision DLL. Wisdom is imported from and exported to a relative file named `wisdom`; success is not checked or logged, so reuse cannot currently be proven.
- AVX, AVX2, and AVX-512 R2IQ translation units all include the same implementation. Their difference is the per-file MSVC `/arch` option, so `convert_float`, `shift_freq`, and IQ copy rely on compiler auto-vectorization rather than explicit intrinsics.
- CPUID runtime dispatch chooses AVX2 on an N95, which does not expose AVX-512. The dispatcher does not validate OSXSAVE/XCR0 state; that is normally safe on supported Windows/N95 systems but is an audit limitation.
- Explicit AVX2 should be considered only after MSVC vectorization reports or assembly show a missed hot loop. The randomization branch in `convert_float<true>` is the most likely first candidate.

## Threading and synchronization

Current upstream defines `N_MAX_R2IQ_THREADS` as 1. Raising it is not a tuning-only change: each worker owns independent overlap state, while blocks are removed from one shared input queue. Multiple workers would not automatically preserve contiguous overlap history or output ordering. A safe multi-worker design needs sequence numbers, ordered output commit, and overlap derived from the immediately preceding input block. Until that exists, benchmark worker count only at 1.

The rings are bounded SPSC queues in the current pipeline. The optimized API gives the producer ownership from acquire through commit and the consumer ownership from acquire through release. Mutex/CV synchronization remains at block boundaries; it is deliberately not replaced with a lock-free queue. The USB callback still waits if R2IQ falls behind, so input-ring full counts remain the key overload signal.

The former `mutexR2iqControl` only wrapped the input-ring `pop()` call, was not used by control setters, and could not order parallel block processing. With the upstream one-worker limit it was redundant, so the optimized path removes that extra lock while retaining the ring's ownership synchronization.

## Windows Release build

Release keeps precise floating-point semantics and now requests MSVC `/O2`, `/Ob2`, and CMake IPO/LTCG when supported. CMake supplies `NDEBUG` for Release configurations. The AVX2 R2IQ translation unit retains `/arch:AVX2`; other dispatch variants remain available.

The GitHub Windows x64 job now publishes `SDDCSupport.dll` as its own artifact. A local MSVC build should use:

```powershell
cmake -S . -B build-x64 -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build-x64 --config Release --target SDDCSupport
```

Expected module output: `build-x64/SoapySDDC/Release/SDDCSupport.dll`.

Runtime dependencies include the existing SoapySDR 0.8 runtime used by SDR++, `libfftw3f-3.dll`, the Cypress CyUSB device driver, Windows `SetupAPI.dll`, and the matching Microsoft Visual C++ runtime. The Windows module must not depend on `libusb-1.0.dll`: Windows uses the restored native CyAPI backend so both bootloader and post-firmware modes remain on the Cypress driver. Dependency names and machine architecture must be verified on the produced DLL with `dumpbin /DEPENDENTS` and `dumpbin /HEADERS` before deployment.

## Validation status and next measurements

- Clean local Release build: passed on macOS arm64.
- Existing R2IQ and tune tests: passed.
- Deterministic non-constant int16 input produced bit-identical first-block CF32 hashes against pristine upstream for decimation indices 0 through 4 when both runs used the same FFTW wisdom. Repeated optimized runs were stable. ADC randomization, tuning offsets, and both sidebands still need equivalent coverage.
- AddressSanitizer build and R2IQ test completed without an instrumented-code finding; the FFTW window overrun described above remains because FFTW itself was not ASan-instrumented.
- Existing `BasicTest`: fails identically on pristine upstream because the test expects the old 64 MHz default while `DEFAULT_ADC_FREQ` is now 1 MHz.
- The optimized common code previously passed an MSVC x64 compile, but the restored CyAPI backend still requires a fresh Windows build, DLL dependency inspection, attached RX888 MkII firmware/streaming test, and total SDR++ CPU measurements.
- DSP equivalence still requires deterministic baseline/optimized CF32 comparison across all decimations, tuning offsets, ADC randomization states, and USB/LSB modes before this is treated as a production DLL.
