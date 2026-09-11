#include "nstu/setup/diagnostics.hpp"

#include "nstu/deployment.hpp"
#include "nstu/protocol.hpp"
#include "nstu/setup/driver_scan.hpp"
#include "nstu/setup/hardware_scan.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <ipifcons.h>
#include <netlistmgr.h>
#include <versionhelpers.h>
#include <wbemidl.h>
#include <winsvc.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace nstu::setup {
namespace {

using Microsoft::WRL::ComPtr;

#if NSTU_INTERNAL_TEST_BUILD
constexpr bool kInternalTestBuild = true;
#else
constexpr bool kInternalTestBuild = false;
#endif

constexpr std::uint32_t kProductEnterprise = 0x00000004;
constexpr std::uint32_t kProductEducation = 0x00000079;
constexpr std::uint32_t kProductEducationN = 0x0000007a;
constexpr std::uint32_t kProductEnterpriseS = 0x0000007d;
constexpr std::uint32_t kProductEnterpriseSN = 0x0000007e;
constexpr std::uint32_t kProductEnterpriseN = 0x0000001b;
constexpr std::uint32_t kProductIotEnterprise = 0x000000bc;
constexpr std::uint32_t kProductIotEnterpriseS = 0x000000bf;

void set_error(std::string* error, const char* message) {
    if (error != nullptr) {
        *error = message;
    }
}

std::wstring utf8_to_wide(std::string_view input) {
    if (input.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
        static_cast<int>(input.size()), nullptr, 0);
    if (length <= 0) {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
                        static_cast<int>(input.size()), result.data(), length);
    return result;
}

std::string wide_to_utf8(std::wstring_view input) {
    if (input.empty()) {
        return {};
    }
    const int length = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, input.data(),
        static_cast<int>(input.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(),
                        static_cast<int>(input.size()), result.data(), length,
                        nullptr, nullptr);
    return result;
}

DiagnosticResult result(std::string id, DiagnosticSeverity severity,
                        const wchar_t* title_en, const wchar_t* title_vi,
                        std::wstring detail_en, std::wstring detail_vi,
                        std::wstring remediation_en = {},
                        std::wstring remediation_vi = {},
                        std::uint32_t error_code = 0) {
    return {std::move(id), severity, title_en, title_vi,
            std::move(detail_en), std::move(detail_vi),
            std::move(remediation_en), std::move(remediation_vi), error_code};
}

bool read_reg_string(HKEY root, const wchar_t* path, const wchar_t* name,
                     std::wstring& value) {
    std::array<wchar_t, 1024> buffer{};
    DWORD bytes = static_cast<DWORD>(buffer.size() * sizeof(wchar_t));
    const LSTATUS status = RegGetValueW(root, path, name, RRF_RT_REG_SZ,
                                        nullptr, buffer.data(), &bytes);
    if (status != ERROR_SUCCESS) {
        return false;
    }
    value.assign(buffer.data());
    return true;
}

std::wstring os_product_name() {
    std::wstring value;
    if (read_reg_string(HKEY_LOCAL_MACHINE,
                        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                        L"ProductName", value)) {
        return value;
    }
    return L"Unknown Windows edition";
}

std::wstring os_display_version() {
    std::wstring value;
    if (read_reg_string(HKEY_LOCAL_MACHINE,
                        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                        L"DisplayVersion", value)) {
        return value;
    }
    read_reg_string(HKEY_LOCAL_MACHINE,
                    L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                    L"ReleaseId", value);
    return value;
}

std::uint32_t os_product_type() {
    DWORD product = 0;
    if (GetProductInfo(10, 0, 0, 0, &product)) {
        return product;
    }
    return 0;
}

std::wstring os_version() {
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    const auto module = GetModuleHandleW(L"ntdll.dll");
    RtlGetVersionFn function = nullptr;
    if (module != nullptr) {
        const FARPROC raw = GetProcAddress(module, "RtlGetVersion");
        static_assert(sizeof(function) == sizeof(raw));
        std::memcpy(&function, &raw, sizeof(function));
    }
    RTL_OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    if (function == nullptr || function(&version) != 0) {
        return L"unknown build";
    }
    std::wostringstream text;
    text << version.dwMajorVersion << L'.' << version.dwMinorVersion << L'.'
         << version.dwBuildNumber;
    return text.str();
}

class Bstr {
public:
    explicit Bstr(const wchar_t* value) : value_(SysAllocString(value)) {}
    ~Bstr() { SysFreeString(value_); }
    Bstr(const Bstr&) = delete;
    Bstr& operator=(const Bstr&) = delete;
    [[nodiscard]] BSTR get() const noexcept { return value_; }

private:
    BSTR value_ = nullptr;
};

class WmiSession {
public:
    bool connect(const wchar_t* namespace_name) {
        ComPtr<IWbemLocator> locator;
        if (FAILED(CoCreateInstance(CLSID_WbemLocator, nullptr,
                                     CLSCTX_INPROC_SERVER,
                                     IID_PPV_ARGS(&locator)))) {
            return false;
        }
        Bstr ns(namespace_name);
        if (ns.get() == nullptr ||
            FAILED(locator->ConnectServer(ns.get(), nullptr, nullptr, nullptr,
                                           0, nullptr, nullptr, &services_))) {
            return false;
        }
        return SUCCEEDED(CoSetProxyBlanket(
            services_.Get(), RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
            RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr,
            EOAC_NONE));
    }

