#include "nstu/remote_session_process.hpp"

#include "nstu/client_registry.hpp"
#include "nstu/multicast.hpp"
#include "nstu/network.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winver.h>

#include <algorithm>
#include <array>
#include <deque>
#include <mutex>
#include <span>
#include <thread>
#include <utility>

namespace nstu::server {
namespace {

class Handle {
public:
    Handle() noexcept = default;
    explicit Handle(HANDLE value) noexcept : value_(value) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept
        : value_(std::exchange(other.value_, nullptr)) {}
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) {
            reset();
            value_ = std::exchange(other.value_, nullptr);
        }
        return *this;
    }
    [[nodiscard]] HANDLE get() const noexcept { return value_; }
    [[nodiscard]] explicit operator bool() const noexcept {
        return value_ != nullptr && value_ != INVALID_HANDLE_VALUE;
    }
    void reset(HANDLE value = nullptr) noexcept {
        if (*this) CloseHandle(value_);
        value_ = value;
    }

private:
    HANDLE value_ = nullptr;
};

void set_error(std::string* error, std::string message) {
    if (error != nullptr) *error = std::move(message);
}

bool valid_numeric_host(std::wstring_view host) {
    if (host.empty() || host.size() > 45) return false;
    IN_ADDR address4{};
    IN6_ADDR address6{};
    const std::wstring text(host);
    if (InetPtonW(AF_INET, text.c_str(), &address4) == 1) {
        return address4.S_un.S_addr != INADDR_ANY;
    }
    if (InetPtonW(AF_INET6, text.c_str(), &address6) != 1) return false;
    return std::any_of(address6.u.Byte, address6.u.Byte + 16,
                       [](BYTE value) { return value != 0; });
}

std::string utf8(std::wstring_view input) {
    if (input.empty()) return {};
    const int bytes = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, input.data(),
        static_cast<int>(input.size()), nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return {};
    std::string output(static_cast<std::size_t>(bytes), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(),
                            static_cast<int>(input.size()), output.data(), bytes,
                            nullptr, nullptr) != bytes) {
        return {};
    }
    return output;
}

std::wstring file_version(const std::filesystem::path& path) {
    DWORD ignored = 0;
    const DWORD bytes = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (bytes == 0) return {};
    std::vector<std::byte> data(bytes);
    if (!GetFileVersionInfoW(path.c_str(), 0, bytes, data.data())) return {};
    VS_FIXEDFILEINFO* fixed = nullptr;
    UINT fixed_bytes = 0;
    if (!VerQueryValueW(data.data(), L"\\",
                        reinterpret_cast<void**>(&fixed), &fixed_bytes) ||
        fixed == nullptr || fixed_bytes < sizeof(VS_FIXEDFILEINFO)) {
        return {};
    }
    return std::to_wstring(HIWORD(fixed->dwFileVersionMS)) + L"." +
           std::to_wstring(LOWORD(fixed->dwFileVersionMS)) + L"." +
           std::to_wstring(HIWORD(fixed->dwFileVersionLS)) + L"." +
           std::to_wstring(LOWORD(fixed->dwFileVersionLS));
}

std::wstring command_line(std::span<const std::wstring> arguments) {
    std::wstring output;
    for (const auto& argument : arguments) {
        if (!output.empty()) output.push_back(L' ');
        output += quote_windows_process_argument(argument);
    }
    return output;
}

bool drain_pipe(HANDLE pipe, std::string& output, std::size_t limit) {
    std::array<char, 8192> buffer{};
    bool read_any = false;
    for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) ||
            available == 0) return read_any;
        const DWORD wanted = std::min<DWORD>(
            static_cast<DWORD>(buffer.size()), available);
        DWORD read = 0;
        if (!ReadFile(pipe, buffer.data(), wanted, &read, nullptr) || read == 0)
            return read_any;
        read_any = true;
        const std::size_t retained = std::min<std::size_t>(
            read, limit > output.size() ? limit - output.size() : 0);
        output.append(buffer.data(), retained);
    }
}

void sanitize_diagnostic(std::string& output) {
    for (char& value : output) {
        const auto byte = static_cast<unsigned char>(value);
        if (byte < 0x20 && value != '\r' && value != '\n' && value != '\t')
            value = '?';
    }
}

void close_process_windows(DWORD process_id) {
    EnumWindows(
        [](HWND window, LPARAM opaque) -> BOOL {
            DWORD owner = 0;
            GetWindowThreadProcessId(window, &owner);
            if (owner == static_cast<DWORD>(opaque) && IsWindowVisible(window))
                PostMessageW(window, WM_CLOSE, 0, 0);
            return TRUE;
        },
        static_cast<LPARAM>(process_id));
}

} // namespace

