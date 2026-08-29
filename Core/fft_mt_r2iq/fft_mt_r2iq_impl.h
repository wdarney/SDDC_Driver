#define TAG "fft_mt_r2iq_def"

{
    TracePrintln(TAG, "%p", th);
    DebugPrintln(TAG, "Initialization...");

    const int deci_ratio = decimation_ratio[decimation];

    const int deci_fft_scrap_size = (BASE_FFT_SCRAP_SIZE / 2) / deci_ratio;
    const int fft_output_size = this->fft_size_per_decimation[decimation];
    const int fft_output_half_size = fft_output_size / 2;
    const int fft_useful_size = fft_output_size - deci_fft_scrap_size;

    DebugPrintln(TAG, "Decimation : %d (index %d)", deci_ratio, decimation);
    DebugPrintln(TAG, "Scrap size : %d", deci_fft_scrap_size);
    DebugPrintln(TAG, "FFT output size : %d", fft_output_size);
    DebugPrintln(TAG, "FFT useful output size : %d", fft_useful_size);
    DebugPrintln(TAG, "Initialization done");

    const fftwf_complex* filter = filterHw[decimation];
    const auto filter2 = &filter[BASE_FFT_HALF_SIZE - fft_output_half_size];

    plan_freq2time = &plan_freq2time_per_decimation[decimation];
    int decimate_count = 0;

    float* iq_output = nullptr;
    size_t output_buffer_offset = 0;
    [[maybe_unused]] const size_t output_complex_capacity = outputbuffer->getBlockSize() / 2;

    const int16_t* input_current_block = nullptr;
    bool first_input_block = true;
    // Fixed overlap storage avoids constructing a vector on every input block.
    std::array<int16_t, BASE_FFT_SCRAP_SIZE> last_block_end{};

    // Diagnostic-only, once-per-second counters. These make a blank waterfall
    // distinguishable as USB starvation, R2IQ backlog, or downstream backlog
    // without changing the processing or ownership path.
    auto telemetry_start = std::chrono::steady_clock::now();
    uint64_t telemetry_input_blocks = 0;
    uint64_t telemetry_output_blocks = 0;
    int telemetry_input_full = inputbuffer->getFullCount();
    int telemetry_output_full = outputbuffer->getFullCount();

    while(r2iqOn)
    {
        input_current_block = inputbuffer->acquireReadBlock();
        if (input_current_block == nullptr)
            return 0;
        if (!r2iqOn)
        {
            inputbuffer->releaseReadBlock();
            return 0;
        }

        if (first_input_block)
            WarnPrintln(TAG, "STARTUP DSP: first input block acquired");

        if (iq_output == nullptr)
        {
            iq_output = outputbuffer->acquireWriteBlock();
            if (iq_output == nullptr)
            {
                inputbuffer->releaseReadBlock();
                return 0;
            }
            if (first_input_block)
                WarnPrintln(TAG, "STARTUP DSP: first output block acquired");
            // The former vector<float>(size) value-initialized every output
            // block. Some high-decimation offset paths do not overwrite every
            // element, so preserve those zero-filled gaps when reusing blocks.
            std::fill_n(iq_output, outputbuffer->getBlockSize(), 0.0f);
            if (first_input_block)
                WarnPrintln(TAG, "STARTUP DSP: first output block cleared");
        }

        // @todo: move the following int16_t conversion to (32-bit) float
        // directly inside the following loop (for "k < ffts_per_blocks")
        //   just before the forward fft "fftwf_execute_dft_r2c" is called
        // idea: this should improve cache/memory locality
#if PRINT_INPUT_RANGE
        std::pair<int16_t, int16_t> blockMinMax = std::make_pair<int16_t, int16_t>(0, 0);
#endif
        if (!this->getRand())        // plain samples no ADC rand set
        {
            r2iq_convert_float<false>(
                /*source=*/last_block_end.data(),
                /*dest=*/th->ADCinTime,
                /*len=*/BASE_FFT_SCRAP_SIZE
            );
            if (first_input_block)
                WarnPrintln(TAG, "STARTUP DSP: first overlap converted");
#if PRINT_INPUT_RANGE
            auto minmax = std::minmax_element(input_current_block, input_current_block + inputbuffer_block_size);
            blockMinMax.first = *minmax.first;
            blockMinMax.second = *minmax.second;
#endif
            r2iq_convert_float<false>(
                /*source=*/input_current_block,
                /*dest=*/th->ADCinTime + BASE_FFT_SCRAP_SIZE,
                /*len=*/inputbuffer_block_size
            );
            if (first_input_block)
                WarnPrintln(TAG, "STARTUP DSP: first input payload converted");
        }
        else
        {
            r2iq_convert_float<true>(
                /*source=*/last_block_end.data(),
                /*dest=*/th->ADCinTime,
                /*len=*/BASE_FFT_SCRAP_SIZE
            );
            if (first_input_block)
                WarnPrintln(TAG, "STARTUP DSP: first randomized overlap converted");
            r2iq_convert_float<true>(
                /*source=*/input_current_block,
                /*dest=*/th->ADCinTime + BASE_FFT_SCRAP_SIZE,
                /*len=*/inputbuffer_block_size
            );
            if (first_input_block)
                WarnPrintln(TAG, "STARTUP DSP: first randomized input payload converted");
        }

        std::copy_n(
            input_current_block + inputbuffer_block_size - BASE_FFT_SCRAP_SIZE,
            BASE_FFT_SCRAP_SIZE,
            last_block_end.data()
        );
        if (first_input_block)
            WarnPrintln(TAG, "STARTUP DSP: first overlap history saved");
        if (first_input_block) {
            WarnPrintln(TAG, "STARTUP DSP: first input block converted");
            first_input_block = false;
        }

#if PRINT_INPUT_RANGE
        th->MinValue = std::min(blockMinMax.first, th->MinValue);
        th->MaxValue = std::max(blockMinMax.second, th->MaxValue);
        ++th->MinMaxBlockCount;
        if (th->MinMaxBlockCount * processor_count / 3 >= DEFAULT_TRANSFERS_PER_SEC )
        {
            float minBits = (th->MinValue < 0) ? (log10f((float)(-th->MinValue)) / log10f(2.0f)) : -1.0f;
            float maxBits = (th->MaxValue > 0) ? (log10f((float)(th->MaxValue)) / log10f(2.0f)) : -1.0f;
            printf("r2iq: min = %d (%.1f bits) %.2f%%, max = %d (%.1f bits) %.2f%%\n",
                (int)th->MinValue, minBits, th->MinValue *-100.0f / 32768.0f,
                (int)th->MaxValue, maxBits, th->MaxValue * 100.0f / 32768.0f);
            th->MinValue = 0;
            th->MaxValue = 0;
            th->MinMaxBlockCount = 0;
        }
#endif
        
        // decimate in frequency plus tuning

        const int _center_frequency_bin = this->center_frequency_bin;

        // Calculate the parameters for the first half
        // Includes all frequencies above _center_frequency_bin
        const auto upper_frequencies_source = &th->ADCinFreq[_center_frequency_bin];
        const auto upper_frequencies_len = std::min(
            BASE_FFT_HALF_SIZE - _center_frequency_bin, // Desired value
            fft_output_half_size // Overflow protection
        );

        // Calculate the parameters for the second half
        // Includes all frequencies below _center_frequency_bin
        const auto lower_frequencies_source = &th->ADCinFreq[_center_frequency_bin - fft_output_half_size];
        const auto lower_frequencies_start = std::max(
            fft_output_half_size - _center_frequency_bin,
            0
        );
        
        // Main processing loop based on overlap-save method
        // It also includes filtering and decimation
        for (int k = 0; k < ffts_per_blocks; k++)
        {
            // core of fast convolution including filter and decimation
            //   main part is 'overlap-scrap' (IMHO better name for 'overlap-save'), see
            //   https://en.wikipedia.org/wiki/Overlap%E2%80%93save_method
            {
                // FFT first stage: time to frequency, real to complex
                // Input buffer: th->ADCinTime + k * (0.75 * BASE_FFT_SIZE)
                // Transformation size: BASE_FFT_SIZE
                // Output buffer: th->ADCinFreq[]
                // Output size: BASE_FFT_HALF_SIZE + 1
                fftwf_execute_dft_r2c(plan_time2freq_r2c, th->ADCinTime + k * (BASE_FFT_SIZE - BASE_FFT_SCRAP_SIZE), th->ADCinFreq);

                // circular shift (mixing in full bins) and low/bandpass filtering (complex multiplication)
                {
                    // circular shift tune fs/2 first half array into th->inFreqTmp[]
                    r2iq_shift_freq(
                        /*destination=*/th->inFreqTmp,
                        /*source1=*/upper_frequencies_source,
                        /*source2=*/filter,
                        /*start=*/0,
                        /*end=*/upper_frequencies_len
                    );

                    // Pad with zeroes if needed
                    if(fft_output_half_size != upper_frequencies_len)
                        memset(th->inFreqTmp[upper_frequencies_len], 0, (fft_output_half_size - upper_frequencies_len) * sizeof(fftwf_complex));

                    // circular shift tune fs/2 second half array
                    r2iq_shift_freq(
                        /*destination=*/&th->inFreqTmp[fft_output_half_size],
                        /*source1=*/lower_frequencies_source,
                        /*source2=*/filter2,
                        /*start=*/lower_frequencies_start,
                        /*end=*/fft_output_half_size
                    );

                    if (lower_frequencies_start != 0)
                        memset(th->inFreqTmp[fft_output_half_size], 0, lower_frequencies_start * sizeof(fftwf_complex));
                }
                // result now in th->inFreqTmp[]
                // Size: fft_output_size (depending on the decimation)

                // 'shorter' inverse FFT transform (decimation) -> frequency (back) to COMPLEX time domain
                // transform size: fft_output_size (depending on the decimation)
                fftwf_execute_dft(*plan_freq2time, th->inFreqTmp, th->inFreqTmp);
                // result now in th->inFreqTmp[]
            }

            // postprocessing
            // @todo: is it possible to ..
            //  1)
            //    let inverse FFT produce/save it's result directly
            //    in "this->obuffers[modx] + offset" (pout)
            //    ( obuffers[] would need to have additional space ..;
            //      need to move 'scrap' of 'ovelap-scrap'? )
            //    at least FFTW would allow so,
            //      see http://www.fftw.org/fftw3_doc/New_002darray-Execute-Functions.html
            //    attention: multithreading!
            //  2)
            //    could mirroring (lower sideband) get calculated together
            //    with fine mixer - modifying the mixer frequency? (fs - fc)/fs
            //    (this would reduce one memory pass)

            const size_t destination_offset = output_buffer_offset +
                static_cast<size_t>(k) * fft_useful_size;
            assert(destination_offset + fft_useful_size <= output_complex_capacity);

            if (this->getSideband()) // lower sideband
            {
                // mirror just by negating the imaginary Q of complex I/Q
                r2iq_copy<true>((fftwf_complex*)&iq_output[destination_offset * 2],
                    &th->inFreqTmp[0], fft_useful_size);
            }
            else // upper sideband
            {
                r2iq_copy<false>((fftwf_complex*)&iq_output[destination_offset * 2],
                    &th->inFreqTmp[0], fft_useful_size);
            }
        }

        output_buffer_offset += static_cast<size_t>(ffts_per_blocks) * fft_useful_size;
        decimate_count = (decimate_count + 1) & (deci_ratio - 1);
        if (decimate_count == 0) {
            assert(output_buffer_offset == output_complex_capacity);
            outputbuffer->commitWriteBlock();
            ++telemetry_output_blocks;
            iq_output = nullptr;
            output_buffer_offset = 0;
        }

        inputbuffer->releaseReadBlock();
        ++telemetry_input_blocks;

        const auto telemetry_now = std::chrono::steady_clock::now();
        const std::chrono::duration<double> telemetry_elapsed = telemetry_now - telemetry_start;
        if (telemetry_elapsed.count() >= 1.0)
        {
            const int current_input_full = inputbuffer->getFullCount();
            const int current_output_full = outputbuffer->getFullCount();
            const double raw_msps = telemetry_input_blocks * inputbuffer_block_size /
                telemetry_elapsed.count() / 1000000.0;
            const double iq_msps = telemetry_output_blocks * output_complex_capacity /
                telemetry_elapsed.count() / 1000000.0;
            WarnPrintln(TAG,
                "R2IQ RATE: raw=%.2f MS/s IQ=%.2f MS/s blocks=%llu/%llu inFull=+%d outFull=+%d",
                raw_msps, iq_msps,
                static_cast<unsigned long long>(telemetry_input_blocks),
                static_cast<unsigned long long>(telemetry_output_blocks),
                current_input_full - telemetry_input_full,
                current_output_full - telemetry_output_full);
            telemetry_start = telemetry_now;
            telemetry_input_blocks = 0;
            telemetry_output_blocks = 0;
            telemetry_input_full = current_input_full;
            telemetry_output_full = current_output_full;
        }
    }
    return 0;
}
