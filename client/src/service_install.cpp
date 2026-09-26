#include "nstu/service_install.hpp"

#include "nstu/client_freeze.hpp"
#include "nstu/session.hpp"

#include <windows.h>
#include <tlhelp32.h>

#include <array>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nstu::client {
namespace {

void set_error(std::string* error, std::string message) {
    if (error != nullptr) *error = std::move(message);
}

std::string narrow(std::wstring_view value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0,
        nullptr, nullptr);
    if (size <= 0) return {};
    std::string output(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, value.data(),
                            static_cast<int>(value.size()), output.data(), size,
                            nullptr, nullptr) != size) {
        return {};
    }
    return output;
}

std::string win32_error(std::string_view operation, DWORD code) {
    std::array<wchar_t, 512> buffer{};
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
        code, 0, buffer.data(), static_cast<DWORD>(buffer.size()), nullptr);
    std::wstring detail(buffer.data(), length);
    while (!detail.empty() &&
           (detail.back() == L'\r' || detail.back() == L'\n' ||
            detail.back() == L' ' || detail.back() == L'.')) {
        detail.pop_back();
    }
    std::string message(operation);
    message += " failed (Win32 ";
    message += std::to_string(code);
    message += ')';
    if (!detail.empty()) {
        message += ": ";
        message += narrow(detail);
    }
    return message;
}

std::wstring current_executable() {
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) return {};
    path.resize(length);
    return path;
}

std::wstring normalized_path(const std::filesystem::path& path) {
    std::error_code error;
    auto normalized = std::filesystem::weakly_canonical(path, error);
    if (error) normalized = path.lexically_normal();
    auto value = normalized.wstring();
    if (value.starts_with(L"\\\\?\\")) value.erase(0, 4);
    while (value.size() > 3 &&
           (value.back() == L'\\' || value.back() == L'/')) {
        value.pop_back();
    }
    return value;
}

bool paths_equal(const std::filesystem::path& left,
                 const std::filesystem::path& right) {
    const auto a = normalized_path(left);
    const auto b = normalized_path(right);
    return !a.empty() && !b.empty() && _wcsicmp(a.c_str(), b.c_str()) == 0;
}

std::filesystem::path service_binary_path(std::wstring_view command_line) {
    if (command_line.empty()) return {};
    if (command_line.front() == L'"') {
        const auto closing = command_line.find(L'"', 1);
        if (closing == std::wstring_view::npos) return {};
        return std::filesystem::path(command_line.substr(1, closing - 1));
    }
    const auto space = command_line.find(L' ');
    return std::filesystem::path(command_line.substr(0, space));
}

bool query_service_image(SC_HANDLE service, std::filesystem::path& image,
                         std::string* error) {
    DWORD required = 0;
    SetLastError(ERROR_SUCCESS);
    (void)QueryServiceConfigW(service, nullptr, 0, &required);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || required == 0) {
        set_error(error,
                  win32_error("query NSTU service image path", GetLastError()));
        return false;
    }
    std::vector<std::byte> buffer(required);
    auto* config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buffer.data());
    if (!QueryServiceConfigW(service, config, required, &required)) {
        set_error(error,
                  win32_error("query NSTU service image path", GetLastError()));
        return false;
    }
    image = service_binary_path(config->lpBinaryPathName == nullptr
                                    ? std::wstring_view{}
                                    : std::wstring_view(config->lpBinaryPathName));
    if (image.empty()) {
        set_error(error, "NSTU service image path is empty or malformed");
        return false;
    }
    return true;
}

bool wait_for_state(SC_HANDLE service, DWORD wanted, DWORD timeout_ms) {
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    SERVICE_STATUS_PROCESS status{};
    DWORD bytes = 0;
    do {
        if (!QueryServiceStatusEx(
                service, SC_STATUS_PROCESS_INFO,
                reinterpret_cast<BYTE*>(&status), sizeof(status), &bytes)) {
            return false;
        }
        if (status.dwCurrentState == wanted) return true;
        Sleep(100);
    } while (GetTickCount64() < deadline);
    return false;
}

