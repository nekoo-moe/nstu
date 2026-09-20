// End-to-end verified pairing over the real control plane: a client that
// holds no key at all reaches an operator prompt, and only an approval there
// turns into an enrolled key it can authenticate with.
#include "nstu/control_channel.hpp"
#include "nstu/control_plane.hpp"
#include "nstu/keyring.hpp"
#include "nstu/multicast.hpp"
#include "nstu/pairing.hpp"

#include <windows.h>

#include <array>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr std::uint64_t kHelloRequestId = 91;
constexpr const char* kClientUuid = "6F2C41A8-1B7D-4E55-9C03-8A1F27D0B4E6";
constexpr const char* kClientHostname = "LAB-PC-07";

bool receive_exact(nstu::net::TcpSocket& socket, std::span<std::byte> output) {
    std::size_t offset = 0;
    while (offset < output.size()) {
        const int received = socket.receive(output.subspan(offset), nullptr);
        if (received <= 0) {
            return false;
        }
        offset += static_cast<std::size_t>(received);
    }
    return true;
}

std::optional<nstu::protocol::TcpFrame> receive_frame(
    nstu::net::TcpSocket& socket) {
    std::array<std::byte, 4> prefix{};
    if (!receive_exact(socket, prefix)) {
        return std::nullopt;
    }
    const std::uint32_t body_bytes =
        std::to_integer<std::uint32_t>(prefix[0]) |
        (std::to_integer<std::uint32_t>(prefix[1]) << 8u) |
        (std::to_integer<std::uint32_t>(prefix[2]) << 16u) |
        (std::to_integer<std::uint32_t>(prefix[3]) << 24u);
    std::vector<std::byte> body(body_bytes);
    if (!receive_exact(socket, body)) {
        return std::nullopt;
    }
    std::vector<std::byte> wire(prefix.begin(), prefix.end());
    wire.insert(wire.end(), body.begin(), body.end());
    nstu::protocol::TcpFrameParser parser;
    std::vector<nstu::protocol::TcpFrame> frames;
    return parser.feed(wire, frames) && frames.size() == 1
        ? std::optional{std::move(frames.front())}
        : std::nullopt;
}

bool send_frame(nstu::net::TcpSocket& socket, nstu::protocol::CommandType type,
                std::uint64_t request_id,
                std::span<const std::byte> payload) {
    const nstu::protocol::CommandEnvelope envelope{
        .version = nstu::protocol::kCommandVersion,
        .type = type,
        .payload_bytes = static_cast<std::uint32_t>(payload.size()),
        .request_id = request_id,
    };
    const auto wire = nstu::protocol::encode_tcp_frame(envelope, payload);
    return socket.send_all(wire, nullptr) == static_cast<int>(wire.size());
}

// Everything a paired-but-unapproved client is holding while it waits.
struct ClientSide {
    nstu::net::TcpSocket socket;
    nstu::pairing::PairingTranscript transcript;
    nstu::pairing::PairingSecrets secrets;
};

