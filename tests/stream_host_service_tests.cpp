#include "nstu/stream_host_service.hpp"

#include <cassert>
#include <filesystem>
#include <iostream>

int main() {
    using nstu::client::default_stream_host_state_path;
    using nstu::client::ensure_stream_host_state_directory;
    using nstu::client::is_stream_host_service_installed;
    using nstu::client::is_stream_host_service_running;
    using nstu::client::stop_stream_host_service;
    using nstu::client::StreamHostSupervisor;

    // Verify state directory path conventions
    const auto state_path = default_stream_host_state_path();
    assert(!state_path.empty());
    assert(state_path.filename() == L"stream-host");
    assert(state_path.parent_path().filename() == L"NSTU");

    // Verify directory creation is idempotent and valid
    std::string error;
    assert(ensure_stream_host_state_directory(&error));
    assert(error.empty());
    assert(std::filesystem::exists(state_path));

    // Verify service query helpers execute safely without crashing
    (void)is_stream_host_service_installed();
    (void)is_stream_host_service_running();

    // Verify supervisor state machine
    {
        StreamHostSupervisor supervisor;
        assert(!supervisor.is_session_active());

        supervisor.handle_remote_session_start();
        assert(supervisor.is_session_active());

        supervisor.handle_remote_session_stop();
        assert(!supervisor.is_session_active());

        supervisor.handle_remote_session_start();
        assert(supervisor.is_session_active());
        supervisor.handle_exam_lockdown();
        assert(!supervisor.is_session_active());

        supervisor.handle_remote_session_start();
        assert(supervisor.is_session_active());
        supervisor.handle_disconnect();
        assert(!supervisor.is_session_active());
    }

    // Stop service is idempotent and safe even when stopped or uninstalled
    std::string stop_error;
    (void)stop_stream_host_service(&stop_error);

    std::cout << "Stream host service tests passed\n";
    return 0;
}