struct FrozenValueSnapshot {
    DWORD type = REG_NONE;
    std::vector<std::byte> data;
};

bool snapshot_frozen_value(FrozenValueSnapshot& snapshot, std::string* error) {
    HKEY key = nullptr;
    LONG status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\NSTU", 0,
                                KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key);
    if (status != ERROR_SUCCESS) {
        set_error(error, win32_error("open NSTU Managed-mode state", status));
        return false;
    }
    DWORD bytes = 0;
    status = RegQueryValueExW(key, L"Frozen", nullptr, &snapshot.type, nullptr,
                              &bytes);
    if (status == ERROR_SUCCESS) {
        snapshot.data.resize(bytes);
        status = RegQueryValueExW(
            key, L"Frozen", nullptr, &snapshot.type,
            snapshot.data.empty()
                ? nullptr
                : reinterpret_cast<BYTE*>(snapshot.data.data()),
            &bytes);
    }
    RegCloseKey(key);
    if (status != ERROR_SUCCESS) {
        set_error(error, win32_error("read NSTU Managed-mode state", status));
        return false;
    }
    snapshot.data.resize(bytes);
    return true;
}

bool restore_frozen_value(const FrozenValueSnapshot& snapshot,
                          std::string* error) {
    HKEY key = nullptr;
    LONG status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\NSTU", 0,
                                KEY_SET_VALUE | KEY_WOW64_64KEY, &key);
    if (status == ERROR_SUCCESS) {
        status = RegSetValueExW(
            key, L"Frozen", 0, snapshot.type,
            snapshot.data.empty()
                ? nullptr
                : reinterpret_cast<const BYTE*>(snapshot.data.data()),
            static_cast<DWORD>(snapshot.data.size()));
        RegCloseKey(key);
    }
    if (status != ERROR_SUCCESS) {
        set_error(error,
                  win32_error("restore NSTU Managed-mode state", status));
        return false;
    }
    return true;
}

bool configure_recovery(SC_HANDLE service, std::string* error) {
    SERVICE_DESCRIPTIONW description{
        const_cast<wchar_t*>(L"NSTU classroom client service")};
    if (!ChangeServiceConfig2W(service, SERVICE_CONFIG_DESCRIPTION,
                               &description)) {
        set_error(error, win32_error("configure NSTU service description",
                                     GetLastError()));
        return false;
    }

    std::array<SC_ACTION, 3> actions{
        SC_ACTION{SC_ACTION_RESTART, 5000},
        SC_ACTION{SC_ACTION_RESTART, 15000},
        SC_ACTION{SC_ACTION_RESTART, 60000},
    };
    SERVICE_FAILURE_ACTIONSW failures{};
    failures.dwResetPeriod = 86400;
    failures.cActions = static_cast<DWORD>(actions.size());
    failures.lpsaActions = actions.data();
    if (!ChangeServiceConfig2W(service, SERVICE_CONFIG_FAILURE_ACTIONS,
                               &failures)) {
        set_error(error, win32_error("configure NSTU service restart actions",
                                     GetLastError()));
        return false;
    }

    SERVICE_FAILURE_ACTIONS_FLAG flag{TRUE};
    if (!ChangeServiceConfig2W(service,
                               SERVICE_CONFIG_FAILURE_ACTIONS_FLAG, &flag)) {
        set_error(error,
                  win32_error("enable NSTU service non-crash recovery",
                              GetLastError()));
        return false;
    }
    return true;
}