// Drives the client half up to and including the confirmation tag. The server
// only shows a request to the operator once this much has happened.
std::optional<ClientSide> begin_pairing(std::uint16_t port, bool send_confirm,
                                        bool corrupt_confirm) {
    auto key_pair = nstu::pairing::EphemeralKeyPair::generate(nullptr);
    if (!key_pair) {
        return std::nullopt;
    }
    nstu::pairing::PairingHello hello;
    hello.agreement = key_pair->agreement();
    hello.client_uuid = kClientUuid;
    hello.client_hostname = kClientHostname;
    const auto public_key = key_pair->public_key();
    hello.client_public_key.assign(public_key.begin(), public_key.end());
    if (!nstu::security::generate_random(hello.client_nonce)) {
        return std::nullopt;
    }
    ClientSide client;
    std::string error;
    if (!client.socket.connect("127.0.0.1", port, &error) ||
        !client.socket.set_io_timeouts(5000, 5000, &error) ||
        !send_frame(client.socket, nstu::protocol::CommandType::pairing_hello,
                    kHelloRequestId,
                    nstu::pairing::encode_pairing_hello(hello))) {
        return std::nullopt;
    }
    const auto offered = receive_frame(client.socket);
    if (!offered ||
        offered->envelope.type != nstu::protocol::CommandType::pairing_offer) {
        return std::nullopt;
    }
    const auto offer = nstu::pairing::decode_pairing_offer(offered->payload);
    if (!offer) {
        return std::nullopt;
    }
    auto transcript = nstu::pairing::make_transcript(hello, *offer);
    auto shared = key_pair->agree(offer->server_public_key, nullptr);
    if (!transcript || !shared) {
        return std::nullopt;
    }
    auto secrets = nstu::pairing::derive_pairing_secrets(*shared, *transcript);
    nstu::security::secure_zero(*shared);
    if (!secrets) {
        return std::nullopt;
    }
    client.transcript = std::move(*transcript);
    client.secrets = std::move(*secrets);
    if (!send_confirm) {
        return client;
    }
    const auto tag = nstu::pairing::confirmation_tag(
        client.secrets, client.transcript,
        nstu::pairing::ConfirmationRole::client);
    if (!tag) {
        return std::nullopt;
    }
    nstu::pairing::PairingConfirm confirm{.client_tag = *tag};
    if (corrupt_confirm) {
        confirm.client_tag[0] ^= static_cast<std::byte>(0x40);
    }
    if (!send_frame(client.socket,
                    nstu::protocol::CommandType::pairing_confirm,
                    kHelloRequestId,
                    nstu::pairing::encode_pairing_confirm(confirm))) {
        return std::nullopt;
    }
    return client;
}