    bool first(const wchar_t* query, ComPtr<IWbemClassObject>& object) const {
        if (!services_) {
            return false;
        }
        Bstr language(L"WQL");
        Bstr text(query);
        ComPtr<IEnumWbemClassObject> enumerator;
        if (language.get() == nullptr || text.get() == nullptr ||
            FAILED(services_->ExecQuery(language.get(), text.get(),
                                         WBEM_FLAG_FORWARD_ONLY |
                                             WBEM_FLAG_RETURN_IMMEDIATELY,
                                         nullptr, &enumerator))) {
            return false;
        }
        ULONG returned = 0;
        return SUCCEEDED(enumerator->Next(2000, 1, &object, &returned)) &&
               returned == 1;
    }

private:
    ComPtr<IWbemServices> services_;
};

bool variant_uint32(IWbemClassObject* object, const wchar_t* name,
                    std::uint32_t& value) {
    VARIANT variant;
    VariantInit(&variant);
    Bstr property(name);
    const HRESULT result = object->Get(property.get(), 0, &variant, nullptr,
                                       nullptr);
    bool ok = false;
    if (SUCCEEDED(result)) {
        if (variant.vt == VT_UI4) {
            value = variant.ulVal;
            ok = true;
        } else if (variant.vt == VT_I4 && variant.lVal >= 0) {
            value = static_cast<std::uint32_t>(variant.lVal);
            ok = true;
        }
    }
    VariantClear(&variant);
    return ok;
}

bool variant_bool(IWbemClassObject* object, const wchar_t* name, bool& value) {
    VARIANT variant;
    VariantInit(&variant);
    Bstr property(name);
    const HRESULT result = object->Get(property.get(), 0, &variant, nullptr,
                                       nullptr);
    bool ok = false;
    if (SUCCEEDED(result) && variant.vt == VT_BOOL) {
        value = variant.boolVal != VARIANT_FALSE;
        ok = true;
    }
    VariantClear(&variant);
    return ok;
}

bool query_optional_feature(bool& known, bool& enabled) {
    WmiSession session;
    if (!session.connect(L"ROOT\\cimv2")) {
        return false;
    }
    ComPtr<IWbemClassObject> object;
    if (!session.first(
            L"SELECT InstallState FROM Win32_OptionalFeature WHERE Name='Client-UnifiedWriteFilter'",
            object)) {
        known = true;
        enabled = false;
        return true;
    }
    std::uint32_t state = 0;
    if (!variant_uint32(object.Get(), L"InstallState", state)) {
        return false;
    }
    known = true;
    enabled = state == 1;
    return true;
}

bool query_uwf_filter(UwfProbeSnapshot& snapshot) {
    WmiSession session;
    if (!session.connect(L"ROOT\\standardcimv2\\embedded")) {
        return false;
    }
    snapshot.provider_available = true;
    ComPtr<IWbemClassObject> object;
    if (!session.first(L"SELECT CurrentEnabled, NextEnabled FROM UWF_Filter",
                      object)) {
        return false;
    }
    bool current = false;
    bool next = false;
    if (!variant_bool(object.Get(), L"CurrentEnabled", current) ||
        !variant_bool(object.Get(), L"NextEnabled", next)) {
        return false;
    }
    snapshot.filter_state_known = true;
    snapshot.current_enabled = current;
    snapshot.next_enabled = next;
    return true;
}

DiagnosticResult check_os() {
    const bool supported = IsWindows10OrGreater() && sizeof(void*) == 8;
    const auto detail = os_product_name() + L" (build " + os_version() +
                         L", " + os_display_version() + L")";
    if (!supported) {
        return result("os", DiagnosticSeverity::failure, L"Operating system",
                      L"Hệ điều hành", detail, detail,
                      L"NSTU requires 64-bit Windows 10 or Windows 11.",
                      L"NSTU yêu cầu Windows 10 hoặc Windows 11 64-bit.", 1);
    }
    return result("os", DiagnosticSeverity::pass, L"Operating system",
                  L"Hệ điều hành", detail, detail);
}

DiagnosticResult check_uwf(DiagnosticRole role) {
    if (role != DiagnosticRole::client) {
        return result(
            "uwf", DiagnosticSeverity::not_applicable,
            L"Reboot-to-restore scope", L"Phạm vi khôi phục sau reboot",
            L"UWF is client-only. The server keeps persistent exam, lecture, enrollment, and diagnostic data.",
            L"UWF chỉ áp dụng cho client. Server giữ dữ liệu exam, bài giảng, enrollment và diagnostics một cách bền vững.",
            L"Run UWF qualification with --target=client on a separate client image.",
            L"Chạy qualification UWF bằng --target=client trên image client riêng.");
    }
    UwfProbeSnapshot snapshot;
    snapshot.product_type = os_product_type();
    query_optional_feature(snapshot.feature_known, snapshot.feature_enabled);
    query_uwf_filter(snapshot);
    const auto state = classify_uwf(snapshot);
    const auto edition = os_product_name();
    if (state == UwfState::unsupported_edition) {
        return result(
            "uwf", DiagnosticSeverity::warning, L"Reboot-to-restore support",
            L"Hỗ trợ khôi phục sau reboot",
            L"Current edition " + edition +
                L" does not support Microsoft UWF.",
            L"Edition hiện tại " + edition + L" không hỗ trợ Microsoft UWF.",
            L"Install a properly licensed Education, Enterprise, or IoT Enterprise edition, or use supported third-party freezing software.",
            L"Cài Windows Education, Enterprise hoặc IoT Enterprise có bản quyền, hoặc dùng phần mềm đóng băng bên thứ ba được hỗ trợ.");
    }
    if (state == UwfState::feature_missing) {
        return result("uwf", DiagnosticSeverity::warning,
                      L"Reboot-to-restore support", L"Hỗ trợ khôi phục sau reboot",
                      L"This edition may support UWF, but the UWF optional feature is not enabled.",
                      L"Edition này có thể hỗ trợ UWF nhưng optional feature UWF chưa được bật.",
                      L"A future maintenance workflow must enable the documented Windows feature and reboot; this diagnostic does not change it.",
                      L"Quy trình maintenance tương lai phải bật feature Windows được tài liệu hóa và reboot; chẩn đoán này không tự thay đổi.");
    }
    if (state == UwfState::provider_unavailable) {
        return result("uwf", DiagnosticSeverity::warning,
                      L"Reboot-to-restore support", L"Hỗ trợ khôi phục sau reboot",
                      L"The UWF WMI provider is unavailable on this installation.",
                      L"UWF WMI provider không khả dụng trên installation này.",
                      L"Verify the exact Windows image and optional-feature installation.",
                      L"Kiểm tra image Windows và việc cài optional feature chính xác.");
    }
    if (state == UwfState::inconsistent) {
        return result("uwf", DiagnosticSeverity::failure,
                      L"UWF state", L"Trạng thái UWF",
                      L"Current and next UWF states are inconsistent; recovery is required.",
                      L"Trạng thái UWF hiện tại và kế tiếp không nhất quán; cần recovery.",
                      L"Do not mutate UWF. Use the documented recovery image and technician procedure.",
                      L"Không thay đổi UWF. Dùng recovery image và quy trình kỹ thuật viên đã tài liệu hóa.", 2);
    }
    if (state == UwfState::reboot_pending) {
        return result("uwf", DiagnosticSeverity::warning,
                      L"UWF state", L"Trạng thái UWF",
                      L"UWF configuration is waiting for a Windows restart.",
                      L"Cấu hình UWF đang chờ Windows restart.");
    }
    if (state == UwfState::enabled) {
        return result("uwf", DiagnosticSeverity::pass,
                      L"UWF state", L"Trạng thái UWF",
                      L"UWF is enabled for the current and next session.",
                      L"UWF đang bật cho phiên hiện tại và kế tiếp.");
    }
    return result("uwf", DiagnosticSeverity::pass,
                  L"Reboot-to-restore support", L"Hỗ trợ khôi phục sau reboot",
                  L"Supported edition and UWF provider detected; protection is not enabled.",
                  L"Đã phát hiện edition và provider UWF được hỗ trợ; protection chưa bật.");
}

DiagnosticResult check_safe_mode() {
    const int mode = GetSystemMetrics(SM_CLEANBOOT);
    if (mode != 0) {
        return result("safe_mode", DiagnosticSeverity::failure,
                      L"Safe Mode", L"Safe Mode",
                      L"Windows is running in Safe Mode; normal NSTU service, graphics, and networking are unavailable.",
                      L"Windows đang chạy Safe Mode; service, đồ họa và mạng NSTU bình thường không khả dụng.",
                      L"Restart Windows normally before installation or classroom operation.",
                      L"Khởi động lại Windows ở chế độ bình thường trước khi cài đặt hoặc vận hành lớp học.", 3);
    }
    return result("safe_mode", DiagnosticSeverity::pass, L"Safe Mode",
                  L"Safe Mode", L"Normal Windows boot detected.",
                  L"Đã phát hiện Windows khởi động bình thường.");
}

const NetworkScan* fastest_physical_network(const HardwareScan& hardware) {
    const auto is_usable = [](const NetworkScan& network) {
        // Tunnel adapters (including Tailscale) report overlay capacity, not
        // the negotiated Ethernet/Wi-Fi link used by classroom traffic.
        return network.operational && !network.tunnel &&
               network.interface_type != IF_TYPE_SOFTWARE_LOOPBACK;
    };
    const NetworkScan* fastest = nullptr;
    for (const auto& network : hardware.networks) {
        if (!is_usable(network)) {
            continue;
        }
        if (fastest == nullptr ||
            std::max(network.transmit_link_speed_mbps,
                     network.receive_link_speed_mbps) >
                std::max(fastest->transmit_link_speed_mbps,
                         fastest->receive_link_speed_mbps)) {
            fastest = &network;
        }
    }
    return fastest;
}

DiagnosticResult check_hardware(const HardwareScan& hardware) {
    const auto cpu = classify_processor(hardware.processor.x64,
                                        hardware.processor.physical_cores,
                                        hardware.processor.logical_processors);
    const auto memory = classify_memory_gib(
        hardware.memory.total_physical_bytes / (1024ull * 1024ull * 1024ull));
    const auto* fastest = fastest_physical_network(hardware);
    const std::uint64_t link = fastest == nullptr
        ? 0
        : std::max(fastest->transmit_link_speed_mbps,
                   fastest->receive_link_speed_mbps);
    const auto network = classify_link_speed_mbps(link);
    const bool cpu_failure = cpu == Readiness::minimum_not_met;
    const bool memory_failure = memory == Readiness::minimum_not_met;
    const bool network_failure = network == Readiness::minimum_not_met;
    const bool capacity_failure = cpu_failure || memory_failure;
    const bool failure = network_failure ||
                         (capacity_failure && !kInternalTestBuild);
    const auto severity = failure
        ? DiagnosticSeverity::failure
        : (capacity_failure ? DiagnosticSeverity::warning
                            : DiagnosticSeverity::pass);
    std::wstring detail = hardware.processor.model + L" | " +
                          std::to_wstring(hardware.processor.physical_cores) +
                          L" physical / " +
                          std::to_wstring(hardware.processor.logical_processors) +
                          L" logical cores | " +
                          std::to_wstring(hardware.memory.total_physical_bytes /
                                          (1024ull * 1024ull * 1024ull)) +
                          L" GiB RAM | " + std::to_wstring(link) + L" Mbps link";
    std::wstring remediation =
        L"Install minimum: x64 with at least 4 physical/logical cores, 6 GiB RAM, and a 100 Mbps physical link. 8 GiB RAM and i5-6400-class performance are recommended references; the CPU model name is not required.";
    std::wstring remediation_vi =
        L"Mức tối thiểu để cài: x64 với ít nhất 4 core vật lý/logical, RAM 6 GiB và link vật lý 100 Mbps. Khuyến nghị RAM 8 GiB và hiệu năng tương đương i5-6400; không bắt buộc đúng tên model CPU.";
    if (capacity_failure && kInternalTestBuild && !network_failure) {
        remediation =
            L"INTERNAL VM TEST override: CPU/RAM capacity is below the release minimum, but installation may continue for development testing only.";
        remediation_vi =
            L"Ghi đè INTERNAL VM TEST: CPU/RAM thấp hơn mức tối thiểu của bản phát hành nhưng có thể tiếp tục cài chỉ để kiểm thử phát triển.";
    }
    return result("hardware", severity,
                  L"Hardware readiness", L"Mức đáp ứng phần cứng", detail, detail,
                  std::move(remediation), std::move(remediation_vi),
                  failure ? 4 : 0);
}

DiagnosticResult check_graphics() {
    const auto graphics = scan_graphics();
    const auto severity = graphics.hardware_d3d11_available
        ? DiagnosticSeverity::pass
        : DiagnosticSeverity::warning;
    std::wstring detail = std::to_wstring(graphics.adapters.size()) +
                          L" DXGI adapter(s); D3D11 hardware: " +
                          (graphics.hardware_d3d11_available ? L"available" :
                                                               L"unavailable") +
                          L"; WARP: " +
                          (graphics.warp_d3d11_available ? L"available" :
                                                           L"unavailable");
    return result("graphics", severity, L"Graphics driver", L"Driver đồ họa",
                  detail, detail,
                  L"Install a supported display driver. WARP is a diagnostic fallback; it is not suitable for production streaming.",
                  L"Cài driver màn hình được hỗ trợ. WARP chỉ là dự phòng chẩn đoán; không phù hợp cho streaming production.",
                  graphics.hardware_d3d11_available ? 0 : 5);
}

DiagnosticResult check_encoder(DiagnosticRole role) {
    if (role != DiagnosticRole::server) {
        return result("encoder", DiagnosticSeverity::not_applicable,
                      L"Hardware H.264 encoder", L"Bộ mã hóa H.264 phần cứng",
                      L"Informational for the client role.",
                      L"Chỉ mang tính thông tin với role client.");
    }
    std::string error;
    const auto encoders = scan_hardware_h264_encoders(&error);
    if (encoders.empty()) {
        return result("encoder", DiagnosticSeverity::warning,
                      L"Hardware H.264 encoder", L"Bộ mã hóa H.264 phần cứng",
                      L"No hardware H.264 encoder was found; snapshot mode remains supported.",
                      L"Không tìm thấy bộ mã hóa H.264 phần cứng; chế độ snapshot vẫn được hỗ trợ.",
                      L"Continuous H.264 is deferred and must not block snapshot-first classroom operation.",
                      L"H.264 liên tục đang tạm hoãn và không được chặn vận hành snapshot trong lớp học.");
    }
    return result("encoder", DiagnosticSeverity::pass,
                  L"Hardware H.264 encoder", L"Bộ mã hóa H.264 phần cứng",
                  std::to_wstring(encoders.size()) + L" hardware encoder(s) registered.",
                  std::to_wstring(encoders.size()) + L" bộ mã hóa phần cứng đã đăng ký.");
}

DiagnosticResult check_network(const HardwareScan& hardware) {
    const auto* fastest = fastest_physical_network(hardware);
    if (fastest == nullptr) {
        return result("network", DiagnosticSeverity::failure,
                      L"Network link", L"Kết nối mạng",
                      L"No operational physical Ethernet or Wi-Fi adapter was found; tunnel adapters are not accepted as link-capacity evidence.",
                      L"Không tìm thấy adapter Ethernet hoặc Wi-Fi vật lý đang hoạt động; adapter tunnel không được dùng làm bằng chứng dung lượng link.",
                      L"Connect an Ethernet or Wi-Fi adapter before installation.",
                      L"Kết nối adapter Ethernet hoặc Wi-Fi trước khi cài đặt.", 6);
    }
    const auto link = std::max(fastest->transmit_link_speed_mbps,
                               fastest->receive_link_speed_mbps);
    const auto severity = link < 100 ? DiagnosticSeverity::warning
                                     : DiagnosticSeverity::pass;
    const std::wstring detail = fastest->adapter_name + L": " +
                                std::to_wstring(link) + L" Mbps negotiated link";
    return result("network", severity, L"Network link", L"Kết nối mạng",
                  detail, detail,
                  L"100 Mbps is the minimum; 1 Gbps is recommended for larger rooms. This is link capacity, not an Internet speed test.",
                  L"100 Mbps là mức tối thiểu; khuyến nghị 1 Gbps cho phòng lớn. Đây là dung lượng link, không phải đo tốc độ Internet.",
                  link < 100 ? 7 : 0);
}

DiagnosticResult check_server(const DiagnosticOptions& options) {
    if (options.role != DiagnosticRole::client) {
        return result("server", DiagnosticSeverity::not_applicable,
                      L"NSTU server reachability", L"Khả năng kết nối server NSTU",
                      L"Not required for the server role.",
                      L"Không yêu cầu với role server.");
    }
    if (options.server_address.empty()) {
        return result("server", DiagnosticSeverity::failure,
                      L"NSTU server reachability", L"Khả năng kết nối server NSTU",
                      L"No NSTU server address is configured.",
                      L"Chưa cấu hình địa chỉ server NSTU.",
                      L"Set the school-LAN server address before client enrollment.",
                      L"Đặt địa chỉ server trong mạng trường trước khi enrollment client.", 8);
    }
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        return result("server", DiagnosticSeverity::failure,
                      L"NSTU server reachability", L"Khả năng kết nối server NSTU",
                      L"Winsock initialization failed.", L"Khởi tạo Winsock thất bại.", {}, {}, 9);
    }
    addrinfoW hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfoW* addresses = nullptr;
    const auto service = std::to_wstring(options.server_port);
    const int resolve = GetAddrInfoW(options.server_address.c_str(), service.c_str(),
                                     &hints, &addresses);
    bool connected = false;
    if (resolve == 0) {
        for (auto* address = addresses; address != nullptr && !connected;
             address = address->ai_next) {
            SOCKET socket_handle = socket(address->ai_family,
                                           address->ai_socktype,
                                           address->ai_protocol);
            if (socket_handle == INVALID_SOCKET) {
                continue;
            }
            u_long non_blocking = 1;
            ioctlsocket(socket_handle, FIONBIO, &non_blocking);
            const int connect_result = connect(
                socket_handle, address->ai_addr,
                static_cast<int>(address->ai_addrlen));
            if (connect_result == 0) {
                connected = true;
            } else if (WSAGetLastError() == WSAEWOULDBLOCK) {
                fd_set write_set;
                FD_ZERO(&write_set);
                FD_SET(socket_handle, &write_set);
                timeval timeout{2, 0};
                if (select(0, nullptr, &write_set, nullptr, &timeout) > 0) {
                    int socket_error = 0;
                    int length = sizeof(socket_error);
                    connected = getsockopt(
                        socket_handle, SOL_SOCKET, SO_ERROR,
                        reinterpret_cast<char*>(&socket_error), &length) == 0 &&
                        socket_error == 0;
                }
            }
            closesocket(socket_handle);
        }
    }
    if (addresses != nullptr) {
        FreeAddrInfoW(addresses);
    }
    WSACleanup();
    if (!connected) {
        return result("server", DiagnosticSeverity::failure,
                      L"NSTU server reachability", L"Khả năng kết nối server NSTU",
                      L"The configured NSTU server cannot be reached on TCP " +
                          std::to_wstring(options.server_port) + L".",
                      L"Không thể kết nối server NSTU qua TCP " +
                          std::to_wstring(options.server_port) + L".",
                      L"Verify the school-LAN address, firewall, and server listener.",
                      L"Kiểm tra địa chỉ trong mạng trường, firewall và listener server.", 10);
    }
    return result("server", DiagnosticSeverity::pass,
                  L"NSTU server reachability", L"Khả năng kết nối server NSTU",
                  L"TCP endpoint is reachable.", L"TCP endpoint có thể kết nối.");
}

