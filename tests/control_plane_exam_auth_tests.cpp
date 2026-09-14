#include "nstu/control_channel.hpp"
#include "nstu/control_plane.hpp"
#include "nstu/exam_control.hpp"
#include "nstu/exam_sync.hpp"
#include "nstu/multicast.hpp"

#include <cassert>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

nstu::security::ClientId client_id() {
    nstu::security::ClientId id{};
    for (std::size_t index = 0; index < id.size(); ++index) {
        id[index] = static_cast<std::byte>(0x20u + index);
    }
    return id;
}

std::vector<std::byte> client_key() {
    std::vector<std::byte> key(nstu::security::kMinimumProtocolKeyBytes);
    for (std::size_t index = 0; index < key.size(); ++index) {
        key[index] = static_cast<std::byte>(0x70u + index);
    }
    return key;
}

template <typename Predicate>
bool wait_until(Predicate predicate) {
    for (int attempt = 0; attempt < 300; ++attempt) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

std::filesystem::path journal_path() {
    const auto tick = std::chrono::high_resolution_clock::now()
                          .time_since_epoch()
                          .count();
    return std::filesystem::temp_directory_path() /
           (std::string("nstu-control-plane-auth-") + std::to_string(tick) +
            ".bin");
}

nstu::exam::ExamStartRequest make_start(
    const nstu::security::ClientId& id) {
    nstu::exam::ExamStartRequest request;
    request.package_root = "C:/ProgramData/NSTU/exams/auth-test";
    request.web_root =
        "C:/ProgramData/NSTU/exams/auth-test/exam/web";
    request.user_data_root = "C:/ProgramData/NSTU/exam-user-data";
    request.package_id = "auth-test-package";
    request.candidate_id = "candidate-auth-01";
    request.client_id = id;
    for (std::size_t index = 0; index < request.package_digest.size();
         ++index) {
        request.package_digest[index] =
            static_cast<std::byte>(0x40u + index);
    }
    for (std::size_t index = 0; index < request.session_id.size(); ++index) {
        request.session_id[index] = static_cast<std::byte>(0x90u + index);
    }
    return request;
}

nstu::exam::AnswerEvent make_event(
    const nstu::exam::ExamStartRequest& start, std::uint64_t sequence) {
    nstu::exam::AnswerEvent event;
    event.package_id = start.package_id;
    event.package_digest = start.package_digest;
    event.client_id = start.client_id;
    event.session_id = start.session_id;
    event.candidate_id = start.candidate_id;
    event.question_id = "reading-01";
    event.question_revision = 1;
    event.sequence = sequence;
    event.client_time_unix_milliseconds = 1'700'000'000'000ULL + sequence;
    event.kind = nstu::exam::AnswerKind::upsert;
    event.answer = {static_cast<std::byte>('B')};
    return event;
}

nstu::exam::StateRequest make_state_request(
    const nstu::exam::ExamStartRequest& start) {
    nstu::exam::StateRequest request;
    request.package_id = start.package_id;
    request.package_digest = start.package_digest;
    request.client_id = start.client_id;
    request.session_id = start.session_id;
    request.candidate_id = start.candidate_id;
    return request;
}

std::optional<nstu::control::AuthenticatedControlChannel> connect_channel(
    nstu::server::ServerControlPlane& control_plane,
    const nstu::security::ClientId& id, std::uint32_t key_id,
    std::span<const std::byte> key, std::string* error) {
    nstu::net::TcpSocket socket;
    if (!socket.connect("127.0.0.1", control_plane.local_port(), error) ||
        !socket.set_io_timeouts(5000, 5000, error)) {
        return std::nullopt;
    }
    const auto now = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    auto session = nstu::control::client_handshake(
        socket, id, key_id, key, now, std::chrono::seconds(120), error);
    if (!session) {
        return std::nullopt;
    }
    return nstu::control::AuthenticatedControlChannel(std::move(socket),
                                                       std::move(*session));
}

} // namespace

