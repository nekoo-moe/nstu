#include "nstu/agent_protocol.hpp"
#include "nstu/client_config.hpp"
#include "nstu/client_control.hpp"
#include "nstu/client_freeze.hpp"
#include "nstu/client_pairing.hpp"
#include "nstu/client_uwf_request.hpp"
#include "nstu/control_messages.hpp"
#include "nstu/deployment.hpp"
#include "nstu/exam_control.hpp"
#include "nstu/exam_sync.hpp"
#include "nstu/session.hpp"
#include "nstu/setup/diagnostics.hpp"
#include "nstu/setup/uwf.hpp"

#include <windows.h>
#include <reason.h>
#include <wtsapi32.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <chrono>
#include <condition_variable>
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

constexpr const wchar_t* kServiceName = nstu::client::kManagedServiceName;
constexpr DWORD kServiceControlLock = 128;
constexpr DWORD kServiceControlUnlock = 129;
constexpr DWORD kServiceControlReloadFreeze =
    static_cast<DWORD>(nstu::client::kFreezeReloadControl);
SERVICE_STATUS_HANDLE g_status_handle = nullptr;
std::mutex g_service_status_mutex;
DWORD g_service_state = SERVICE_STOPPED;
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
// Managed mode as this service currently believes it to be. The registry
// value is the source of truth; this is the copy the control handler can
// consult without touching the registry on every SCM callback.
std::atomic_bool g_frozen = false;
std::atomic_bool g_uwf_configuring = false;
std::mutex g_uwf_worker_mutex;
std::thread g_uwf_worker;
std::mutex g_server_endpoint_mutex;
std::string g_server_address;
std::uint16_t g_server_port = 0;
std::atomic_bool g_agent_locked = false;
std::atomic_bool g_agent_streaming = false;
std::atomic<std::uint8_t> g_agent_stream_fps = 0;
std::atomic_bool g_agent_snapshotting = false;
std::atomic<std::uint16_t> g_agent_snapshot_interval_seconds = 0;
std::atomic_bool g_agent_viewing_broadcast = false;
std::atomic_bool g_desired_locked = false;
std::mutex g_agent_launch_mutex;
std::chrono::steady_clock::time_point g_next_agent_launch{};
// Pairing state. Only the control loop runs an attempt, but the answer
// to the selection menu arrives on the pipe thread, so the handoff is
// guarded.
std::mutex g_pairing_mutex;
std::condition_variable g_pairing_signal;
std::optional<std::uint16_t> g_pairing_selection;
std::size_t g_pairing_choice_count = 0;
std::chrono::steady_clock::time_point g_next_pairing_sweep{};
constexpr std::size_t kMaximumQueuedAgentMessages = 256;
constexpr std::size_t kMaximumQueuedOutboundMessages = 32;
constexpr std::size_t kMaximumQueuedExamIngress = 64;
constexpr auto kExamTransientRetryDelay = std::chrono::seconds(2);
constexpr auto kExamRejectedRetryDelay = std::chrono::seconds(30);
constexpr auto kExamAckTimeout = std::chrono::seconds(5);
// An unenrolled machine sweeps the LAN until it finds a server. Doing
// that on the reconnect cadence would put a broadcast from every machine
// in a lab on the wire every few seconds at bell time, which is noise
// nobody benefits from: a teacher opening the pairing window is not in a
// hurry.
constexpr auto kPairingSweepInterval = std::chrono::seconds(10);
// Long enough for somebody to read a short list and point at a name.
constexpr auto kPairingSelectionTimeout = std::chrono::seconds(120);
constexpr auto kPairingSelectionSlice = std::chrono::milliseconds(200);

void report_status(DWORD state, DWORD error = NO_ERROR);
void refresh_running_status();
void reload_local_freeze_state();
bool apply_remote_freeze_state(bool frozen, std::string* error);
bool begin_service_stop();