DiagnosticResult check_time() {
    SYSTEMTIME time{};
    GetSystemTime(&time);
    const bool sane = time.wYear >= 2020 && time.wYear <= 2100;
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    SC_HANDLE service = manager == nullptr
        ? nullptr
        : OpenServiceW(manager, L"W32Time", SERVICE_QUERY_STATUS);
    SERVICE_STATUS status{};
    const bool running = service != nullptr &&
        QueryServiceStatus(service, &status) != FALSE &&
        status.dwCurrentState == SERVICE_RUNNING;
    if (service != nullptr) CloseServiceHandle(service);
    if (manager != nullptr) CloseServiceHandle(manager);
    const auto severity = sane && running ? DiagnosticSeverity::pass
                                          : DiagnosticSeverity::warning;
    return result("time", severity, L"System time", L"Thời gian hệ thống",
                  sane ? (running ? L"System time is sane and Windows Time is running."
                                  : L"System time is sane; Windows Time is not running.")
                       : L"System time is outside the expected deployment range.",
                  sane ? (running ? L"Thời gian hợp lệ và Windows Time đang chạy."
                                  : L"Thời gian hợp lệ; Windows Time chưa chạy.")
                       : L"Thời gian hệ thống nằm ngoài khoảng triển khai dự kiến.",
                  L"Do not adjust time automatically; synchronize it through the school's approved policy.",
                  L"Không tự động chỉnh giờ; đồng bộ theo policy được trường phê duyệt.", sane ? 0 : 11);
}

