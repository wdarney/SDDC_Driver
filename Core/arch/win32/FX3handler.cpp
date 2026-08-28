#include <windows.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "../../config.h"
#include "FX3handler.h"
#include "CyAPI/CyAPI.h"
#include "firmware.h"

namespace {

constexpr const char* TAG = "FX3Handler/CyAPI";
constexpr size_t USB_READ_CONCURRENT = 4;
constexpr DWORD BLOCK_TIMEOUT_MS = 100;
constexpr int REENUMERATION_ATTEMPTS = 40;
constexpr int REENUMERATION_DELAY_MS = 250;

#define firmware_data (reinterpret_cast<const UCHAR*>(FIRMWARE))
#define firmware_size (sizeof(FIRMWARE))

struct ReadContext {
    PUCHAR cy_context = nullptr;
    OVERLAPPED overlap {};
    // CyAPI's XMODE_DIRECT contract writes a SINGLE_TRANSFER immediately after
    // the caller-provided OVERLAPPED (see CCyUSBEndPoint::BeginDirectXfer).
    // This field is required storage, even though our code never reads it
    // directly.  Omitting it lets CyAPI overwrite buffer and size below.
    SINGLE_TRANSFER transfer {};
    uint8_t* buffer = nullptr;
    long size = 0;
};

static_assert(offsetof(ReadContext, transfer) ==
              offsetof(ReadContext, overlap) + sizeof(OVERLAPPED),
              "CyAPI direct-transfer storage must immediately follow OVERLAPPED");

std::string narrow(const wchar_t* value)
{
    if (value == nullptr || *value == L'\0') return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1) return {};
    std::string result(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), length, nullptr, nullptr);
    result.resize(static_cast<size_t>(length - 1));
    return result;
}

bool isSddcDevice(const CCyFX3Device& device)
{
    return device.VendorID == VENDOR_ID &&
        (device.ProductID == STREAMER_ID || device.ProductID == BOOTLOADER_ID);
}

} // namespace

fx3class* CreateUsbHandler()
{
    return new fx3handler();
}

fx3handler::fx3handler() = default;

fx3handler::~fx3handler()
{
    StopStream();
    Close();
}

std::vector<SDDC::DeviceItem> fx3handler::GetDeviceList()
{
    bool firmware_loaded = false;

    // The Cypress bootloader and streamer use the same CyUSB driver. Upload the
    // embedded firmware before publishing the device list, just as the original
    // Windows backend did. No external firmware path or fixed serial is used.
    {
        CCyFX3Device scanner;
        const UCHAR count = scanner.DeviceCount();
        for (UCHAR index = 0; index < count; ++index) {
            if (!scanner.Open(index)) continue;
            if (!isSddcDevice(scanner)) {
                scanner.Close();
                continue;
            }

            if (scanner.ProductID == BOOTLOADER_ID || scanner.IsBootLoaderRunning()) {
                DebugPrintln(TAG, "Uploading embedded firmware to CyUSB device %u", index);
                const auto status = scanner.DownloadFwToRam(firmware_data, firmware_size);
                if (status != SUCCESS) {
                    ErrorPrintln(TAG, "Firmware upload failed for CyUSB device %u (status %d)",
                        index, static_cast<int>(status));
                }
                else {
                    firmware_loaded = true;
                }
            }
            scanner.Close();
        }
    }

    std::vector<SDDC::DeviceItem> devices;
    const int attempts = firmware_loaded ? REENUMERATION_ATTEMPTS : 1;
    for (int attempt = 0; attempt < attempts; ++attempt) {
        if (attempt != 0 || firmware_loaded) {
            std::this_thread::sleep_for(std::chrono::milliseconds(REENUMERATION_DELAY_MS));
        }

        devices.clear();
        CCyFX3Device scanner;
        const UCHAR count = scanner.DeviceCount();
        for (UCHAR index = 0; index < count; ++index) {
            if (!scanner.Open(index)) continue;
            if (scanner.VendorID == VENDOR_ID && scanner.ProductID == STREAMER_ID) {
                SDDC::DeviceItem item;
                item.index = index;
                item.product = narrow(scanner.Product);
                if (item.product.empty()) item.product = "SDDC RX888";
                item.serial_number = narrow(scanner.SerialNumber);
                devices.push_back(std::move(item));
            }
            scanner.Close();
        }

        if (!devices.empty() || !firmware_loaded) break;
    }

    if (firmware_loaded && devices.empty()) {
        ErrorPrintln(TAG, "Firmware uploaded, but the RX888 did not re-enumerate within %d ms",
            REENUMERATION_ATTEMPTS * REENUMERATION_DELAY_MS);
    }
    return devices;
}

size_t fx3handler::GetDeviceListLength()
{
    return GetDeviceList().size();
}

