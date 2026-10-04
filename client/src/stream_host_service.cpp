#include "nstu/stream_host_service.hpp"

#include <windows.h>
#include <winsvc.h>

#include <system_error>
#include <utility>

namespace nstu::client {
namespace {

constexpr const wchar_t* kStreamHostServiceName = L"SunshineService";

class ServiceHandle {
public:
    ServiceHandle() noexcept = default;
    explicit ServiceHandle(SC_HANDLE handle) noexcept : handle_(handle) {}
    ~ServiceHandle() { reset(); }

    ServiceHandle(const ServiceHandle&) = delete;
    ServiceHandle& operator=(const ServiceHandle&) = delete;

    ServiceHandle(ServiceHandle&& other) noexcept
        : handle_(std::exchange(other.handle_, nullptr)) {}

    ServiceHandle& operator=(ServiceHandle&& other) noexcept {
        if (this != &other) {
            reset();
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }

    [[nodiscard]] SC_HANDLE get() const noexcept { return handle_; }
    [[nodiscard]] explicit operator bool() const noexcept {
        return handle_ != nullptr;
    }

    void reset(SC_HANDLE handle = nullptr) noexcept {
        if (handle_ != nullptr) {
            CloseServiceHandle(handle_);
        }
        handle_ = handle;
    }

private:
    SC_HANDLE handle_ = nullptr;
};

void set_error(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
}

} // namespace

std::filesystem::path default_stream_host_state_path() {
    wchar_t prog_data[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"ProgramData", prog_data, MAX_PATH) > 0) {
        return std::filesystem::path(prog_data) / L"NSTU" / L"stream-host";
    }
    return L"C:\\ProgramData\\NSTU\\stream-host";
}

bool ensure_stream_host_state_directory(std::string* error) {
    std::error_code ec;
    const auto path = default_stream_host_state_path();
    std::filesystem::create_directories(path, ec);
    if (ec) {
        set_error(error,
                  "failed to create stream host state directory: " + ec.message());
        return false;
    }
    return true;
}

bool is_stream_host_service_installed() noexcept {
    ServiceHandle manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!manager) {
        return false;
    }
    ServiceHandle service(OpenServiceW(manager.get(), kStreamHostServiceName,
                                       SERVICE_QUERY_STATUS));
    return static_cast<bool>(service);
}

bool is_stream_host_service_running() noexcept {
    ServiceHandle manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!manager) {
        return false;
    }
    ServiceHandle service(OpenServiceW(manager.get(), kStreamHostServiceName,
                                       SERVICE_QUERY_STATUS));
    if (!service) {
        return false;
    }
    SERVICE_STATUS_PROCESS status{};
    DWORD bytes = 0;
    if (!QueryServiceStatusEx(service.get(), SC_STATUS_PROCESS_INFO,
                              reinterpret_cast<BYTE*>(&status), sizeof(status),
                              &bytes)) {
        return false;
    }
    return status.dwCurrentState == SERVICE_RUNNING;
}

bool start_stream_host_service(std::string* error) {
    (void)ensure_stream_host_state_directory(error);
    ServiceHandle manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!manager) {
        set_error(error, "cannot open service control manager");
        return false;
    }
    ServiceHandle service(OpenServiceW(manager.get(), kStreamHostServiceName,
                                       SERVICE_START | SERVICE_QUERY_STATUS));
    if (!service) {
        const DWORD err = GetLastError();
        if (err == ERROR_SERVICE_DOES_NOT_EXIST) {
            set_error(error, "stream host service is not installed");
            return false;
        }
        set_error(error,
                  "cannot open stream host service (error " +
                      std::to_string(err) + ")");
        return false;
    }
    SERVICE_STATUS_PROCESS status{};
    DWORD bytes = 0;
    if (QueryServiceStatusEx(service.get(), SC_STATUS_PROCESS_INFO,
                             reinterpret_cast<BYTE*>(&status), sizeof(status),
                             &bytes)) {
        if (status.dwCurrentState == SERVICE_RUNNING) {
            return true;
        }
    }
    if (!StartServiceW(service.get(), 0, nullptr)) {
        const DWORD err = GetLastError();
        if (err == ERROR_SERVICE_ALREADY_RUNNING) {
            return true;
        }
        set_error(error,
                  "failed to start stream host service (error " +
                      std::to_string(err) + ")");
        return false;
    }
    return true;
}

bool stop_stream_host_service(std::string* error) {
    ServiceHandle manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!manager) {
        set_error(error, "cannot open service control manager");
        return false;
    }
    ServiceHandle service(OpenServiceW(manager.get(), kStreamHostServiceName,
                                       SERVICE_STOP | SERVICE_QUERY_STATUS));
    if (!service) {
        const DWORD err = GetLastError();
        if (err == ERROR_SERVICE_DOES_NOT_EXIST) {
            return true;
        }
        set_error(error,
                  "cannot open stream host service (error " +
                      std::to_string(err) + ")");
        return false;
    }
    SERVICE_STATUS_PROCESS status{};
    DWORD bytes = 0;
    if (QueryServiceStatusEx(service.get(), SC_STATUS_PROCESS_INFO,
                             reinterpret_cast<BYTE*>(&status), sizeof(status),
                             &bytes)) {
        if (status.dwCurrentState == SERVICE_STOPPED) {
            return true;
        }
    }
    SERVICE_STATUS stop_status{};
    if (!ControlService(service.get(), SERVICE_CONTROL_STOP, &stop_status)) {
        const DWORD err = GetLastError();
        if (err == ERROR_SERVICE_NOT_ACTIVE) {
            return true;
        }
        set_error(error,
                  "failed to stop stream host service (error " +
                      std::to_string(err) + ")");
        return false;
    }
    return true;
}

StreamHostSupervisor::~StreamHostSupervisor() {
    if (session_active_) {
        handle_remote_session_stop();
    }
}

void StreamHostSupervisor::handle_remote_session_start() {
    session_active_ = true;
    if (!is_stream_host_service_running()) {
        if (start_stream_host_service()) {
            service_started_by_supervisor_ = true;
        }
    }
}

void StreamHostSupervisor::handle_remote_session_stop() {
    session_active_ = false;
    if (service_started_by_supervisor_) {
        stop_stream_host_service();
        service_started_by_supervisor_ = false;
    }
}

void StreamHostSupervisor::handle_exam_lockdown() {
    session_active_ = false;
    stop_stream_host_service();
    service_started_by_supervisor_ = false;
}

void StreamHostSupervisor::handle_disconnect() {
    session_active_ = false;
    if (service_started_by_supervisor_) {
        stop_stream_host_service();
        service_started_by_supervisor_ = false;
    }
}

bool StreamHostSupervisor::is_session_active() const noexcept {
    return session_active_;
}

} // namespace nstu::client