bool verify_recovery(SC_HANDLE service, std::string* error) {
    DWORD required = 0;
    SetLastError(ERROR_SUCCESS);
    (void)QueryServiceConfig2W(service, SERVICE_CONFIG_FAILURE_ACTIONS, nullptr,
                               0, &required);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || required == 0) {
        set_error(error, win32_error("query NSTU service restart actions",
                                     GetLastError()));
        return false;
    }
    std::vector<std::byte> buffer(required);
    if (!QueryServiceConfig2W(
            service, SERVICE_CONFIG_FAILURE_ACTIONS,
            reinterpret_cast<BYTE*>(buffer.data()), required, &required)) {
        set_error(error, win32_error("query NSTU service restart actions",
                                     GetLastError()));
        return false;
    }
    const auto* failures =
        reinterpret_cast<const SERVICE_FAILURE_ACTIONSW*>(buffer.data());
    constexpr std::array<DWORD, 3> delays{5000, 15000, 60000};
    if (failures->dwResetPeriod != 86400 ||
        failures->cActions != static_cast<DWORD>(delays.size()) ||
        failures->lpsaActions == nullptr) {
        set_error(error, "NSTU service restart actions did not persist");
        return false;
    }
    for (std::size_t index = 0; index < delays.size(); ++index) {
        if (failures->lpsaActions[index].Type != SC_ACTION_RESTART ||
            failures->lpsaActions[index].Delay != delays[index]) {
            set_error(error, "NSTU service restart actions did not persist");
            return false;
        }
    }

    SERVICE_FAILURE_ACTIONS_FLAG flag{};
    required = 0;
    if (!QueryServiceConfig2W(
            service, SERVICE_CONFIG_FAILURE_ACTIONS_FLAG,
            reinterpret_cast<BYTE*>(&flag), sizeof(flag), &required)) {
        set_error(error,
                  win32_error("query NSTU service non-crash recovery",
                              GetLastError()));
        return false;
    }
    if (flag.fFailureActionsOnNonCrashFailures == FALSE) {
        set_error(error, "NSTU service non-crash recovery did not persist");
        return false;
    }
    return true;
}

bool stop_service(SC_HANDLE service, bool allow_managed, std::string* error) {
    std::optional<FrozenValueSnapshot> frozen;
    if (allow_managed && machine_frozen()) {
        frozen.emplace();
        if (!snapshot_frozen_value(*frozen, error) ||
            !set_machine_frozen(false, {}, error)) {
            return false;
        }
        SERVICE_STATUS status{};
        if (!ControlService(service, static_cast<DWORD>(kFreezeReloadControl),
                            &status)) {
            const DWORD code = GetLastError();
            std::string restore_error;
            if (!restore_frozen_value(*frozen, &restore_error)) {
                set_error(error, "NSTU Managed mode could not be restored: " +
                                     restore_error);
            } else {
                set_error(error,
                          win32_error("reload NSTU Managed-mode state", code));
            }
            return false;
        }
    }

    SERVICE_STATUS status{};
    DWORD stop_error = ERROR_SUCCESS;
    bool stopped = false;
    const ULONGLONG deadline = GetTickCount64() + 5000;
    do {
        stopped = ControlService(service, SERVICE_CONTROL_STOP, &status) != FALSE;
        stop_error = stopped ? ERROR_SUCCESS : GetLastError();
        if (stopped || stop_error == ERROR_SERVICE_NOT_ACTIVE) {
            stopped = true;
            break;
        }
        if (stop_error != ERROR_SERVICE_CANNOT_ACCEPT_CTRL) break;
        Sleep(100);
    } while (GetTickCount64() < deadline);
    if (stopped) stopped = wait_for_state(service, SERVICE_STOPPED, 15000);

    if (frozen) {
        std::string restore_error;
        if (!restore_frozen_value(*frozen, &restore_error)) {
            set_error(error, "NSTU Managed mode could not be restored: " +
                                 restore_error);
            return false;
        }
    }
    if (!stopped) {
        if (stop_error != ERROR_SUCCESS &&
            stop_error != ERROR_SERVICE_NOT_ACTIVE) {
            set_error(error,
                      win32_error("stop existing NSTU service", stop_error));
        } else {
            set_error(error, "existing NSTU service did not stop in time");
        }
        return false;
    }
    return true;
}