std::wstring quote_windows_process_argument(std::wstring_view argument) {
    if (!argument.empty() &&
        argument.find_first_of(L" \t\n\v\"") == std::wstring_view::npos)
        return std::wstring(argument);
    std::wstring output{L'\"'};
    std::size_t slashes = 0;
    for (const wchar_t value : argument) {
        if (value == L'\\') {
            ++slashes;
            continue;
        }
        if (value == L'\"') {
            output.append(slashes * 2 + 1, L'\\');
            output.push_back(L'\"');
        } else {
            output.append(slashes, L'\\');
            output.push_back(value);
        }
        slashes = 0;
    }
    output.append(slashes * 2, L'\\');
    output.push_back(L'\"');
    return output;
}

bool resolve_remote_session_target(
    std::span<const ClientRecord> clients,
    std::uint64_t client_id,
    RemoteSessionTarget* target,
    std::string* error) {
    if (client_id == 0 || target == nullptr) {
        set_error(error, "invalid client id or target output");
        return false;
    }
    const ClientRecord* matched = nullptr;
    for (const auto& record : clients) {
        if (record.id == client_id) {
            if (matched != nullptr) {
                set_error(error, "duplicate client records found in registry snapshot");
                return false;
            }
            matched = &record;
        }
    }
    if (matched == nullptr) {
        set_error(error, "client was not found in registry");
        return false;
    }
    if (matched->status == ClientStatus::offline) {
        set_error(error, "client is offline");
        return false;
    }
    if (matched->status == ClientStatus::connecting) {
        set_error(error, "client connection is not authenticated");
        return false;
    }

    std::string host_str = matched->address;
    if (host_str.empty()) {
        set_error(error, "client address is empty");
        return false;
    }
    if (host_str.front() == '[') {
        const auto close_bracket = host_str.find(']');
        if (close_bracket == std::string::npos) {
            set_error(error, "client address has malformed IPv6 brackets");
            return false;
        }
        host_str = host_str.substr(1, close_bracket - 1);
    } else {
        const auto colon = host_str.find(':');
        if (colon != std::string::npos) {
            if (host_str.find(':', colon + 1) == std::string::npos) {
                // Exactly one colon -> IPv4:port, strip port
                host_str = host_str.substr(0, colon);
            }
        }
    }

    const std::wstring host_w(host_str.begin(), host_str.end());
    if (!valid_numeric_host(host_w)) {
        set_error(error, "client address is not a valid numeric IPv4 or IPv6 host");
        return false;
    }

    target->client_id = client_id;
    target->numeric_host = std::move(host_w);
    return true;
}

struct RemoteSessionProcess::Impl {
    explicit Impl(RemoteSessionProcessConfig value) : config(std::move(value)) {}

    struct Process {
        Handle process;
        Handle thread;
        Handle pipe;
        DWORD id = 0;
    };

    RemoteSessionProcessConfig config;
    mutable std::mutex mutex;
    RemoteSessionSnapshot current;
    std::deque<RemoteSessionEvent> events;
    std::thread worker;
    Handle child;
    Handle job;
    DWORD child_id = 0;
    bool stop_requested = false;
    RemoteSessionStopReason stop_reason = RemoteSessionStopReason::explicit_stop;

    void set_state(RemoteSessionState state, RemoteSessionStopReason reason,
                   std::string diagnostic = {},
                   std::optional<std::uint32_t> exit_code = std::nullopt) {
        std::scoped_lock lock(mutex);
        current.state = state;
        current.exit_code = exit_code;
        current.bounded_diagnostic = std::move(diagnostic);
        events.push_back({state, reason, current.client_id, exit_code,
                          current.bounded_diagnostic});
    }

