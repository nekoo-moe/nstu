#include "nstu/service_install.hpp"

#include "nstu/client_freeze.hpp"
#include "nstu/session.hpp"

#include <windows.h>

#include <array>
#include <string>

namespace nstu::client {
namespace {

void set_error(std::string* error, const char* message) {
    if (error != nullptr) *error = message;
}

std::wstring current_executable() {
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) return {};
    path.resize(length);
    return path;
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
        set_error(error, "service control manager could not be opened");
        return false;
    }
    SC_HANDLE service = OpenServiceW(
        manager, kManagedServiceName,
        SERVICE_CHANGE_CONFIG | SERVICE_QUERY_STATUS | SERVICE_STOP |
            WRITE_DAC);
    bool created = false;
    if (service == nullptr && GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST) {
        service = CreateServiceW(
            manager, kManagedServiceName, L"NSTU Client Service",
            SERVICE_CHANGE_CONFIG | SERVICE_QUERY_STATUS | SERVICE_STOP |
                WRITE_DAC | DELETE,
            SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START,
            SERVICE_ERROR_NORMAL, quoted.c_str(), nullptr, nullptr, nullptr,
            L"LocalSystem", nullptr);
        created = service != nullptr;
    }
    if (service == nullptr) {
        CloseServiceHandle(manager);
        set_error(error, "NSTU service could not be created or opened");
        return false;
    }
    const auto rollback = [&] {
        if (created) (void)DeleteService(service);
        CloseServiceHandle(service);
        CloseServiceHandle(manager);
    };
    if (!created) {
        SERVICE_STATUS status{};
        if (!ControlService(service, SERVICE_CONTROL_STOP, &status)) {
            const DWORD stop_error = GetLastError();
            if (stop_error != ERROR_SERVICE_NOT_ACTIVE) {
                rollback();
                set_error(error,
                          "existing NSTU service must be thawed and stopped "
                          "before it can be updated");
                return false;
            }
        }
        if (!wait_for_state(service, SERVICE_STOPPED, 15000)) {
            rollback();
            set_error(error, "existing NSTU service did not stop in time");
            return false;
        }
    }
    if (!ChangeServiceConfigW(
            service, SERVICE_NO_CHANGE, SERVICE_AUTO_START,
            SERVICE_ERROR_NORMAL, quoted.c_str(), nullptr, nullptr, nullptr,
            L"LocalSystem", nullptr, L"NSTU Client Service")) {
        rollback();
        set_error(error, "NSTU service configuration failed");
        return false;
    }
    SERVICE_DESCRIPTIONW description{
        const_cast<wchar_t*>(L"NSTU classroom client service")};
    SERVICE_FAILURE_ACTIONSW failures{};
    std::array<SC_ACTION, 3> actions{
        SC_ACTION{SC_ACTION_RESTART, 5000},
        SC_ACTION{SC_ACTION_RESTART, 15000},
        SC_ACTION{SC_ACTION_RESTART, 60000},
    };
    failures.dwResetPeriod = 86400;
    failures.cActions = static_cast<DWORD>(actions.size());
    failures.lpsaActions = actions.data();
    SERVICE_FAILURE_ACTIONS_FLAG flag{TRUE};
    SERVICE_SID_INFO sid{SERVICE_SID_TYPE_UNRESTRICTED};
    if (!ChangeServiceConfig2W(service, SERVICE_CONFIG_DESCRIPTION,
                               &description) ||
        !ChangeServiceConfig2W(service, SERVICE_CONFIG_FAILURE_ACTIONS,
                               &failures) ||
        !ChangeServiceConfig2W(service,
                               SERVICE_CONFIG_FAILURE_ACTIONS_FLAG, &flag) ||
        !ChangeServiceConfig2W(service, SERVICE_CONFIG_SERVICE_SID_INFO,
                               &sid)) {
        rollback();
        set_error(error, "NSTU service recovery configuration failed");
        return false;
    }
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    if (!harden_service_dacl(kManagedServiceName, error)) {
        // A newly created service without its intended DACL must not survive a
        // failed install. Existing registrations are left for repair/retry.
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

bool uninstall_service(std::string* error) {
    if (machine_frozen()) {
        set_error(error,
                  "this NSTU client is in Managed mode; disable it from the "
                  "server or run --thaw-local first");
        return false;
    }
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (manager == nullptr) {
        set_error(error, "service control manager could not be opened");
        return false;
    }
    SC_HANDLE service = OpenServiceW(
        manager, kManagedServiceName,
        SERVICE_QUERY_STATUS | SERVICE_STOP | SERVICE_CHANGE_CONFIG | DELETE);
    if (service == nullptr) {
        const DWORD status = GetLastError();
        CloseServiceHandle(manager);
        if (status == ERROR_SERVICE_DOES_NOT_EXIST) return true;
        set_error(error, "NSTU service could not be opened for removal");
        return false;
    }
    SERVICE_FAILURE_ACTIONSW failures{};
    SERVICE_FAILURE_ACTIONS_FLAG flag{FALSE};
    if (!ChangeServiceConfig2W(service, SERVICE_CONFIG_FAILURE_ACTIONS,
                               &failures) ||
        !ChangeServiceConfig2W(service,
                               SERVICE_CONFIG_FAILURE_ACTIONS_FLAG, &flag)) {
        CloseServiceHandle(service);
        CloseServiceHandle(manager);
        set_error(error, "NSTU service recovery could not be disabled");
        return false;
    }
    SERVICE_STATUS status{};
    if (!ControlService(service, SERVICE_CONTROL_STOP, &status)) {
        const DWORD stop_error = GetLastError();
        if (stop_error != ERROR_SERVICE_NOT_ACTIVE) {
            CloseServiceHandle(service);
            CloseServiceHandle(manager);
            set_error(error, "NSTU service could not be stopped");
            return false;
        }
    }
    if (!wait_for_state(service, SERVICE_STOPPED, 15000)) {
        CloseServiceHandle(service);
        CloseServiceHandle(manager);
        set_error(error, "NSTU service did not stop in time");
        return false;
    }
    const bool deleted = DeleteService(service) != FALSE ||
                         GetLastError() == ERROR_SERVICE_MARKED_FOR_DELETE;
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    if (!deleted) {
        set_error(error, "NSTU service registration could not be removed");
    }
    return deleted;
}

} // namespace nstu::client
