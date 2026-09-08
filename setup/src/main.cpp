#include "nstu/setup/driver_scan.hpp"
#include "nstu/setup/hardware_scan.hpp"
#include "nstu/deployment.hpp"
#include "nstu/protocol.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mfapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <versionhelpers.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr wchar_t kWindowClass[] = L"NstuDiagnosticsWindow";
constexpr UINT_PTR kStepTimer = 1;
constexpr UINT_PTR kCloseTimer = 2;
constexpr int kListId = 1001;
constexpr int kStatusId = 1002;
constexpr int kCloseId = 1003;

enum class Role { client, server };
enum class ResultKind { pass, warning, failure };

struct Options {
    Role role = Role::client;
    std::wstring server_address;
    std::uint16_t server_port = 47001;
    bool installer = false;
    bool boot_check = false;
    bool auto_close = false;
    std::filesystem::path log_path;
};

struct CheckResult {
    std::wstring name;
    ResultKind kind = ResultKind::pass;
    std::wstring detail;
};

Options g_options;
HWND g_window = nullptr;
HWND g_list = nullptr;
HWND g_status = nullptr;
std::vector<std::function<CheckResult()>> g_checks;
std::vector<CheckResult> g_results;
std::size_t g_next_check = 0;
bool g_failed = false;
bool g_has_warning = false;
bool g_complete = false;

std::optional<std::wstring> argument_value(int argc, wchar_t** argv,
                                           std::wstring_view prefix) {
    for (int index = 1; index < argc; ++index) {
        const std::wstring_view argument(argv[index]);
        if (argument.starts_with(prefix)) {
            return std::wstring(argument.substr(prefix.size()));
        }
    }
    return std::nullopt;
}

bool has_argument(int argc, wchar_t** argv, std::wstring_view value) {
    const std::wstring expected(value);
    for (int index = 1; index < argc; ++index) {
        if (_wcsicmp(argv[index], expected.c_str()) == 0) {
            return true;
        }
    }
    return false;
}

std::wstring read_registry_string(const wchar_t* name) {
    wchar_t value[512]{};
    DWORD bytes = sizeof(value);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"Software\\NSTU", name,
                     RRF_RT_REG_SZ, nullptr, value, &bytes) != ERROR_SUCCESS) {
        return {};
    }
    return value;
}

std::uint16_t read_registry_port() {
    DWORD value = 0;
    DWORD bytes = sizeof(value);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"Software\\NSTU", L"ServerPort",
                     RRF_RT_REG_DWORD, nullptr, &value, &bytes) !=
            ERROR_SUCCESS ||
        value == 0 || value > 65535) {
        return 47001;
    }
    return static_cast<std::uint16_t>(value);
}

Options parse_options() {
    Options options;
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv == nullptr) {
        return options;
    }
    if (const auto target = argument_value(argc, argv, L"--target="); target &&
        _wcsicmp(target->c_str(), L"server") == 0) {
        options.role = Role::server;
    }
    if (const auto server = argument_value(argc, argv, L"--server-ip=")) {
        options.server_address = *server;
    }
    if (const auto port = argument_value(argc, argv, L"--server-port=")) {
        wchar_t* end = nullptr;
        const unsigned long parsed = wcstoul(port->c_str(), &end, 10);
        if (end != port->c_str() && *end == L'\0' && parsed > 0 &&
            parsed <= 65535) {
            options.server_port = static_cast<std::uint16_t>(parsed);
        }
    }
    if (const auto log = argument_value(argc, argv, L"--log=")) {
        options.log_path = *log;
    }
    options.installer = has_argument(argc, argv, L"--installer");
    options.boot_check = has_argument(argc, argv, L"--boot-check");
    options.auto_close = has_argument(argc, argv, L"--auto-close");
    LocalFree(argv);
    if (options.role == Role::client && options.server_address.empty()) {
        options.server_address = read_registry_string(L"ServerAddress");
        options.server_port = read_registry_port();
    }
    return options;
}

bool is_administrator() {
    BOOL member = FALSE;
    SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
    PSID administrators = nullptr;
    if (!AllocateAndInitializeSid(&authority, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                  DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0,
                                  &administrators)) {
        return false;
    }
    const BOOL queried = CheckTokenMembership(nullptr, administrators, &member);
    FreeSid(administrators);
    return queried != FALSE && member != FALSE;
}

CheckResult check_privilege() {
    if (!g_options.installer) {
        return {L"Security context", ResultKind::pass,
                L"Boot diagnostics run in the signed-in user's session."};
    }
    if (!is_administrator()) {
        return {L"Administrator token", ResultKind::failure,
                L"Installation diagnostics require elevation."};
    }
    return {L"Administrator token", ResultKind::pass,
            L"Elevated installer context confirmed."};
}

