#include "nstu/setup/hardware_scan.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>

#include <array>
#include <limits>
#include <utility>
#include <vector>

namespace nstu::setup {
namespace {

std::wstring processor_model() {
    std::array<wchar_t, 256> value{};
    DWORD bytes = static_cast<DWORD>(value.size() * sizeof(wchar_t));
    if (RegGetValueW(
            HKEY_LOCAL_MACHINE,
            L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
            L"ProcessorNameString", RRF_RT_REG_SZ, nullptr, value.data(),
            &bytes) != ERROR_SUCCESS) {
        return L"Unknown processor";
    }
    return value.data();
}

std::uint32_t physical_core_count() {
    DWORD bytes = 0;
    if (GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr,
                                         &bytes) != FALSE ||
        GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes == 0) {
        return 0;
    }
    std::vector<std::byte> buffer(bytes);
    if (!GetLogicalProcessorInformationEx(
            RelationProcessorCore,
            reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(
                buffer.data()),
            &bytes)) {
        return 0;
    }
    std::uint32_t cores = 0;
    std::size_t offset = 0;
    while (offset + sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX) <=
           buffer.size()) {
        const auto* information =
            reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(
                buffer.data() + offset);
        if (information->Size == 0 ||
            information->Size > buffer.size() - offset) {
            break;
        }
        if (information->Relationship == RelationProcessorCore) {
            ++cores;
        }
        offset += information->Size;
    }
    return cores;
}

ProcessorScan scan_processor() {
    SYSTEM_INFO information{};
    GetNativeSystemInfo(&information);
    ProcessorScan result;
    result.model = processor_model();
    result.logical_processors = information.dwNumberOfProcessors;
    result.physical_cores = physical_core_count();
    switch (information.wProcessorArchitecture) {
    case PROCESSOR_ARCHITECTURE_AMD64:
        result.architecture = L"x64";
        result.x64 = true;
        break;
    case PROCESSOR_ARCHITECTURE_ARM64:
        result.architecture = L"ARM64";
        break;
    case PROCESSOR_ARCHITECTURE_INTEL:
        result.architecture = L"x86";
        break;
    default:
        result.architecture = L"Unknown";
        break;
    }
    return result;
}

MemoryScan scan_memory() {
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (!GlobalMemoryStatusEx(&status)) {
        return {};
    }
    return {status.ullTotalPhys, status.ullAvailPhys};
}

} // namespace

HardwareScan scan_hardware() {
    HardwareScan result;
    result.processor = scan_processor();
    result.memory = scan_memory();
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode) != FALSE) {
        result.display = {mode.dmPelsWidth, mode.dmPelsHeight,
                          mode.dmDisplayFrequency};
    }

    ULONG bytes = 0;
    if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, nullptr,
                             nullptr, &bytes) != ERROR_BUFFER_OVERFLOW) {
        return result;
    }
    std::vector<std::byte> buffer(bytes);
    auto* addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, nullptr,
                             addresses, &bytes) != NO_ERROR) {
        return result;
    }
    for (auto* adapter = addresses; adapter != nullptr;
         adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp ||
            adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
            continue;
        }
        NetworkScan network;
        if (adapter->FriendlyName != nullptr) {
            network.adapter_name = adapter->FriendlyName;
        }
        network.transmit_link_speed_mbps =
            adapter->TransmitLinkSpeed / 1'000'000u;
        network.receive_link_speed_mbps =
            adapter->ReceiveLinkSpeed / 1'000'000u;
        network.interface_type = adapter->IfType;
        network.operational = true;
        network.tunnel = adapter->IfType == IF_TYPE_TUNNEL;
        result.networks.push_back(std::move(network));
    }
    return result;
}

} // namespace nstu::setup
