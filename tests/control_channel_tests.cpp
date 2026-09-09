#include "nstu/control_channel.hpp"
#include "nstu/control_messages.hpp"
#include "nstu/multicast.hpp"
#include "nstu/secret_store.hpp"

#include <windows.h>

#include <array>
#include <atomic>
#include <cassert>
#include <filesystem>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace {

nstu::security::ClientId client_id() {
    nstu::security::ClientId id{};
    for (std::size_t index = 0; index < id.size(); ++index) {
        id[index] = static_cast<std::byte>(index + 1);
    }
    return id;
}

std::vector<std::byte> pre_shared_key() {
    std::vector<std::byte> key(nstu::security::kMinimumProtocolKeyBytes);
    for (std::size_t index = 0; index < key.size(); ++index) {
        key[index] = static_cast<std::byte>(0x40 + index);
    }
    return key;
}

} // namespace

int main() {
    nstu::net::WinsockRuntime winsock;
    assert(winsock.ready());
    const auto id = client_id();
    auto key = pre_shared_key();
    constexpr std::uint32_t key_id = 42;
    constexpr std::uint64_t now = 20'000;

    nstu::net::TcpSocket listener;
    std::string error;
    assert(listener.listen(0, 8, &error));
    const auto port = listener.local_port(&error);
    assert(port != 0);
    std::atomic_bool server_succeeded = false;
    std::thread server([&] {
        std::string server_error;
        auto socket = listener.accept(&server_error);
        assert(socket.is_open());
        assert(socket.set_io_timeouts(5000, 5000, &server_error));
        nstu::security::ReplayProtector replay;
        const nstu::control::KeyResolver resolver =
            [&](const nstu::security::ClientId& requested_id,
                std::uint32_t requested_key_id)
            -> std::optional<std::vector<std::byte>> {
            if (requested_id != id || requested_key_id != key_id) {
                return std::nullopt;
            }
            return key;
        };
        auto session = nstu::control::server_handshake(
            socket, resolver, replay, now, &server_error);
        assert(session.has_value());
        nstu::control::AuthenticatedControlChannel channel(
            std::move(socket), std::move(*session));
        const auto command = channel.receive(&server_error);
        assert(command.has_value());
        assert(command->envelope.type == nstu::protocol::CommandType::lock);
        assert(command->envelope.request_id == 99);
        assert(command->payload ==
               std::vector<std::byte>({std::byte{0x10}, std::byte{0x20}}));
        const std::array<std::byte, 1> response{std::byte{0x01}};
        assert(channel.send(nstu::protocol::CommandType::hello_ack, 99,
                            response, &server_error));
        server_succeeded = true;
    });

    nstu::net::TcpSocket client_socket;
    assert(client_socket.connect("127.0.0.1", port, &error));
    assert(client_socket.set_io_timeouts(5000, 5000, &error));
    auto client_session = nstu::control::client_handshake(
        client_socket, id, key_id, key, now, std::chrono::seconds(120), &error);
    assert(client_session.has_value());
    nstu::control::AuthenticatedControlChannel client_channel(
        std::move(client_socket), std::move(*client_session));
    const std::array<std::byte, 2> command_payload{
        std::byte{0x10}, std::byte{0x20}};
    assert(client_channel.send(nstu::protocol::CommandType::lock, 99,
                               command_payload, &error));
    const auto response = client_channel.receive(&error);
    assert(response.has_value());
    assert(response->envelope.type == nstu::protocol::CommandType::hello_ack);
    assert(response->sequence == 0);
    server.join();
    assert(server_succeeded);

    nstu::protocol::CommandEnvelope envelope{
        .version = nstu::protocol::kCommandVersion,
        .type = nstu::protocol::CommandType::chat,
        .payload_bytes = 2,
        .request_id = 123,
    };
    auto wire = nstu::control::encode_authenticated_command(
        key, envelope, 7, command_payload);
    assert(!wire.empty());
    nstu::protocol::TcpFrameParser parser;
    std::vector<nstu::protocol::TcpFrame> frames;
    assert(parser.feed(wire, frames));
    assert(frames.size() == 1);
    const auto decoded =
        nstu::control::decode_authenticated_command(key, frames[0], &error);
    assert(decoded.has_value());
    assert(decoded->sequence == 7);
    frames[0].payload.back() ^= std::byte{1};
    assert(!nstu::control::decode_authenticated_command(key, frames[0], &error)
                .has_value());
    envelope.type = nstu::protocol::CommandType::auth_hello;
    assert(nstu::control::encode_authenticated_command(
               key, envelope, 8, command_payload)
               .empty());

    const std::array<std::byte, 8> entropy{
        std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4},
        std::byte{5}, std::byte{6}, std::byte{7}, std::byte{8}};
    const auto protected_blob =
        nstu::security::protect_machine_secret(key, entropy, &error);
    assert(!protected_blob.empty());
    auto unprotected =
        nstu::security::unprotect_machine_secret(protected_blob, entropy, &error);
    assert(unprotected == key);
    nstu::security::secure_zero(unprotected);

    wchar_t temporary_directory[MAX_PATH]{};
    assert(GetTempPathW(MAX_PATH, temporary_directory) != 0);
    const auto secret_path = std::filesystem::path(temporary_directory) /
        (L"nstu-secret-test-" + std::to_wstring(GetCurrentProcessId()) +
         L".bin");
    assert(nstu::security::save_machine_secret(secret_path.wstring(), key,
                                                entropy, &error));
    auto loaded = nstu::security::load_machine_secret(
        secret_path.wstring(), entropy, &error);
    assert(loaded == key);
    nstu::security::secure_zero(loaded);
    assert(DeleteFileW(secret_path.c_str()));
    nstu::security::secure_zero(key);

    nstu::net::IocpPort iocp;
    assert(iocp.create(1, &error));
    const nstu::net::IocpPort::Completion posted{
        .bytes_transferred = 17,
        .completion_key = 0x1234,
        .overlapped = 0,
        .error_code = 0,
    };
    assert(iocp.post(posted, &error));
    nstu::net::IocpPort::Completion received_completion{};
    assert(iocp.wait(received_completion, 1000, &error));
    assert(received_completion.bytes_transferred == posted.bytes_transferred);
    assert(received_completion.completion_key == posted.completion_key);
    assert(received_completion.overlapped == 0);
    assert(received_completion.error_code == 0);

    nstu::control::ClientStatusReport status;
    status.hostname = "SNAPSHOT-PC";
    status.snapshotting = true;
    status.snapshot_interval_seconds = 7;
    status.viewing_broadcast = true;
    const auto status_wire = nstu::control::encode_status_report(status);
    const auto decoded_status =
        nstu::control::decode_status_report(status_wire);
    assert(decoded_status.has_value());
    assert(decoded_status->snapshotting);
    assert(decoded_status->snapshot_interval_seconds == 7);
    assert(decoded_status->viewing_broadcast);

    const auto schedule = nstu::control::encode_snapshot_schedule(8);
    assert(nstu::control::decode_snapshot_schedule(schedule) == 8);
    assert(nstu::control::encode_snapshot_schedule(4).empty());

    nstu::control::SnapshotFrame snapshot;
    snapshot.width = 480;
    snapshot.height = 270;
    snapshot.captured_at_unix_milliseconds = 123'456;
    snapshot.jpeg = {std::byte{0xff}, std::byte{0xd8}, std::byte{0xff},
                     std::byte{0xd9}};
    const auto snapshot_wire =
        nstu::control::encode_snapshot_frame(snapshot);
    const auto decoded_snapshot =
        nstu::control::decode_snapshot_frame(snapshot_wire);
    assert(decoded_snapshot.has_value());
    assert(decoded_snapshot->width == snapshot.width);
    assert(decoded_snapshot->jpeg == snapshot.jpeg);
    auto corrupt_snapshot = snapshot_wire;
    corrupt_snapshot[6] = std::byte{1};
    assert(!nstu::control::decode_snapshot_frame(corrupt_snapshot)
                .has_value());
    auto oversized_snapshot = snapshot;
    oversized_snapshot.width =
        static_cast<std::uint16_t>(nstu::control::kMaximumSnapshotWidth + 1);
    assert(nstu::control::encode_snapshot_frame(oversized_snapshot).empty());
    oversized_snapshot = snapshot;
    oversized_snapshot.height =
        static_cast<std::uint16_t>(nstu::control::kMaximumSnapshotHeight + 1);
    assert(nstu::control::encode_snapshot_frame(oversized_snapshot).empty());
    auto oversized_wire = snapshot_wire;
    const auto oversized_width = static_cast<std::uint16_t>(
        nstu::control::kMaximumSnapshotWidth + 1);
    oversized_wire[2] = static_cast<std::byte>(oversized_width & 0xffu);
    oversized_wire[3] = static_cast<std::byte>(oversized_width >> 8u);
    assert(!nstu::control::decode_snapshot_frame(oversized_wire).has_value());

    const nstu::control::OverlayStroke stroke{
        .x0 = 100,
        .y0 = 200,
        .x1 = 300,
        .y1 = 400,
        .thickness = 5,
        .rgba = 0xff2020ffu,
    };
    const auto stroke_wire = nstu::control::encode_overlay_stroke(stroke);
    const auto decoded_stroke =
        nstu::control::decode_overlay_stroke(stroke_wire);
    assert(decoded_stroke.has_value());
    assert(decoded_stroke->x1 == stroke.x1);
    assert(decoded_stroke->rgba == stroke.rgba);
    return 0;
}