    bool launch(std::span<const std::wstring> arguments, bool suspended,
                Process& output, std::string* error) {
        SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        HANDLE read_pipe = nullptr;
        HANDLE write_pipe = nullptr;
        if (!CreatePipe(&read_pipe, &write_pipe, &security, 65536)) {
            set_error(error, "could not create remote-session diagnostic pipe");
            return false;
        }
        output.pipe.reset(read_pipe);
        Handle writer(write_pipe);
        SetHandleInformation(output.pipe.get(), HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdOutput = writer.get();
        startup.hStdError = writer.get();
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        PROCESS_INFORMATION process{};
        std::wstring command = command_line(arguments);
        const DWORD flags = CREATE_NO_WINDOW | (suspended ? CREATE_SUSPENDED : 0);
        if (!CreateProcessW(config.executable.c_str(), command.data(), nullptr,
                            nullptr, TRUE, flags, nullptr, nullptr, &startup,
                            &process)) {
            set_error(error, "could not launch remote-session process");
            return false;
        }
        output.process.reset(process.hProcess);
        output.thread.reset(process.hThread);
        output.id = process.dwProcessId;
        return true;
    }

    bool run_preflight(const RemoteSessionTarget& target, std::string* diagnostic) {
        Process process;
        const std::vector<std::wstring> arguments{
            config.executable.wstring(), L"list", target.numeric_host};
        if (!launch(arguments, false, process, diagnostic)) return false;
        std::string output;
        const auto deadline = std::chrono::steady_clock::now() +
                              config.preflight_timeout;
        for (;;) {
            const bool drained = drain_pipe(process.pipe.get(), output,
                                            config.maximum_diagnostic_bytes);
            if (WaitForSingleObject(process.process.get(), drained ? 0 : 10) ==
                WAIT_OBJECT_0)
                break;
            {
                std::scoped_lock lock(mutex);
                if (stop_requested) {
                    TerminateProcess(process.process.get(), 1);
                    WaitForSingleObject(process.process.get(), INFINITE);
                    *diagnostic = "remote-session preflight cancelled";
                    return false;
                }
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                TerminateProcess(process.process.get(), 1);
                WaitForSingleObject(process.process.get(), INFINITE);
                *diagnostic = "remote-session preflight timed out";
                return false;
            }
        }
        drain_pipe(process.pipe.get(), output,
                   config.maximum_diagnostic_bytes);
        sanitize_diagnostic(output);
        DWORD exit_code = 1;
        GetExitCodeProcess(process.process.get(), &exit_code);
        if (exit_code != 0) {
            *diagnostic = output.empty() ? "remote-session preflight failed" : output;
            return false;
        }
        const auto application = utf8(config.application);
        if (application.empty() || output.find(application) == std::string::npos) {
            *diagnostic = "remote-session application was not found";
            return false;
        }
        return true;
    }

    void run(RemoteSessionTarget target, StartAuthorization authorization) {
        std::string diagnostic;
        if (!run_preflight(target, &diagnostic)) {
            set_state(RemoteSessionState::failed,
                      RemoteSessionStopReason::preflight_failed,
                      std::move(diagnostic));
            return;
        }
        set_state(RemoteSessionState::launching,
                  RemoteSessionStopReason::launch_failed);
        if (!authorization(target.client_id, &diagnostic)) {
            set_state(RemoteSessionState::failed,
                      RemoteSessionStopReason::preflight_failed,
                      diagnostic.empty() ? "remote-session start was denied"
                                         : std::move(diagnostic));
            return;
        }
        Handle local_job(CreateJobObjectW(nullptr, nullptr));
        if (!local_job) {
            set_state(RemoteSessionState::failed,
                      RemoteSessionStopReason::launch_failed,
                      "could not create remote-session process job");
            return;
        }
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(local_job.get(),
                                     JobObjectExtendedLimitInformation,
                                     &limits, sizeof(limits))) {
            set_state(RemoteSessionState::failed,
                      RemoteSessionStopReason::launch_failed,
                      "could not configure remote-session process job");
            return;
        }
        std::vector<std::wstring> arguments{
            config.executable.wstring(), L"stream", target.numeric_host,
            config.application};
        arguments.insert(arguments.end(), config.stream_arguments.begin(),
                         config.stream_arguments.end());
        Process process;
        if (!launch(arguments, true, process, &diagnostic)) {
            set_state(RemoteSessionState::failed,
                      RemoteSessionStopReason::launch_failed,
                      std::move(diagnostic));
            return;
        }
        if (!AssignProcessToJobObject(local_job.get(), process.process.get()) ||
            ResumeThread(process.thread.get()) == MAXDWORD) {
            TerminateProcess(process.process.get(), 1);
            WaitForSingleObject(process.process.get(), INFINITE);
            set_state(RemoteSessionState::failed,
                      RemoteSessionStopReason::launch_failed,
                      "could not contain remote-session process");
            return;
        }
        process.thread.reset();
        {
            std::scoped_lock lock(mutex);
            child = std::move(process.process);
            job = std::move(local_job);
            child_id = process.id;
            current.state = RemoteSessionState::running;
            events.push_back({RemoteSessionState::running,
                              RemoteSessionStopReason::child_exited,
                              current.client_id, std::nullopt, {}});
        }
        std::string output;
        for (;;) {
            const bool drained = drain_pipe(process.pipe.get(), output,
                                            config.maximum_diagnostic_bytes);
            if (WaitForSingleObject(child.get(), drained ? 0 : 10) == WAIT_OBJECT_0)
                break;
            bool should_stop = false;
            RemoteSessionStopReason requested = RemoteSessionStopReason::explicit_stop;
            {
                std::scoped_lock lock(mutex);
                should_stop = stop_requested;
                requested = stop_reason;
                if (should_stop && current.state != RemoteSessionState::stopping) {
                    current.state = RemoteSessionState::stopping;
                    events.push_back({RemoteSessionState::stopping, requested,
                                      current.client_id, std::nullopt, {}});
                }
            }
            if (!should_stop) continue;
            close_process_windows(child_id);
            const DWORD timeout = static_cast<DWORD>(std::clamp<std::int64_t>(
                config.stop_timeout.count(), 1, MAXDWORD - 1));
            if (WaitForSingleObject(child.get(), timeout) == WAIT_TIMEOUT) {
                TerminateJobObject(job.get(), 1);
                WaitForSingleObject(child.get(), INFINITE);
            }
            break;
        }
        DWORD code = 0;
        GetExitCodeProcess(child.get(), &code);
        drain_pipe(process.pipe.get(), output,
                   config.maximum_diagnostic_bytes);
        sanitize_diagnostic(output);
        RemoteSessionStopReason reason = RemoteSessionStopReason::child_exited;
        {
            std::scoped_lock lock(mutex);
            if (stop_requested) reason = stop_reason;
            child.reset();
            job.reset();
            child_id = 0;
            current.state = RemoteSessionState::exited;
            current.exit_code = code;
            current.bounded_diagnostic = output;
            events.push_back({RemoteSessionState::exited, reason,
                              current.client_id, code, output});
        }
    }
};

