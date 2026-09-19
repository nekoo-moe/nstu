#include "nstu/control_plane.hpp"

#include "nstu/control_channel.hpp"
#include "nstu/control_messages.hpp"
#include "nstu/discovery.hpp"
#include "nstu/enrollment.hpp"
#include "nstu/exam_control.hpp"
#include "nstu/exam_sync.hpp"
#include "nstu/keyring.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>

namespace nstu::server {
namespace {

void set_error(std::string* error, const char* message) {
    if (error != nullptr) {
        *error = message;
    }
}

std::uint64_t unix_seconds_now() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<
        std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
                                          .count());
}

std::uint64_t unix_milliseconds_now() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<
        std::chrono::milliseconds>(std::chrono::system_clock::now()
                                       .time_since_epoch())
                                          .count());
}

std::uint32_t read_u32_le(std::span<const std::byte> bytes) {
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        value |= std::to_integer<std::uint32_t>(bytes[index]) << (index * 8u);
    }
    return value;
}

std::vector<std::byte> encode_u32(std::uint32_t value) {
    std::vector<std::byte> bytes(sizeof(value));
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        bytes[index] = static_cast<std::byte>(value & 0xffu);
        value >>= 8u;
    }
    return bytes;
}

std::vector<std::byte> plain_frame(protocol::CommandType type,
                                   std::uint64_t request_id,
                                   std::span<const std::byte> payload) {
    const protocol::CommandEnvelope envelope{
        .version = protocol::kCommandVersion,
        .type = type,
        .payload_bytes = static_cast<std::uint32_t>(payload.size()),
        .request_id = request_id,
    };
    return protocol::encode_tcp_frame(envelope, payload);
}

std::uint64_t registry_id_for(const security::ClientId& identity) noexcept {
    constexpr std::uint64_t offset_basis = 14695981039346656037ull;
    constexpr std::uint64_t prime = 1099511628211ull;
    std::uint64_t hash = offset_basis;
    for (const auto value : identity) {
        hash ^= std::to_integer<std::uint8_t>(value);
        hash *= prime;
    }
    return hash == 0 ? 1 : hash;
}

std::string fallback_hostname(std::uint64_t registry_id) {
    char text[32]{};
    sprintf_s(text, "Client-%08llx",
              static_cast<unsigned long long>(registry_id & 0xffffffffull));
    return text;
}