void update_diagnostic_endpoint_cache(
    const nstu::discovery::ServerEndpoint& endpoint) noexcept {
    HKEY key = nullptr;
    const LONG opened = RegOpenKeyExW(
        HKEY_LOCAL_MACHINE, L"Software\\NSTU", 0,
        KEY_SET_VALUE | KEY_WOW64_64KEY, &key);
    if (opened != ERROR_SUCCESS) {
        OutputDebugStringA(
            "NSTU diagnostic endpoint registry cache could not be opened\n");
        return;
    }
    const std::wstring address(endpoint.address.begin(), endpoint.address.end());
    const DWORD address_bytes = static_cast<DWORD>(
        (address.size() + 1) * sizeof(wchar_t));
    const DWORD port = endpoint.port;
    const LONG address_result = RegSetValueExW(
        key, L"ServerAddress", 0, REG_SZ,
        reinterpret_cast<const BYTE*>(address.c_str()), address_bytes);
    const LONG port_result = RegSetValueExW(
        key, L"ServerPort", 0, REG_DWORD,
        reinterpret_cast<const BYTE*>(&port), sizeof(port));
    RegCloseKey(key);
    if (address_result != ERROR_SUCCESS || port_result != ERROR_SUCCESS) {
        OutputDebugStringA(
            "NSTU diagnostic endpoint registry cache update failed\n");
    }
}

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

void queue_pairing_status(nstu::client::PairingOutcome outcome,
                          std::string_view detail) {
    nstu::client::AgentPairingStatus status;
    status.outcome = static_cast<std::uint8_t>(outcome);
    status.detail = detail.empty()
        ? std::string(nstu::client::pairing_outcome_text(outcome))
        : std::string(detail.substr(
              0, std::min(detail.size(),
                          nstu::client::kMaximumPairingTextBytes)));
    auto payload = nstu::client::encode_agent_pairing_status(status);
    if (payload.empty()) {
        // The detail is whatever the failing layer wrote, so it can carry
        // characters the codec will not put on a screen. The outcome
        // still has to reach the agent, so fall back to the sentence that
        // goes with it.
        status.detail = nstu::client::pairing_outcome_text(outcome);
        payload = nstu::client::encode_agent_pairing_status(status);
    }
    if (!payload.empty()) {
        queue_agent_message(
            {nstu::client::AgentMessageType::pairing_status,
             std::move(payload)});
    }
}

void open_pairing_menu(std::size_t choices) {
    std::scoped_lock lock(g_pairing_mutex);
    g_pairing_choice_count = choices;
    g_pairing_selection.reset();
}

// An index for a menu nobody is showing, or one past its end, is dropped
// rather than trusted. The agent runs as the interactive user, and this
// is the service deciding which server it is about to hand an identity.
void record_pairing_selection(std::span<const std::byte> payload) {
    const auto index =
        nstu::client::decode_agent_pairing_selection(payload);
    std::scoped_lock lock(g_pairing_mutex);
    if (!index || *index >= g_pairing_choice_count) {
        return;
    }
    g_pairing_selection = *index;
    g_pairing_signal.notify_all();
}

void cancel_pairing_menu() {
    std::scoped_lock lock(g_pairing_mutex);
    g_pairing_choice_count = 0;
    g_pairing_selection.reset();
    g_pairing_signal.notify_all();
}