DiagnosticResult check_internet() {
    ComPtr<INetworkListManager> manager;
    const HRESULT created = CoCreateInstance(
        CLSID_NetworkListManager, nullptr, CLSCTX_ALL,
        IID_PPV_ARGS(&manager));
    if (FAILED(created)) {
        return result("internet", DiagnosticSeverity::not_applicable,
                      L"Public Internet", L"Internet công cộng",
                      L"Public Internet state is unavailable; it is not required for classroom control.",
                      L"Không đọc được trạng thái Internet công cộng; không bắt buộc cho điều khiển lớp học.");
    }
    NLM_CONNECTIVITY connectivity = NLM_CONNECTIVITY_DISCONNECTED;
    const HRESULT queried = manager->GetConnectivity(&connectivity);
    const bool connected = SUCCEEDED(queried) &&
        (connectivity & (NLM_CONNECTIVITY_IPV4_INTERNET |
                         NLM_CONNECTIVITY_IPV6_INTERNET)) != 0;
    const auto severity = connected
        ? DiagnosticSeverity::pass : DiagnosticSeverity::warning;
    return result("internet", severity, L"Public Internet", L"Internet công cộng",
                  connected ? L"Windows reports an Internet connection."
                            : L"No public Internet connection reported; school-LAN control can still operate.",
                  connected ? L"Windows báo có kết nối Internet."
                            : L"Không có Internet công cộng; điều khiển trong mạng trường vẫn có thể hoạt động.");
}

