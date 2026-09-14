#include "nstu/agent_protocol.hpp"
#include "nstu/client_config.hpp"
#include "nstu/client_control.hpp"
#include "nstu/control_messages.hpp"
#include "nstu/deployment.hpp"
#include "nstu/exam_control.hpp"
#include "nstu/exam_sync.hpp"
#include "nstu/session.hpp"

#include <windows.h>
#include <wtsapi32.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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
nstu::exam::AnswerOutbox g_exam_outbox;
std::mutex g_exam_inflight_mutex;
// The value is the complete exam context key.  Keeping it alongside the
// event key lets the scheduler enforce one in-flight event per context while
// still matching acknowledgements by the event hash.
std::map<std::string, std::string> g_exam_inflight;
std::map<std::string, std::chrono::steady_clock::time_point>
    g_exam_retry_after;
std::mutex g_exam_ingress_mutex;
std::deque<nstu::exam::AnswerEvent> g_exam_ingress_queue;
std::mutex g_client_identity_mutex;
nstu::security::ClientId g_configured_client_id{};
bool g_configured_client_id_ready = false;
std::filesystem::path g_exam_outbox_path;
std::mutex g_exam_outbox_state_mutex;
bool g_exam_outbox_ready = false;
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
constexpr std::size_t kMaximumQueuedExamIngress = 64;
constexpr auto kExamTransientRetryDelay = std::chrono::seconds(2);
constexpr auto kExamRejectedRetryDelay = std::chrono::seconds(30);
constexpr auto kExamAckTimeout = std::chrono::seconds(5);

std::string exam_inflight_key(const nstu::exam::AnswerEvent& event) {
    // Keep the session/sequence/hash prefix fixed so an ACK can retire only
    // the exact event it acknowledges. The variable-length context suffix
    // prevents colliding contexts from suppressing one another in the set.
    const auto& session_id = event.session_id;
    std::string key(reinterpret_cast<const char*>(session_id.data()),
                    session_id.size());
    for (std::size_t index = 0; index < sizeof(event.sequence); ++index) {
        key.push_back(static_cast<char>((event.sequence >> (index * 8u)) &
                                        0xffu));
    }
    const auto event_hash = nstu::exam::hash_answer_event(event);
    if (event_hash) {
        key.append(reinterpret_cast<const char*>(event_hash->data()),
                   event_hash->size());
    } else {
        // Pending outbox events have already passed validation, but retain a
        // fixed-width key if hashing unexpectedly fails so parsing remains
        // bounded and deterministic.
        key.resize(key.size() + nstu::security::kSha256Bytes, '\0');
    }
    key.append(event.package_id);
    key.push_back('\0');
    key.append(reinterpret_cast<const char*>(event.package_digest.data()),
               event.package_digest.size());
    key.append(reinterpret_cast<const char*>(event.client_id.data()),
               event.client_id.size());
    key.append(reinterpret_cast<const char*>(event.candidate_id.data()),
               event.candidate_id.size());
    return key;
}

std::string exam_context_key(const nstu::exam::AnswerEvent& event) {
    // Length-prefix the variable fields and append the fixed-size identities
    // verbatim.  This is an internal map key, not a wire or security token,
    // but the framing prevents ambiguous concatenations.
    std::string key;
    const auto append_text = [&key](std::string_view value) {
        for (std::size_t index = 0; index < sizeof(std::uint32_t); ++index) {
            key.push_back(static_cast<char>((value.size() >> (index * 8u)) &
                                             0xffu));
        }
        key.append(value);
    };
    append_text(event.package_id);
    key.append(reinterpret_cast<const char*>(event.package_digest.data()),
               event.package_digest.size());
    key.append(reinterpret_cast<const char*>(event.client_id.data()),
               event.client_id.size());
    key.append(reinterpret_cast<const char*>(event.session_id.data()),
               event.session_id.size());
    append_text(event.candidate_id);
    return key;
}

bool inflight_matches_boundary(std::string_view key,
                               const nstu::exam::SessionId& session_id,
                               std::uint64_t sequence) noexcept {
    constexpr std::size_t boundary_bytes =
        sizeof(nstu::exam::SessionId) + sizeof(std::uint64_t);
    if (key.size() < boundary_bytes) {
        return false;
    }
    if (!nstu::security::constant_time_equal(
            std::span<const std::byte>(
                reinterpret_cast<const std::byte*>(key.data()),
                session_id.size()),
            std::span<const std::byte>(session_id))) {
        return false;
    }
    for (std::size_t index = 0; index < sizeof(sequence); ++index) {
        const auto expected = static_cast<char>((sequence >> (index * 8u)) &
                                                0xffu);
        if (key[session_id.size() + index] != expected) {
            return false;
        }
    }
    return true;
}

