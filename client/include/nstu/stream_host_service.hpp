#pragma once

#include <filesystem>
#include <string>

namespace nstu::client {

// Standard directory where the stream host configuration, pairing state,
// and SSL certificates are stored on student endpoints.
// On UWF-protected endpoints, %ProgramData%\NSTU is excluded from write filtering,
// guaranteeing that host state survives reboots without losing pairing.
[[nodiscard]] std::filesystem::path default_stream_host_state_path();

// Ensures that the state directory (%ProgramData%\NSTU\stream-host) exists and is accessible.
bool ensure_stream_host_state_directory(std::string* error = nullptr);

// Queries whether the stream host Windows service is registered in the SCM.
[[nodiscard]] bool is_stream_host_service_installed() noexcept;

// Queries whether the stream host Windows service is currently running.
[[nodiscard]] bool is_stream_host_service_running() noexcept;

// Starts the stream host Windows service on demand if installed and not already running.
// If the service is not installed, fails gracefully without throwing.
bool start_stream_host_service(std::string* error = nullptr);

// Stops the stream host Windows service if running.
// If the service is not installed or already stopped, succeeds idempotently.
bool stop_stream_host_service(std::string* error = nullptr);

// Manages the on-demand lifecycle of the stream host service on student endpoints.
// Started on demand when an authenticated teacher initiates a remote session;
// stopped immediately when the remote session ends, an exam lockdown engages,
// or the control session disconnects.
class StreamHostSupervisor {
public:
    StreamHostSupervisor() = default;
    ~StreamHostSupervisor();

    StreamHostSupervisor(const StreamHostSupervisor&) = delete;
    StreamHostSupervisor& operator=(const StreamHostSupervisor&) = delete;
    StreamHostSupervisor(StreamHostSupervisor&&) noexcept = default;
    StreamHostSupervisor& operator=(StreamHostSupervisor&&) noexcept = default;

    // Called when authenticated teacher starts a remote control session.
    void handle_remote_session_start();

    // Called when remote control session terminates.
    void handle_remote_session_stop();

    // Called when exam begins, enforcing lockdown.
    void handle_exam_lockdown();

    // Called when authenticated teacher connection drops.
    void handle_disconnect();

    // Queries whether the supervisor currently considers the remote session active.
    [[nodiscard]] bool is_session_active() const noexcept;

private:
    bool session_active_ = false;
    bool service_started_by_supervisor_ = false;
};

} // namespace nstu::client