RemoteSessionProcess::RemoteSessionProcess(RemoteSessionProcessConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}

RemoteSessionProcess::~RemoteSessionProcess() {
    stop(RemoteSessionStopReason::teacher_shutdown);
    if (impl_->worker.joinable()) impl_->worker.join();
}

bool RemoteSessionProcess::start(RemoteSessionTarget target,
                                 StartAuthorization authorization,
                                 std::string* error) {
    if (!std::filesystem::is_regular_file(impl_->config.executable)) {
        set_error(error, "remote-session executable is unavailable");
        return false;
    }
    if (!impl_->config.required_file_version.empty() &&
        file_version(impl_->config.executable) != impl_->config.required_file_version) {
        set_error(error, "remote-session file version does not match the pin");
        return false;
    }
    if (target.client_id == 0 || !valid_numeric_host(target.numeric_host)) {
        set_error(error, "remote-session target is invalid");
        return false;
    }
    if (impl_->config.host_port != 0) {
        nstu::net::WinsockRuntime winsock;
        nstu::net::TcpSocket socket;
        const auto host = utf8(target.numeric_host);
        if (!winsock.ready() || host.empty() ||
            !socket.connect_with_timeout(
                host, impl_->config.host_port,
                static_cast<std::uint32_t>(std::clamp<std::int64_t>(
                    impl_->config.preflight_timeout.count(), 1, UINT32_MAX)),
                error)) return false;
    }
    {
        std::scoped_lock lock(impl_->mutex);
        if (impl_->current.state == RemoteSessionState::preflighting ||
            impl_->current.state == RemoteSessionState::launching ||
            impl_->current.state == RemoteSessionState::running ||
            impl_->current.state == RemoteSessionState::stopping) {
            set_error(error, "a remote session is already active");
            return false;
        }
    }
    if (impl_->worker.joinable()) impl_->worker.join();
    {
        std::scoped_lock lock(impl_->mutex);
        impl_->current = {RemoteSessionState::preflighting, target.client_id,
                          std::nullopt, {}};
        impl_->events.push_back({RemoteSessionState::preflighting,
                                 RemoteSessionStopReason::preflight_failed,
                                 target.client_id, std::nullopt, {}});
        impl_->stop_requested = false;
    }
    impl_->worker = std::thread(
        [state = impl_.get(), target = std::move(target),
         authorization = std::move(authorization)]() mutable {
            state->run(std::move(target), std::move(authorization));
        });
    return true;
}

void RemoteSessionProcess::stop(RemoteSessionStopReason reason) noexcept {
    std::scoped_lock lock(impl_->mutex);
    if (impl_->current.state == RemoteSessionState::preflighting ||
        impl_->current.state == RemoteSessionState::launching ||
        impl_->current.state == RemoteSessionState::running ||
        impl_->current.state == RemoteSessionState::stopping) {
        impl_->stop_requested = true;
        impl_->stop_reason = reason;
    }
}

RemoteSessionSnapshot RemoteSessionProcess::snapshot() const {
    std::scoped_lock lock(impl_->mutex);
    return impl_->current;
}

std::vector<RemoteSessionEvent> RemoteSessionProcess::drain_events() {
    std::scoped_lock lock(impl_->mutex);
    std::vector<RemoteSessionEvent> output;
    output.reserve(impl_->events.size());
    while (!impl_->events.empty()) {
        output.push_back(std::move(impl_->events.front()));
        impl_->events.pop_front();
    }
    return output;
}

} // namespace nstu::server