bool inflight_matches_ack(std::string_view key,
                          const nstu::exam::SessionId& session_id,
                          std::uint64_t sequence,
                          std::span<const std::byte> event_hash) noexcept {
    constexpr std::size_t prefix_bytes =
        sizeof(nstu::exam::SessionId) + sizeof(std::uint64_t) +
        nstu::security::kSha256Bytes;
    if (key.size() < prefix_bytes) {
        return false;
    }
    if (!inflight_matches_boundary(key, session_id, sequence) ||
        event_hash.size() != nstu::security::kSha256Bytes) {
        return false;
    }
    return nstu::security::constant_time_equal(
        std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(key.data()) +
                sizeof(nstu::exam::SessionId) + sizeof(std::uint64_t),
            nstu::security::kSha256Bytes),
        event_hash);
}

void clear_exam_inflight() {
    std::scoped_lock lock(g_exam_inflight_mutex);
    g_exam_inflight.clear();
    g_exam_retry_after.clear();
}

void clear_outbound_queue() {
    std::scoped_lock lock(g_outbound_queue_mutex);
    g_outbound_queue.clear();
}

void clear_agent_queue() {
    std::scoped_lock lock(g_agent_queue_mutex);
    g_agent_queue.clear();
}

void queue_exam_stop() {
    std::scoped_lock lock(g_agent_queue_mutex);
    // A stop is a terminal boundary for the current authenticated session.
    // Remove stale start/stop commands so a reconnect cannot resurrect an old
    // exam before the fail-closed stop is delivered.
    std::erase_if(g_agent_queue, [](const auto& message) {
        return message.type == nstu::client::AgentMessageType::exam_start ||
               message.type == nstu::client::AgentMessageType::exam_stop;
    });
    if (g_agent_queue.size() >= kMaximumQueuedAgentMessages) {
        g_agent_queue.pop_front();
    }
    g_agent_queue.push_back(
        {nstu::client::AgentMessageType::exam_stop, {}});
}

void clear_exam_ingress() {
    std::scoped_lock lock(g_exam_ingress_mutex);
    g_exam_ingress_queue.clear();
}

void set_configured_client_id(const nstu::security::ClientId& client_id) {
    bool changed = false;
    {
        std::scoped_lock lock(g_client_identity_mutex);
        changed = !g_configured_client_id_ready ||
                  g_configured_client_id != client_id;
        g_configured_client_id = client_id;
        g_configured_client_id_ready = true;
    }
    if (changed) {
        // Commands and in-flight exam records are identity-scoped.  Do not
        // carry them across reprovisioning, where a new authenticated session
        // could otherwise transmit an old client's data.
        clear_exam_inflight();
        clear_outbound_queue();
        clear_exam_ingress();
        clear_agent_queue();
    }
}

void clear_configured_client_id() {
    bool was_ready = false;
    {
        std::scoped_lock lock(g_client_identity_mutex);
        was_ready = g_configured_client_id_ready;
        g_configured_client_id.fill(std::byte{0});
        g_configured_client_id_ready = false;
    }
    if (was_ready) {
        clear_exam_inflight();
        clear_outbound_queue();
        clear_exam_ingress();
        clear_agent_queue();
    }
}

std::chrono::steady_clock::duration exam_retry_delay(
    nstu::exam::AnswerAckStatus status) noexcept {
    switch (status) {
    case nstu::exam::AnswerAckStatus::rejected:
    case nstu::exam::AnswerAckStatus::conflict:
        return kExamRejectedRetryDelay;
    case nstu::exam::AnswerAckStatus::gap:
    case nstu::exam::AnswerAckStatus::unavailable:
        return kExamTransientRetryDelay;
    default:
        return kExamTransientRetryDelay;
    }
}

bool configured_client_id_matches(
    const nstu::security::ClientId& client_id) noexcept {
    std::scoped_lock lock(g_client_identity_mutex);
    return g_configured_client_id_ready && client_id == g_configured_client_id;
}

std::optional<nstu::security::ClientId> configured_client_id() {
    std::scoped_lock lock(g_client_identity_mutex);
    if (!g_configured_client_id_ready) return std::nullopt;
    return g_configured_client_id;
}

