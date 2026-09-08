#include "nstu/agent_protocol.hpp"
#include "nstu/client_config.hpp"
#include "nstu/client_control.hpp"
#include "nstu/control_messages.hpp"
#include "nstu/deployment.hpp"
#include "nstu/session.hpp"

#include <windows.h>
#include <wtsapi32.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr wchar_t kServiceName[] = L"nstu-service";
constexpr DWORD kServiceControlLock = 128;
constexpr DWORD kServiceControlUnlock = 129;
SERVICE_STATUS_HANDLE g_status_handle = nullptr;
HANDLE g_stop_event = nullptr;
std::filesystem::path g_agent_path;
std::mutex g_agent_path_mutex;
std::mutex g_agent_queue_mutex;
std::deque<nstu::client::AgentMessage> g_agent_queue;
std::mutex g_outbound_queue_mutex;
std::deque<nstu::client::ClientOutboundCommand> g_outbound_queue;
std::atomic_bool g_stop_requested = false;
std::atomic_bool g_agent_connected = false;
std::atomic_bool g_agent_locked = false;
std::atomic_bool g_agent_streaming = false;
std::atomic<std::uint8_t> g_agent_stream_fps = 0;
std::atomic_bool g_agent_snapshotting = false;
std::atomic<std::uint16_t> g_agent_snapshot_interval_seconds = 0;
std::atomic_bool g_agent_viewing_broadcast = false;
std::atomic_bool g_desired_locked = false;
std::mutex g_agent_launch_mutex;
std::chrono::steady_clock::time_point g_next_agent_launch{};
constexpr std::size_t kMaximumQueuedAgentMessages = 256;
constexpr std::size_t kMaximumQueuedOutboundMessages = 32;

void queue_outbound_message(nstu::protocol::CommandType type,
                            std::vector<std::byte> payload) {
    std::scoped_lock lock(g_outbound_queue_mutex);
    if (type == nstu::protocol::CommandType::snapshot_frame) {
        std::erase_if(g_outbound_queue, [](const auto& message) {
            return message.type == nstu::protocol::CommandType::snapshot_frame;
        });
    }
    while (g_outbound_queue.size() >= kMaximumQueuedOutboundMessages) {
        g_outbound_queue.pop_front();
    }
    g_outbound_queue.push_back({type, std::move(payload)});
}

std::optional<nstu::client::ClientOutboundCommand> pop_outbound_message() {
    std::scoped_lock lock(g_outbound_queue_mutex);
    if (g_outbound_queue.empty()) {
        return std::nullopt;
    }
    auto message = std::move(g_outbound_queue.front());
    g_outbound_queue.pop_front();
    return message;
}

void queue_agent_message(nstu::client::AgentMessage message) {
    std::scoped_lock lock(g_agent_queue_mutex);
    if (g_agent_queue.size() >= kMaximumQueuedAgentMessages) {
        g_agent_queue.pop_front();
    }
    g_agent_queue.push_back(std::move(message));
}

void set_desired_lock(bool locked) {
    g_desired_locked = locked;
    queue_agent_message({locked ? nstu::client::AgentMessageType::lock
                                : nstu::client::AgentMessageType::unlock,
                         {}});
}

void wake_pipe_listener() {
    nstu::client::NamedPipe wake;
    if (wake.connect_client(nstu::client::kControlPipeName, 100, nullptr)) {
        (void)nstu::client::send_agent_message(
            wake, {nstu::client::AgentMessageType::status_request, {}}, nullptr);
    }
}

void launch_agent();