CheckResult check_os() {
    if (!IsWindows10OrGreater() || sizeof(void*) != 8) {
        return {L"Operating system", ResultKind::failure,
                L"NSTU requires 64-bit Windows 10 or Windows 11."};
    }
    return {L"Operating system", ResultKind::pass,
            L"Supported 64-bit Windows version detected."};
}

CheckResult check_display() {
    const auto hardware = nstu::setup::scan_hardware();
    if (hardware.display.width == 0 || hardware.display.height == 0) {
        return {L"Display session", ResultKind::failure,
                L"No active display mode was detected."};
    }
    return {L"Display session", ResultKind::pass,
            std::to_wstring(hardware.display.width) + L" x " +
                std::to_wstring(hardware.display.height) + L" @ " +
                std::to_wstring(hardware.display.refresh_hz) + L" Hz"};
}

CheckResult check_network() {
    const auto hardware = nstu::setup::scan_hardware();
    if (hardware.networks.empty()) {
        return {L"Network adapter", ResultKind::failure,
                L"No operational non-loopback adapter was found."};
    }
    const auto fastest = std::max_element(
        hardware.networks.begin(), hardware.networks.end(),
        [](const auto& left, const auto& right) {
            return left.link_speed_mbps < right.link_speed_mbps;
        });
    const std::wstring detail = fastest->adapter_name + L": " +
                                std::to_wstring(fastest->link_speed_mbps) +
                                L" Mbps";
    return {L"Network adapter",
            fastest->link_speed_mbps < 100 ? ResultKind::warning
                                           : ResultKind::pass,
            detail};
}

CheckResult check_encoder() {
    if (g_options.role != Role::server) {
        return {L"Video encoder", ResultKind::pass,
                L"Client role does not require a hardware encoder."};
    }
    std::string error;
    const auto encoders = nstu::setup::scan_hardware_h264_encoders(&error);
    if (encoders.empty()) {
        return {L"Hardware H.264 encoder", ResultKind::warning,
                L"No hardware encoder was found; software fallback may use more CPU."};
    }
    return {L"Hardware H.264 encoder", ResultKind::pass,
            std::to_wstring(encoders.size()) + L" compatible encoder(s) found."};
}

enum class ServerProbeResult { unreachable, incompatible, confirmed };

bool send_all(SOCKET socket_handle, std::span<const std::byte> bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const int sent = send(
            socket_handle,
            reinterpret_cast<const char*>(bytes.data() + offset),
            static_cast<int>(bytes.size() - offset), 0);
        if (sent <= 0) {
            return false;
        }
        offset += static_cast<std::size_t>(sent);
    }
    return true;
}

bool receive_exact(SOCKET socket_handle, std::span<std::byte> bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const int received = recv(
            socket_handle, reinterpret_cast<char*>(bytes.data() + offset),
            static_cast<int>(bytes.size() - offset), 0);
        if (received <= 0) {
            return false;
        }
        offset += static_cast<std::size_t>(received);
    }
    return true;
}