bool stop_installed_agents(const std::filesystem::path& expected_directory,
                           std::string* error) {
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        set_error(error,
                  win32_error("enumerate NSTU agent processes", GetLastError()));
        return false;
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool success = true;
    if (Process32FirstW(snapshot, &entry) != FALSE) {
        do {
            if (_wcsicmp(entry.szExeFile, L"nstu-agent.exe") != 0) continue;
            const HANDLE process = OpenProcess(
                PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE |
                    SYNCHRONIZE,
                FALSE, entry.th32ProcessID);
            if (process == nullptr) continue;
            std::array<wchar_t, 32768> image{};
            DWORD length = static_cast<DWORD>(image.size());
            const bool queried = QueryFullProcessImageNameW(
                process, 0, image.data(), &length) != FALSE;
            const auto actual = queried
                ? std::filesystem::path(std::wstring_view(image.data(), length))
                : std::filesystem::path();
            if (queried && paths_equal(actual.parent_path(), expected_directory)) {
                if (!TerminateProcess(process, 0) ||
                    WaitForSingleObject(process, 5000) == WAIT_TIMEOUT) {
                    success = false;
                }
            }
            CloseHandle(process);
        } while (Process32NextW(snapshot, &entry) != FALSE);
    }
    CloseHandle(snapshot);
    if (!success) {
        set_error(error, "installed NSTU agent did not stop in time");
    }
    return success;
}

} // namespace

bool install_service(std::string* error) {
    const auto executable = current_executable();
    if (executable.empty()) {
        set_error(error, "service executable path is unavailable");
        return false;
    }
    const std::wstring quoted = L"\"" + executable + L"\"";
    SC_HANDLE manager = OpenSCManagerW(
        nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE);
    if (manager == nullptr) {
        set_error(error,
                  win32_error("open service control manager", GetLastError()));
        return false;
    }
    SC_HANDLE service = OpenServiceW(
        manager, kManagedServiceName,
        SERVICE_CHANGE_CONFIG | SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS |
            SERVICE_START | SERVICE_STOP | WRITE_DAC);
    bool created = false;
    if (service == nullptr && GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST) {
        service = CreateServiceW(
            manager, kManagedServiceName, L"NSTU Client Service",
            SERVICE_CHANGE_CONFIG | SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS |
                SERVICE_START | SERVICE_STOP | WRITE_DAC | DELETE,
            SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START,
            SERVICE_ERROR_NORMAL, quoted.c_str(), nullptr, nullptr, nullptr,
            L"LocalSystem", nullptr);
        created = service != nullptr;
    }
    if (service == nullptr) {
        const DWORD code = GetLastError();
        CloseServiceHandle(manager);
        set_error(error, win32_error("create or open NSTU service", code));
        return false;
    }
    const auto close = [&] {
        CloseServiceHandle(service);
        CloseServiceHandle(manager);
    };
    const auto fail = [&] {
        if (created) (void)DeleteService(service);
        close();
        return false;
    };
    if (!created) {
        SERVICE_STATUS_PROCESS status{};
        DWORD bytes = 0;
        if (!QueryServiceStatusEx(
                service, SC_STATUS_PROCESS_INFO,
                reinterpret_cast<BYTE*>(&status), sizeof(status), &bytes)) {
            set_error(error,
                      win32_error("query NSTU service state", GetLastError()));
            return fail();
        }
        if (status.dwCurrentState != SERVICE_STOPPED &&
            !stop_service(service, false, error)) {
            return fail();
        }
    }
    if (!ChangeServiceConfigW(
            service, SERVICE_NO_CHANGE, SERVICE_AUTO_START,
            SERVICE_ERROR_NORMAL, quoted.c_str(), nullptr, nullptr, nullptr,
            L"LocalSystem", nullptr, L"NSTU Client Service")) {
        set_error(error, win32_error("configure NSTU service", GetLastError()));
        return fail();
    }
    if (!configure_recovery(service, error) ||
        !verify_recovery(service, error)) {
        return fail();
    }
    close();
    if (!harden_service_dacl(kManagedServiceName, error)) {
        if (created) {
            manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
            if (manager != nullptr) {
                service = OpenServiceW(manager, kManagedServiceName, DELETE);
                if (service != nullptr) {
                    (void)DeleteService(service);
                    CloseServiceHandle(service);
                }
                CloseServiceHandle(manager);
            }
        }
        return false;
    }
    return true;
}

