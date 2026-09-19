#include "nstu/discovery.hpp"
#include "nstu/key_store.hpp"
#include "nstu/multicast.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace {

nstu::security::ClientId make_client_id() {
    nstu::security::ClientId client_id{};
    for (std::size_t index = 0; index < client_id.size(); ++index) {
        client_id[index] = static_cast<std::byte>(index + 1);
    }
    return client_id;
}

std::vector<std::byte> make_key(std::uint8_t seed) {
    std::vector<std::byte> key(nstu::security::kMinimumProtocolKeyBytes);
    for (std::size_t index = 0; index < key.size(); ++index) {
        key[index] = static_cast<std::byte>(seed + index);
    }
    return key;
}

std::uint64_t unix_seconds_now() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<
        std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
                                          .count());
}

class ScopedSocket {
public:
    explicit ScopedSocket(SOCKET socket) noexcept : socket_(socket) {}
    ~ScopedSocket() {
        if (socket_ != INVALID_SOCKET) {
            closesocket(socket_);
        }
    }
    ScopedSocket(const ScopedSocket&) = delete;
    ScopedSocket& operator=(const ScopedSocket&) = delete;

    [[nodiscard]] SOCKET get() const noexcept { return socket_; }

private:
    SOCKET socket_ = INVALID_SOCKET;
};

} // namespace