ServerProbeResult probe_nstu_server(const std::wstring& host,
                                    std::uint16_t port) {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        return ServerProbeResult::unreachable;
    }
    addrinfoW hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfoW* addresses = nullptr;
    const std::wstring service = std::to_wstring(port);
    if (GetAddrInfoW(host.c_str(), service.c_str(), &hints, &addresses) != 0) {
        WSACleanup();
        return ServerProbeResult::unreachable;
    }
    bool endpoint_reached = false;
    ServerProbeResult probe_result = ServerProbeResult::unreachable;
    for (auto* address = addresses; address != nullptr;
         address = address->ai_next) {
        SOCKET socket_handle = socket(address->ai_family, address->ai_socktype,
                                      address->ai_protocol);
        if (socket_handle == INVALID_SOCKET) {
            continue;
        }
        u_long non_blocking = 1;
        if (ioctlsocket(socket_handle, FIONBIO, &non_blocking) != 0) {
            closesocket(socket_handle);
            continue;
        }
        const int result = connect(socket_handle, address->ai_addr,
                                   static_cast<int>(address->ai_addrlen));
        bool connected = result == 0;
        if (!connected && WSAGetLastError() == WSAEWOULDBLOCK) {
            fd_set write_set;
            FD_ZERO(&write_set);
            FD_SET(socket_handle, &write_set);
            timeval timeout{2, 0};
            if (select(0, nullptr, &write_set, nullptr, &timeout) > 0) {
                int socket_error = 0;
                int length = sizeof(socket_error);
                connected = getsockopt(socket_handle, SOL_SOCKET, SO_ERROR,
                                       reinterpret_cast<char*>(&socket_error),
                                       &length) == 0 && socket_error == 0;
            }
        }
        if (connected) {
            endpoint_reached = true;
            non_blocking = 0;
            const DWORD timeout_ms = 2000;
            if (ioctlsocket(socket_handle, FIONBIO, &non_blocking) == 0 &&
                setsockopt(socket_handle, SOL_SOCKET, SO_RCVTIMEO,
                           reinterpret_cast<const char*>(&timeout_ms),
                           sizeof(timeout_ms)) == 0 &&
                setsockopt(socket_handle, SOL_SOCKET, SO_SNDTIMEO,
                           reinterpret_cast<const char*>(&timeout_ms),
                           sizeof(timeout_ms)) == 0) {
                nstu::protocol::ConnectionPreamble request;
                request.role = nstu::protocol::ConnectionRole::diagnostic;
                const auto wire =
                    nstu::protocol::encode_connection_preamble(request);
                std::array<std::byte,
                           nstu::protocol::kConnectionPreambleBytes>
                    response{};
                if (!wire.empty() && send_all(socket_handle, wire) &&
                    receive_exact(socket_handle, response)) {
                    const auto decoded =
                        nstu::protocol::decode_connection_preamble(response);
                    if (decoded && decoded->role ==
                                       nstu::protocol::ConnectionRole::server) {
                        probe_result = ServerProbeResult::confirmed;
                    } else {
                        probe_result = ServerProbeResult::incompatible;
                    }
                } else {
                    probe_result = ServerProbeResult::incompatible;
                }
            } else {
                probe_result = ServerProbeResult::incompatible;
            }
        }
        closesocket(socket_handle);
        if (probe_result == ServerProbeResult::confirmed) {
            break;
        }
    }
    FreeAddrInfoW(addresses);
    WSACleanup();
    return endpoint_reached ? probe_result : ServerProbeResult::unreachable;
}

CheckResult check_server() {
    if (g_options.role != Role::client) {
        return {L"Server connectivity", ResultKind::pass,
                L"Not required for the server role."};
    }
    if (g_options.server_address.empty()) {
        return {L"Server configuration", ResultKind::failure,
                L"No server address is configured."};
    }
    const auto probe =
        probe_nstu_server(g_options.server_address, g_options.server_port);
    if (probe == ServerProbeResult::unreachable) {
        return {L"Server connectivity", ResultKind::failure,
                L"Cannot connect to " + g_options.server_address + L":" +
                    std::to_wstring(g_options.server_port) + L"."};
    }
    if (probe != ServerProbeResult::confirmed) {
        return {L"NSTU protocol", ResultKind::failure,
                L"The endpoint at " + g_options.server_address + L":" +
                    std::to_wstring(g_options.server_port) +
                    L" did not return a valid NSTU server preamble."};
    }
    return {L"Server connectivity", ResultKind::pass,
            L"NSTU server confirmed at " + g_options.server_address + L":" +
                std::to_wstring(g_options.server_port) + L"."};
}

CheckResult check_client_configuration() {
    if (g_options.role != Role::client) {
        return {L"Client enrollment", ResultKind::pass,
                L"Not required for the server role."};
    }
    if (!g_options.boot_check) {
        return {L"Client enrollment", ResultKind::pass,
                L"Protected runtime configuration is verified after restart."};
    }
    std::string error;
    const auto root = nstu::deployment::data_root(&error);
    if (root.empty()) {
        return {L"Client enrollment", ResultKind::failure,
                L"The protected NSTU data root is unavailable."};
    }
    const auto config_path = root / L"client-config.bin";
    const DWORD attributes = GetFileAttributesW(config_path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return {L"Client enrollment", ResultKind::failure,
                L"No protected client configuration was found. Run "
                L"nstu-provision.exe while the machine is Thawed."};
    }
    return {L"Client enrollment", ResultKind::pass,
            L"Protected client runtime configuration is present."};
}

struct ServiceSnapshot {
    bool exists = false;
    bool running = false;
    bool auto_start = false;
    bool local_system = false;
    bool session_zero = false;
};