DiagnosticResult check_installation(const DiagnosticOptions& options) {
    std::wstring role;
    const bool has_role = read_reg_string(HKEY_LOCAL_MACHINE, L"Software\\NSTU",
                                          L"InstallRole", role);
    std::wstring location;
    const bool has_location = read_reg_string(
        HKEY_LOCAL_MACHINE,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\NSTU",
        L"InstallLocation", location);
    const std::filesystem::path root = has_location
        ? std::filesystem::path(location)
        : std::filesystem::path();
    const bool client_binary = !root.empty() &&
        std::filesystem::exists(root / L"client" / L"nstu-service.exe");
    const bool server_binary = !root.empty() &&
        std::filesystem::exists(root / L"server" / L"nstu-server.exe");
    const bool conflict = client_binary && server_binary;
    const bool expected = !options.boot_check ||
        (options.role == DiagnosticRole::client ? client_binary : server_binary);
    if (conflict) {
        return result("installation", DiagnosticSeverity::failure,
                      L"Installation integrity", L"Toàn vẹn cài đặt",
                      L"Both Client and Server payloads are installed in one root.",
                      L"Cả payload Client và Server đang được cài trong cùng một root.",
                      L"Uninstall the conflicting role and restart before installing one role.",
                      L"Gỡ role xung đột và restart trước khi cài một role.", 12);
    }
    if (!has_role && !options.boot_check) {
        return result("installation", DiagnosticSeverity::pass,
                      L"Installation integrity", L"Toàn vẹn cài đặt",
                      L"No existing NSTU installation was found; installer preflight may continue.",
                      L"Chưa tìm thấy NSTU đã cài; installer preflight có thể tiếp tục.");
    }
    if (options.boot_check && !expected) {
        return result("installation", DiagnosticSeverity::failure,
                      L"Installation integrity", L"Toàn vẹn cài đặt",
                      L"The expected NSTU role binary is missing.",
                      L"Thiếu binary NSTU của role được yêu cầu.",
                      L"Repair or reinstall the selected role using the signed installer.",
                      L"Sửa chữa hoặc cài lại role bằng installer đã ký.", 13);
    }
    return result("installation", DiagnosticSeverity::pass,
                  L"Installation integrity", L"Toàn vẹn cài đặt",
                  L"NSTU role and installation metadata are consistent.",
                  L"Role NSTU và metadata cài đặt nhất quán.");
}

