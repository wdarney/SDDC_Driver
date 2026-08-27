#include <SoapySDR/Device.hpp>
#include <SoapySDR/Errors.hpp>
#include <SoapySDR/Formats.hpp>
#include <SoapySDR/Types.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct Options {
    double duration_seconds = 10.0;
    double warmup_seconds = 2.0;
    double sample_rate = 64'000'000.0;
    double frequency = 10'000'000.0;
    double rf_gain = std::numeric_limits<double>::quiet_NaN();
    double if_gain = std::numeric_limits<double>::quiet_NaN();
    long timeout_us = 500'000;
    std::size_t device_index = 0;
    std::size_t read_size = 0;
    std::string antenna;
    bool adc_randomization = false;
    bool list_only = false;
};

void usage(const char* program)
{
    std::cout
        << "Usage: " << program << " [options]\n"
        << "  --list                    List SDDC devices without streaming\n"
        << "  --device INDEX            Enumerated SDDC device index (default 0)\n"
        << "  --duration SECONDS        Measured streaming duration (default 10)\n"
        << "  --warmup SECONDS          Unmeasured warmup duration (default 2)\n"
        << "  --sample-rate HZ          Requested CF32 rate (default 64000000)\n"
        << "  --frequency HZ            Center frequency (default 10000000)\n"
        << "  --antenna HF|VHF          Optional explicit antenna selection\n"
        << "  --rf-gain DB              Optional RF gain\n"
        << "  --if-gain DB              Optional IF gain\n"
        << "  --rand                    Enable ADC digital randomization\n"
        << "  --read-size ELEMENTS      readStream size (default stream MTU)\n"
        << "  --timeout-us MICROSECONDS readStream timeout (default 500000)\n";
}

std::string require_value(int argc, char** argv, int& index)
{
    if (++index >= argc) throw std::runtime_error("missing value after " + std::string(argv[index - 1]));
    return argv[index];
}

Options parse_options(int argc, char** argv)
{
    Options options;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg(argv[i]);
        if (arg == "--help" || arg == "-h")
        {
            usage(argv[0]);
            std::exit(0);
        }
        if (arg == "--list") options.list_only = true;
        else if (arg == "--rand") options.adc_randomization = true;
        else if (arg == "--device") options.device_index = std::stoull(require_value(argc, argv, i));
        else if (arg == "--duration") options.duration_seconds = std::stod(require_value(argc, argv, i));
        else if (arg == "--warmup") options.warmup_seconds = std::stod(require_value(argc, argv, i));
        else if (arg == "--sample-rate") options.sample_rate = std::stod(require_value(argc, argv, i));
        else if (arg == "--frequency") options.frequency = std::stod(require_value(argc, argv, i));
        else if (arg == "--antenna") options.antenna = require_value(argc, argv, i);
        else if (arg == "--rf-gain") options.rf_gain = std::stod(require_value(argc, argv, i));
        else if (arg == "--if-gain") options.if_gain = std::stod(require_value(argc, argv, i));
        else if (arg == "--read-size") options.read_size = std::stoull(require_value(argc, argv, i));
        else if (arg == "--timeout-us") options.timeout_us = std::stol(require_value(argc, argv, i));
        else throw std::runtime_error("unknown argument: " + arg);
    }
    if (options.duration_seconds <= 0.0) throw std::runtime_error("duration must be positive");
    if (options.warmup_seconds < 0.0) throw std::runtime_error("warmup cannot be negative");
    return options;
}

class DeviceOwner {
public:
    explicit DeviceOwner(SoapySDR::Device* device): device_(device) {}
    ~DeviceOwner() { if (device_ != nullptr) SoapySDR::Device::unmake(device_); }
    SoapySDR::Device* get() const { return device_; }

private:
    SoapySDR::Device* device_;
};

} // namespace

