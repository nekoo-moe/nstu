#include "nstu/control_plane.hpp"

#include "nstu/control_channel.hpp"
#include "nstu/control_messages.hpp"
#include "nstu/enrollment.hpp"
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

    bool start(ServerControlPlaneConfig config, std::string* error) {
        if (dispatcher_.running()) {
            set_error(error, "server control plane is already running");
            return false;
        }
        config_ = std::move(config);
        if (keyring_file_exists(config_.keyring_path) &&
            !security::load_keyring(key_store_, config_.keyring_path,
                                    config_.keyring_entropy, error)) {
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
            return false;
        }
        return true;
    }

    void stop() noexcept {
        dispatcher_.stop();
        {
            std::scoped_lock lock(states_mutex_);
            states_.clear();
            client_connections_.clear();
        }
        enrollment_authority_.reset();
        security::secure_zero(config_.enrollment_secret);
        security::secure_zero(config_.keyring_entropy);
        config_ = {};
    }

    bool send_command(std::uint64_t client_id, protocol::CommandType type,
                      std::span<const std::byte> payload,
                      std::string* error) {
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
        const protocol::CommandEnvelope envelope{
            .version = protocol::kCommandVersion,
            .type = type,
            .payload_bytes = static_cast<std::uint32_t>(payload.size()),
            .request_id = next_request_id_.fetch_add(1),
        };
        const auto wire = control::encode_authenticated_command(
            state->session_key, envelope, state->send_sequence, payload);
        if (wire.empty() ||
            !dispatcher_.send(state->connection_id, wire, error)) {
            return false;
        }
        ++state->send_sequence;
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
            const protocol::CommandEnvelope envelope{
                .version = protocol::kCommandVersion,
                .type = type,
                .payload_bytes = static_cast<std::uint32_t>(payload.size()),
                .request_id = next_request_id_.fetch_add(1),
            };
            const auto wire = control::encode_authenticated_command(
                state->session_key, envelope, state->send_sequence, payload);
            if (wire.empty() ||
                !dispatcher_.send(state->connection_id, wire, nullptr)) {
                succeeded = false;
                continue;
            }
            ++state->send_sequence;
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
    net::IocpDispatcher dispatcher_;
    security::ReplayProtector replay_protector_;
    std::unique_ptr<security::EnrollmentAuthority> enrollment_authority_;
    std::mutex enrollment_mutex_;
    mutable std::mutex states_mutex_;
    std::unordered_map<net::ConnectionId,
                       std::shared_ptr<ConnectionState>> states_;
    std::unordered_map<std::uint64_t, net::ConnectionId> client_connections_;
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