DiagnosticResult check_service(const DiagnosticOptions& options) {
    if (options.role != DiagnosticRole::client || !options.boot_check) {
        return result("service", DiagnosticSeverity::not_applicable,
                      L"Client service", L"Client service",
                      L"Service verification runs on client boot checks.",
                      L"Kiểm tra service chạy trong boot check của client.");
    }
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    SC_HANDLE service = manager == nullptr
        ? nullptr
        : OpenServiceW(manager, L"nstu-service",
                       SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS);
    if (service == nullptr) {
        if (manager != nullptr) CloseServiceHandle(manager);
        return result("service", DiagnosticSeverity::failure,
                      L"Client service", L"Client service",
                      L"nstu-service is missing or cannot be queried.",
                      L"Thiếu nstu-service hoặc không thể truy vấn.", {}, {}, 14);
    }
    SERVICE_STATUS_PROCESS status{};
    DWORD bytes = 0;
    const bool running = QueryServiceStatusEx(
        service, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&status),
        sizeof(status), &bytes) != FALSE &&
        status.dwCurrentState == SERVICE_RUNNING;
    DWORD required = 0;
    QueryServiceConfigW(service, nullptr, 0, &required);
    bool automatic = false;
    bool local_system = false;
    if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && required > 0) {
        std::vector<std::byte> buffer(required);
        auto* config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buffer.data());
        if (QueryServiceConfigW(service, config, required, &required)) {
            automatic = config->dwStartType == SERVICE_AUTO_START;
            local_system = config->lpServiceStartName != nullptr &&
                (_wcsicmp(config->lpServiceStartName, L"LocalSystem") == 0 ||
                 _wcsicmp(config->lpServiceStartName,
                          L"NT AUTHORITY\\SYSTEM") == 0);
        }
    }
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    const bool ok = running && automatic && local_system;
    return result("service", ok ? DiagnosticSeverity::pass
                                 : DiagnosticSeverity::failure,
                  L"Client service", L"Client service",
                  ok ? L"Automatic LocalSystem service is running in Session 0."
                     : L"nstu-service is not running with the required automatic LocalSystem configuration.",
                  ok ? L"Service LocalSystem tự động đang chạy trong Session 0."
                     : L"nstu-service chưa chạy với cấu hình LocalSystem tự động bắt buộc.", {}, {}, ok ? 0 : 15);
}