int main() {
    nstu::net::WinsockRuntime winsock;
    assert(winsock.ready());

    constexpr std::uint32_t key_id = 19;
    constexpr std::uint64_t timestamp = 1'900'000'000;
    const auto client_id = make_client_id();
    auto key = make_key(0x31);
    auto wrong_key = make_key(0x71);

    const auto request = nstu::discovery::create_request(
        client_id, key_id, timestamp, key);
    assert(request.has_value());
    const auto request_wire = nstu::discovery::encode_request(*request);
    assert(request_wire.size() == nstu::discovery::kDiscoveryPacketBytes);
    const auto decoded_request =
        nstu::discovery::decode_request(request_wire);
    assert(decoded_request.has_value());
    assert(decoded_request->client_id == client_id);
    assert(decoded_request->key_id == key_id);
    assert(decoded_request->client_nonce == request->client_nonce);
    assert(nstu::discovery::verify_request(*decoded_request, key, timestamp));
    assert(!nstu::discovery::verify_request(*decoded_request, wrong_key,
                                             timestamp));
    assert(!nstu::discovery::verify_request(*decoded_request, key,
                                             timestamp + 121));

    auto tampered_request_wire = request_wire;
    tampered_request_wire[8] ^= static_cast<std::byte>(0x01);
    const auto tampered_request =
        nstu::discovery::decode_request(tampered_request_wire);
    assert(tampered_request.has_value());
    assert(!nstu::discovery::verify_request(*tampered_request, key,
                                             timestamp));

    constexpr std::uint16_t control_port = 47001;
    const auto response = nstu::discovery::create_response(
        *request, control_port, timestamp + 1, key);
    assert(response.has_value());
    const auto response_wire = nstu::discovery::encode_response(*response);
    assert(response_wire.size() == nstu::discovery::kDiscoveryPacketBytes);
    const auto decoded_response =
        nstu::discovery::decode_response(response_wire);
    assert(decoded_response.has_value());
    assert(decoded_response->control_port == control_port);
    assert(nstu::discovery::verify_response(*decoded_response, *request, key,
                                             timestamp + 1));
    assert(!nstu::discovery::verify_response(*decoded_response, *request,
                                              wrong_key, timestamp + 1));

    const auto other_request = nstu::discovery::create_request(
        client_id, key_id, timestamp, key);
    assert(other_request.has_value());
    assert(other_request->client_nonce != request->client_nonce);
    assert(!nstu::discovery::verify_response(*decoded_response,
                                              *other_request, key,
                                              timestamp + 1));

    auto tampered_response_wire = response_wire;
    tampered_response_wire.back() ^= static_cast<std::byte>(0x80);
    const auto tampered_response =
        nstu::discovery::decode_response(tampered_response_wire);
    assert(tampered_response.has_value());
    assert(!nstu::discovery::verify_response(*tampered_response, *request, key,
                                              timestamp + 1));

    nstu::security::KeyStore key_store;
    std::string error;
    assert(key_store.enroll(client_id, key_id, key, &error));
    nstu::discovery::AuthenticatedDiscoveryResponder responder;
    assert(responder.start(0, control_port, key_store, &error));
    assert(responder.running());
    assert(responder.local_port() != 0);

    const auto duplicate_request = nstu::discovery::create_request(
        client_id, key_id, unix_seconds_now(), key);
    assert(duplicate_request.has_value());
    const auto duplicate_wire =
        nstu::discovery::encode_request(*duplicate_request);
    ScopedSocket duplicate_sender(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    assert(duplicate_sender.get() != INVALID_SOCKET);
    sockaddr_in duplicate_target{};
    duplicate_target.sin_family = AF_INET;
    duplicate_target.sin_port = htons(responder.local_port());
    assert(InetPtonA(AF_INET, "127.0.0.1", &duplicate_target.sin_addr) == 1);
    for (int duplicate = 0; duplicate < 10; ++duplicate) {
        assert(sendto(duplicate_sender.get(),
                      reinterpret_cast<const char*>(duplicate_wire.data()),
                      static_cast<int>(duplicate_wire.size()), 0,
                      reinterpret_cast<const sockaddr*>(&duplicate_target),
                      sizeof(duplicate_target)) ==
               static_cast<int>(duplicate_wire.size()));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    assert(responder.running());

    nstu::discovery::ClientDiscoveryOptions options;
    options.discovery_port = responder.local_port();
    options.timeout = std::chrono::milliseconds(250);
    options.maximum_endpoints = 2;
    options.target_addresses = {"127.0.0.1"};

    auto excessive_target_options = options;
    excessive_target_options.target_addresses.assign(33, "127.0.0.1");
    const auto excessive_targets =
        nstu::discovery::discover_authenticated_servers(
            client_id, key_id, key, excessive_target_options, &error);
    assert(excessive_targets.empty());
    assert(error == "too many explicit IPv4 discovery targets");

    const auto rejected = nstu::discovery::discover_authenticated_servers(
        client_id, key_id, wrong_key, options, &error);
    assert(rejected.empty());
    assert(!error.empty());

    const auto endpoints = nstu::discovery::discover_authenticated_servers(
        client_id, key_id, key, options, &error);
    assert(error.empty());
    assert(endpoints.size() == 1);
    assert(endpoints[0].address == "127.0.0.1");
    assert(endpoints[0].port == control_port);

    // Pairing discovery. A machine that has never enrolled holds no key, so
    // the beacon is the only thing it can see - and only while the operator
    // has the enrollment window open.
    auto pairing_options = options;
    assert(!responder.pairing_beacon_enabled());
    const auto closed_window =
        nstu::discovery::discover_pairing_candidates(pairing_options, &error);
    assert(closed_window.empty());
    assert(error == "no NSTU server answered the pairing probe");

    // A blank display name leaves the window closed rather than advertising an
    // anonymous server.
    responder.set_pairing_beacon(true, "");
    assert(!responder.pairing_beacon_enabled());

    responder.set_pairing_beacon(true, "Lab A\tRoom 201");
    assert(responder.pairing_beacon_enabled());
    const auto candidates =
        nstu::discovery::discover_pairing_candidates(pairing_options, &error);
    assert(error.empty());
    assert(candidates.size() == 1);
    assert(candidates[0].address == "127.0.0.1");
    assert(candidates[0].port == control_port);
    // Control characters never reach the selection menu.
    assert(candidates[0].server_name == "Lab A?Room 201");

    const std::string long_name(
        nstu::discovery::kMaximumServerNameBytes + 40, 'N');
    responder.set_pairing_beacon(true, long_name);
    const auto truncated =
        nstu::discovery::discover_pairing_candidates(pairing_options, &error);
    assert(truncated.size() == 1);
    assert(truncated[0].server_name.size() ==
           nstu::discovery::kMaximumServerNameBytes);

    // An open pairing window does not disturb the authenticated sweep.
    const auto still_authenticated =
        nstu::discovery::discover_authenticated_servers(client_id, key_id, key,
                                                        options, &error);
    assert(still_authenticated.size() == 1);
    assert(still_authenticated[0].port == control_port);

    responder.set_pairing_beacon(false, "Lab A");
    assert(!responder.pairing_beacon_enabled());
    const auto after_close =
        nstu::discovery::discover_pairing_candidates(pairing_options, &error);
    assert(after_close.empty());

    responder.stop();
    assert(!responder.running());
    assert(responder.local_port() == 0);
    nstu::security::secure_zero(key);
    nstu::security::secure_zero(wrong_key);
    return 0;
}