int main() {
    nstu::net::WinsockRuntime winsock;
    assert(winsock.ready());

    const auto id = client_id();
    auto key = client_key();
    constexpr std::uint32_t key_id = 9;
    nstu::security::KeyStore key_store;
    std::string error;
    assert(key_store.enroll(id, key_id, key, &error));

    nstu::server::ClientRegistry registry;
    nstu::server::ServerControlPlane control_plane(registry, key_store);
    nstu::server::ServerControlPlaneConfig config;
    config.port = 0;
    config.maximum_clients = 16;
    const auto path = journal_path();
    config.exam_journal_path = path;
    assert(control_plane.start(std::move(config), &error));

    auto channel = connect_channel(control_plane, id, key_id, key, &error);
    assert(channel.has_value());

    nstu::control::ClientStatusReport status;
    status.hostname = "AUTH-TEST-PC";
    const auto status_payload = nstu::control::encode_status_report(status);
    assert(channel->send(nstu::protocol::CommandType::hello, 1,
                         status_payload, &error));
    assert(wait_until([&] { return registry.snapshot().size() == 1; }));
    const auto registry_snapshot = registry.snapshot();
    assert(registry_snapshot.size() == 1);
    const auto registry_id = registry_snapshot.front().id;

    const auto start = make_start(id);
    assert(control_plane.start_exam(registry_id, start, &error));
    const auto start_command = channel->receive(&error);
    assert(start_command.has_value());
    assert(start_command->envelope.type ==
           nstu::protocol::CommandType::exam_start);

    // A second start cannot replace the server-issued context while the
    // current session is active.
    assert(!control_plane.start_exam(registry_id, start, &error));

    auto accepted_event = make_event(start, 1);
    const auto accepted_wire = nstu::exam::encode_answer_event(accepted_event);
    assert(!accepted_wire.empty());
    assert(channel->send(nstu::protocol::CommandType::exam_answer_event, 10,
                         accepted_wire, &error));
    const auto accepted_ack = channel->receive(&error);
    assert(accepted_ack.has_value());
    assert(accepted_ack->envelope.type ==
           nstu::protocol::CommandType::exam_answer_ack);
    const auto decoded_ack =
        nstu::exam::decode_answer_ack(accepted_ack->payload);
    assert(decoded_ack.has_value());
    assert(decoded_ack->status == nstu::exam::AnswerAckStatus::accepted);
    assert(decoded_ack->sequence == 1);

    const auto state_request = make_state_request(start);
    const auto state_wire = nstu::exam::encode_state_request(state_request);
    assert(!state_wire.empty());
    assert(channel->send(nstu::protocol::CommandType::exam_state_request, 11,
                         state_wire, &error));
    const auto state_response = channel->receive(&error);
    assert(state_response.has_value());
    assert(state_response->envelope.type ==
           nstu::protocol::CommandType::exam_state_response);
    const auto decoded_state =
        nstu::exam::decode_state_response(state_response->payload);
    assert(decoded_state.has_value());
    assert(decoded_state->session_id == start.session_id);
    assert(decoded_state->answers.size() == 1);

    const auto accepted_hash = nstu::exam::hash_answer_event(accepted_event);
    assert(accepted_hash.has_value());

    // A mismatched package ID is not allowed to reach the journal, even when
    // the embedded digest, client identity, session, and candidate fields are
    // otherwise valid. The server closes the authenticated channel on this
    // violation.
    auto mismatched_event = make_event(start, 2);
    mismatched_event.package_id = "another-package";
    const auto mismatched_wire =
        nstu::exam::encode_answer_event(mismatched_event);
    assert(!mismatched_wire.empty());
    assert(channel->send(nstu::protocol::CommandType::exam_answer_event, 12,
                         mismatched_wire, &error));
    error.clear();
    assert(!channel->receive(&error).has_value());
    channel->close();

    // The active context survives an ordinary reconnect. A valid event for
    // the original tuple is still accepted on the new authenticated channel.
    assert(wait_until([&] {
        const auto snapshot = registry.snapshot();
        return !snapshot.empty() &&
               snapshot.front().status == nstu::server::ClientStatus::offline;
    }));
    auto reconnected = connect_channel(control_plane, id, key_id, key, &error);
    assert(reconnected.has_value());
    assert(wait_until([&] { return registry.snapshot().size() == 1; }));

    auto recovered_event = make_event(start, 2);
    recovered_event.previous_event_hash = *accepted_hash;
    const auto recovered_wire = nstu::exam::encode_answer_event(recovered_event);
    assert(!recovered_wire.empty());
    assert(reconnected->send(nstu::protocol::CommandType::exam_answer_event,
                             20, recovered_wire, &error));
    const auto recovered_ack = reconnected->receive(&error);
    assert(recovered_ack.has_value());
    const auto decoded_recovered_ack =
        nstu::exam::decode_answer_ack(recovered_ack->payload);
    assert(decoded_recovered_ack.has_value());
    assert(decoded_recovered_ack->status ==
           nstu::exam::AnswerAckStatus::accepted);
    assert(decoded_recovered_ack->sequence == 2);

    assert(control_plane.stop_exam(registry_id, &error));
    const auto stop_command = reconnected->receive(&error);
    assert(stop_command.has_value());
    assert(stop_command->envelope.type ==
           nstu::protocol::CommandType::exam_stop);

    // Once explicitly stopped, even a correctly shaped event is rejected.
    auto stopped_event = make_event(start, 3);
    const auto stopped_wire = nstu::exam::encode_answer_event(stopped_event);
    assert(!stopped_wire.empty());
    assert(reconnected->send(nstu::protocol::CommandType::exam_answer_event,
                             21, stopped_wire, &error));
    error.clear();
    assert(!reconnected->receive(&error).has_value());
    reconnected->close();

    control_plane.stop();
    nstu::security::secure_zero(key);
    std::error_code cleanup_error;
    std::filesystem::remove(path, cleanup_error);
    auto lock_path = path;
    lock_path += L".lock";
    std::filesystem::remove(lock_path, cleanup_error);
    return 0;
}