ServiceSnapshot query_service() {
    ServiceSnapshot result;
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (manager == nullptr) {
        return result;
    }
    SC_HANDLE service = OpenServiceW(manager, L"nstu-service",
                                     SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS);
    if (service == nullptr) {
        CloseServiceHandle(manager);
        return result;
    }
    result.exists = true;
    SERVICE_STATUS_PROCESS status{};
    DWORD bytes = 0;
    if (QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
                             reinterpret_cast<BYTE*>(&status), sizeof(status),
                             &bytes)) {
        result.running = status.dwCurrentState == SERVICE_RUNNING;
        DWORD session_id = 0;
        result.session_zero = status.dwProcessId != 0 &&
                              ProcessIdToSessionId(status.dwProcessId,
                                                   &session_id) &&
                              session_id == 0;
    }
    DWORD required = 0;
    QueryServiceConfigW(service, nullptr, 0, &required);
    if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && required > 0) {
        std::vector<std::byte> buffer(required);
        auto* config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buffer.data());
        if (QueryServiceConfigW(service, config, required, &required)) {
            result.auto_start = config->dwStartType == SERVICE_AUTO_START;
            if (config->lpServiceStartName != nullptr) {
                result.local_system =
                    _wcsicmp(config->lpServiceStartName, L"LocalSystem") == 0 ||
                    _wcsicmp(config->lpServiceStartName,
                             L"NT AUTHORITY\\SYSTEM") == 0;
            }
        }
    }
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return result;
}

CheckResult check_service() {
    if (!g_options.boot_check || g_options.role != Role::client) {
        return {L"Client service", ResultKind::pass,
                L"Service verification begins after the required restart."};
    }
    ServiceSnapshot service;
    for (int attempt = 0; attempt < 20; ++attempt) {
        service = query_service();
        if (service.running || !service.exists) {
            break;
        }
        Sleep(250);
    }
    if (!service.exists || !service.running || !service.auto_start ||
        !service.local_system || !service.session_zero) {
        return {L"Client service", ResultKind::failure,
                L"nstu-service must start automatically and run in Session 0 "
                L"as LocalSystem."};
    }
    return {L"Client service", ResultKind::pass,
            L"Automatic LocalSystem service is running."};
}

std::filesystem::path installed_agent_path() {
    wchar_t install_root[1024]{};
    DWORD install_root_bytes = sizeof(install_root);
    if (RegGetValueW(
            HKEY_LOCAL_MACHINE,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\NSTU",
            L"InstallLocation", RRF_RT_REG_SZ, nullptr, install_root,
            &install_root_bytes) == ERROR_SUCCESS &&
        install_root[0] != L'\0') {
        return std::filesystem::path(install_root) / L"client" /
               L"nstu-agent.exe";
    }
    wchar_t program_files[MAX_PATH]{};
    if (SHGetFolderPathW(nullptr, CSIDL_PROGRAM_FILES, nullptr,
                         SHGFP_TYPE_CURRENT, program_files) != S_OK) {
        return {};
    }
    return std::filesystem::path(program_files) / L"NSTU" / L"client" /
           L"nstu-agent.exe";
}

bool agent_in_current_session() {
    DWORD current_session = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &current_session) ||
        current_session == 0) {
        return false;
    }
    const auto expected = installed_agent_path();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return false;
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool found = false;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, L"nstu-agent.exe") != 0) {
                continue;
            }
            DWORD session = 0;
            if (!ProcessIdToSessionId(entry.th32ProcessID, &session) ||
                session != current_session) {
                continue;
            }
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                                         FALSE, entry.th32ProcessID);
            if (process == nullptr) {
                continue;
            }
            std::array<wchar_t, 32768> path{};
            DWORD length = static_cast<DWORD>(path.size());
            if (QueryFullProcessImageNameW(process, 0, path.data(), &length)) {
                std::error_code error;
                found = std::filesystem::equivalent(
                    expected, std::filesystem::path(path.data()), error) &&
                        !error;
            }
            CloseHandle(process);
        } while (!found && Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
}

CheckResult check_agent() {
    if (!g_options.boot_check || g_options.role != Role::client) {
        return {L"Interactive agent", ResultKind::pass,
                L"Agent verification begins at first logon after restart."};
    }
    if (!agent_in_current_session()) {
        return {L"Interactive agent", ResultKind::failure,
                L"The installed agent is not running in this interactive session."};
    }
    return {L"Interactive agent", ResultKind::pass,
            L"Agent is running in the signed-in user's session."};
}

std::wstring result_line(const CheckResult& result) {
    const wchar_t* prefix = L"[PASS]";
    if (result.kind == ResultKind::warning) {
        prefix = L"[WARN]";
    } else if (result.kind == ResultKind::failure) {
        prefix = L"[FAIL]";
    }
    return std::wstring(prefix) + L" " + result.name + L" - " + result.detail;
}