bool keyring_file_exists(std::wstring_view path) {
    if (path.empty()) {
        return false;
    }
    const std::wstring owned(path);
    const DWORD attributes = GetFileAttributesW(owned.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

} // namespace

class ServerControlPlane::Impl {
public:
    Impl(ClientRegistry& registry, security::KeyStore& key_store)
        : registry_(registry), key_store_(key_store) {}

    ~Impl() { stop(); }

    enum class Stage : std::uint8_t {
        initial,
        enrollment,
        enrollment_complete,
        probe_complete,
        auth_hello,
        auth_proof,
        authenticated,
    };

    struct ConnectionState {
        ~ConnectionState() {
            security::secure_zero(pre_shared_key);
            security::secure_zero(session_key);
        }

        net::ConnectionId connection_id = 0;
        std::string source;
        Stage stage = Stage::initial;
        std::vector<std::byte> initial_buffer;
        protocol::TcpFrameParser parser;
        protocol::ConnectionPreamble preamble;
        security::AuthHello hello;
        security::AuthChallenge challenge;
        std::vector<std::byte> pre_shared_key;
        security::Sha256Digest session_key{};
        security::ControlSequenceGuard receive_sequence;
        std::uint64_t send_sequence = 0;
        std::uint64_t handshake_request_id = 0;
        std::atomic<std::uint64_t> registry_id = 0;
        std::mutex mutex;
    };

    // The server is the authority for which exam session a client may
    // participate in.  This record intentionally outlives a TCP connection:
    // a client can reconnect after a transient network failure and continue
    // recovering its answer state.  It is cleared on an explicit stop or
    // server shutdown, never merely because a socket went away.
    struct ActiveExamContext {
        security::ClientId client_id{};
        security::Sha256Digest package_digest{};
        exam::SessionId session_id{};
        std::string package_id;
        std::string candidate_id;
    };

    bool start(ServerControlPlaneConfig config, std::string* error) {
        if (dispatcher_.running()) {
            set_error(error, "server control plane is already running");
            return false;
        }
        config_ = std::move(config);
        if (!config_.exam_journal_path.empty() &&
            !exam_journal_.open(config_.exam_journal_path, error)) {
            config_ = {};
            return false;
        }
        if (keyring_file_exists(config_.keyring_path) &&
            !security::load_keyring(key_store_, config_.keyring_path,
                                    config_.keyring_entropy, error)) {
            exam_journal_.close();
            config_ = {};
            return false;
        }
        if (config_.enrollment_secret.size() >=
            security::kMinimumProtocolKeyBytes) {
            enrollment_authority_ = std::make_unique<
                security::EnrollmentAuthority>(config_.enrollment_secret);
        }
        security::secure_zero(config_.enrollment_secret);
        config_.enrollment_secret.clear();

        net::IocpDispatcherConfig dispatcher_config;
        dispatcher_config.port = config_.port;
        dispatcher_config.maximum_connections = config_.maximum_clients;
        dispatcher_config.idle_timeout = std::chrono::seconds(15);
        dispatcher_config.accept_depth = std::clamp<std::size_t>(
            config_.maximum_clients / 8, 8, 64);
        net::IocpDispatcherCallbacks callbacks;
        callbacks.on_connected = [this](net::ConnectionId id,
                                        const std::string& source) {
            auto state = std::make_shared<ConnectionState>();
            state->connection_id = id;
            state->source = source;
            std::scoped_lock lock(states_mutex_);
            states_.emplace(id, std::move(state));
        };
        callbacks.on_bytes = [this](net::ConnectionId id,
                                    std::vector<std::byte> bytes) {
            on_bytes(id, std::move(bytes));
        };
        callbacks.on_disconnected = [this](net::ConnectionId id) {
            on_disconnected(id);
        };
        if (!dispatcher_.start(dispatcher_config, std::move(callbacks), error)) {
            enrollment_authority_.reset();
            exam_journal_.close();
            return false;
        }
        const auto control_port = dispatcher_.local_port();
        if (!discovery_responder_.start(control_port, control_port, key_store_,
                                        error)) {
            dispatcher_.stop();
            enrollment_authority_.reset();
            exam_journal_.close();
            security::secure_zero(config_.keyring_entropy);
            config_ = {};
            return false;
        }
        return true;
    }

    void stop() noexcept {
        discovery_responder_.stop();
        dispatcher_.stop();
        {
            std::scoped_lock lock(states_mutex_);
            states_.clear();
            client_connections_.clear();
        }
        {
            std::scoped_lock lock(exam_contexts_mutex_);
            active_exam_contexts_.clear();
        }
        enrollment_authority_.reset();
        exam_journal_.close();
        security::secure_zero(config_.enrollment_secret);
        security::secure_zero(config_.keyring_entropy);
        config_ = {};
    }

    bool send_command(std::uint64_t client_id, protocol::CommandType type,
                      std::span<const std::byte> payload,
                      std::string* error) {
        // Exam start/stop have side effects on the server authorization map;
        // route callers through the stateful entry points so a raw command
        // cannot bypass that bookkeeping.
        if (type == protocol::CommandType::exam_start) {
            const auto request = exam::decode_exam_start_request(payload);
            if (!request) {
                set_error(error, "invalid exam start request");
                return false;
            }
            return start_exam(client_id, *request, error);
        }
        if (type == protocol::CommandType::exam_stop) {
            if (!payload.empty()) {
                set_error(error, "exam stop command must not have a payload");
                return false;
            }
            return stop_exam(client_id, error);
        }
        auto state = state_for_client(client_id);
        if (!state) {
            set_error(error, "client is not authenticated");
            return false;
        }
        std::scoped_lock lock(state->mutex);
        if (state->stage != Stage::authenticated ||
            state->send_sequence == std::numeric_limits<std::uint64_t>::max()) {
            set_error(error, "authenticated command sequence is unavailable");
            return false;
        }
        return send_authenticated_locked(
            *state, type, next_request_id_.fetch_add(1), payload, error);
    }

    bool start_exam(std::uint64_t client_id,
                    const exam::ExamStartRequest& request,
                    std::string* error) {
        if (!exam::validate_exam_start_request(request)) {
            set_error(error, "invalid exam start request");
            return false;
        }
        const auto state = state_for_client(client_id);
        if (!state) {
            set_error(error, "client is not authenticated");
            return false;
        }
        const auto payload = exam::encode_exam_start_request(request);
        if (payload.empty()) {
            set_error(error, "exam start request could not be encoded");
            return false;
        }

        // Keep the connection lock held while the authorization record is
        // installed and the command is queued.  Inbound answer/state traffic
        // takes the same lock, so it cannot observe a half-started session.
        std::scoped_lock state_lock(state->mutex);
        if (state->stage != Stage::authenticated ||
            state->hello.client_id != request.client_id ||
            state->registry_id.load() == 0) {
            set_error(error, "exam request identity does not match client");
            return false;
        }
        const auto registry_id = state->registry_id.load();
        std::scoped_lock context_lock(exam_contexts_mutex_);
        if (active_exam_contexts_.find(registry_id) !=
            active_exam_contexts_.end()) {
            set_error(error, "client already has an active exam session");
            return false;
        }
        ActiveExamContext context;
        context.client_id = request.client_id;
        context.package_digest = request.package_digest;
        context.session_id = request.session_id;
        context.package_id = request.package_id;
        context.candidate_id = request.candidate_id;
        const auto [inserted_context, inserted] =
            active_exam_contexts_.try_emplace(registry_id, std::move(context));
        if (!inserted) {
            set_error(error, "client already has an active exam session");
            return false;
        }
        if (!send_authenticated_locked(
                *state, protocol::CommandType::exam_start,
                next_request_id_.fetch_add(1), payload, error)) {
            active_exam_contexts_.erase(inserted_context);
            return false;
        }
        return true;
    }

    bool stop_exam(std::uint64_t client_id, std::string* error) {
        auto state = state_for_client(client_id);
        if (!state) {
            set_error(error, "client is not authenticated");
            return false;
        }
        std::scoped_lock state_lock(state->mutex);
        if (state->stage != Stage::authenticated ||
            state->registry_id.load() == 0) {
            set_error(error, "authenticated command sequence is unavailable");
            return false;
        }
        if (!send_authenticated_locked(
                *state, protocol::CommandType::exam_stop,
                next_request_id_.fetch_add(1), {}, error)) {
            return false;
        }
        // Remove only after the stop command was queued successfully.  If a
        // connection is unavailable, retaining the context allows a later
        // reconnect to recover answers and receive an explicit stop.
        std::scoped_lock context_lock(exam_contexts_mutex_);
        active_exam_contexts_.erase(state->registry_id.load());
        return true;
    }

    bool broadcast_command(protocol::CommandType type,
                           std::span<const std::byte> payload,
                           std::string* error) {
        std::vector<std::shared_ptr<ConnectionState>> states;
        {
            std::scoped_lock lock(states_mutex_);
            states.reserve(client_connections_.size());
            for (const auto& [client_id, connection_id] :
                 client_connections_) {
                (void)client_id;
                const auto found = states_.find(connection_id);
                if (found != states_.end()) {
                    states.push_back(found->second);
                }
            }
        }
        if (states.empty()) {
            set_error(error, "no authenticated clients are connected");
            return false;
        }
        bool succeeded = true;
        for (const auto& state : states) {
            std::scoped_lock lock(state->mutex);
            if (state->stage != Stage::authenticated ||
                state->send_sequence ==
                    std::numeric_limits<std::uint64_t>::max()) {
                succeeded = false;
                continue;
            }
            if (!send_authenticated_locked(
                    *state, type, next_request_id_.fetch_add(1), payload,
                    nullptr)) {
                succeeded = false;
                continue;
            }
        }
        if (!succeeded) {
            set_error(error, "one or more clients rejected the broadcast");
        }
        return succeeded;
    }

    std::size_t authenticated_client_count() const noexcept {
        std::scoped_lock lock(states_mutex_);
        return client_connections_.size();
    }

private:
    bool send_authenticated_locked(ConnectionState& state,
                                    protocol::CommandType type,
                                    std::uint64_t request_id,
                                    std::span<const std::byte> payload,
                                    std::string* error) {
        if (state.stage != Stage::authenticated ||
            state.send_sequence == std::numeric_limits<std::uint64_t>::max() ||
            payload.size() > protocol::kMaxCommandPayload) {
            set_error(error, "authenticated command sequence is unavailable");
            return false;
        }
        const protocol::CommandEnvelope envelope{
            .version = protocol::kCommandVersion,
            .type = type,
            .payload_bytes = static_cast<std::uint32_t>(payload.size()),
            .request_id = request_id,
        };
        const auto wire = control::encode_authenticated_command(
            state.session_key, envelope, state.send_sequence, payload);
        if (wire.empty() ||
            !dispatcher_.send(state.connection_id, wire, error)) {
            return false;
        }
        ++state.send_sequence;
        return true;
    }

    std::shared_ptr<ConnectionState> state_for_connection(
        net::ConnectionId id) const {
        std::scoped_lock lock(states_mutex_);
        const auto found = states_.find(id);
        return found == states_.end() ? nullptr : found->second;
    }

    std::shared_ptr<ConnectionState> state_for_client(
        std::uint64_t client_id) const {
        std::scoped_lock lock(states_mutex_);
        const auto mapping = client_connections_.find(client_id);
        if (mapping == client_connections_.end()) {
            return nullptr;
        }
        const auto found = states_.find(mapping->second);
        return found == states_.end() ? nullptr : found->second;
    }

    std::optional<ActiveExamContext> active_exam_for(
        std::uint64_t registry_id) const {
        if (registry_id == 0) {
            return std::nullopt;
        }
        std::scoped_lock lock(exam_contexts_mutex_);
        const auto found = active_exam_contexts_.find(registry_id);
        return found == active_exam_contexts_.end()
                   ? std::nullopt
                   : std::optional<ActiveExamContext>(found->second);
    }

    static bool matches_exam_context(const ActiveExamContext& context,
                                     const exam::AnswerEvent& event) noexcept {
        return context.client_id == event.client_id &&
               context.package_digest == event.package_digest &&
               context.session_id == event.session_id &&
               context.package_id == event.package_id &&
               context.candidate_id == event.candidate_id;
    }

    static bool matches_exam_context(const ActiveExamContext& context,
                                     const exam::StateRequest& request) noexcept {
        return context.client_id == request.client_id &&
               context.package_digest == request.package_digest &&
               context.session_id == request.session_id &&
               context.package_id == request.package_id &&
               context.candidate_id == request.candidate_id;
    }

    void on_bytes(net::ConnectionId id, std::vector<std::byte> bytes) {
        auto state = state_for_connection(id);
        if (!state) {
            return;
        }
        std::scoped_lock lock(state->mutex);
        if (state->stage == Stage::initial) {
            state->initial_buffer.insert(state->initial_buffer.end(),
                                         bytes.begin(), bytes.end());
            if (state->initial_buffer.size() < sizeof(std::uint32_t)) {
                return;
            }
            if (read_u32_le(state->initial_buffer) == protocol::kMagic) {
                if (state->initial_buffer.size() <
                    protocol::kConnectionPreambleBytes) {
                    return;
                }
                const auto preamble = protocol::decode_connection_preamble(
                    std::span<const std::byte>(state->initial_buffer)
                        .first(protocol::kConnectionPreambleBytes));
                if (!preamble) {
                    fail_handshake(*state, "invalid client preamble");
                    return;
                }
                if (preamble->role == protocol::ConnectionRole::diagnostic) {
                    protocol::ConnectionPreamble response;
                    response.role = protocol::ConnectionRole::server;
                    const auto probe_response =
                        protocol::encode_connection_preamble(response);
                    if (probe_response.empty() ||
                        !dispatcher_.send(state->connection_id, probe_response,
                                          nullptr)) {
                        fail_handshake(*state,
                                       "diagnostic response could not be sent");
                        return;
                    }
                    state->initial_buffer.clear();
                    state->stage = Stage::probe_complete;
                    return;
                }
                if (preamble->role != protocol::ConnectionRole::client) {
                    fail_handshake(*state, "invalid client preamble");
                    return;
                }
                state->preamble = *preamble;
                state->stage = Stage::auth_hello;
                bytes.assign(
                    state->initial_buffer.begin() +
                        protocol::kConnectionPreambleBytes,
                    state->initial_buffer.end());
            } else {
                state->stage = Stage::enrollment;
                bytes.swap(state->initial_buffer);
            }
            state->initial_buffer.clear();
        }
        if (bytes.empty()) {
            return;
        }
        std::vector<protocol::TcpFrame> frames;
        if (!state->parser.feed(bytes, frames)) {
            fail_handshake(*state, "invalid control framing");
            return;
        }
        for (const auto& frame : frames) {
            if (!process_frame(*state, frame)) {
                return;
            }
        }
    }

    bool process_frame(ConnectionState& state,
                       const protocol::TcpFrame& frame) {
        if (state.stage == Stage::enrollment) {
            return process_enrollment(state, frame);
        }
        if (state.stage == Stage::enrollment_complete ||
            state.stage == Stage::probe_complete) {
            dispatcher_.disconnect(state.connection_id);
            return false;
        }
        if (state.stage == Stage::auth_hello) {
            return process_auth_hello(state, frame);
        }
        if (state.stage == Stage::auth_proof) {
            return process_auth_proof(state, frame);
        }
        if (state.stage == Stage::authenticated) {
            return process_authenticated(state, frame);
        }
        fail_handshake(state, "unexpected control-plane state");
        return false;
    }

    bool process_enrollment(ConnectionState& state,
                            const protocol::TcpFrame& frame) {
        std::scoped_lock enrollment_lock(enrollment_mutex_);
        if (!enrollment_authority_ ||
            frame.envelope.type != protocol::CommandType::enrollment_request) {
            fail_handshake(state, "enrollment is disabled or malformed");
            return false;
        }
        const auto request =
            security::decode_enrollment_request(frame.payload);
        if (!request) {
            fail_handshake(state, "invalid enrollment payload");
            return false;
        }
        auto previous = key_store_.snapshot();
        std::string enrollment_error;
        if (!enrollment_authority_->accept(*request, unix_seconds_now(),
                                           key_store_, &enrollment_error)) {
            fail_handshake(state, enrollment_error);
            return false;
        }
        if (!config_.keyring_path.empty() &&
            !security::save_keyring(key_store_, config_.keyring_path,
                                    config_.keyring_entropy,
                                    &enrollment_error)) {
            (void)key_store_.replace(previous, nullptr);
            fail_handshake(state, "enrolled key could not be persisted");
            return false;
        }
        for (auto& record : previous) {
            security::secure_zero(record.key);
        }
        const auto response = encode_u32(request->key_id);
        const auto wire = plain_frame(protocol::CommandType::enrollment_accept,
                                      frame.envelope.request_id, response);
        if (!dispatcher_.send(state.connection_id, wire, nullptr)) {
            return false;
        }
        state.stage = Stage::enrollment_complete;
        dispatcher_.record_handshake_success(state.connection_id);
        return true;
    }

    bool process_auth_hello(ConnectionState& state,
                            const protocol::TcpFrame& frame) {
        if (frame.envelope.type != protocol::CommandType::auth_hello) {
            fail_handshake(state, "expected authentication hello");
            return false;
        }
        const auto hello = security::decode_auth_hello(frame.payload);
        if (!hello || !security::validate_auth_hello(*hello) ||
            hello->client_id != state.preamble.identity ||
            hello->key_id != state.preamble.key_id) {
            fail_handshake(state, "authentication identity mismatch");
            return false;
        }
        auto key = key_store_.resolve(hello->client_id, hello->key_id);
        if (!key) {
            fail_handshake(state, "unknown or revoked client key");
            return false;
        }
        state.hello = *hello;
        state.pre_shared_key = std::move(*key);
        state.challenge.unix_time_seconds = unix_seconds_now();
        if (!security::generate_random(state.challenge.server_nonce)) {
            fail_handshake(state, "challenge generation failed");
            return false;
        }
        protocol::ConnectionPreamble server_preamble;
        server_preamble.role = protocol::ConnectionRole::server;
        auto wire = protocol::encode_connection_preamble(server_preamble);
        const auto challenge = plain_frame(
            protocol::CommandType::auth_challenge, frame.envelope.request_id,
            security::encode_auth_challenge(state.challenge));
        wire.insert(wire.end(), challenge.begin(), challenge.end());
        if (!dispatcher_.send(state.connection_id, wire, nullptr)) {
            return false;
        }
        state.handshake_request_id = frame.envelope.request_id;
        state.stage = Stage::auth_proof;
        return true;
    }

    bool process_auth_proof(ConnectionState& state,
                            const protocol::TcpFrame& frame) {
        if (frame.envelope.type != protocol::CommandType::auth_proof ||
            frame.envelope.request_id != state.handshake_request_id ||
            frame.payload.size() != security::kSha256Bytes) {
            fail_handshake(state, "unexpected authentication proof");
            return false;
        }
        auto expected = security::compute_client_proof(
            state.pre_shared_key, state.hello, state.challenge);
        if (!expected || !security::constant_time_equal(*expected,
                                                        frame.payload) ||
            !replay_protector_.accept(state.hello, unix_seconds_now())) {
            if (expected) {
                security::secure_zero(*expected);
            }
            fail_handshake(state, "authentication proof or replay rejected");
            return false;
        }
        security::secure_zero(*expected);
        auto session_key = security::derive_session_key(
            state.pre_shared_key, state.hello, state.challenge);
        security::secure_zero(state.pre_shared_key);
        state.pre_shared_key.clear();
        if (!session_key) {
            fail_handshake(state, "session key derivation failed");
            return false;
        }
        auto server_proof = security::compute_server_proof(
            *session_key, state.hello, state.challenge);
        if (!server_proof) {
            security::secure_zero(*session_key);
            fail_handshake(state, "server proof generation failed");
            return false;
        }
        const auto wire = plain_frame(protocol::CommandType::auth_accept,
                                      state.handshake_request_id,
                                      *server_proof);
        security::secure_zero(*server_proof);
        if (!dispatcher_.send(state.connection_id, wire, nullptr)) {
            security::secure_zero(*session_key);
            return false;
        }
        state.session_key = *session_key;
        security::secure_zero(*session_key);
        state.receive_sequence.reset(0);
        state.send_sequence = 0;
        const auto registry_id = registry_id_for(state.hello.client_id);
        state.registry_id = registry_id;
        state.stage = Stage::authenticated;

        net::ConnectionId previous_connection = 0;
        {
            std::scoped_lock lock(states_mutex_);
            const auto previous = client_connections_.find(registry_id);
            if (previous != client_connections_.end()) {
                previous_connection = previous->second;
            }
            client_connections_[registry_id] = state.connection_id;
        }
        ClientRecord record;
        record.id = registry_id;
        record.hostname = fallback_hostname(registry_id);
        record.address = state.source;
        record.status = ClientStatus::online;
        record.last_seen = std::chrono::steady_clock::now();
        registry_.upsert(std::move(record));
        dispatcher_.record_handshake_success(state.connection_id);
        if (previous_connection != 0 &&
            previous_connection != state.connection_id) {
            dispatcher_.disconnect(previous_connection);
        }
        return true;
    }

    bool process_authenticated(ConnectionState& state,
                               const protocol::TcpFrame& frame) {
        std::string decode_error;
        const auto command = control::decode_authenticated_command(
            state.session_key, frame, &decode_error);
        if (!command || !state.receive_sequence.accept(command->sequence)) {
            dispatcher_.disconnect(state.connection_id);
            return false;
        }
        if (command->envelope.type == protocol::CommandType::exam_answer_event) {
            return process_exam_answer_event(state, *command);
        }
        if (command->envelope.type == protocol::CommandType::exam_state_request) {
            return process_exam_state_request(state, *command);
        }
        if (command->envelope.type == protocol::CommandType::heartbeat) {
            (void)registry_.touch(state.registry_id.load());
            return true;
        }
        if (command->envelope.type == protocol::CommandType::status_report ||
            command->envelope.type == protocol::CommandType::hello) {
            const auto report = control::decode_status_report(command->payload);
            if (!report) {
                dispatcher_.disconnect(state.connection_id);
                return false;
            }
            ClientRecord record;
            record.id = state.registry_id.load();
            record.hostname = report->hostname;
            record.address = state.source;
            record.status = report->locked ? ClientStatus::locked
                                           : ClientStatus::online;
            record.delivery = report->delivery;
            record.latency_ms = report->latency_ms;
            record.packet_loss_per_mille = report->packet_loss_per_mille;
            record.packet_loss_sample_size = report->packet_loss_sample_size;
            record.streaming = report->streaming;
            record.snapshotting = report->snapshotting;
            record.viewing_broadcast = report->viewing_broadcast;
            record.frames_per_second = report->frames_per_second;
            record.snapshot_interval_seconds =
                report->snapshot_interval_seconds;
            record.last_seen = std::chrono::steady_clock::now();
            registry_.upsert(std::move(record));
            return true;
        }
        if (command->envelope.type ==
            protocol::CommandType::snapshot_frame) {
            const auto snapshot =
                control::decode_snapshot_frame(command->payload);
            if (!snapshot || !registry_.update_snapshot(
                                 state.registry_id.load(), *snapshot)) {
                dispatcher_.disconnect(state.connection_id);
                return false;
            }
            return true;
        }
        return true;
    }

    bool process_exam_answer_event(
        ConnectionState& state, const control::AuthenticatedCommand& command) {
        std::string decode_error;
        const auto event = exam::decode_answer_event(command.payload,
                                                     &decode_error);
        // The client id is authenticated by the handshake, but the complete
        // exam tuple is server-issued.  Never accept an event merely because
        // its embedded client id matches the socket identity.
        const auto context = active_exam_for(state.registry_id.load());
        if (!event || !context || event->client_id != state.hello.client_id ||
            !matches_exam_context(*context, *event)) {
            dispatcher_.disconnect(state.connection_id);
            return false;
        }

        exam::AppendOutcome outcome;
        if (exam_journal_.is_open()) {
            outcome = exam_journal_.append(*event, &decode_error);
        } else {
            outcome.status = exam::AppendStatus::unavailable;
            outcome.ack.status = exam::AnswerAckStatus::unavailable;
            outcome.ack.session_id = event->session_id;
            outcome.ack.sequence = event->sequence;
            outcome.ack.server_time_unix_milliseconds =
                unix_milliseconds_now();
        }
        const auto ack = exam::encode_answer_ack(outcome.ack);
        if (ack.empty() || !send_authenticated_locked(
                                state, protocol::CommandType::exam_answer_ack,
                                command.envelope.request_id, ack, nullptr)) {
            dispatcher_.disconnect(state.connection_id);
            return false;
        }
        return true;
    }

    bool process_exam_state_request(
        ConnectionState& state, const control::AuthenticatedCommand& command) {
        const auto request = exam::decode_state_request(command.payload);
        const auto context = active_exam_for(state.registry_id.load());
        if (!request || !context || request->client_id != state.hello.client_id ||
            !matches_exam_context(*context, *request) ||
            !exam_journal_.is_open()) {
            dispatcher_.disconnect(state.connection_id);
            return false;
        }
        std::string state_error;
        const auto response = exam_journal_.state(*request, &state_error);
        if (!response) {
            dispatcher_.disconnect(state.connection_id);
            return false;
        }
        const auto payloads = exam::encode_state_response_chunks(*response);
        if (payloads.empty()) {
            dispatcher_.disconnect(state.connection_id);
            return false;
        }
        for (const auto& payload : payloads) {
            if (!send_authenticated_locked(
                    state, protocol::CommandType::exam_state_response,
                    command.envelope.request_id, payload, nullptr)) {
                dispatcher_.disconnect(state.connection_id);
                return false;
            }
        }
        return true;
    }

    void fail_handshake(ConnectionState& state, std::string detail) {
        dispatcher_.record_handshake_failure(state.connection_id,
                                             std::move(detail));
    }

    void on_disconnected(net::ConnectionId id) {
        std::shared_ptr<ConnectionState> state;
        {
            std::scoped_lock lock(states_mutex_);
            const auto found = states_.find(id);
            if (found == states_.end()) {
                return;
            }
            state = found->second;
            states_.erase(found);
            const auto registry_id = state->registry_id.load();
            if (registry_id != 0) {
                const auto mapping = client_connections_.find(
                    registry_id);
                if (mapping != client_connections_.end() &&
                    mapping->second == id) {
                    client_connections_.erase(mapping);
                }
            }
        }
        const auto registry_id = state->registry_id.load();
        if (registry_id != 0) {
            (void)registry_.set_status(registry_id,
                                       ClientStatus::offline);
        }
    }

public:
    ClientRegistry& registry_;
    security::KeyStore& key_store_;
    ServerControlPlaneConfig config_;
    // This journal is server-owned and intentionally never placed under a
    // client freeze/UWF overlay. It remains authoritative across reconnects.
    exam::AnswerJournal exam_journal_;
    discovery::AuthenticatedDiscoveryResponder discovery_responder_;
    net::IocpDispatcher dispatcher_;
    security::ReplayProtector replay_protector_;
    std::unique_ptr<security::EnrollmentAuthority> enrollment_authority_;
    std::mutex enrollment_mutex_;
    mutable std::mutex states_mutex_;
    mutable std::mutex exam_contexts_mutex_;
    std::unordered_map<net::ConnectionId,
                       std::shared_ptr<ConnectionState>> states_;
    std::unordered_map<std::uint64_t, net::ConnectionId> client_connections_;
    std::unordered_map<std::uint64_t, ActiveExamContext>
        active_exam_contexts_;
    std::atomic<std::uint64_t> next_request_id_ = 1;
};

ServerControlPlane::ServerControlPlane(ClientRegistry& registry,
                                       security::KeyStore& key_store)
    : impl_(std::make_unique<Impl>(registry, key_store)) {}

ServerControlPlane::~ServerControlPlane() = default;

bool ServerControlPlane::start(ServerControlPlaneConfig config,
                               std::string* error) {
    return impl_->start(std::move(config), error);
}

void ServerControlPlane::stop() noexcept { impl_->stop(); }

bool ServerControlPlane::send_command(std::uint64_t client_id,
                                      protocol::CommandType type,
                                      std::span<const std::byte> payload,
                                      std::string* error) {
    return impl_->send_command(client_id, type, payload, error);
}

bool ServerControlPlane::set_locked(std::uint64_t client_id, bool locked,
                                    std::string* error) {
    return send_command(client_id, locked ? protocol::CommandType::lock
                                          : protocol::CommandType::unlock,
                        {}, error);
}

bool ServerControlPlane::set_streaming(std::uint64_t client_id, bool enabled,
                                       std::uint8_t frames_per_second,
                                       std::string* error) {
    if (!enabled) {
        return send_command(client_id, protocol::CommandType::stop_stream, {},
                            error);
    }
    const auto payload =
        control::encode_start_stream_request(frames_per_second);
    if (payload.empty()) {
        set_error(error, "stream frame rate must be between 5 and 15 fps");
        return false;
    }
    return send_command(client_id, protocol::CommandType::start_stream,
                        payload, error);
}

bool ServerControlPlane::set_snapshots(std::uint64_t client_id, bool enabled,
                                       std::uint16_t interval_seconds,
                                       std::string* error) {
    if (!enabled) {
        return send_command(client_id,
                            protocol::CommandType::stop_snapshots, {}, error);
    }
    const auto payload = control::encode_snapshot_schedule(interval_seconds);
    if (payload.empty()) {
        set_error(error, "snapshot interval must be between 5 and 10 seconds");
        return false;
    }
    return send_command(client_id, protocol::CommandType::start_snapshots,
                        payload, error);
}

bool ServerControlPlane::send_overlay_stroke(
    std::uint64_t client_id, const control::OverlayStroke& stroke,
    std::string* error) {
    const auto payload = control::encode_overlay_stroke(stroke);
    if (payload.empty()) {
        set_error(error, "invalid overlay stroke");
        return false;
    }
    return send_command(client_id, protocol::CommandType::overlay_stroke,
                        payload, error);
}

bool ServerControlPlane::clear_overlay(std::uint64_t client_id,
                                       std::string* error) {
    return send_command(client_id, protocol::CommandType::overlay_clear, {},
                        error);
}

bool ServerControlPlane::broadcast_host_snapshot(
    const control::SnapshotFrame& frame, std::string* error) {
    const auto payload = control::encode_snapshot_frame(frame);
    if (payload.empty()) {
        set_error(error, "invalid host snapshot");
        return false;
    }
    return impl_->broadcast_command(protocol::CommandType::host_snapshot,
                                    payload, error);
}

bool ServerControlPlane::stop_host_broadcast(std::string* error) {
    return impl_->broadcast_command(
        protocol::CommandType::host_broadcast_stop, {}, error);
}

bool ServerControlPlane::request_keyframe(std::uint64_t client_id,
                                          std::string* error) {
    return send_command(client_id, protocol::CommandType::keyframe_request, {},
                        error);
}

bool ServerControlPlane::send_chat(std::uint64_t client_id,
                                   std::string_view utf8_message,
                                   std::string* error) {
    if (utf8_message.empty() || utf8_message.size() > 4096) {
        set_error(error, "chat message is empty or too long");
        return false;
    }
    const auto payload = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(utf8_message.data()),
        utf8_message.size());
    return send_command(client_id, protocol::CommandType::chat, payload, error);
}

