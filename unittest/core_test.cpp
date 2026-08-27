#include "FX3Class.h"
#include "CppUnitTestFramework.hpp"
#include <thread>
#include <chrono>
#include <cmath>
#include <complex>
#include <mutex>
#include <vector>
#include <inttypes.h>  // For portable 64-bit type printf codes

#include "RadioHandler.h"

using namespace std::chrono;

class fx3handler2 : public fx3class
{
    ~fx3handler2() {}

    bool Open(SDDC::DeviceItem)
    {
        return true;
    }

    bool Control(FX3Command, uint8_t)
    {
        return true;
    }

    bool Control(FX3Command, uint32_t)
    {
        return true;
    }

    bool Control(FX3Command, uint64_t)
    {
        return true;
    }

    bool SetArgument(uint16_t, uint16_t)
    {
        return true;
    }

    bool GetHardwareInfo(uint32_t* data) {
        const uint8_t d[4] = {
            hardwareModel, FIRMWARE_VER_MAJOR, FIRMWARE_VER_MINOR, 0
        };

        memcpy(data, d, 4);

        return true;
    }

    bool Enumerate(unsigned char&, char*)
    {
        return true;
    }

    bool ReadDebugTrace(uint8_t*, uint8_t) {
        return true;
    }

    std::thread emuthread;
    bool run;
	long nxfers;
    uint8_t hardwareModel = NORADIO;
    bool generateTone = false;
    uint32_t toneSampleRate = 64000000;
    void StartStream(ringbuffer<int16_t>& input)
    {
        input.setBlockSize(transferSamples);
        run = true;
        emuthread = std::thread([&input, this]{
            uint32_t block_index = 0;
            uint64_t sample_index = 0;
            while(run)
            {
                vector<int16_t> put(transferSamples);
                if (generateTone)
                {
                    const double adc_rate = static_cast<double>(toneSampleRate);
                    // Deliberately avoid an integer number of cycles per USB
                    // block so discontinuities and phase resets cannot hide.
                    constexpr double tone_frequency = 4820123.0;
                    constexpr double amplitude = 20000.0;
                    constexpr double two_pi = 6.28318530717958647692;
                    for (uint32_t i = 0; i < transferSamples; ++i)
                    {
                        const double phase = two_pi * tone_frequency *
                            static_cast<double>(sample_index + i) / adc_rate;
                        put[i] = static_cast<int16_t>(std::lround(amplitude * std::cos(phase)));
                    }
                    sample_index += transferSamples;
                }
                else
                {
                    for (uint32_t i = 0; i < transferSamples; ++i)
                        put[i] = static_cast<int16_t>(i * 257u + block_index * 31u);
                }
                input.push(put);
                ++block_index;
                ++nxfers;
                std::this_thread::sleep_for(1ms);
            }
        });
    }

	void StopStream() {
        run = false;
        emuthread.join();
    }

    size_t GetDeviceListLength()
    {
        return 0;
    }

    vector<SDDC::DeviceItem> GetDeviceList()
    {
        return vector<SDDC::DeviceItem>();
    }

    
public:
	long Xfers(bool clear) { long rv=nxfers; if (clear) nxfers=0; return rv; }
    void SetHardwareModel(uint8_t model) { hardwareModel = model; }
    void SetGenerateTone(bool enabled) { generateTone = enabled; }
    void SetToneSampleRate(uint32_t rate) { toneSampleRate = rate; }


};

class testRadioHandler: public RadioHandler
{
public:
    testRadioHandler()
    {
        RadioHandler();
        testFx3 = new fx3handler2();
        fx3 = testFx3;
    }

    void SetHardwareModel(uint8_t model) { testFx3->SetHardwareModel(model); }
    void SetGenerateTone(bool enabled) { testFx3->SetGenerateTone(enabled); }
    void SetToneSampleRate(uint32_t rate) { testFx3->SetToneSampleRate(rate); }

    static vector<SDDC::DeviceItem> GetDeviceList()
    {
        vector<SDDC::DeviceItem> devs;
        devs.push_back(SDDC::DeviceItem{
            .index = 0,
            .product = "Blank",
            .serial_number = "Blank"
        });
        return devs;
    }

private:
    fx3handler2* testFx3;
};

