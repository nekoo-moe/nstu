#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace nstu::setup {
struct EncoderScan { std::wstring friendly_name; };

struct GraphicsAdapterScan {
    std::wstring name;
    std::wstring driver_version;
    std::uint32_t vendor_id = 0;
    std::uint64_t dedicated_video_memory_bytes = 0;
    bool software = false;
};

struct GraphicsScan {
    std::vector<GraphicsAdapterScan> adapters;
    bool hardware_d3d11_available = false;
    bool warp_d3d11_available = false;
    long hardware_hresult = 0;
    long warp_hresult = 0;
};

[[nodiscard]] std::vector<EncoderScan> scan_hardware_h264_encoders(
    std::string* error = nullptr);
[[nodiscard]] GraphicsScan scan_graphics();
} // namespace nstu::setup
