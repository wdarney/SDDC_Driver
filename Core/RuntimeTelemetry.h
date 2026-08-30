#pragma once

#include <cstdlib>
#include <cstring>
#include <cstdint>

namespace sddc {

static inline bool runtimeTelemetryEnabled()
{
    static const bool enabled = [] {
        const char* request = std::getenv("SDDC_RUNTIME_TELEMETRY");
        return request != nullptr && request[0] != '\0' &&
            std::strcmp(request, "0") != 0;
    }();
    return enabled;
}

constexpr uint64_t runtimeTelemetryClockSamplePeriod = 16;
constexpr double runtimeTelemetryReportSeconds = 1.0;

}
