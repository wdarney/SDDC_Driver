#include "FX3Class.h"
#include "CppUnitTestFramework.hpp"
#include <thread>
#include <chrono>
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
            0, FIRMWARE_VER_MAJOR, FIRMWARE_VER_MINOR, 0
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
    void StartStream(ringbuffer<int16_t>& input)
    {
        input.setBlockSize(transferSamples);
        run = true;
        emuthread = std::thread([&input, this]{
            uint32_t block_index = 0;
            while(run)
            {
                vector<int16_t> put(transferSamples);
                for (uint32_t i = 0; i < transferSamples; ++i)
                    put[i] = static_cast<int16_t>(i * 257u + block_index * 31u);
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


};

class testRadioHandler: public RadioHandler
{
public:
    testRadioHandler()
    {
        RadioHandler();
        fx3 = new fx3handler2();
    }

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
};

static uint32_t frame_count;
static uint64_t totalsize;
static uint64_t first_block_hash;

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

    REQUIRE_EQUAL(radio->GetADCSampleRate(), 64000000u);
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