int main(int argc, char** argv)
{
    try
    {
        const Options options = parse_options(argc, argv);
        const SoapySDR::KwargsList devices = SoapySDR::Device::enumerate("driver=SDDC");

        std::cout << "SDDC devices: " << devices.size() << "\n";
        for (std::size_t i = 0; i < devices.size(); ++i)
            std::cout << "  [" << i << "] " << SoapySDR::KwargsToString(devices[i]) << "\n";

        if (options.list_only) return 0;
        if (devices.empty())
        {
            std::cerr << "No SDDC device found. Verify the RX888 USB connection and SOAPY_SDR_PLUGIN_PATH.\n";
            return 2;
        }
        if (options.device_index >= devices.size()) throw std::runtime_error("device index is out of range");

        DeviceOwner owner(SoapySDR::Device::make(devices[options.device_index]));
        SoapySDR::Device* device = owner.get();
        if (device == nullptr) throw std::runtime_error("SoapySDR failed to create the selected device");

        if (!options.antenna.empty()) device->setAntenna(SOAPY_SDR_RX, 0, options.antenna);
        device->setSampleRate(SOAPY_SDR_RX, 0, options.sample_rate);
        device->setFrequency(SOAPY_SDR_RX, 0, options.frequency);
        if (!std::isnan(options.rf_gain)) device->setGain(SOAPY_SDR_RX, 0, "RF", options.rf_gain);
        if (!std::isnan(options.if_gain)) device->setGain(SOAPY_SDR_RX, 0, "IF", options.if_gain);
        device->writeSetting("SetRand", options.adc_randomization ? "true" : "false");

        SoapySDR::Stream* stream = device->setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32, {0});
        if (stream == nullptr) throw std::runtime_error("setupStream returned null");

        const std::size_t mtu = device->getStreamMTU(stream);
        const std::size_t read_size = options.read_size == 0 ? mtu : options.read_size;
        std::vector<std::complex<float>> samples(read_size);
        void* buffers[] = {samples.data()};

        if (device->activateStream(stream) != 0)
        {
            device->closeStream(stream);
            throw std::runtime_error("activateStream failed");
        }

        const auto stream_start = std::chrono::steady_clock::now();
        const auto measure_start = stream_start + std::chrono::duration<double>(options.warmup_seconds);
        const auto measure_end = measure_start + std::chrono::duration<double>(options.duration_seconds);
        bool measuring = false;
        std::clock_t cpu_start = 0;
        std::uint64_t total_samples = 0;
        std::uint64_t read_calls = 0;
        std::uint64_t timeouts = 0;
        std::uint64_t overflows = 0;
        std::uint64_t other_errors = 0;
        std::size_t min_batch = std::numeric_limits<std::size_t>::max();
        std::size_t max_batch = 0;

        while (std::chrono::steady_clock::now() < measure_end)
        {
            const auto now = std::chrono::steady_clock::now();
            if (!measuring && now >= measure_start)
            {
                measuring = true;
                cpu_start = std::clock();
            }

            int flags = 0;
            long long time_ns = 0;
            const int result = device->readStream(stream, buffers, read_size, flags, time_ns, options.timeout_us);
            if (!measuring) continue;
            ++read_calls;
            if (result > 0)
            {
                const std::size_t count = static_cast<std::size_t>(result);
                total_samples += count;
                min_batch = std::min(min_batch, count);
                max_batch = std::max(max_batch, count);
            }
            else if (result == SOAPY_SDR_TIMEOUT) ++timeouts;
            else if (result == SOAPY_SDR_OVERFLOW) ++overflows;
            else ++other_errors;
        }

        const std::clock_t cpu_end = std::clock();
        const auto wall_end = std::chrono::steady_clock::now();
        device->deactivateStream(stream);
        device->closeStream(stream);

        const double wall_seconds = std::chrono::duration<double>(wall_end - measure_start).count();
        const double cpu_seconds = static_cast<double>(cpu_end - cpu_start) / CLOCKS_PER_SEC;
        const double samples_per_second = total_samples / wall_seconds;
        const double realtime_ratio = samples_per_second / device->getSampleRate(SOAPY_SDR_RX, 0);

        std::cout << std::fixed << std::setprecision(3)
                  << "{\n"
                  << "  \"driver\": \"SDDC\",\n"
                  << "  \"hardware\": \"" << device->getHardwareKey() << "\",\n"
                  << "  \"requested_sample_rate\": " << options.sample_rate << ",\n"
                  << "  \"actual_sample_rate\": " << device->getSampleRate(SOAPY_SDR_RX, 0) << ",\n"
                  << "  \"frequency\": " << device->getFrequency(SOAPY_SDR_RX, 0) << ",\n"
                  << "  \"stream_mtu\": " << mtu << ",\n"
                  << "  \"read_size\": " << read_size << ",\n"
                  << "  \"wall_seconds\": " << wall_seconds << ",\n"
                  << "  \"process_cpu_seconds\": " << cpu_seconds << ",\n"
                  << "  \"process_cpu_percent_of_one_core\": " << (100.0 * cpu_seconds / wall_seconds) << ",\n"
                  << "  \"samples\": " << total_samples << ",\n"
                  << "  \"samples_per_second\": " << samples_per_second << ",\n"
                  << "  \"realtime_ratio\": " << realtime_ratio << ",\n"
                  << "  \"read_calls\": " << read_calls << ",\n"
                  << "  \"timeouts\": " << timeouts << ",\n"
                  << "  \"overflows\": " << overflows << ",\n"
                  << "  \"other_errors\": " << other_errors << ",\n"
                  << "  \"min_batch\": " << (total_samples == 0 ? 0 : min_batch) << ",\n"
                  << "  \"max_batch\": " << max_batch << "\n"
                  << "}\n";

        return other_errors == 0 ? 0 : 3;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Benchmark failed: " << error.what() << "\n";
        return 1;
    }
}