bool queue_exam_event_payload(std::span<const std::byte> payload) {
    std::string decode_error;
    const auto event = nstu::exam::decode_answer_event(payload, &decode_error);
    if (!event) {
        OutputDebugStringA(("NSTU exam event rejected: " + decode_error + "\n").c_str());
        return false;
    }
    const auto client_id = configured_client_id();
    if (client_id && event->client_id != *client_id) {
        // The interactive agent is authenticated as a process, but its
        // variable-length exam payload must still be bound to the client
        // identity provisioned for this service instance.
        OutputDebugStringA("NSTU exam event rejected: client identity mismatch\n");
        return false;
    }
    std::string outbox_error;
    {
        std::scoped_lock lock(g_exam_outbox_state_mutex);
        if (g_exam_outbox_ready && client_id &&
            g_exam_outbox.enqueue(*event, &outbox_error)) {
            return true;
        }
    }
    // Keep a validated event until the configuration/outbox becomes usable;
    // the browser's own durable queue remains the second recovery layer.
    {
        std::scoped_lock lock(g_exam_ingress_mutex);
        if (g_exam_ingress_queue.size() >= kMaximumQueuedExamIngress) {
            g_exam_ingress_queue.pop_front();
        }
        g_exam_ingress_queue.push_back(*event);
    }
    if (!outbox_error.empty()) {
        OutputDebugStringA(("NSTU exam event queued for retry: " + outbox_error +
                            "\n").c_str());
    }
    return false;
}

void drain_exam_ingress() {
    const auto client_id = configured_client_id();
    if (!client_id) return;
    std::deque<nstu::exam::AnswerEvent> batch;
    {
        std::scoped_lock lock(g_exam_ingress_mutex);
        batch.swap(g_exam_ingress_queue);
    }
    if (batch.empty()) return;
    std::deque<nstu::exam::AnswerEvent> retry;
    while (!batch.empty()) {
        auto event = std::move(batch.front());
        batch.pop_front();
        if (event.client_id != *client_id) {
            OutputDebugStringA("NSTU queued exam event discarded after identity change\n");
            continue;
        }
        std::string error;
        bool queued = false;
        {
            std::scoped_lock lock(g_exam_outbox_state_mutex);
            queued = g_exam_outbox_ready && g_exam_outbox.enqueue(event, &error);
        }
        if (!queued) {
            retry.push_back(std::move(event));
            if (!error.empty()) {
                OutputDebugStringA(("NSTU queued exam event remains pending: " +
                                    error + "\n").c_str());
            }
        }
    }
    if (!retry.empty()) {
        std::scoped_lock lock(g_exam_ingress_mutex);
        while (!retry.empty()) {
            if (g_exam_ingress_queue.size() >= kMaximumQueuedExamIngress) {
                g_exam_ingress_queue.pop_front();
            }
            g_exam_ingress_queue.push_back(std::move(retry.front()));
            retry.pop_front();
        }
    }
}

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
    {
        std::scoped_lock lock(g_outbound_queue_mutex);
        if (!g_outbound_queue.empty()) {
            auto message = std::move(g_outbound_queue.front());
            g_outbound_queue.pop_front();
            return message;
        }
    }

    const auto client_id = configured_client_id();
    if (!client_id) {
        return std::nullopt;
    }
    std::vector<nstu::exam::AnswerEvent> pending;
    {
        std::scoped_lock lock(g_exam_outbox_state_mutex);
        if (!g_exam_outbox_ready) {
            return std::nullopt;
        }
        pending = g_exam_outbox.pending_for_client(*client_id, 8);
    }
    const auto now = std::chrono::steady_clock::now();
    for (const auto& event : pending) {
        const auto key = exam_inflight_key(event);
        const auto context_key = exam_context_key(event);
        {
            std::scoped_lock lock(g_exam_inflight_mutex);
            // Never pipeline events from one exam context.  If the first ACK
            // is lost, sending the next sequence would let a cumulative ACK
            // retire both records while the browser still owns the first
            // pending event.  A timed retry may release the guard, but only
            // after the same event has had a chance to be acknowledged.
            bool context_inflight = false;
            for (auto inflight = g_exam_inflight.begin();
                 inflight != g_exam_inflight.end();) {
                if (inflight->second != context_key) {
                    ++inflight;
                    continue;
                }
                const auto retry = g_exam_retry_after.find(inflight->first);
                if (retry != g_exam_retry_after.end() &&
                    retry->second <= now) {
                    g_exam_retry_after.erase(retry);
                    inflight = g_exam_inflight.erase(inflight);
                    continue;
                }
                context_inflight = true;
                break;
            }
            if (context_inflight) {
                continue;
            }
            const auto retry = g_exam_retry_after.find(key);
            if (retry != g_exam_retry_after.end()) {
                if (retry->second > now) {
                    continue;
                }
                g_exam_retry_after.erase(retry);
            }
            if (!g_exam_inflight.emplace(key, context_key).second) {
                continue;
            }
            g_exam_retry_after.insert_or_assign(
                key, now + kExamAckTimeout);
        }
        const auto payload = nstu::exam::encode_answer_event(event);
        if (payload.empty()) {
            std::scoped_lock lock(g_exam_inflight_mutex);
            g_exam_inflight.erase(key);
            continue;
        }
        return nstu::client::ClientOutboundCommand{
            nstu::protocol::CommandType::exam_answer_event,
            payload};
    }
    return std::nullopt;
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
                } else if (message->type ==
                           nstu::client::AgentMessageType::exam_answer_event) {
                    (void)queue_exam_event_payload(message->payload);
                } else if (message->type ==
                           nstu::client::AgentMessageType::exam_state_request) {
                    const auto request =
                        nstu::exam::decode_state_request(message->payload);
                    if (request &&
                        configured_client_id_matches(request->client_id)) {
                        queue_outbound_message(
                            nstu::protocol::CommandType::exam_state_request,
                            std::move(message->payload));
                    }
                }
            }
            Sleep(25);
        }
        g_agent_connected = false;
        pipe.close();
        if (!g_stop_requested.load()) {
            // A replacement agent must never inherit an active exam from a
            // stale interactive process. The agent also stops its host locally
            // when it observes this pipe loss; this queued command covers the
            // short reconnect window and any newly launched process.
            queue_exam_stop();
        }
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