bool ServerControlPlane::start_remote_control(std::uint64_t client_id,
                                              std::string* error) {
    return send_command(client_id, protocol::CommandType::remote_start, {},
                        error);
}

bool ServerControlPlane::send_remote_input(
    std::uint64_t client_id, const wire::RemoteInputPacket& packet,
    std::string* error) {
    if ((packet.input_type !=
             static_cast<std::uint8_t>(wire::RemoteInputType::mouse) &&
         packet.input_type !=
             static_cast<std::uint8_t>(wire::RemoteInputType::keyboard)) ||
        packet.reserved != 0) {
        set_error(error, "invalid remote input packet");
        return false;
    }
    std::vector<std::byte> payload(sizeof(packet));
    std::memcpy(payload.data(), &packet, sizeof(packet));
    return send_command(client_id, protocol::CommandType::remote_input,
                        payload, error);
}

bool ServerControlPlane::stop_remote_control(std::uint64_t client_id,
                                              std::string* error) {
    return send_command(client_id, protocol::CommandType::remote_end, {},
                        error);
}

bool ServerControlPlane::start_exam(
    std::uint64_t client_id, const exam::ExamStartRequest& request,
    std::string* error) {
    return impl_->start_exam(client_id, request, error);
}

bool ServerControlPlane::stop_exam(std::uint64_t client_id,
                                   std::string* error) {
    return impl_->stop_exam(client_id, error);
}

bool ServerControlPlane::running() const noexcept {
    return impl_->dispatcher_.running();
}

std::uint16_t ServerControlPlane::local_port() const noexcept {
    return impl_->dispatcher_.local_port();
}

std::size_t ServerControlPlane::authenticated_client_count() const noexcept {
    return impl_->authenticated_client_count();
}

} // namespace nstu::server