void agent_pipe_loop() {
    while (!g_stop_requested.load()) {
        nstu::client::NamedPipe pipe;
        if (!pipe.create_server(nstu::client::kControlPipeName, nullptr) ||
            !pipe.wait_for_client(nullptr)) {
            if (!g_stop_requested.load()) {
                Sleep(250);
            }
            continue;
        }
        std::filesystem::path expected_agent_path;
        {
            std::scoped_lock lock(g_agent_path_mutex);
            expected_agent_path = g_agent_path;
        }
        if (!pipe.validate_client_process(expected_agent_path.wstring(),
                                          nullptr, nullptr)) {
            // The pipe DACL permits the interactive user to connect, but only
            // the installed agent may occupy the service channel. Closing an
            // untrusted client here also lets the watchdog accept the real
            // agent on the next pipe instance.
            if (!g_stop_requested.load()) {
                Sleep(250);
            }
            continue;
        }
        g_agent_connected = true;
        {
            std::scoped_lock launch_lock(g_agent_launch_mutex);
            // A completed pipe connection proves that the previous launch
            // succeeded. Permit the disconnect path to replace this process
            // even when it exits inside the normal launch cooldown.
            g_next_agent_launch = std::chrono::steady_clock::now() +
                                  std::chrono::milliseconds(500);
        }
        queue_agent_message({g_desired_locked.load()
                                 ? nstu::client::AgentMessageType::lock
                                 : nstu::client::AgentMessageType::unlock,
                             {}});
        queue_agent_message(
            {nstu::client::AgentMessageType::status_request, {}});
        while (!g_stop_requested.load()) {
            std::deque<nstu::client::AgentMessage> pending;
            {
                std::scoped_lock lock(g_agent_queue_mutex);
                pending.swap(g_agent_queue);
            }
            bool write_failed = false;
            while (!pending.empty()) {
                auto message = std::move(pending.front());
                pending.pop_front();
                if (!nstu::client::send_agent_message(pipe, message, nullptr)) {
                    pending.push_front(std::move(message));
                    write_failed = true;
                    break;
                }
            }
            if (write_failed) {
                std::scoped_lock lock(g_agent_queue_mutex);
                while (!pending.empty()) {
                    if (g_agent_queue.size() >=
                        kMaximumQueuedAgentMessages) {
                        g_agent_queue.pop_back();
                    }
                    g_agent_queue.push_front(std::move(pending.back()));
                    pending.pop_back();
                }
                break;
            }
            std::uint32_t available = 0;
            if (!pipe.available_bytes(available, nullptr)) {
                break;
            }
            if (available != 0) {
                const auto message =
                    nstu::client::receive_agent_message(pipe, nullptr);
                if (!message) {
                    break;
                }
                if (message->type ==
                    nstu::client::AgentMessageType::status_report) {
                    const auto status =
                        nstu::client::decode_agent_status(message->payload);
                    if (status) {
                        g_agent_locked = status->locked;
                        g_agent_streaming = status->streaming;
                        g_agent_stream_fps = status->frames_per_second;
                        g_agent_snapshotting = status->snapshotting;
                        g_agent_snapshot_interval_seconds =
                            status->snapshot_interval_seconds;
                        g_agent_viewing_broadcast =
                            status->viewing_broadcast;
                    }
                } else if (message->type ==
                           nstu::client::AgentMessageType::snapshot_frame) {
                    if (nstu::control::decode_snapshot_frame(message->payload)) {
                        queue_outbound_message(
                            nstu::protocol::CommandType::snapshot_frame,
                            std::move(message->payload));
                    }
                }
            }
            Sleep(25);
        }
        g_agent_connected = false;
        pipe.close();
        if (!g_stop_requested.load()) {
            // A student can close the interactive agent with End Task. Give
            // the desktop a short settling period, then restore the agent in
            // the active session without creating a tight restart loop.
            for (int tick = 0; tick < 10 && !g_stop_requested.load(); ++tick) {
                Sleep(50);
            }
            if (!g_stop_requested.load()) {
                (void)launch_agent();
            }
        }
    }
}

std::filesystem::path client_config_path() {
    const auto root = nstu::deployment::data_root(nullptr);
    return root.empty() ? std::filesystem::path{}
                        : root / L"client-config.bin";
}

nstu::control::ClientStatusReport current_status() {
    char hostname[MAX_COMPUTERNAME_LENGTH + 1]{};
    DWORD hostname_bytes = static_cast<DWORD>(std::size(hostname));
    if (!GetComputerNameA(hostname, &hostname_bytes) || hostname_bytes == 0) {
        strcpy_s(hostname, "NSTU-CLIENT");
    }
    nstu::control::ClientStatusReport status;
    status.hostname = hostname;
    status.locked = g_agent_locked.load();
    status.streaming = g_agent_streaming.load();
    status.snapshotting = g_agent_snapshotting.load();
    status.viewing_broadcast = g_agent_viewing_broadcast.load();
    status.frames_per_second = g_agent_stream_fps.load();
    status.snapshot_interval_seconds =
        g_agent_snapshot_interval_seconds.load();
    status.session_id = WTSGetActiveConsoleSessionId();
    return status;
}