bool prepare_service_update(std::string* error) {
    const auto executable = std::filesystem::path(current_executable());
    if (executable.empty()) {
        set_error(error, "update helper executable path is unavailable");
        return false;
    }
    const auto install_root = executable.parent_path().parent_path();
    const auto expected_service =
        install_root / L"client" / L"nstu-service.exe";

    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (manager == nullptr) {
        set_error(error,
                  win32_error("open service control manager", GetLastError()));
        return false;
    }
    SC_HANDLE service = OpenServiceW(
        manager, kManagedServiceName,
        SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS | SERVICE_STOP);
    if (service == nullptr) {
        const DWORD code = GetLastError();
        CloseServiceHandle(manager);
        set_error(error, win32_error("open existing NSTU service", code));
        return false;
    }
    SERVICE_STATUS_PROCESS service_status{};
    DWORD status_bytes = 0;
    if (!QueryServiceStatusEx(
            service, SC_STATUS_PROCESS_INFO,
            reinterpret_cast<BYTE*>(&service_status), sizeof(service_status),
            &status_bytes)) {
        const DWORD code = GetLastError();
        CloseServiceHandle(service);
        CloseServiceHandle(manager);
        set_error(error, win32_error("query existing NSTU service state", code));
        return false;
    }
    std::filesystem::path configured_service;
    const bool queried = query_service_image(service, configured_service, error);
    if (!queried || !paths_equal(configured_service, expected_service)) {
        if (queried) {
            set_error(error,
                      "existing NSTU service image is outside the installed client directory");
        }
        CloseServiceHandle(service);
        CloseServiceHandle(manager);
        return false;
    }
    const bool stopped = service_status.dwCurrentState == SERVICE_STOPPED ||
                         stop_service(service, true, error);
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return stopped && stop_installed_agents(expected_service.parent_path(), error);
}

bool uninstall_service(std::string* error) {
    if (machine_frozen()) {
        set_error(error,
                  "this NSTU client is in Managed mode; disable it from the "
                  "server or run --thaw-local first");
        return false;
    }
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (manager == nullptr) {
        set_error(error,
                  win32_error("open service control manager", GetLastError()));
        return false;
    }
    SC_HANDLE service = OpenServiceW(
        manager, kManagedServiceName,
        SERVICE_QUERY_STATUS | SERVICE_STOP | SERVICE_CHANGE_CONFIG | DELETE);
    if (service == nullptr) {
        const DWORD status = GetLastError();
        CloseServiceHandle(manager);
        if (status == ERROR_SERVICE_DOES_NOT_EXIST) return true;
        set_error(error, win32_error("open NSTU service for removal", status));
        return false;
    }
    SERVICE_FAILURE_ACTIONSW failures{};
    SERVICE_FAILURE_ACTIONS_FLAG flag{FALSE};
    if (!ChangeServiceConfig2W(service, SERVICE_CONFIG_FAILURE_ACTIONS,
                               &failures)) {
        set_error(error, win32_error("disable NSTU service restart actions",
                                     GetLastError()));
        CloseServiceHandle(service);
        CloseServiceHandle(manager);
        return false;
    }
    if (!ChangeServiceConfig2W(service,
                               SERVICE_CONFIG_FAILURE_ACTIONS_FLAG, &flag)) {
        set_error(error,
                  win32_error("disable NSTU service non-crash recovery",
                              GetLastError()));
        CloseServiceHandle(service);
        CloseServiceHandle(manager);
        return false;
    }
    if (!stop_service(service, false, error)) {
        CloseServiceHandle(service);
        CloseServiceHandle(manager);
        return false;
    }
    const bool deleted = DeleteService(service) != FALSE ||
                         GetLastError() == ERROR_SERVICE_MARKED_FOR_DELETE;
    const DWORD delete_error = deleted ? ERROR_SUCCESS : GetLastError();
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    if (!deleted) {
        set_error(error,
                  win32_error("remove NSTU service registration", delete_error));
    }
    return deleted;
}

} // namespace nstu::client