void append_result(const CheckResult& result) {
    g_results.push_back(result);
    g_failed = g_failed || result.kind == ResultKind::failure;
    g_has_warning = g_has_warning || result.kind == ResultKind::warning;
    const std::wstring line = result_line(result);
    SendMessageW(g_list, LB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(line.c_str()));
    const LRESULT count = SendMessageW(g_list, LB_GETCOUNT, 0, 0);
    if (count > 0) {
        SendMessageW(g_list, LB_SETTOPINDEX, count - 1, 0);
    }
}

void write_log() {
    if (g_options.log_path.empty()) {
        return;
    }
    std::wofstream output(g_options.log_path, std::ios::trunc);
    if (!output) {
        return;
    }
    for (const auto& result : g_results) {
        output << result_line(result) << L'\n';
    }
}

void finish_checks() {
    g_complete = true;
    write_log();
    SetWindowTextW(g_status,
                   g_failed || g_has_warning
                       ? L"Warnings or errors were found. This window will remain open."
                       : L"All required checks passed.");
    EnableWindow(GetDlgItem(g_window, kCloseId), TRUE);
    if (!g_failed && !g_has_warning && g_options.auto_close) {
        SetTimer(g_window, kCloseTimer, 1400, nullptr);
    }
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam,
                             LPARAM lparam) {
    switch (message) {
    case WM_CREATE: {
        HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        CreateWindowExW(0, L"STATIC", L"NSTU system diagnostics",
                        WS_CHILD | WS_VISIBLE, 16, 14, 600, 24, window,
                        nullptr, nullptr, nullptr);
        g_list = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"LISTBOX", nullptr,
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOINTEGRALHEIGHT,
            16, 46, 700, 260, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kListId)), nullptr,
            nullptr);
        g_status = CreateWindowExW(
            0, L"STATIC", L"Starting checks...", WS_CHILD | WS_VISIBLE,
            16, 320, 580, 40, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kStatusId)), nullptr,
            nullptr);
        HWND close = CreateWindowExW(
            0, L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            626, 320, 90, 30, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCloseId)), nullptr,
            nullptr);
        for (HWND child : {g_list, g_status, close}) {
            SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        }
        EnableWindow(close, FALSE);
        SetTimer(window, kStepTimer, 220, nullptr);
        return 0;
    }
    case WM_TIMER:
        if (wparam == kStepTimer) {
            if (g_next_check < g_checks.size()) {
                append_result(g_checks[g_next_check++]());
                const std::wstring progress =
                    L"Running check " + std::to_wstring(g_next_check) + L" of " +
                    std::to_wstring(g_checks.size()) + L"...";
                SetWindowTextW(g_status, progress.c_str());
            }
            if (g_next_check == g_checks.size()) {
                KillTimer(window, kStepTimer);
                finish_checks();
            }
            return 0;
        }
        if (wparam == kCloseTimer) {
            DestroyWindow(window);
            return 0;
        }
        break;
    case WM_COMMAND:
        if (LOWORD(wparam) == kCloseId && HIWORD(wparam) == BN_CLICKED) {
            DestroyWindow(window);
            return 0;
        }
        break;
    case WM_CLOSE:
        if (g_complete) {
            DestroyWindow(window);
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(g_failed ? 1 : 0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, wchar_t*, int show) {
    g_options = parse_options();
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitialize_com = SUCCEEDED(com);
    const HRESULT media = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(media)) {
        MessageBoxW(nullptr, L"Windows Media Foundation initialization failed.",
                    L"NSTU diagnostics", MB_OK | MB_ICONERROR);
        if (uninitialize_com) {
            CoUninitialize();
        }
        return 1;
    }
    g_checks = {check_privilege, check_os, check_display, check_network,
                check_encoder, check_server, check_client_configuration,
                check_service, check_agent};

    WNDCLASSW window_class{};
    window_class.hInstance = instance;
    window_class.lpfnWndProc = window_proc;
    window_class.lpszClassName = kWindowClass;
    window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    window_class.hbrBackground =
        static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
    if (!RegisterClassW(&window_class)) {
        MFShutdown();
        if (uninitialize_com) {
            CoUninitialize();
        }
        return 1;
    }
    g_window = CreateWindowExW(
        WS_EX_APPWINDOW, kWindowClass, L"NSTU Diagnostics",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 750, 410, nullptr, nullptr, instance,
        nullptr);
    if (g_window == nullptr) {
        MFShutdown();
        if (uninitialize_com) {
            CoUninitialize();
        }
        return 1;
    }
    ShowWindow(g_window, show == 0 ? SW_SHOWNORMAL : show);
    UpdateWindow(g_window);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    MFShutdown();
    if (uninitialize_com) {
        CoUninitialize();
    }
    return static_cast<int>(message.wParam);
}