static uint32_t frame_count;
static uint64_t totalsize;
static uint64_t first_block_hash;
static std::mutex tone_capture_mutex;
static std::vector<std::complex<float>> tone_capture;
static uint32_t tone_blocks_seen;

static void Callback(void*, const sddc_complex_t* data, uint32_t len)
{
    if (frame_count == 0)
    {
        first_block_hash = 1469598103934665603ULL;
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(data);
        for (size_t i = 0; i < len * sizeof(sddc_complex_t); ++i)
        {
            first_block_hash ^= bytes[i];
            first_block_hash *= 1099511628211ULL;
        }
    }
    frame_count++;
    totalsize += len;
}

static void ToneCallback(void*, const sddc_complex_t* data, uint32_t len)
{
    std::lock_guard<std::mutex> lock(tone_capture_mutex);
    ++tone_blocks_seen;
    if (tone_blocks_seen <= 3 || tone_capture.size() >= 3 * len) return;

    tone_capture.reserve(3 * len);
    for (uint32_t i = 0; i < len; ++i)
        tone_capture.emplace_back(data[i][0], data[i][1]);
}

namespace {
    struct CoreFixture {};
}

TEST_CASE(CoreFixture, OpenTest)
{
    auto radio = new testRadioHandler();

    radio = new testRadioHandler();
    delete radio;
}

TEST_CASE(CoreFixture, BasicTest)
{
    vector<SDDC::DeviceItem> devices = testRadioHandler::GetDeviceList();
    auto radio = new testRadioHandler();
    radio->Init(devices[0]);

    radio->AttachIQ(Callback);

    REQUIRE_EQUAL(radio->getHardwareModel(), NORADIO);
    REQUIRE_EQUAL(radio->getHardwareName(), "Dummy");

    REQUIRE_EQUAL(radio->GetADCSampleRate(), DEFAULT_ADC_FREQ);
    radio->SetADCSampleRate(32000000);
    REQUIRE_EQUAL(radio->GetADCSampleRate(), 32000000u);

    // Test values out of bounds
    radio->SetADCSampleRate(0);
    REQUIRE_EQUAL(radio->GetADCSampleRate(), 1000000u);
    radio->SetADCSampleRate(128000000);
    REQUIRE_EQUAL(radio->GetADCSampleRate(), 64000000u);

    REQUIRE_EQUAL(radio->GetDither(), false);
    radio->SetDither(true);
    REQUIRE_EQUAL(radio->GetDither(), true);

    REQUIRE_EQUAL(radio->GetRand(), false);
    radio->SetRand(true);
    REQUIRE_EQUAL(radio->GetRand(), true);

    REQUIRE_EQUAL(radio->GetPGA(), false);
    radio->SetPGA(true);
    REQUIRE_EQUAL(radio->GetPGA(), true);

    REQUIRE_EQUAL(radio->GetBiasT_HF(), false);
    radio->SetBiasT_HF(true);
    REQUIRE_EQUAL(radio->GetBiasT_HF(), true);

    REQUIRE_EQUAL(radio->GetBiasT_VHF(), false);
    radio->SetBiasT_VHF(true);
    REQUIRE_EQUAL(radio->GetBiasT_VHF(), true);

    delete radio;
}

TEST_CASE(CoreFixture, R2IQTest)
{
    vector<SDDC::DeviceItem> devices = testRadioHandler::GetDeviceList();
    auto radio = new testRadioHandler();
    radio->Init(devices[0]);

    radio->AttachIQ(Callback);

    for (int decimate = 0; decimate < 5; decimate++)
    {
        frame_count = 0;
        totalsize = 0;
        first_block_hash = 0;
        radio->SetDecimation(decimate);
        radio->Start(true); // full bandwidth
        std::this_thread::sleep_for(1s);
        radio->Stop();

        REQUIRE_TRUE(frame_count > 0);
        REQUIRE_TRUE(totalsize > 0);
        REQUIRE_EQUAL(totalsize / frame_count, transferSamples/2);
        printf("R2IQ signature decimation=%d hash=%016" PRIx64 " samples=%" PRIu64 "\n",
            decimate, first_block_hash, totalsize / frame_count);
    }

    delete radio;
}