DiagnosticResult check_registry() {
    DWORD value = 0;
    DWORD bytes = sizeof(value);
    const LSTATUS status = RegGetValueW(
        HKEY_LOCAL_MACHINE, L"Software\\NSTU", L"ServerPort",
        RRF_RT_REG_DWORD, nullptr, &value, &bytes);
    if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) {
        return result("registry", DiagnosticSeverity::warning,
                      L"NSTU registry", L"Registry NSTU",
                      L"NSTU registry values could not be read completely.",
                      L"Không thể đọc đầy đủ giá trị registry NSTU.", {}, {}, status);
    }
    if (status == ERROR_SUCCESS && (value == 0 || value > 65535)) {
        return result("registry", DiagnosticSeverity::failure,
                      L"NSTU registry", L"Registry NSTU",
                      L"Configured ServerPort is outside 1-65535.",
                      L"ServerPort cấu hình nằm ngoài 1-65535.",
                      L"Correct the configuration through the signed installer; diagnostics does not modify it.",
                      L"Sửa cấu hình bằng installer đã ký; diagnostics không tự thay đổi.", 16);
    }
    return result("registry", DiagnosticSeverity::pass,
                  L"NSTU registry", L"Registry NSTU",
                  L"Documented NSTU registry values are readable.",
                  L"Các giá trị registry NSTU được tài liệu hóa có thể đọc.");
}

} // namespace

bool is_uwf_supported_product(std::uint32_t product_type) noexcept {
    switch (product_type) {
    case kProductEnterprise:
    case kProductEnterpriseN:
    case kProductEnterpriseS:
    case kProductEnterpriseSN:
    case kProductEducation:
    case kProductEducationN:
    case kProductIotEnterprise:
    case kProductIotEnterpriseS:
        return true;
    default:
        return false;
    }
}

UwfState classify_uwf(const UwfProbeSnapshot& snapshot) noexcept {
    if (!is_uwf_supported_product(snapshot.product_type)) {
        return UwfState::unsupported_edition;
    }
    if (!snapshot.feature_known || !snapshot.feature_enabled) {
        return UwfState::feature_missing;
    }
    if (!snapshot.provider_available || !snapshot.filter_state_known) {
        return UwfState::provider_unavailable;
    }
    if (snapshot.current_enabled != snapshot.next_enabled) {
        return UwfState::reboot_pending;
    }
    return snapshot.current_enabled ? UwfState::enabled
                                    : UwfState::available_unconfigured;
}

Readiness classify_memory_gib(std::uint64_t gib) noexcept {
    return gib >= 6 ? Readiness::good : Readiness::minimum_not_met;
}

Readiness classify_link_speed_mbps(std::uint64_t mbps) noexcept {
    if (mbps < 100) {
        return Readiness::minimum_not_met;
    }
    return mbps >= 1000 ? Readiness::good : Readiness::recommended_not_met;
}

Readiness classify_processor(bool x64, std::uint32_t physical,
                             std::uint32_t logical) noexcept {
    if (!x64 || physical < 4 || logical < 4) {
        return Readiness::minimum_not_met;
    }
    return Readiness::good;
}