void handle_server_command(
    const nstu::control::AuthenticatedCommand& command) {
    switch (command.envelope.type) {
    case nstu::protocol::CommandType::lock:
        set_desired_lock(true);
        break;
    case nstu::protocol::CommandType::unlock:
        set_desired_lock(false);
        break;
    case nstu::protocol::CommandType::chat:
        if (command.payload.size() <= nstu::client::kMaximumAgentPayloadBytes) {
            queue_agent_message({nstu::client::AgentMessageType::chat,
                                 command.payload});
        }
        break;
    case nstu::protocol::CommandType::start_stream: {
        const auto fps =
            nstu::control::decode_start_stream_request(command.payload);
        if (fps) {
            queue_agent_message(
                {nstu::client::AgentMessageType::start_stream,
                 {static_cast<std::byte>(*fps)}});
        }
        break;
    }
    case nstu::protocol::CommandType::stop_stream:
        queue_agent_message(
            {nstu::client::AgentMessageType::stop_stream, {}});
        break;
    case nstu::protocol::CommandType::keyframe_request:
        queue_agent_message(
            {nstu::client::AgentMessageType::keyframe_request, {}});
        break;
    case nstu::protocol::CommandType::start_snapshots:
        if (nstu::control::decode_snapshot_schedule(command.payload)) {
            queue_agent_message(
                {nstu::client::AgentMessageType::start_snapshots,
                 command.payload});
        }
        break;
    case nstu::protocol::CommandType::stop_snapshots:
        queue_agent_message(
            {nstu::client::AgentMessageType::stop_snapshots, {}});
        break;
    case nstu::protocol::CommandType::overlay_stroke:
        if (nstu::control::decode_overlay_stroke(command.payload)) {
            queue_agent_message(
                {nstu::client::AgentMessageType::overlay_stroke,
                 command.payload});
        }
        break;
    case nstu::protocol::CommandType::overlay_clear:
        queue_agent_message(
            {nstu::client::AgentMessageType::overlay_clear, {}});
        break;
    case nstu::protocol::CommandType::host_snapshot:
        if (nstu::control::decode_snapshot_frame(command.payload)) {
            queue_agent_message(
                {nstu::client::AgentMessageType::host_snapshot,
                 command.payload});
        }
        break;
    case nstu::protocol::CommandType::host_broadcast_stop:
        queue_agent_message(
            {nstu::client::AgentMessageType::host_broadcast_stop, {}});
        break;
    case nstu::protocol::CommandType::remote_start:
        queue_agent_message({nstu::client::AgentMessageType::remote_start, {}});
        break;
    case nstu::protocol::CommandType::remote_input:
        if (nstu::client::decode_remote_input(command.payload)) {
            queue_agent_message({nstu::client::AgentMessageType::remote_input,
                                 command.payload});
        }
        break;
    case nstu::protocol::CommandType::remote_end:
        queue_agent_message({nstu::client::AgentMessageType::remote_end, {}});
        break;
    default:
        break;
    }
}

void remote_control_loop(std::stop_token stop_token) {
    const auto path = client_config_path();
    if (path.empty()) {
        return;
    }
    constexpr char entropy_text[] = "NSTU-CLIENT-CONFIG-V1";
    const auto entropy = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(entropy_text),
        sizeof(entropy_text) - 1);
    while (!stop_token.stop_requested()) {
        nstu::client::ClientRuntimeConfig config;
        if (nstu::client::load_client_runtime_config(
                config, path.wstring(), entropy, nullptr)) {
            std::string ignored_error;
            (void)nstu::client::run_client_control_session(
                config, stop_token, current_status, handle_server_command,
                pop_outbound_message,
                &ignored_error);
            queue_agent_message({nstu::client::AgentMessageType::remote_end, {}});
            nstu::client::clear_client_runtime_config(config);
        }
        for (int tick = 0; tick < 50 && !stop_token.stop_requested(); ++tick) {
            Sleep(100);
        }
    }
}

void launch_agent() {
    std::scoped_lock launch_lock(g_agent_launch_mutex);
    if (g_stop_requested.load()) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now < g_next_agent_launch) {
        return;
    }
    g_next_agent_launch = now + std::chrono::seconds(2);
    std::filesystem::path agent_path;
    {
        std::scoped_lock lock(g_agent_path_mutex);
        agent_path = g_agent_path;
    }
    if (agent_path.empty()) {
        return;
    }
    std::string ignored_error;
    const bool agent_started = nstu::client::launch_agent_in_active_session(
        agent_path.wstring(), &ignored_error);
    if (!agent_started) {
        g_next_agent_launch = now + std::chrono::seconds(5);
    }
}