bool fx3handler::OpenStreamer(const SDDC::DeviceItem& selector)
{
    delete fx3dev;
    fx3dev = new CCyFX3Device;

    const UCHAR count = fx3dev->DeviceCount();
    for (UCHAR index = 0; index < count; ++index) {
        if (!fx3dev->Open(index)) continue;

        const bool streamer = fx3dev->VendorID == VENDOR_ID && fx3dev->ProductID == STREAMER_ID;
        const std::string serial = narrow(fx3dev->SerialNumber);
        const bool selected = !selector.serial_number.empty()
            ? serial == selector.serial_number
            : index == selector.index;
        if (streamer && selected) return true;
        fx3dev->Close();
    }
    return false;
}

bool fx3handler::Open(SDDC::DeviceItem selector)
{
    Close();
    if (!OpenStreamer(selector)) {
        // Handles callers that open without first requesting the device list.
        const auto devices = GetDeviceList();
        if (devices.empty()) {
            ErrorPrintln(TAG, "No RX888 streamer device is available through the Cypress driver");
            return false;
        }

        auto selected = devices.front();
        for (const auto& device : devices) {
            if ((!selector.serial_number.empty() && device.serial_number == selector.serial_number) ||
                (selector.serial_number.empty() && device.index == selector.index)) {
                selected = device;
                break;
            }
        }
        if (!OpenStreamer(selected)) {
            ErrorPrintln(TAG, "Unable to open the selected RX888 after firmware enumeration");
            return false;
        }
    }

    endpoint = fx3dev->BulkInEndPt;
    if (endpoint == nullptr) {
        ErrorPrintln(TAG, "The RX888 has no bulk input endpoint");
        Close();
        return false;
    }

    endpoint->SetXferSize(transferSize);
    uint32_t hardware_info = 0;
    if (!GetHardwareInfo(&hardware_info)) {
        ErrorPrintln(TAG, "Unable to read RX888 firmware information");
        Close();
        return false;
    }

    const auto* version = reinterpret_cast<const uint8_t*>(&hardware_info);
    if (version[1] != FIRMWARE_VER_MAJOR || version[2] != FIRMWARE_VER_MINOR) {
        ErrorPrintln(TAG, "Firmware version mismatch: expected %u.%u, device has %u.%u",
            FIRMWARE_VER_MAJOR, FIRMWARE_VER_MINOR, version[1], version[2]);
        Control(RESETFX3, static_cast<uint8_t>(0));
        Close();
        return false;
    }

    if (!Control(STOPFX3, static_cast<uint8_t>(0))) {
        ErrorPrintln(TAG, "Unable to stop the RX888 stream during initialization");
        Close();
        return false;
    }
    return true;
}

bool fx3handler::Close()
{
    endpoint = nullptr;
    if (fx3dev != nullptr) {
        fx3dev->Close();
        delete fx3dev;
        fx3dev = nullptr;
    }
    return true;
}

bool fx3handler::Control(FX3Command command, uint8_t data)
{
    if (fx3dev == nullptr || fx3dev->ControlEndPt == nullptr) return false;
    long length = sizeof(data);
    fx3dev->ControlEndPt->ReqCode = command;
    fx3dev->ControlEndPt->Value = 0;
    fx3dev->ControlEndPt->Index = 0;
    return fx3dev->ControlEndPt->Write(&data, length);
}

bool fx3handler::Control(FX3Command command, uint32_t data)
{
    if (fx3dev == nullptr || fx3dev->ControlEndPt == nullptr) return false;
    long length = sizeof(data);
    fx3dev->ControlEndPt->ReqCode = command;
    fx3dev->ControlEndPt->Value = 0;
    fx3dev->ControlEndPt->Index = 0;
    return fx3dev->ControlEndPt->Write(reinterpret_cast<PUCHAR>(&data), length);
}

bool fx3handler::Control(FX3Command command, uint64_t data)
{
    if (fx3dev == nullptr || fx3dev->ControlEndPt == nullptr) return false;
    long length = sizeof(data);
    fx3dev->ControlEndPt->ReqCode = command;
    fx3dev->ControlEndPt->Value = 0;
    fx3dev->ControlEndPt->Index = 0;
    return fx3dev->ControlEndPt->Write(reinterpret_cast<PUCHAR>(&data), length);
}

bool fx3handler::SetArgument(uint16_t index, uint16_t value)
{
    if (fx3dev == nullptr || fx3dev->ControlEndPt == nullptr) return false;
    uint8_t data = 0;
    long length = sizeof(data);
    fx3dev->ControlEndPt->ReqCode = SETARGFX3;
    fx3dev->ControlEndPt->Value = value;
    fx3dev->ControlEndPt->Index = index;
    return fx3dev->ControlEndPt->Write(&data, length);
}

bool fx3handler::GetHardwareInfo(uint32_t* data)
{
    if (data == nullptr || fx3dev == nullptr || fx3dev->ControlEndPt == nullptr) return false;
    long length = sizeof(*data);
    fx3dev->ControlEndPt->ReqCode = TESTFX3;
#ifdef _DEBUG
    fx3dev->ControlEndPt->Value = 1;
#else
    fx3dev->ControlEndPt->Value = 0;
#endif
    fx3dev->ControlEndPt->Index = 0;
    return fx3dev->ControlEndPt->Read(reinterpret_cast<PUCHAR>(data), length);
}

