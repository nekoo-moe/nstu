#ifndef NSTU_REMOTE_SESSION_PROCESS_HPP
#define NSTU_REMOTE_SESSION_PROCESS_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nstu::server {

struct ClientRecord;

enum class RemoteSessionState {
    idle,
    preflighting,
    launching,
    running,
    stopping,
    exited,
    failed,
};

enum class RemoteSessionStopReason {
    explicit_stop,
    selected_client_changed,
    client_offline,
    exam_started,
    child_exited,
    teacher_shutdown,
    launch_failed,
    preflight_failed,
    stop_timeout,
};

struct RemoteSessionProcessConfig {
    std::filesystem::path executable;
    std::wstring required_file_version;
    std::wstring application = L"Desktop";
    std::vector<std::wstring> stream_arguments;
    std::uint16_t host_port = 47984;
    std::chrono::milliseconds preflight_timeout{5000};
    std::chrono::milliseconds stop_timeout{3000};
    std::size_t maximum_diagnostic_bytes = 4096;
};

struct RemoteSessionTarget {
    std::uint64_t client_id = 0;
    std::wstring numeric_host;
};

struct RemoteSessionSnapshot {
    RemoteSessionState state = RemoteSessionState::idle;
    std::uint64_t client_id = 0;
    std::optional<std::uint32_t> exit_code;
    std::string bounded_diagnostic;
};

struct RemoteSessionEvent {
    RemoteSessionState state = RemoteSessionState::idle;
    RemoteSessionStopReason reason = RemoteSessionStopReason::preflight_failed;
    std::uint64_t client_id = 0;
    std::optional<std::uint32_t> exit_code;
    std::string bounded_diagnostic;
};

[[nodiscard]] std::wstring quote_windows_process_argument(std::wstring_view argument);

[[nodiscard]] bool resolve_remote_session_target(
    std::span<const ClientRecord> clients,
    std::uint64_t client_id,
    RemoteSessionTarget* target,
    std::string* error = nullptr);

class RemoteSessionProcess {
public:
    using StartAuthorization =
        std::function<bool(std::uint64_t client_id, std::string* denial_reason)>;

    explicit RemoteSessionProcess(RemoteSessionProcessConfig config);
    ~RemoteSessionProcess();

    RemoteSessionProcess(const RemoteSessionProcess&) = delete;
    RemoteSessionProcess& operator=(const RemoteSessionProcess&) = delete;

    [[nodiscard]] bool start(RemoteSessionTarget target,
                             StartAuthorization authorization,
                             std::string* error = nullptr);
    void stop(RemoteSessionStopReason reason) noexcept;
    [[nodiscard]] RemoteSessionSnapshot snapshot() const;
    [[nodiscard]] std::vector<RemoteSessionEvent> drain_events();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nstu::server

#endif