std::vector<DiagnosticCheck> diagnostic_checks(const DiagnosticOptions&) {
    std::vector<DiagnosticCheck> checks = {
        {"os", L"Operating system", L"Hệ điều hành"},
        {"uwf", L"Reboot-to-restore support", L"Hỗ trợ khôi phục sau reboot"},
        {"safe_mode", L"Safe Mode", L"Safe Mode"},
        {"installation", L"Installation integrity", L"Toàn vẹn cài đặt"},
        {"registry", L"NSTU registry", L"Registry NSTU"},
        {"hardware", L"Hardware readiness", L"Mức đáp ứng phần cứng"},
        {"network", L"Network link", L"Kết nối mạng"},
        {"graphics", L"Graphics driver", L"Driver đồ họa"},
        {"encoder", L"Hardware H.264 encoder", L"Bộ mã hóa H.264 phần cứng"},
        {"time", L"System time", L"Thời gian hệ thống"},
        {"internet", L"Public Internet", L"Internet công cộng"},
        {"server", L"NSTU server reachability", L"Khả năng kết nối server NSTU"},
    };
    checks.push_back({"service", L"Client service", L"Client service"});
    return checks;
}

void run_startup_diagnostics(const DiagnosticOptions& options,
                             const DiagnosticStartSink& on_start,
                             const DiagnosticResultSink& on_result) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitialize = com == S_OK || com == S_FALSE;
    const auto hardware = scan_hardware();
    const auto checks = diagnostic_checks(options);
    auto run = [&](const DiagnosticCheck& check, auto&& function) {
        if (on_start) on_start(check);
        try {
            on_result(function());
        } catch (...) {
            on_result(result(check.id, DiagnosticSeverity::failure,
                             check.title_en.c_str(), check.title_vi.c_str(),
                             L"The diagnostic probe failed unexpectedly.",
                             L"Probe chẩn đoán thất bại ngoài dự kiến.", {}, {}, 0xffff));
        }
    };
    run(checks[0], [] { return check_os(); });
    run(checks[1], [&] { return check_uwf(options.role); });
    run(checks[2], [] { return check_safe_mode(); });
    run(checks[3], [&] { return check_installation(options); });
    run(checks[4], [] { return check_registry(); });
    run(checks[5], [&] { return check_hardware(hardware); });
    run(checks[6], [&] { return check_network(hardware); });
    run(checks[7], [] { return check_graphics(); });
    run(checks[8], [&] { return check_encoder(options.role); });
    run(checks[9], [] { return check_time(); });
    run(checks[10], [] { return check_internet(); });
    run(checks[11], [&] { return check_server(options); });
    run(checks[12], [&] { return check_service(options); });
    if (uninitialize) {
        CoUninitialize();
    }
}

std::string json_escape(std::wstring_view value) {
    const auto utf8 = wide_to_utf8(value);
    std::ostringstream output;
    output << '"';
    for (const unsigned char character : utf8) {
        switch (character) {
        case '"': output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (character < 0x20) {
                output << "\\u" << std::hex << std::setw(4)
                       << std::setfill('0') << static_cast<unsigned int>(character)
                       << std::dec << std::setfill(' ');
            } else {
                output << static_cast<char>(character);
            }
            break;
        }
    }
    output << '"';
    return output.str();
}

const char* severity_name(DiagnosticSeverity severity) noexcept {
    switch (severity) {
    case DiagnosticSeverity::pass: return "pass";
    case DiagnosticSeverity::warning: return "warning";
    case DiagnosticSeverity::failure: return "failure";
    case DiagnosticSeverity::not_applicable: return "not_applicable";
    }
    return "unknown";
}

bool write_diagnostic_report_json(
    const std::filesystem::path& path, const DiagnosticOptions& options,
    const std::vector<DiagnosticResult>& results, std::string* error) {
    std::error_code directory_error;
    if (const auto parent = path.parent_path(); !parent.empty()) {
        std::filesystem::create_directories(parent, directory_error);
        if (directory_error) {
            set_error(error, "diagnostic report directory could not be created");
            return false;
        }
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        set_error(error, "diagnostic report could not be opened");
        return false;
    }
    output << "{\n  \"role\": "
            << (options.role == DiagnosticRole::client ? "\"client\"" :
                                                          "\"server\"")
            << ",\n  \"build_channel\": "
            << (kInternalTestBuild ? "\"internal_vm_test\"" : "\"release\"")
            << ",\n  \"server_port\": " << options.server_port
            << ",\n  \"results\": [\n";
    for (std::size_t index = 0; index < results.size(); ++index) {
        const auto& item = results[index];
        output << "    {\"id\": " << json_escape(utf8_to_wide(item.id))
                << ", \"severity\": \"" << severity_name(item.severity)
                << "\", \"detail_en\": " << json_escape(item.detail_en)
                << ", \"detail_vi\": " << json_escape(item.detail_vi)
                << ", \"error_code\": " << item.error_code << "}";
        if (index + 1 != results.size()) output << ',';
        output << '\n';
    }
    output << "  ]\n}\n";
    output.flush();
    if (!output) {
        set_error(error, "diagnostic report write failed");
        return false;
    }
    return true;
}

} // namespace nstu::setup
