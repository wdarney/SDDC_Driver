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

    int decimate_count = 0;

    float* iq_output = nullptr;
    size_t output_buffer_offset = 0;
    [[maybe_unused]] const size_t output_complex_capacity = outputbuffer->getBlockSize() / 2;

    const int16_t* input_current_block = nullptr;
    bool first_input_block = true;
    // Fixed overlap storage avoids constructing a vector on every input block.
    std::array<int16_t, BASE_FFT_SCRAP_SIZE> last_block_end{};

    // Preserve overlap and ring ordering in this coordinator. Only the FFT
    // chunks within one converted input block are handed to helper workers.
    std::mutex chunk_mutex;
    std::condition_variable chunk_ready;
    std::condition_variable chunk_done;
    std::atomic<int> next_chunk{0};
    uint64_t chunk_sequence = 0;
    uint32_t completed_workers = 0;
    bool stop_workers = false;
    int work_center_frequency_bin = 0;
    float* work_iq_output = nullptr;
    size_t work_output_buffer_offset = 0;

    auto process_chunk = [&](r2iqThreadArg* worker, int k,
                             int block_center_frequency_bin,
                             float* block_iq_output,
                             size_t block_output_buffer_offset) {
        fftwf_execute_dft_r2c(
            worker->plan_time2freq_r2c,
            th->ADCinTime + k * (BASE_FFT_SIZE - BASE_FFT_SCRAP_SIZE),
            worker->ADCinFreq);

        const auto upper_frequencies_source =
            &worker->ADCinFreq[block_center_frequency_bin];
        const auto upper_frequencies_len = std::min(
            BASE_FFT_HALF_SIZE - block_center_frequency_bin,
            fft_output_half_size);
        const auto lower_frequencies_source =
            &worker->ADCinFreq[block_center_frequency_bin - fft_output_half_size];
        const auto lower_frequencies_start = std::max(
            fft_output_half_size - block_center_frequency_bin, 0);

        r2iq_shift_freq(worker->inFreqTmp, upper_frequencies_source, filter,
            0, upper_frequencies_len);
        if (fft_output_half_size != upper_frequencies_len)
            memset(worker->inFreqTmp[upper_frequencies_len], 0,
                (fft_output_half_size - upper_frequencies_len) * sizeof(fftwf_complex));

        r2iq_shift_freq(&worker->inFreqTmp[fft_output_half_size],
            lower_frequencies_source, filter2,
            lower_frequencies_start, fft_output_half_size);
        if (lower_frequencies_start != 0)
            memset(worker->inFreqTmp[fft_output_half_size], 0,
                lower_frequencies_start * sizeof(fftwf_complex));

        fftwf_execute_dft(
            worker->plan_freq2time_per_decimation[decimation],
            worker->inFreqTmp, worker->inFreqTmp);

        const size_t destination_offset = block_output_buffer_offset +
            static_cast<size_t>(k) * fft_useful_size;
        assert(destination_offset + fft_useful_size <= output_complex_capacity);
        if (this->getSideband())
            r2iq_copy<true>(
                reinterpret_cast<fftwf_complex*>(&block_iq_output[destination_offset * 2]),
                &worker->inFreqTmp[0], fft_useful_size);
        else
            r2iq_copy<false>(
                reinterpret_cast<fftwf_complex*>(&block_iq_output[destination_offset * 2]),
                &worker->inFreqTmp[0], fft_useful_size);
    };

    std::vector<std::thread> chunk_workers;
    for (uint32_t worker_index = 1; worker_index < processor_count; worker_index++)
    {
        chunk_workers.emplace_back([&, worker_index] {
            uint64_t seen_sequence = 0;
            while (true)
            {
                int block_center_frequency_bin = 0;
                float* block_iq_output = nullptr;
                size_t block_output_buffer_offset = 0;
                {
                    std::unique_lock<std::mutex> lock(chunk_mutex);
                    chunk_ready.wait(lock, [&] {
                        return stop_workers || chunk_sequence != seen_sequence;
                    });
                    if (stop_workers)
                        return;
                    seen_sequence = chunk_sequence;
                    block_center_frequency_bin = work_center_frequency_bin;
                    block_iq_output = work_iq_output;
                    block_output_buffer_offset = work_output_buffer_offset;
                }

                while (true)
                {
                    const int k = next_chunk.fetch_add(1);
                    if (k >= ffts_per_blocks)
                        break;
                    process_chunk(threadArgs[worker_index], k,
                        block_center_frequency_bin, block_iq_output,
                        block_output_buffer_offset);
                }

                {
                    std::lock_guard<std::mutex> lock(chunk_mutex);
                    completed_workers++;
                }
                chunk_done.notify_one();
            }
        });
    }

    while(r2iqOn)
    {
        input_current_block = inputbuffer->acquireReadBlock();
        if (input_current_block == nullptr)
            break;
        if (!r2iqOn)
        {
            inputbuffer->releaseReadBlock();
            break;
        }

        if (first_input_block)
            WarnPrintln(TAG, "STARTUP DSP: first input block acquired");

        if (iq_output == nullptr)
        {
            iq_output = outputbuffer->acquireWriteBlock();
            if (iq_output == nullptr)
            {
                inputbuffer->releaseReadBlock();
                break;
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
        
        const int block_center_frequency_bin = this->center_frequency_bin;
        if (processor_count <= 1)
        {
            for (int k = 0; k < ffts_per_blocks; k++)
                process_chunk(threadArgs[0], k, block_center_frequency_bin,
                    iq_output, output_buffer_offset);
        }
        else
        {
            {
                std::lock_guard<std::mutex> lock(chunk_mutex);
                work_center_frequency_bin = block_center_frequency_bin;
                work_iq_output = iq_output;
                work_output_buffer_offset = output_buffer_offset;
                next_chunk = 0;
                completed_workers = 0;
                chunk_sequence++;
            }
            chunk_ready.notify_all();

            while (true)
            {
                const int k = next_chunk.fetch_add(1);
                if (k >= ffts_per_blocks)
                    break;
                process_chunk(threadArgs[0], k, block_center_frequency_bin,
                    iq_output, output_buffer_offset);
            }

            std::unique_lock<std::mutex> lock(chunk_mutex);
            chunk_done.wait(lock, [&] {
                return completed_workers >= processor_count - 1;
            });
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

    {
        std::lock_guard<std::mutex> lock(chunk_mutex);
        stop_workers = true;
        chunk_sequence++;
    }
    chunk_ready.notify_all();
    for (auto& worker : chunk_workers)
        if (worker.joinable())
            worker.join();
    return 0;
}