TEST_CASE(CoreFixture, R2IQTonePurityTest)
{
    vector<SDDC::DeviceItem> devices = testRadioHandler::GetDeviceList();
    auto radio = new testRadioHandler();
    radio->SetHardwareModel(RX888r2);
    radio->SetGenerateTone(true);
    radio->Init(devices[0]);
    radio->SetADCSampleRate(64000000);
    radio->SetRFMode(VHFMODE);
    radio->SetCenterFrequency(120000000);

    radio->AttachIQ(ToneCallback);
    for (int scenario = 0; scenario <= 5; ++scenario)
    {
        const uint32_t adc_rate = scenario == 5 ? 32000000 : 64000000;
        const int decimation = scenario == 5 ? 1 : scenario;
        {
            std::lock_guard<std::mutex> lock(tone_capture_mutex);
            tone_capture.clear();
            tone_blocks_seen = 0;
        }
        radio->SetToneSampleRate(adc_rate);
        radio->SetADCSampleRate(adc_rate);
        radio->SetDecimation(decimation);
        // Fine tuning is normalized to the selected output rate.
        radio->SetCenterFrequency(120000000);
        radio->Start(true);

        for (int i = 0; i < 200; ++i)
        {
            {
                std::lock_guard<std::mutex> lock(tone_capture_mutex);
                if (tone_capture.size() >= 3 * transferSamples / 2) break;
            }
            std::this_thread::sleep_for(5ms);
        }
        radio->Stop();

        std::vector<std::complex<float>> samples;
        {
            std::lock_guard<std::mutex> lock(tone_capture_mutex);
            samples = tone_capture;
        }
        REQUIRE_EQUAL(samples.size(), static_cast<size_t>(3 * transferSamples / 2));

        const double output_rate = static_cast<double>(adc_rate) / 2.0 /
            static_cast<double>(1 << decimation);
        constexpr double output_tone = 250123.0;
        constexpr double two_pi = 6.28318530717958647692;
        double total_energy = 0.0;
        std::complex<double> positive_sum(0.0, 0.0);
        std::complex<double> negative_sum(0.0, 0.0);
        std::complex<double> phase_step_sum(0.0, 0.0);
        for (size_t i = 0; i < samples.size(); ++i)
        {
            const std::complex<double> value(samples[i].real(), samples[i].imag());
            const double phase = two_pi * output_tone * static_cast<double>(i) / output_rate;
            positive_sum += value * std::polar(1.0, -phase);
            negative_sum += value * std::polar(1.0, phase);
            total_energy += std::norm(value);
            if (i != 0)
                phase_step_sum += std::conj(std::complex<double>(samples[i - 1].real(),
                    samples[i - 1].imag())) * value;
        }

        const double count = static_cast<double>(samples.size());
        const double tone_energy = std::max(std::norm(positive_sum), std::norm(negative_sum)) /
            (count * count);
        const double mean_energy = total_energy / count;
        const double purity = tone_energy / mean_energy;
        const double measured_frequency = std::arg(phase_step_sum) * output_rate / two_pi;
        printf("R2IQ tone ADC=%u decimation=%d frequency=%.3f purity=%.9f residual=%.3f dBc\n",
            adc_rate, decimation, measured_frequency, purity,
            10.0 * std::log10(std::max(1.0e-20, 1.0 - purity)));
        REQUIRE_TRUE(purity > 0.999);
    }

    delete radio;
}

TEST_CASE(CoreFixture, TuneTest)
{
    vector<SDDC::DeviceItem> devices = testRadioHandler::GetDeviceList();
    auto radio = new testRadioHandler();
    radio->Init(devices[0]);

    radio->AttachIQ(Callback);

    radio->SetDecimation(1); // full bandwidth
    radio->Start(true);

    for (uint64_t i = 1000; i < 15000000;  i+=377000)
    {
        radio->SetCenterFrequency(i);
        std::this_thread::sleep_for(0.011s);
    }

    radio->Stop();


    delete radio;
}