std::optional<std::uint16_t> await_pairing_selection(
    const std::stop_token& stop_token) {
    const auto deadline =
        std::chrono::steady_clock::now() + kPairingSelectionTimeout;
    std::unique_lock lock(g_pairing_mutex);
    while (!g_pairing_selection && !stop_token.stop_requested() &&
           g_agent_connected.load() &&
           std::chrono::steady_clock::now() < deadline) {
        // A desktop that goes away mid-menu never signals the condition
        // variable, so the wait is sliced rather than left to a wake-up
        // that may never arrive.
        g_pairing_signal.wait_for(lock, kPairingSelectionSlice);
    }
    auto selection = g_pairing_selection;
    g_pairing_selection.reset();
    g_pairing_choice_count = 0;
    return selection;
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
            {nstu::client::AgentMessageType::managed_state,
             nstu::control::encode_freeze_state(g_frozen.load())});
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
                           nstu::client::AgentMessageType::pairing_select) {
                    record_pairing_selection(message->payload);
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
        cancel_pairing_menu();
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

nstu::control::UwfConfigureOutcome wire_uwf_outcome(
    nstu::setup::UwfConfigureOutcome outcome) noexcept {
    using Setup = nstu::setup::UwfConfigureOutcome;
    using Wire = nstu::control::UwfConfigureOutcome;
    switch (outcome) {
    case Setup::armed: return Wire::armed;
    case Setup::already_enabled: return Wire::already_enabled;
    case Setup::unsupported_edition: return Wire::unsupported_edition;
    case Setup::feature_missing: return Wire::feature_missing;
    case Setup::probe_unavailable: return Wire::probe_unavailable;
    case Setup::provider_unavailable: return Wire::provider_unavailable;
    case Setup::reboot_pending: return Wire::reboot_pending;
    case Setup::invalid_data_root: return Wire::invalid_data_root;
    case Setup::readiness_failed: return Wire::readiness_failed;
    case Setup::checkpoint_required: return Wire::checkpoint_required;
    case Setup::access_denied: return Wire::access_denied;
    case Setup::failed: return Wire::failed;
    }
    return Wire::failed;
}

void queue_uwf_report(nstu::control::UwfConfigureReport report) {
    auto payload = nstu::control::encode_uwf_configure_report(report);
    if (!payload.empty()) {
        queue_outbound_message(nstu::protocol::CommandType::uwf_report,
                               std::move(payload));
    }
}

bool schedule_uwf_restart() noexcept {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        return false;
    }
    TOKEN_PRIVILEGES requested{};
    requested.PrivilegeCount = 1;
    if (!LookupPrivilegeValueW(nullptr, L"SeShutdownPrivilege",
                               &requested.Privileges[0].Luid)) {
        CloseHandle(token);
        return false;
    }
    requested.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    TOKEN_PRIVILEGES previous{};
    DWORD previous_bytes = sizeof(previous);
    SetLastError(ERROR_SUCCESS);
    const bool enabled =
        AdjustTokenPrivileges(token, FALSE, &requested, sizeof(previous),
                              &previous, &previous_bytes) != FALSE &&
        GetLastError() == ERROR_SUCCESS;
    bool scheduled = false;
    if (enabled) {
        // Visible countdown, no forced app close. The operator can cancel with
        // `shutdown /a` during the minute if work still needs saving.
        // Sources:
        // https://learn.microsoft.com/en-us/windows/win32/api/winreg/nf-winreg-initiatesystemshutdownexw
        // https://learn.microsoft.com/en-us/windows/win32/shutdown/system-shutdown-reason-codes
        scheduled = InitiateSystemShutdownExW(
            nullptr,
            const_cast<wchar_t*>(
                L"NSTU reboot-to-restore setup completed. Save work; this "
                L"computer will restart in 60 seconds."),
            60, FALSE, TRUE,
            SHTDN_REASON_MAJOR_APPLICATION |
                SHTDN_REASON_MINOR_INSTALLATION |
                SHTDN_REASON_FLAG_PLANNED) != FALSE;
        if (previous.PrivilegeCount != 0) {
            (void)AdjustTokenPrivileges(token, FALSE, &previous, 0, nullptr,
                                        nullptr);
        }
    }
    CloseHandle(token);
    return scheduled;
}

