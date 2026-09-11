#include "nstu/setup/driver_scan.hpp"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mfidl.h>
#include <wrl/client.h>

#include <sstream>
#include <utility>

namespace nstu::setup {
namespace { void set_error(std::string* error, const char* message) { if (error != nullptr) *error = message; } }

std::vector<EncoderScan> scan_hardware_h264_encoders(std::string* error) {
    const HRESULT startup = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(startup)) {
        set_error(error, "MFStartup failed");
        return {};
    }
    MFT_REGISTER_TYPE_INFO input{MFMediaType_Video, MFVideoFormat_NV12};
    MFT_REGISTER_TYPE_INFO output{MFMediaType_Video, MFVideoFormat_H264};
    IMFActivate** activations = nullptr;
    UINT32 count = 0;
    const HRESULT result = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
        MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
        &input, &output, &activations, &count);
    if (FAILED(result)) {
        MFShutdown();
        set_error(error, "MFTEnumEx failed");
        return {};
    }
    std::vector<EncoderScan> encoders;
    encoders.reserve(count);
    for (UINT32 index = 0; index < count; ++index) {
        EncoderScan encoder;
        wchar_t* name = nullptr;
        UINT32 name_length = 0;
        if (SUCCEEDED(activations[index]->GetAllocatedString(
                MFT_FRIENDLY_NAME_Attribute, &name, &name_length)) &&
            name != nullptr) {
            encoder.friendly_name.assign(name, name_length);
            CoTaskMemFree(name);
        }
        encoders.push_back(std::move(encoder));
        activations[index]->Release();
    }
    CoTaskMemFree(activations);
    MFShutdown();
    return encoders;
}

GraphicsScan scan_graphics() {
    using Microsoft::WRL::ComPtr;
    GraphicsScan result;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        return result;
    }
    constexpr D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> adapter;
        const HRESULT enumerated = factory->EnumAdapters1(index, &adapter);
        if (enumerated == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (FAILED(enumerated)) {
            break;
        }
        DXGI_ADAPTER_DESC1 description{};
        if (FAILED(adapter->GetDesc1(&description))) {
            continue;
        }
        GraphicsAdapterScan scanned;
        scanned.name = description.Description;
        scanned.vendor_id = description.VendorId;
        scanned.dedicated_video_memory_bytes = description.DedicatedVideoMemory;
        scanned.software =
            (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
        LARGE_INTEGER version{};
        if (SUCCEEDED(adapter->CheckInterfaceSupport(
                __uuidof(IDXGIDevice), &version))) {
            const auto high = static_cast<std::uint32_t>(version.HighPart);
            const auto low = static_cast<std::uint32_t>(version.LowPart);
            std::wostringstream text;
            text << HIWORD(high) << L'.' << LOWORD(high) << L'.'
                 << HIWORD(low) << L'.' << LOWORD(low);
            scanned.driver_version = text.str();
        }
        result.adapters.push_back(std::move(scanned));
        if (!result.adapters.back().software &&
            !result.hardware_d3d11_available) {
            ComPtr<ID3D11Device> device;
            ComPtr<ID3D11DeviceContext> context;
            result.hardware_hresult = D3D11CreateDevice(
                adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, ARRAYSIZE(levels),
                D3D11_SDK_VERSION, &device, nullptr, &context);
            result.hardware_d3d11_available =
                SUCCEEDED(result.hardware_hresult);
        }
    }
    ComPtr<ID3D11Device> warp_device;
    ComPtr<ID3D11DeviceContext> warp_context;
    result.warp_hresult = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, ARRAYSIZE(levels),
        D3D11_SDK_VERSION, &warp_device, nullptr, &warp_context);
    result.warp_d3d11_available = SUCCEEDED(result.warp_hresult);
    return result;
}
} // namespace nstu::setup
