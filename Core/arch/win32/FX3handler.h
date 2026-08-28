#ifndef FX3HANDLER_H
#define FX3HANDLER_H

#include <atomic>
#include <thread>
#include <vector>

#include "../../FX3Class.h"
#include "../../dsp/ringbuffer.h"

#define VENDOR_ID     (0x04B4)
#define STREAMER_ID   (0x00F1)
#define BOOTLOADER_ID (0x00F3)

class CCyFX3Device;
class CCyUSBEndPoint;

class fx3handler : public fx3class
{
public:
    fx3handler();
    ~fx3handler() override;

    bool Open(SDDC::DeviceItem device) override;
    bool Control(FX3Command command, uint8_t data) override;
    bool Control(FX3Command command, uint32_t data) override;
    bool Control(FX3Command command, uint64_t data) override;
    bool SetArgument(uint16_t index, uint16_t value) override;
    bool GetHardwareInfo(uint32_t* data) override;
    bool ReadDebugTrace(uint8_t* data, uint8_t len) override;
    void StartStream(ringbuffer<int16_t>& input) override;
    void StopStream() override;
    size_t GetDeviceListLength() override;
    std::vector<SDDC::DeviceItem> GetDeviceList() override;

private:
    bool Close();
    bool OpenStreamer(const SDDC::DeviceItem& selector);
    bool BeginDataXfer(uint8_t* buffer, long size, void** context);
    bool FinishDataXfer(void** context);
    void CleanupDataXfer(void** context);
    void AdcSamplesProcess();

    CCyFX3Device* fx3dev = nullptr;
    CCyUSBEndPoint* endpoint = nullptr;
    ringbuffer<int16_t>* inputbuffer = nullptr;
    std::thread adc_samples_thread;
    std::atomic<bool> run { false };
};

#endif