std::vector<nstu::server::PendingPairing> wait_for_pending(
    const nstu::server::ServerControlPlane& server, std::size_t expected) {
    for (int attempt = 0; attempt < 100; ++attempt) {
        auto pending = server.pending_pairings();
        if (pending.size() == expected) {
            return pending;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return server.pending_pairings();
}

} // namespace

int main() {
    nstu::net::WinsockRuntime winsock;
    assert(winsock.ready());

    const std::array<std::byte, 4> entropy{
        std::byte{9}, std::byte{4}, std::byte{6}, std::byte{2}};
    wchar_t temporary_directory[MAX_PATH]{};
    assert(GetTempPathW(MAX_PATH, temporary_directory) != 0);
    const auto keyring_path = std::filesystem::path(temporary_directory) /
        (L"nstu-pairing-keyring-" + std::to_wstring(GetCurrentProcessId()) +
         L".bin");

    nstu::security::KeyStore key_store;
    nstu::server::ClientRegistry registry;
    nstu::server::ServerControlPlane server(registry, key_store);
    nstu::server::ServerControlPlaneConfig config;
    config.port = 0;
    config.keyring_path = keyring_path.wstring();
    config.keyring_entropy.assign(entropy.begin(), entropy.end());
    // Deliberately no enrollment secret: pairing must work on a server that
    // has never had one, because removing the copied secret is the point.
    std::string error;
    assert(server.start(std::move(config), &error));
    const auto port = server.local_port();

    // A closed window refuses the exchange outright rather than leaking that
    // there is something here worth talking to.
    assert(!server.pairing_window_open());
    {
        nstu::net::TcpSocket early;
        assert(early.connect("127.0.0.1", port, &error));
        assert(early.set_io_timeouts(5000, 5000, &error));
        nstu::pairing::PairingHello hello;
        auto key_pair = nstu::pairing::EphemeralKeyPair::generate(nullptr);
        assert(key_pair.has_value());
        hello.agreement = key_pair->agreement();
        hello.client_uuid = kClientUuid;
        hello.client_hostname = kClientHostname;
        const auto public_key = key_pair->public_key();
        hello.client_public_key.assign(public_key.begin(), public_key.end());
        assert(nstu::security::generate_random(hello.client_nonce));
        assert(send_frame(early, nstu::protocol::CommandType::pairing_hello,
                          kHelloRequestId,
                          nstu::pairing::encode_pairing_hello(hello)));
        const auto refused = receive_frame(early);
        assert(refused.has_value());
        assert(refused->envelope.type ==
               nstu::protocol::CommandType::pairing_reject);
        const auto reason =
            nstu::pairing::decode_pairing_reject(refused->payload);
        assert(reason.has_value());
        assert(*reason == nstu::pairing::PairingRejectReason::unavailable);
        early.close();
    }

    server.set_pairing_window(true, "Lab A");
    assert(server.pairing_window_open());

    // A client that completes the exchange but sends a bad tag never reaches
    // the operator: it is refused by arithmetic, not by judgement.
    {
        auto corrupted = begin_pairing(port, true, true);
        assert(corrupted.has_value());
        const auto refused = receive_frame(corrupted->socket);
        assert(refused.has_value());
        assert(refused->envelope.type ==
               nstu::protocol::CommandType::pairing_reject);
        const auto reason =
            nstu::pairing::decode_pairing_reject(refused->payload);
        assert(reason.has_value());
        assert(*reason ==
               nstu::pairing::PairingRejectReason::confirmation_failed);
        nstu::pairing::secure_zero(corrupted->secrets);
        corrupted->socket.close();
    }
    assert(server.pending_pairings().empty());

    // An operator who declines leaves no key behind.
    {
        auto declined = begin_pairing(port, true, false);
        assert(declined.has_value());
        const auto pending = wait_for_pending(server, 1);
        assert(pending.size() == 1);
        assert(server.reject_pairing(pending[0].pairing_id, &error));
        const auto refused = receive_frame(declined->socket);
        assert(refused.has_value());
        assert(refused->envelope.type ==
               nstu::protocol::CommandType::pairing_reject);
        const auto reason =
            nstu::pairing::decode_pairing_reject(refused->payload);
        assert(reason.has_value());
        assert(*reason ==
               nstu::pairing::PairingRejectReason::operator_declined);
        assert(server.pending_pairings().empty());
        nstu::pairing::secure_zero(declined->secrets);
        declined->socket.close();
    }
    assert(key_store.active_key_count() == 0);

    auto client = begin_pairing(port, true, false);
    assert(client.has_value());
    const auto pending = wait_for_pending(server, 1);
    assert(pending.size() == 1);
    assert(pending[0].client_uuid == kClientUuid);
    assert(pending[0].hostname == kClientHostname);
    // The teacher's whole job: this string has to match the client screen.
    assert(pending[0].short_authentication_string ==
           client->secrets.short_authentication_string);
    assert(pending[0].short_authentication_string.size() ==
           nstu::pairing::kSasDigits);
    assert(pending[0].seconds_remaining > 0);
    assert(!pending[0].address.empty());

    assert(server.approve_pairing(pending[0].pairing_id, &error));
    assert(server.pending_pairings().empty());

    const auto accepted = receive_frame(client->socket);
    assert(accepted.has_value());
    assert(accepted->envelope.type ==
           nstu::protocol::CommandType::pairing_accept);
    assert(accepted->envelope.request_id == kHelloRequestId);
    const auto accept = nstu::pairing::decode_pairing_accept(accepted->payload);
    assert(accept.has_value());
    assert(accept->key_id != 0);
    // The server tag is what tells the client the approval came from the
    // machine it ran the exchange with, not from whoever holds the socket now.
    assert(nstu::pairing::verify_confirmation_tag(
        client->secrets, client->transcript,
        nstu::pairing::ConfirmationRole::server, accept->server_tag));
    client->socket.close();

    // The derived key is a real protocol key: the ordinary authenticated
    // handshake has to succeed with it and nothing else was copied anywhere.
    const auto client_id = nstu::pairing::client_id_from_uuid(kClientUuid);
    assert(client_id.has_value());
    const std::vector<std::byte> enrolled_key(
        client->secrets.enrolled_key.begin(),
        client->secrets.enrolled_key.end());
    const auto now = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    nstu::net::TcpSocket control_socket;
    assert(control_socket.connect("127.0.0.1", port, &error));
    assert(control_socket.set_io_timeouts(5000, 5000, &error));
    const auto session = nstu::control::client_handshake(
        control_socket, *client_id, accept->key_id, enrolled_key, now,
        std::chrono::seconds(120), &error);
    assert(session.has_value());
    control_socket.close();

    // Closing the window stops answering new machines.
    server.set_pairing_window(false);
    assert(!server.pairing_window_open());

    server.stop();

    nstu::security::KeyStore restored;
    assert(nstu::security::load_keyring(restored, keyring_path.wstring(),
                                        entropy, &error));
    assert(restored.resolve(*client_id, accept->key_id) == enrolled_key);
    assert(DeleteFileW(keyring_path.c_str()));
    nstu::pairing::secure_zero(client->secrets);
    return 0;
}