void configure_uwf_async(bool checkpoint_acknowledged,
                         bool clear_installer_request = false) {
    bool expected = false;
    if (!g_uwf_configuring.compare_exchange_strong(expected, true)) {
        queue_uwf_report({
            .outcome = nstu::control::UwfConfigureOutcome::busy,
            .detail = "UWF configuration is already running",
        });
        return;
    }
    try {
        std::scoped_lock worker_lock(g_uwf_worker_mutex);
        if (g_uwf_worker.joinable()) {
            g_uwf_worker.join();
        }
        std::string server_address;
        std::uint16_t server_port = 0;
        {
            std::scoped_lock endpoint_lock(g_server_endpoint_mutex);
            server_address = g_server_address;
            server_port = g_server_port;
        }
        g_uwf_worker = std::thread([
            checkpoint_acknowledged, clear_installer_request,
            server_address = std::move(server_address), server_port] {
            struct BusyGuard {
                ~BusyGuard() { g_uwf_configuring = false; }
            } guard;
            try {
                bool readiness_passed = true;
                nstu::setup::DiagnosticOptions options;
                options.role = nstu::setup::DiagnosticRole::client;
                options.boot_check = true;
                options.server_address.assign(server_address.begin(),
                                              server_address.end());
                options.server_port = server_port;
                nstu::setup::run_startup_diagnostics(
                    options, {}, [&](nstu::setup::DiagnosticResult result) {
                        if (result.severity ==
                            nstu::setup::DiagnosticSeverity::failure) {
                            readiness_passed = false;
                        }
                    });
                const auto data_root = nstu::deployment::data_root(nullptr);
                const auto result = nstu::setup::configure_uwf({
                    .data_root = data_root,
                    .diagnostic_readiness_passed = readiness_passed,
                    .checkpoint_acknowledged = checkpoint_acknowledged,
                });
                std::string detail = result.detail;
                if (detail.empty()) detail = "UWF configuration failed";
                if (result.reboot_required) {
                    detail = schedule_uwf_restart()
                        ? "UWF is ready; restart scheduled in 60 seconds"
                        : "UWF is ready; automatic restart failed, restart manually";
                }
                detail.resize(std::min(
                    detail.size(), nstu::control::kMaximumUwfDetailBytes));
                queue_uwf_report({
                    .outcome = wire_uwf_outcome(result.outcome),
                    .reboot_required = result.reboot_required,
                    .data_exclusion_ready = result.data_exclusion_added,
                    .registry_exclusion_ready =
                        result.registry_exclusion_added,
                    .detail = std::move(detail),
                });
            } catch (...) {
                queue_uwf_report({
                    .outcome = nstu::control::UwfConfigureOutcome::failed,
                    .detail = "UWF configuration failed unexpectedly",
                });
            }
            if (clear_installer_request) {
                std::string clear_error;
                if (!nstu::client::set_uwf_configuration_requested(
                        false, {}, &clear_error)) {
                    OutputDebugStringA(
                        ("NSTU UWF installer request was not cleared: " +
                         clear_error + "\n")
                            .c_str());
                }
            }
        });
    } catch (...) {
        g_uwf_configuring = false;
        queue_uwf_report({
            .outcome = nstu::control::UwfConfigureOutcome::failed,
            .detail = "UWF configuration worker could not start",
        });
    }
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
    case nstu::protocol::CommandType::uwf_configure: {
        const auto acknowledged =
            nstu::control::decode_uwf_configure_request(command.payload);
        if (acknowledged) {
            configure_uwf_async(*acknowledged);
        }
        break;
    }
    case nstu::protocol::CommandType::freeze_set: {
        const auto requested =
            nstu::control::decode_freeze_state(command.payload);
        if (!requested) {
            break;
        }
        std::string freeze_error;
        const bool applied =
            apply_remote_freeze_state(*requested, &freeze_error);
        if (!applied) {
            OutputDebugStringA(("NSTU managed mode not applied: " +
                                (freeze_error.empty()
                                     ? std::string("registry write failed")
                                     : freeze_error) +
                                "\n")
                                   .c_str());
        }
        // What this machine is now, not what it was asked to be. A teacher
        // whose freeze did not take has to see that here rather than find out
        // when a student stops the service.
        queue_outbound_message(
            nstu::protocol::CommandType::freeze_report,
            nstu::control::encode_freeze_state(g_frozen.load()));
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

// One whole pairing attempt for a machine that holds no key yet. Nothing
// here grants anything by itself: the operator at the server decides, and
// the person at this machine has to be able to read the same six digits,
// which is why an attempt is not started at all when no agent is on the
// desktop to show them. Returns true only once a usable identity is on
// disk.
bool attempt_pairing(const std::stop_token& stop_token,
                     const std::filesystem::path& path,
                     std::span<const std::byte> entropy) {
    if (!g_agent_connected.load()) {
        return false;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now < g_next_pairing_sweep) {
        return false;
    }
    g_next_pairing_sweep = now + kPairingSweepInterval;
    auto candidates = nstu::discovery::discover_pairing_candidates();
    if (candidates.empty() || stop_token.stop_requested()) {
        return false;
    }
    if (candidates.size() > nstu::client::kMaximumPairingChoices) {
        candidates.resize(nstu::client::kMaximumPairingChoices);
    }
    // One server on the LAN is the ordinary classroom, and asking a
    // student to pick the only name on a list teaches them nothing. The
    // approval on the far side is what makes this safe to skip, not the
    // menu.
    std::size_t chosen = 0;
    if (candidates.size() > 1) {
        std::vector<nstu::client::AgentPairingChoice> choices;
        choices.reserve(candidates.size());
        for (const auto& candidate : candidates) {
            choices.push_back({candidate.server_name, candidate.address,
                               candidate.port});
        }
        auto payload =
            nstu::client::encode_agent_pairing_choices(choices);
        if (payload.empty()) {
            return false;
        }
        open_pairing_menu(candidates.size());
        queue_agent_message(
            {nstu::client::AgentMessageType::pairing_choices,
             std::move(payload)});
        const auto selection = await_pairing_selection(stop_token);
        if (!selection) {
            return false;
        }
        chosen = *selection;
    }

    std::string identity_error;
    const auto uuid = nstu::client::machine_uuid(&identity_error);
    if (uuid.empty()) {
        queue_pairing_status(nstu::client::PairingOutcome::failed,
                             identity_error);
        return false;
    }
    std::string error;
    auto result = nstu::client::pair_with_server(
        candidates[chosen], uuid, nstu::client::machine_hostname(),
        [](const std::string& code, const std::string& server_name) {
            auto payload = nstu::client::encode_agent_pairing_code(
                {code, server_name});
            if (!payload.empty()) {
                queue_agent_message(
                    {nstu::client::AgentMessageType::pairing_code,
                     std::move(payload)});
            }
        },
        stop_token, {}, &error);
    if (result.outcome != nstu::client::PairingOutcome::enrolled) {
        nstu::client::clear_client_runtime_config(result.config);
        queue_pairing_status(result.outcome, error);
        return false;
    }
    std::string save_error;
    const bool saved = nstu::client::save_client_runtime_config(
        result.config, path.wstring(), entropy, &save_error);
    nstu::client::clear_client_runtime_config(result.config);
    if (!saved) {
        // The server has recorded a key this machine can no longer
        // produce, so claiming success would leave a computer that looks
        // enrolled and never connects. Saying it failed is also what gets
        // it paired again, and the server replaces the key for the same
        // identity rather than accumulating one.
        queue_pairing_status(nstu::client::PairingOutcome::failed,
                             save_error);
        return false;
    }
    queue_pairing_status(nstu::client::PairingOutcome::enrolled, {});
    return true;
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
            // A teacher reconnecting sees managed mode as the machine has it,
            // not as the server last remembered it.
            queue_outbound_message(
                nstu::protocol::CommandType::freeze_report,
                nstu::control::encode_freeze_state(g_frozen.load()));
            std::string ignored_error;
            const auto endpoint_observer =
                [&](const nstu::discovery::ServerEndpoint& endpoint) {
                    // The observer runs only after mutual authentication. An
                    // installer request therefore cannot mutate UWF merely
                    // because a config file names an unreachable or spoofed
                    // endpoint.
                    {
                        std::scoped_lock endpoint_lock(g_server_endpoint_mutex);
                        g_server_address = endpoint.address;
                        g_server_port = endpoint.port;
                    }
                    if (nstu::client::uwf_configuration_requested()) {
                        configure_uwf_async(true, true);
                    }
                    if (endpoint.address == config.server_address &&
                        endpoint.port == config.server_port) {
                        return;
                    }
                    auto updated = config;
                    updated.server_address = endpoint.address;
                    updated.server_port = endpoint.port;
                    std::string save_error;
                    if (!nstu::client::save_client_runtime_config(
                            updated, path.wstring(), entropy, &save_error)) {
                        OutputDebugStringA(
                            ("NSTU authenticated endpoint cache update failed: " +
                             save_error + "\n")
                                .c_str());
                    } else {
                        update_diagnostic_endpoint_cache(endpoint);
                    }
                    nstu::client::clear_client_runtime_config(updated);
                };
            {
                std::scoped_lock endpoint_lock(g_server_endpoint_mutex);
                g_server_address = config.server_address;
                g_server_port = config.server_port;
            }
            (void)nstu::client::run_client_control_session(
                config, stop_token, current_status, handle_server_command,
                pop_outbound_message,
                &ignored_error, endpoint_observer);
            // Commands are only marked in-flight until the authenticated TCP
            // session successfully delivers an acknowledgement. A disconnect
            // must make every unsatisfied event eligible on the next session.
            clear_exam_inflight();
            queue_exam_stop();
            set_desired_lock(false);
            queue_agent_message(
                {nstu::client::AgentMessageType::stop_stream, {}});
            queue_agent_message(
                {nstu::client::AgentMessageType::stop_snapshots, {}});
            queue_agent_message(
                {nstu::client::AgentMessageType::host_broadcast_stop, {}});
            queue_agent_message(
                {nstu::client::AgentMessageType::overlay_clear, {}});
            queue_agent_message({nstu::client::AgentMessageType::remote_end, {}});
            nstu::client::clear_client_runtime_config(config);
        } else if (attempt_pairing(stop_token, path, entropy)) {
            // A machine that just earned an identity should use it now
            // rather than sit out the reconnect delay first.
            continue;
        }
        std::array<std::byte, 2> jitter_bytes{};
        const bool have_jitter = nstu::security::generate_random(jitter_bytes);
        const auto jitter = have_jitter
            ? (std::to_integer<unsigned int>(jitter_bytes[0]) |
               (std::to_integer<unsigned int>(jitter_bytes[1]) << 8u)) % 31u
            : 20u;
        const auto reconnect_ticks = 30u + jitter;
        for (std::uint32_t tick = 0;
             tick < reconnect_ticks && !stop_token.stop_requested(); ++tick) {
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

void publish_status(DWORD state, DWORD error) {
    SERVICE_STATUS status{};
    status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    status.dwCurrentState = state;
    status.dwWin32ExitCode = error;
    // A frozen machine does not advertise stop at all, so Services and
    // `sc stop` grey it out rather than appearing to work and then failing.
    const DWORD running_controls = SERVICE_ACCEPT_SESSIONCHANGE |
        (g_frozen.load() ? 0u : static_cast<DWORD>(SERVICE_ACCEPT_STOP));
    status.dwControlsAccepted =
        state == SERVICE_RUNNING ? running_controls : 0;
    if (g_status_handle != nullptr) {
        SetServiceStatus(g_status_handle, &status);
    }
}

void report_status(DWORD state, DWORD error) {
    std::scoped_lock lock(g_service_status_mutex);
    g_service_state = state;
    publish_status(state, error);
}

void refresh_running_status() {
    std::scoped_lock lock(g_service_status_mutex);
    // A freeze reply can race an SCM stop. Never move the service from
    // STOP_PENDING back to RUNNING merely to update accepted controls.
    if (g_service_state == SERVICE_RUNNING) {
        publish_status(SERVICE_RUNNING, NO_ERROR);
    }
}

void reload_local_freeze_state() {
    std::scoped_lock lock(g_service_status_mutex);
    g_frozen = nstu::client::machine_frozen();
    if (g_service_state == SERVICE_RUNNING) {
        publish_status(SERVICE_RUNNING, NO_ERROR);
    }
    queue_agent_message(
        {nstu::client::AgentMessageType::managed_state,
         nstu::control::encode_freeze_state(g_frozen.load())});
}

bool apply_remote_freeze_state(bool frozen, std::string* error) {
    std::scoped_lock lock(g_service_status_mutex);
    if (g_service_state != SERVICE_RUNNING) {
        if (error != nullptr) {
            *error = "service is stopping";
        }
        return false;
    }
    // Serialize persistence with the SCM stop decision. Once this write
    // succeeds, no stop can slip through before g_frozen is enforced.
    if (!nstu::client::set_machine_frozen(frozen, {}, error)) {
        return false;
    }
    g_frozen = frozen;
    publish_status(SERVICE_RUNNING, NO_ERROR);
    queue_agent_message(
        {nstu::client::AgentMessageType::managed_state,
         nstu::control::encode_freeze_state(frozen)});
    return true;
}

bool begin_service_stop() {
    std::scoped_lock lock(g_service_status_mutex);
    if (g_frozen.load() || g_service_state != SERVICE_RUNNING) {
        return false;
    }
    g_service_state = SERVICE_STOP_PENDING;
    publish_status(SERVICE_STOP_PENDING, NO_ERROR);
    return true;
}

DWORD WINAPI control_handler(DWORD control, DWORD event_type, void*, void*) {
    if (control == SERVICE_CONTROL_STOP && g_stop_event != nullptr) {
        if (!begin_service_stop()) {
            // Refused out loud rather than ignored: whoever asked gets an
            // error they can act on - clear managed mode from the server,
            // or run `nstu-service.exe --thaw-local` elevated.
            refresh_running_status();
            return static_cast<DWORD>(ERROR_ACCESS_DENIED);
        }
        g_stop_requested = true;
        SetEvent(g_stop_event);
        wake_pipe_listener();
    } else if (control == kServiceControlReloadFreeze) {
        // A local thaw writes the registry and then sends this, so the stop
        // verb comes back without waiting for a restart.
        reload_local_freeze_state();
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
    g_frozen = nstu::client::machine_frozen();
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
    {
        std::scoped_lock worker_lock(g_uwf_worker_mutex);
        if (g_uwf_worker.joinable()) {
            g_uwf_worker.join();
        }
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

int main(int argc, char** argv) {
    // The only command line this service takes. Managed mode has to be
    // undoable on the machine itself, by someone who is already an
    // administrator, or it would be a lock with no key for a school whose
    // server has been reinstalled.
    if (argc > 1) {
        const std::string_view argument(
            argc == 2 && argv[1] != nullptr ? argv[1] : "");
        if (argument != "--thaw-local") {
            std::fputs("usage: nstu-service [--thaw-local]\n", stderr);
            return 2;
        }
        std::string error;
        if (!nstu::client::thaw_locally(&error)) {
            std::fputs(("nstu-service: " +
                        (error.empty() ? std::string("managed mode could not "
                                                    "be cleared")
                                       : error) +
                        "\n")
                           .c_str(),
                       stderr);
            return 1;
        }
        std::fputs("nstu-service: managed mode cleared on this computer\n",
                   stdout);
        return 0;
    }
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
