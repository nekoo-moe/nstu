#include "nstu/auth.hpp"

#include <cassert>
#include <string_view>

namespace {

std::span<const std::byte> bytes(std::string_view text) {
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

nstu::security::Sha256Digest expected_hmac() {
    using std::byte;
    return {
        byte{0xf7}, byte{0xbc}, byte{0x83}, byte{0xf4}, byte{0x30}, byte{0x53},
        byte{0x84}, byte{0x24}, byte{0xb1}, byte{0x32}, byte{0x98}, byte{0xe6},
        byte{0xaa}, byte{0x6f}, byte{0xb1}, byte{0x43}, byte{0xef}, byte{0x4d},
        byte{0x59}, byte{0xa1}, byte{0x49}, byte{0x46}, byte{0x17}, byte{0x59},
        byte{0x97}, byte{0x47}, byte{0x9d}, byte{0xbc}, byte{0x2d}, byte{0x1a},
        byte{0x3c}, byte{0xd8},
    };
}

} // namespace

int main() {
    using namespace nstu::security;
    const auto digest = hmac_sha256(
        bytes("key"), bytes("The quick brown fox jumps over the lazy dog"));
    assert(digest.has_value());
    assert(constant_time_equal(*digest, expected_hmac()));

    AuthHello hello;
    hello.unix_time_seconds = 10'000;
    hello.key_id = 7;
    for (std::size_t index = 0; index < hello.client_id.size(); ++index) {
        hello.client_id[index] = static_cast<std::byte>(index);
    }
    for (std::size_t index = 0; index < hello.client_nonce.size(); ++index) {
        hello.client_nonce[index] = static_cast<std::byte>(index + 16);
    }
    AuthChallenge challenge;
    challenge.unix_time_seconds = 10'001;
    for (std::size_t index = 0; index < challenge.server_nonce.size(); ++index) {
        challenge.server_nonce[index] = static_cast<std::byte>(0x80 + index);
    }
    const auto hello_wire = encode_auth_hello(hello);
    const auto challenge_wire = encode_auth_challenge(challenge);
    assert(hello_wire.size() == kAuthHelloBytes);
    assert(challenge_wire.size() == kAuthChallengeBytes);
    const auto decoded_hello = decode_auth_hello(hello_wire);
    const auto decoded_challenge = decode_auth_challenge(challenge_wire);
    assert(decoded_hello.has_value());
    assert(decoded_challenge.has_value());
    assert(decoded_hello->client_id == hello.client_id);
    assert(decoded_hello->client_nonce == hello.client_nonce);
    assert(decoded_hello->key_id == hello.key_id);
    assert(decoded_challenge->server_nonce == challenge.server_nonce);
    assert(!decode_auth_hello({}).has_value());
    assert(validate_auth_hello(hello));
    assert(validate_auth_challenge(challenge));

    const auto pre_shared_key = bytes("32-byte-development-key-material!");
    const auto client_proof =
        compute_client_proof(pre_shared_key, hello, challenge);
    const auto session_key = derive_session_key(pre_shared_key, hello, challenge);
    assert(client_proof.has_value());
    assert(session_key.has_value());
    assert(!constant_time_equal(*client_proof, *session_key));
    const auto server_proof =
        compute_server_proof(*session_key, hello, challenge);
    assert(server_proof.has_value());
    assert(!constant_time_equal(*client_proof, *server_proof));
    assert(!compute_client_proof(bytes("short"), hello, challenge).has_value());

    nstu::protocol::VideoPacketHeader header{
        .version = nstu::protocol::kVideoVersion,
        .flags = nstu::protocol::VideoFlags::end_of_frame,
        .stream_id = 4,
        .packet_sequence = 55,
        .frame_id = 8,
        .fragment_index = 0,
        .fragment_count = 1,
        .payload_bytes = 3,
        .capture_time_100ns = 99,
    };
    const std::array<std::byte, 3> payload{
        std::byte{1}, std::byte{2}, std::byte{3}};
    const auto tag = compute_video_auth_tag(*session_key, header, payload);
    assert(tag.has_value());
    assert(verify_video_auth_tag(*session_key, header, payload, *tag));
    auto tampered_payload = payload;
    tampered_payload[1] ^= std::byte{1};
    assert(!verify_video_auth_tag(*session_key, header, tampered_payload, *tag));
    auto tampered_header = header;
    ++tampered_header.packet_sequence;
    assert(!verify_video_auth_tag(*session_key, tampered_header, payload, *tag));

    nstu::protocol::CommandEnvelope command{
        .version = nstu::protocol::kCommandVersion,
        .type = nstu::protocol::CommandType::lock,
        .payload_bytes = static_cast<std::uint32_t>(payload.size()),
        .request_id = 77,
    };
    const auto control_tag =
        compute_control_auth_tag(*session_key, command, 0, payload);
    assert(control_tag.has_value());
    assert(verify_control_auth_tag(*session_key, command, 0, payload,
                                   *control_tag));
    assert(!verify_control_auth_tag(*session_key, command, 1, payload,
                                     *control_tag));
    const auto client_directional_tag = compute_control_auth_tag(
        *session_key, command, 0, payload,
        ControlDirection::client_to_server);
    const auto server_directional_tag = compute_control_auth_tag(
        *session_key, command, 0, payload,
        ControlDirection::server_to_client);
    assert(client_directional_tag.has_value());
    assert(server_directional_tag.has_value());
    assert(!constant_time_equal(*client_directional_tag,
                                *server_directional_tag));
    assert(verify_control_auth_tag(*session_key, command, 0, payload,
                                   ControlDirection::client_to_server,
                                   *client_directional_tag));
    assert(!verify_control_auth_tag(*session_key, command, 0, payload,
                                    ControlDirection::server_to_client,
                                    *client_directional_tag));
    nstu::security::ControlSequenceGuard sequence_guard;
    sequence_guard.reset(0);
    assert(sequence_guard.accept(0));
    assert(!sequence_guard.accept(0));
    assert(!sequence_guard.accept(2));
    assert(sequence_guard.accept(1));

    ReplayProtector replay(4, std::chrono::seconds(120));
    assert(replay.accept(hello, 10'000));
    assert(!replay.accept(hello, 10'001));
    auto second_hello = hello;
    second_hello.client_nonce[0] ^= std::byte{1};
    assert(replay.accept(second_hello, 10'001));
    auto stale = hello;
    stale.client_nonce[0] ^= std::byte{2};
    stale.unix_time_seconds = 9'000;
    assert(!replay.accept(stale, 10'001));

    Nonce random_nonce{};
    assert(generate_random(random_nonce));
    auto sensitive = *session_key;
    secure_zero(sensitive);
    for (const auto value : sensitive) {
        assert(value == std::byte{0});
    }
    return 0;
}
