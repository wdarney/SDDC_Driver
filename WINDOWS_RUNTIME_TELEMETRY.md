# Windows RX888 runtime telemetry

This diagnostic build retains the two-worker AVX2/FFTW processing path and
adds environment-gated, once-per-second pipeline counters. Telemetry is off by
default. Enable it before starting SDR++:

```powershell
$env:SDDC_RUNTIME_TELEMETRY = '1'
Remove-Item Env:SDDC_R2IQ_PROFILE -ErrorAction SilentlyContinue
.\sdrpp.exe 2>&1 | Tee-Object .\rx888-runtime-telemetry.log
```

The clock is sampled once every 16 blocks to keep the diagnostic overhead low.

- `TELEM USB`: raw USB sample rate, input-ring full waits, and USB errors.
- `TELEM R2IQ`: raw and IQ rates, active worker count, and input/output ring
  full or empty waits.
- `TELEM SOAPY`: IQ entering and leaving the Soapy queue, current queue depth,
  overflows, and read timeouts.

Interpret consecutive warmed-up reports rather than the first or last report:

- Rising `inFull` means USB is filling the input ring faster than R2IQ can
  process it, normally an R2IQ/CPU bottleneck.
- Rising `outFull` or `SOAPY overflow` means downstream SDR++ consumption is
  not keeping up.
- Rising `inEmpty` with a low USB rate or `usbErr` means R2IQ is being starved
  by the USB side.
- Rising `outEmpty` or `SOAPY timeout` means the consumer requested data before
  R2IQ supplied it.
- If rates remain on target and the full/overflow/error counters remain zero,
  the RX888 driver path is healthy. Compare a local SDR++ recording with the
  remote audio: a clean recording with remote-only crackle points to the remote
  session; a crackling recording with healthy driver telemetry points farther
  downstream in SDR++ audio/VFO processing.

Unset telemetry for the normal low-overhead run:

```powershell
Remove-Item Env:SDDC_RUNTIME_TELEMETRY -ErrorAction SilentlyContinue
```
