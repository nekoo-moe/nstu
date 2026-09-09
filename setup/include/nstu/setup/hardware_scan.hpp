#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace nstu::setup {

struct DisplayScan {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t refresh_hz = 0;
};

struct ProcessorScan {
    std::wstring model;
    std::wstring architecture;
    std::uint32_t physical_cores = 0;
    std::uint32_t logical_processors = 0;
    bool x64 = false;
};

struct MemoryScan {
    std::uint64_t total_physical_bytes = 0;
    std::uint64_t available_physical_bytes = 0;
};

struct NetworkScan {
    std::wstring adapter_name;
    std::uint64_t transmit_link_speed_mbps = 0;
    std::uint64_t receive_link_speed_mbps = 0;
    std::uint32_t interface_type = 0;
    bool operational = false;
    bool tunnel = false;
};

struct HardwareScan {
    DisplayScan display;
    ProcessorScan processor;
    MemoryScan memory;
    std::vector<NetworkScan> networks;
};

[[nodiscard]] HardwareScan scan_hardware();

} // namespace nstu::setup
