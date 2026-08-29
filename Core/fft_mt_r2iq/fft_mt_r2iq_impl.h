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
    std::array<int16_t, BASE_FFT_SCRAP_SIZE> last_block_end{};

    while(r2iqOn)
    {
        input_current_block = inputbuffer->acquireReadBlock();
        if (input_current_block == nullptr) return 0;
        if (!r2iqOn) { inputbuffer->releaseReadBlock(); return 0; }
        if (first_input_block) WarnPrintln(TAG, "STARTUP DSP: first input block acquired");

        if (iq_output == nullptr)
        {
            iq_output = outputbuffer->acquireWriteBlock();
            if (iq_output == nullptr) { inputbuffer->releaseReadBlock(); return 0; }
            if (first_input_block) WarnPrintln(TAG, "STARTUP DSP: first output block acquired");
            std::fill_n(iq_output, outputbuffer->getBlockSize(), 0.0f);
            if (first_input_block) WarnPrintln(TAG, "STARTUP DSP: first output block cleared");
        }

#if PRINT_INPUT_RANGE
        std::pair<int16_t, int16_t> blockMinMax = std::make_pair<int16_t, int16_t>(0, 0);
#endif
        if (!this->getRand())
        {
            r2iq_convert_float<false>(last_block_end.data(), th->ADCinTime, BASE_FFT_SCRAP_SIZE);
            if (first_input_block) WarnPrintln(TAG, "STARTUP DSP: first overlap converted");
#if PRINT_INPUT_RANGE
            auto minmax = std::minmax_element(input_current_block, input_current_block + inputbuffer_block_size);
            blockMinMax.first = *minmax.first;
            blockMinMax.second = *minmax.second;
#endif
            r2iq_convert_float<false>(input_current_block, th->ADCinTime + BASE_FFT_SCRAP_SIZE, inputbuffer_block_size);
            if (first_input_block) WarnPrintln(TAG, "STARTUP DSP: first input payload converted");
        }
        else
        {
            r2iq_convert_float<true>(last_block_end.data(), th->ADCinTime, BASE_FFT_SCRAP_SIZE);
            if (first_input_block) WarnPrintln(TAG, "STARTUP DSP: first randomized overlap converted");
            r2iq_convert_float<true>(input_current_block, th->ADCinTime + BASE_FFT_SCRAP_SIZE, inputbuffer_block_size);
            if (first_input_block) WarnPrintln(TAG, "STARTUP DSP: first randomized input payload converted");
        }

        std::copy_n(input_current_block + inputbuffer_block_size - BASE_FFT_SCRAP_SIZE,
                    BASE_FFT_SCRAP_SIZE, last_block_end.data());
        if (first_input_block) WarnPrintln(TAG, "STARTUP DSP: first overlap history saved");
        if (first_input_block) { WarnPrintln(TAG, "STARTUP DSP: first input block converted"); first_input_block = false; }

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
            th->MinValue = 0; th->MaxValue = 0; th->MinMaxBlockCount = 0;
        }
#endif

        const int _center_frequency_bin = this->center_frequency_bin;
        const auto upper_frequencies_source = &th->ADCinFreq[_center_frequency_bin];
        const auto upper_frequencies_len = std::min(BASE_FFT_HALF_SIZE - _center_frequency_bin, fft_output_half_size);
        const auto lower_frequencies_source = &th->ADCinFreq[_center_frequency_bin - fft_output_half_size];
        const auto lower_frequencies_start = std::max(fft_output_half_size - _center_frequency_bin, 0);

        for (int k = 0; k < ffts_per_blocks; k++)
        {
            fftwf_execute_dft_r2c(plan_time2freq_r2c,
                th->ADCinTime + k * (BASE_FFT_SIZE - BASE_FFT_SCRAP_SIZE), th->ADCinFreq);

            r2iq_shift_freq(th->inFreqTmp, upper_frequencies_source, filter, 0, upper_frequencies_len);
            if(fft_output_half_size != upper_frequencies_len)
                memset(th->inFreqTmp[upper_frequencies_len], 0,
                       (fft_output_half_size - upper_frequencies_len) * sizeof(fftwf_complex));
            r2iq_shift_freq(&th->inFreqTmp[fft_output_half_size], lower_frequencies_source,
                            filter2, lower_frequencies_start, fft_output_half_size);
            if (lower_frequencies_start != 0)
                memset(th->inFreqTmp[fft_output_half_size], 0,
                       lower_frequencies_start * sizeof(fftwf_complex));

            fftwf_execute_dft(*plan_freq2time, th->inFreqTmp, th->inFreqTmp);
            const size_t destination_offset = output_buffer_offset + static_cast<size_t>(k) * fft_useful_size;
            assert(destination_offset + fft_useful_size <= output_complex_capacity);
            if (this->getSideband())
                r2iq_copy<true>((fftwf_complex*)&iq_output[destination_offset * 2], &th->inFreqTmp[0], fft_useful_size);
            else
                r2iq_copy<false>((fftwf_complex*)&iq_output[destination_offset * 2], &th->inFreqTmp[0], fft_useful_size);
        }

        output_buffer_offset += static_cast<size_t>(ffts_per_blocks) * fft_useful_size;
        decimate_count = (decimate_count + 1) & (deci_ratio - 1);
        if (decimate_count == 0) {
            assert(output_buffer_offset == output_complex_capacity);
            outputbuffer->commitWriteBlock();
            iq_output = nullptr;
            output_buffer_offset = 0;
        }
        inputbuffer->releaseReadBlock();
    }
    return 0;
}