void agent_supervisor_loop(std::stop_token stop_token) {
    while (!stop_token.stop_requested() && !g_stop_requested.load()) {
        if (!g_agent_connected.load()) {
            launch_agent();
        }
        for (int tick = 0;
             tick < 20 && !stop_token.stop_requested() &&
             !g_stop_requested.load();
             ++tick) {
            Sleep(100);
        }
    }
}

void report_status(DWORD state, DWORD error = NO_ERROR) {
    SERVICE_STATUS status{};
    status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    status.dwCurrentState = state;
    status.dwWin32ExitCode = error;
    status.dwControlsAccepted = state == SERVICE_RUNNING
                                    ? SERVICE_ACCEPT_STOP |
                                          SERVICE_ACCEPT_SESSIONCHANGE
                                    : 0;
    g_status_handle && SetServiceStatus(g_status_handle, &status);
}

DWORD WINAPI control_handler(DWORD control, DWORD event_type, void*, void*) {
    if (control == SERVICE_CONTROL_STOP && g_stop_event != nullptr) {
        report_status(SERVICE_STOP_PENDING);
        g_stop_requested = true;
        SetEvent(g_stop_event);
        wake_pipe_listener();
    } else if (control == kServiceControlLock) {
        set_desired_lock(true);
    } else if (control == kServiceControlUnlock) {
        set_desired_lock(false);
    } else if (control == SERVICE_CONTROL_SESSIONCHANGE &&
               (event_type == WTS_SESSION_LOGON ||
                event_type == WTS_SESSION_UNLOCK ||
                event_type == WTS_CONSOLE_CONNECT)) {
        launch_agent();
    }
    return NO_ERROR;
}

void WINAPI service_main(DWORD, wchar_t**) {
    g_status_handle = RegisterServiceCtrlHandlerExW(kServiceName,
                                                     control_handler, nullptr);
    if (g_status_handle == nullptr) {
        return;
    }
    g_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (g_stop_event == nullptr) {
        report_status(SERVICE_STOPPED, GetLastError());
        return;
    }
    report_status(SERVICE_START_PENDING);
    wchar_t executable[MAX_PATH]{};
    GetModuleFileNameW(nullptr, executable, MAX_PATH);
    {
        std::scoped_lock lock(g_agent_path_mutex);
        g_agent_path = std::filesystem::path(executable).parent_path() /
                       L"nstu-agent.exe";
    }
    std::string dacl_error;
    if (!nstu::client::harden_service_dacl(kServiceName, &dacl_error)) {
        CloseHandle(g_stop_event);
        g_stop_event = nullptr;
        report_status(SERVICE_STOPPED, ERROR_ACCESS_DENIED);
        return;
    }
    g_stop_requested = false;
    std::thread pipe_thread(agent_pipe_loop);
    std::jthread control_thread(remote_control_loop);
    std::jthread agent_supervisor_thread(agent_supervisor_loop);
    launch_agent();
    report_status(SERVICE_RUNNING);
    WaitForSingleObject(g_stop_event, INFINITE);
    g_stop_requested = true;
    queue_agent_message({nstu::client::AgentMessageType::remote_end, {}});
    control_thread.request_stop();
    agent_supervisor_thread.request_stop();
    wake_pipe_listener();
    if (pipe_thread.joinable()) {
        pipe_thread.join();
    }
    if (control_thread.joinable()) {
        control_thread.join();
    }
    if (agent_supervisor_thread.joinable()) {
        agent_supervisor_thread.join();
    }
    CloseHandle(g_stop_event);
    g_stop_event = nullptr;
    {
        std::scoped_lock lock(g_agent_path_mutex);
        g_agent_path.clear();
    }
    report_status(SERVICE_STOPPED);
}

} // namespace

int main() {
    SERVICE_TABLE_ENTRYW table[] = {
        {const_cast<wchar_t*>(kServiceName), service_main},
        {nullptr, nullptr},
    };
    if (!StartServiceCtrlDispatcherW(table) &&
        GetLastError() == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
        return 2;
    }
    return 0;
}