bool fx3handler::ReadDebugTrace(uint8_t* data, uint8_t len)
{
    if (data == nullptr || fx3dev == nullptr || fx3dev->ControlEndPt == nullptr) return false;
    long length = len;
    fx3dev->ControlEndPt->ReqCode = READINFODEBUG;
    fx3dev->ControlEndPt->Value = data[0];
    fx3dev->ControlEndPt->Index = 0;
    return fx3dev->ControlEndPt->Read(data, length);
}

bool fx3handler::BeginDataXfer(uint8_t* buffer, long size, void** context)
{
    if (endpoint == nullptr || context == nullptr) return false;
    auto* read = static_cast<ReadContext*>(*context);
    if (read == nullptr) {
        read = new ReadContext;
        read->overlap.hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (read->overlap.hEvent == nullptr) {
            delete read;
            return false;
        }
        *context = read;
    }

    read->buffer = buffer;
    read->size = size;
    read->cy_context = endpoint->BeginDataXfer(buffer, size, &read->overlap);
    return read->cy_context != nullptr && endpoint->NtStatus == 0 && endpoint->UsbdStatus == 0;
}

bool fx3handler::FinishDataXfer(void** context)
{
    if (endpoint == nullptr || context == nullptr || *context == nullptr) return false;
    auto* read = static_cast<ReadContext*>(*context);
    if (!endpoint->WaitForXfer(&read->overlap, BLOCK_TIMEOUT_MS)) {
        if (run.load()) WarnPrintln(TAG, "USB transfer timed out (NTSTATUS 0x%08lX)", endpoint->NtStatus);
        return false;
    }

    long actual = read->size;
    if (!endpoint->FinishDataXfer(read->buffer, actual, &read->overlap, read->cy_context)) return false;
    if (actual != read->size) {
        WarnPrintln(TAG, "Short USB transfer: received %ld of %ld bytes", actual, read->size);
        return false;
    }
    return true;
}

void fx3handler::CleanupDataXfer(void** context)
{
    if (context == nullptr || *context == nullptr) return;
    auto* read = static_cast<ReadContext*>(*context);
    if (read->overlap.hEvent != nullptr) CloseHandle(read->overlap.hEvent);
    delete read;
    *context = nullptr;
}

void fx3handler::AdcSamplesProcess()
{
    WarnPrintln(TAG, "STARTUP USB 1/3: Cypress receive thread entered");
    std::array<std::vector<uint8_t>, USB_READ_CONCURRENT> buffers;
    std::array<void*, USB_READ_CONCURRENT> contexts {};
    for (size_t i = 0; i < USB_READ_CONCURRENT; ++i) {
        buffers[i].resize(transferSize);
        if (!BeginDataXfer(buffers[i].data(), transferSize, &contexts[i])) {
            ErrorPrintln(TAG, "Unable to queue initial USB transfer %zu", i);
            run = false;
            break;
        }
    }

    if (!run.load()) {
        if (endpoint != nullptr) endpoint->Abort();
        for (auto& context : contexts) CleanupDataXfer(&context);
        return;
    }
    WarnPrintln(TAG, "STARTUP USB 2/3: initial Cypress transfers queued");

    size_t read_index = 0;
    bool first_transfer = true;
    while (run.load()) {
        if (!FinishDataXfer(&contexts[read_index])) break;

        if (first_transfer) {
            WarnPrintln(TAG, "STARTUP USB 3/3: first Cypress transfer completed");
            first_transfer = false;
        }

        int16_t* destination = inputbuffer != nullptr ? inputbuffer->acquireWriteBlock() : nullptr;
        if (destination == nullptr) break;
        std::memcpy(destination, buffers[read_index].data(), transferSize);
        if (!inputbuffer->commitWriteBlock()) break;

        if (!BeginDataXfer(buffers[read_index].data(), transferSize, &contexts[read_index])) {
            ErrorPrintln(TAG, "Unable to requeue USB transfer %zu", read_index);
            break;
        }
        read_index = (read_index + 1) % USB_READ_CONCURRENT;
    }

    run = false;
    if (endpoint != nullptr) endpoint->Abort();
    for (auto& context : contexts) CleanupDataXfer(&context);
}

void fx3handler::StartStream(ringbuffer<int16_t>& input)
{
    if (run.exchange(true)) return;
    WarnPrintln(TAG, "Starting Cypress receive thread");
    inputbuffer = &input;
    inputbuffer->setBlockSize(transferSamples);
    adc_samples_thread = std::thread([this] { AdcSamplesProcess(); });
}

void fx3handler::StopStream()
{
    run = false;
    if (endpoint != nullptr) endpoint->Abort();
    if (adc_samples_thread.joinable()) adc_samples_thread.join();
    inputbuffer = nullptr;
}
