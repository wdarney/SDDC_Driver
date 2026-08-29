#define TAG "fft_mt_r2iq_def"

{
    TracePrintln(TAG, "%p", th);
    DebugPrintln(TAG, "Initialization...");

    using profile_clock = std::chrono::steady_clock;
    using profile_time_point = profile_clock::time_point;
    constexpr uint64_t PROFILE_SAMPLE_PERIOD = 17;
    constexpr uint64_t PROFILE_REPORT_SAMPLES = 64;
    const char* profile_request = std::getenv("SDDC_R2IQ_PROFILE");
    const bool profile_enabled = profile_request != nullptr &&
        profile_request[0] != '\0' && std::strcmp(profile_request, "0") != 0;

    uint64_t profile_blocks_seen = 0;
    uint64_t profile_samples = 0;
    uint64_t profile_input_wait_ns = 0;
    uint64_t profile_output_wait_ns = 0;
    uint64_t profile_output_acquires = 0;
    uint64_t profile_clear_ns = 0;
    uint64_t profile_clear_events = 0;
    uint64_t profile_convert_ns = 0;
    uint64_t profile_overlap_ns = 0;
    uint64_t profile_fft_wall_ns = 0;
    uint64_t profile_worker_wait_ns = 0;
    uint64_t profile_active_ns = 0;
    std::atomic<uint64_t> profile_chunks{0};
    std::atomic<uint64_t> profile_forward_ns{0};
    std::atomic<uint64_t> profile_shift_ns{0};
    std::atomic<uint64_t> profile_inverse_ns{0};
    std::atomic<uint64_t> profile_copy_ns{0};

    if (profile_enabled)
        WarnPrintln(TAG,
            "PROFILE enabled: sampling every %llu blocks; report after %llu samples",
            static_cast<unsigned long long>(PROFILE_SAMPLE_PERIOD),
            static_cast<unsigned long long>(PROFILE_REPORT_SAMPLES));

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
    bool work_profile_sample = false;

    auto process_chunk = [&](r2iqThreadArg* worker, int k,
                             int block_center_frequency_bin,
                             float* block_iq_output,
                             size_t block_output_buffer_offset,
                             bool profile_sample) {
        profile_time_point stage_start;
        if (profile_sample)
            stage_start = profile_clock::now();
        fftwf_execute_dft_r2c(
            worker->plan_time2freq_r2c,
            th->ADCinTime + k * (BASE_FFT_SIZE - BASE_FFT_SCRAP_SIZE),
            worker->ADCinFreq);
        if (profile_sample)
        {
            const auto stage_end = profile_clock::now();
            profile_forward_ns.fetch_add(
                std::chrono::duration_cast<std::chrono::nanoseconds>(stage_end - stage_start).count(),
                std::memory_order_relaxed);
            stage_start = stage_end;
        }

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
        if (profile_sample)
        {
            const auto stage_end = profile_clock::now();
            profile_shift_ns.fetch_add(
                std::chrono::duration_cast<std::chrono::nanoseconds>(stage_end - stage_start).count(),
                std::memory_order_relaxed);
            stage_start = stage_end;
        }

        fftwf_execute_dft(
            worker->plan_freq2time_per_decimation[decimation],
            worker->inFreqTmp, worker->inFreqTmp);
        if (profile_sample)
        {
            const auto stage_end = profile_clock::now();
            profile_inverse_ns.fetch_add(
                std::chrono::duration_cast<std::chrono::nanoseconds>(stage_end - stage_start).count(),
                std::memory_order_relaxed);
            stage_start = stage_end;
        }

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
        if (profile_sample)
        {
            const auto stage_end = profile_clock::now();
            profile_copy_ns.fetch_add(
                std::chrono::duration_cast<std::chrono::nanoseconds>(stage_end - stage_start).count(),
                std::memory_order_relaxed);
            profile_chunks.fetch_add(1, std::memory_order_relaxed);
        }
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
                bool profile_sample = false;
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
                    profile_sample = work_profile_sample;
                }

                while (true)
                {
                    const int k = next_chunk.fetch_add(1);
                    if (k >= ffts_per_blocks)
                        break;
                    process_chunk(threadArgs[worker_index], k,
                        block_center_frequency_bin, block_iq_output,
                        block_output_buffer_offset, profile_sample);
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
        const bool profile_sample = profile_enabled &&
            (profile_blocks_seen++ % PROFILE_SAMPLE_PERIOD == 0);
        profile_time_point input_wait_start;
        if (profile_sample)
            input_wait_start = profile_clock::now();
        input_current_block = inputbuffer->acquireReadBlock();
        if (input_current_block == nullptr)
            break;
        profile_time_point active_start;
        if (profile_sample)
        {
            active_start = profile_clock::now();
            profile_input_wait_ns +=
                std::chrono::duration_cast<std::chrono::nanoseconds>(active_start - input_wait_start).count();
        }
        if (!r2iqOn)
        {
            inputbuffer->releaseReadBlock();
            break;
        }

        if (first_input_block)
            WarnPrintln(TAG, "STARTUP DSP: first input block acquired");

        if (iq_output == nullptr)
        {
            profile_time_point output_wait_start;
            if (profile_sample)
                output_wait_start = profile_clock::now();
            iq_output = outputbuffer->acquireWriteBlock();
            if (iq_output == nullptr)
            {
                inputbuffer->releaseReadBlock();
                break;
            }
            if (profile_sample)
            {
                const auto output_wait_end = profile_clock::now();
                profile_output_wait_ns +=
                    std::chrono::duration_cast<std::chrono::nanoseconds>(output_wait_end - output_wait_start).count();
                profile_output_acquires++;
                output_wait_start = output_wait_end;
            }
            if (first_input_block)
                WarnPrintln(TAG, "STARTUP DSP: first output block acquired");
            // The former vector<float>(size) value-initialized every output
            // block. Some high-decimation offset paths do not overwrite every
            // element, so preserve those zero-filled gaps when reusing blocks.
            std::fill_n(iq_output, outputbuffer->getBlockSize(), 0.0f);
            if (profile_sample)
            {
                const auto clear_end = profile_clock::now();
                profile_clear_ns +=
                    std::chrono::duration_cast<std::chrono::nanoseconds>(clear_end - output_wait_start).count();
                profile_clear_events++;
            }
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
        profile_time_point convert_start;
        if (profile_sample)
            convert_start = profile_clock::now();
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

        profile_time_point overlap_start;
        if (profile_sample)
        {
            overlap_start = profile_clock::now();
            profile_convert_ns +=
                std::chrono::duration_cast<std::chrono::nanoseconds>(overlap_start - convert_start).count();
        }

        std::copy_n(
            input_current_block + inputbuffer_block_size - BASE_FFT_SCRAP_SIZE,
            BASE_FFT_SCRAP_SIZE,
            last_block_end.data()
        );
        if (profile_sample)
        {
            const auto overlap_end = profile_clock::now();
            profile_overlap_ns +=
                std::chrono::duration_cast<std::chrono::nanoseconds>(overlap_end - overlap_start).count();
        }
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
        profile_time_point fft_wall_start;
        if (profile_sample)
            fft_wall_start = profile_clock::now();
        if (processor_count <= 1)
        {
            for (int k = 0; k < ffts_per_blocks; k++)
                process_chunk(threadArgs[0], k, block_center_frequency_bin,
                    iq_output, output_buffer_offset, profile_sample);
        }
        else
        {
            {
                std::lock_guard<std::mutex> lock(chunk_mutex);
                work_center_frequency_bin = block_center_frequency_bin;
                work_iq_output = iq_output;
                work_output_buffer_offset = output_buffer_offset;
                work_profile_sample = profile_sample;
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
                    iq_output, output_buffer_offset, profile_sample);
            }

            profile_time_point worker_wait_start;
            if (profile_sample)
                worker_wait_start = profile_clock::now();
            std::unique_lock<std::mutex> lock(chunk_mutex);
            chunk_done.wait(lock, [&] {
                return completed_workers >= processor_count - 1;
            });
            if (profile_sample)
                profile_worker_wait_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(
                    profile_clock::now() - worker_wait_start).count();
        }

        if (profile_sample)
            profile_fft_wall_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(
                profile_clock::now() - fft_wall_start).count();

        output_buffer_offset += static_cast<size_t>(ffts_per_blocks) * fft_useful_size;
        decimate_count = (decimate_count + 1) & (deci_ratio - 1);
        if (decimate_count == 0) {
            assert(output_buffer_offset == output_complex_capacity);
            outputbuffer->commitWriteBlock();
            iq_output = nullptr;
            output_buffer_offset = 0;
        }

        inputbuffer->releaseReadBlock();

        if (profile_sample)
        {
            profile_active_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(
                profile_clock::now() - active_start).count();
            profile_samples++;

            if (profile_samples >= PROFILE_REPORT_SAMPLES)
            {
                const auto chunks = profile_chunks.exchange(0, std::memory_order_relaxed);
                const auto forward_ns = profile_forward_ns.exchange(0, std::memory_order_relaxed);
                const auto shift_ns = profile_shift_ns.exchange(0, std::memory_order_relaxed);
                const auto inverse_ns = profile_inverse_ns.exchange(0, std::memory_order_relaxed);
                const auto copy_ns = profile_copy_ns.exchange(0, std::memory_order_relaxed);
                const auto fft_cpu_ns = forward_ns + shift_ns + inverse_ns + copy_ns;
                const auto avg_us = [](uint64_t ns, uint64_t count) {
                    return count == 0 ? 0.0 : static_cast<double>(ns) / (1000.0 * count);
                };
                const auto pct = [fft_cpu_ns](uint64_t ns) {
                    return fft_cpu_ns == 0 ? 0.0 : 100.0 * static_cast<double>(ns) / fft_cpu_ns;
                };

                WarnPrintln(TAG,
                    "PROFILE R2IQ samples=%llu/%llu chunks=%llu workers=%u decim=%d avg_us input_wait=%.1f output_wait=%.1f clear=%.1f convert=%.1f overlap=%.1f fft_wall=%.1f worker_wait=%.1f active=%.1f fft_cpu_us forward=%.1f(%.1f%%) shift=%.1f(%.1f%%) inverse=%.1f(%.1f%%) copy=%.1f(%.1f%%)",
                    static_cast<unsigned long long>(profile_samples),
                    static_cast<unsigned long long>(profile_blocks_seen),
                    static_cast<unsigned long long>(chunks),
                    processor_count, decimation,
                    avg_us(profile_input_wait_ns, profile_samples),
                    avg_us(profile_output_wait_ns, profile_output_acquires),
                    avg_us(profile_clear_ns, profile_clear_events),
                    avg_us(profile_convert_ns, profile_samples),
                    avg_us(profile_overlap_ns, profile_samples),
                    avg_us(profile_fft_wall_ns, profile_samples),
                    avg_us(profile_worker_wait_ns, profile_samples),
                    avg_us(profile_active_ns, profile_samples),
                    avg_us(forward_ns, profile_samples), pct(forward_ns),
                    avg_us(shift_ns, profile_samples), pct(shift_ns),
                    avg_us(inverse_ns, profile_samples), pct(inverse_ns),
                    avg_us(copy_ns, profile_samples), pct(copy_ns));

                profile_samples = 0;
                profile_blocks_seen = 0;
                profile_input_wait_ns = 0;
                profile_output_wait_ns = 0;
                profile_output_acquires = 0;
                profile_clear_ns = 0;
                profile_clear_events = 0;
                profile_convert_ns = 0;
                profile_overlap_ns = 0;
                profile_fft_wall_ns = 0;
                profile_worker_wait_ns = 0;
                profile_active_ns = 0;
            }
        }
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