std::filesystem::path exam_outbox_path() {
    const auto root = nstu::deployment::data_root(nullptr);
    return root.empty() ? std::filesystem::path{}
                        : root / L"exam-answer-outbox.bin";
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
    case nstu::protocol::CommandType::exam_start: {
        // The TCP command is authenticated by the client control session.
        // Bind the variable-length request to the identity provisioned for
        // this service before it crosses into the interactive agent.
        const auto request =
            nstu::exam::decode_exam_start_request(command.payload);
        if (request && configured_client_id_matches(request->client_id)) {
            queue_agent_message({nstu::client::AgentMessageType::exam_start,
                                 command.payload});
        }
        break;
    }
    case nstu::protocol::CommandType::exam_stop:
        if (command.payload.empty()) {
            queue_exam_stop();
        }
        break;
    case nstu::protocol::CommandType::exam_answer_ack: {
        const auto ack = nstu::exam::decode_answer_ack(command.payload);
        if (!ack) {
            break;
        }
        const bool durable_ack =
            ack->status == nstu::exam::AnswerAckStatus::accepted ||
            ack->status == nstu::exam::AnswerAckStatus::duplicate;
        bool outbox_ready = false;
        bool outbox_acknowledged = false;
        std::string outbox_ack_error;
        if (durable_ack) {
            std::scoped_lock lock(g_exam_outbox_state_mutex);
            outbox_ready = g_exam_outbox_ready;
            if (outbox_ready) {
                outbox_acknowledged = g_exam_outbox.acknowledge(
                    ack->session_id, ack->sequence, ack->event_hash,
                    &outbox_ack_error);
            }
        }
        std::vector<std::string> matching_keys;
        {
            std::scoped_lock lock(g_exam_inflight_mutex);
            for (const auto& [key, context] : g_exam_inflight) {
                (void)context;
                if (inflight_matches_ack(key, ack->session_id,
                                         ack->sequence, ack->event_hash)) {
                    matching_keys.push_back(key);
                }
            }
            const auto key_is_matched = [&](const std::string& key) {
                return std::find(matching_keys.begin(), matching_keys.end(),
                                 key) != matching_keys.end();
            };
            std::erase_if(g_exam_inflight, [&](const auto& entry) {
                return key_is_matched(entry.first);
            });
            if (outbox_acknowledged) {
                std::erase_if(g_exam_retry_after, [&](const auto& entry) {
                    return key_is_matched(entry.first);
                });
            } else {
                const auto retry_at = std::chrono::steady_clock::now() +
                    (durable_ack ? kExamRejectedRetryDelay
                                 : exam_retry_delay(ack->status));
                for (const auto& key : matching_keys) {
                    // The next send is deliberately delayed. This keeps a
                    // rejected or temporarily unavailable event durable
                    // without spinning the control loop or dropping it.
                    g_exam_retry_after.insert_or_assign(key, retry_at);
                }
            }
            if (matching_keys.empty()) {
                // Do not associate a stale or malformed response with a
                // context using session/sequence alone.  A unique boundary
                // is safe only as a retry hint; the in-flight record remains
                // until the timeout above releases it, and no ACK is exposed
                // to the browser.
                std::vector<std::string> boundary_keys;
                for (const auto& [key, context] : g_exam_inflight) {
                    (void)context;
                    if (inflight_matches_boundary(key, ack->session_id,
                                                   ack->sequence)) {
                        boundary_keys.push_back(key);
                    }
                }
                if (boundary_keys.size() == 1) {
                    g_exam_retry_after.insert_or_assign(
                        boundary_keys.front(),
                        std::chrono::steady_clock::now() +
                            exam_retry_delay(ack->status));
                }
            }
        }
        // A response that cannot be tied to the exact in-flight event is
        // stale or malformed.  Never forward it to the browser, where a
        // session/sequence-only check could mutate the wrong context.
        if (matching_keys.empty()) {
            break;
        }
        if (durable_ack && (!outbox_ready || !outbox_acknowledged)) {
            const auto detail = outbox_ack_error.empty()
                                    ? (outbox_ready
                                           ? "durable outbox rejected ACK"
                                           : "durable outbox is unavailable")
                                    : outbox_ack_error;
            OutputDebugStringA(("NSTU exam ACK suppressed: " + detail +
                                "\n")
                                   .c_str());
            // An accepted/duplicate ACK is exposed to the exam UI only after
            // the local write-ahead outbox has verified and retired the exact
            // event hash. Otherwise the UI could discard an answer that is
            // still only present in volatile transport state.
            break;
        }
        queue_agent_message(
            {nstu::client::AgentMessageType::exam_answer_ack,
             command.payload});
        break;
    }
    case nstu::protocol::CommandType::exam_state_response:
        if (const auto response =
                nstu::exam::decode_state_response(command.payload);
            response && configured_client_id_matches(response->client_id)) {
            queue_agent_message(
                {nstu::client::AgentMessageType::exam_state_response,
                 command.payload});
        }
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
            set_configured_client_id(config.client_id);
            // Replay validated browser events that arrived before the
            // service finished opening its identity-bound outbox.
            drain_exam_ingress();
            std::string ignored_error;
            (void)nstu::client::run_client_control_session(
                config, stop_token, current_status, handle_server_command,
                pop_outbound_message,
                &ignored_error);
            // Commands are only marked in-flight until the authenticated TCP
            // session successfully delivers an acknowledgement. A disconnect
            // must make every unsatisfied event eligible on the next session.
            clear_exam_inflight();
            queue_exam_stop();
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
    {
        const auto outbox_path = exam_outbox_path();
        std::string outbox_error;
        const bool opened = !outbox_path.empty() &&
            g_exam_outbox.open(outbox_path, &outbox_error);
        {
            std::scoped_lock lock(g_exam_outbox_state_mutex);
            g_exam_outbox_path = outbox_path;
            g_exam_outbox_ready = opened;
        }
        if (!opened) {
            OutputDebugStringA(("NSTU exam outbox unavailable: " +
                                (outbox_error.empty()
                                     ? "path is unavailable"
                                     : outbox_error) +
                                "; refusing to start service\n")
                                   .c_str());
            {
                std::scoped_lock lock(g_exam_outbox_state_mutex);
                g_exam_outbox.close();
                g_exam_outbox_ready = false;
                g_exam_outbox_path.clear();
            }
            CloseHandle(g_stop_event);
            g_stop_event = nullptr;
            report_status(SERVICE_STOPPED, ERROR_OPEN_FAILED);
            return;
        }
    }
    g_stop_requested = false;
    std::thread pipe_thread(agent_pipe_loop);
    std::jthread control_thread(remote_control_loop);
    std::jthread agent_supervisor_thread(agent_supervisor_loop);
    launch_agent();
    report_status(SERVICE_RUNNING);
    WaitForSingleObject(g_stop_event, INFINITE);
    g_stop_requested = true;
    queue_exam_stop();
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
    {
        std::scoped_lock lock(g_exam_outbox_state_mutex);
        g_exam_outbox.close();
        g_exam_outbox_ready = false;
        g_exam_outbox_path.clear();
    }
    {
        clear_configured_client_id();
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
