#include "nstu/protocol.hpp"
#include "nstu/multicast.hpp"
#include "nstu/network.hpp"

#include <cassert>
#include <span>
#include <vector>

int main() {
    using namespace nstu::protocol;

    const CommandEnvelope command{
        .version = kVersion,
        .type = CommandType::lock,
        .payload_bytes = 12,
        .request_id = 0x0102030405060708ULL,
    };
    const auto command_wire = encode_command_header(command);
    assert(command_wire.size() == kCommandHeaderBytes);
    const auto decoded_command = decode_command_header(command_wire);
    assert(decoded_command.has_value());
    assert(decoded_command->type == command.type);
    assert(decoded_command->payload_bytes == command.payload_bytes);
    assert(decoded_command->request_id == command.request_id);

    ConnectionPreamble preamble;
    preamble.role = ConnectionRole::client;
    preamble.key_id = 42;
    for (std::size_t index = 0; index < preamble.identity.size(); ++index) {
        preamble.identity[index] = static_cast<std::byte>(index + 1);
    }
    const auto preamble_wire = encode_connection_preamble(preamble);
    assert(preamble_wire.size() == kConnectionPreambleBytes);
    const auto decoded_preamble = decode_connection_preamble(preamble_wire);
    assert(decoded_preamble.has_value());
    assert(decoded_preamble->role == ConnectionRole::client);
    assert(decoded_preamble->key_id == preamble.key_id);
    assert(decoded_preamble->identity == preamble.identity);
    ConnectionPreamble diagnostic;
    diagnostic.role = ConnectionRole::diagnostic;
    const auto diagnostic_wire = encode_connection_preamble(diagnostic);
    const auto decoded_diagnostic =
        decode_connection_preamble(diagnostic_wire);
    assert(decoded_diagnostic.has_value());
    assert(decoded_diagnostic->role == ConnectionRole::diagnostic);
    auto invalid_preamble = preamble_wire;
    invalid_preamble[6] = std::byte{0xff};
    assert(!decode_connection_preamble(invalid_preamble).has_value());

    const VideoPacketHeader video{
        .version = kVideoVersion,
        .flags = VideoFlags::keyframe,
        .stream_id = 42,
        .packet_sequence = 1234,
        .frame_id = 99,
        .fragment_index = 0,
        .fragment_count = 1,
        .payload_bytes = 1200,
        .capture_time_100ns = 123456,
    };
    const auto video_wire = encode_video_header(video);
    assert(video_wire.size() == kVideoHeaderBytes);
    const auto decoded_video = decode_video_header(video_wire);
    assert(decoded_video.has_value());
    assert(decoded_video->stream_id == video.stream_id);
    assert(decoded_video->packet_sequence == video.packet_sequence);
    assert(decoded_video->flags == video.flags);
    assert(decoded_video->capture_time_100ns == video.capture_time_100ns);

    assert(!decode_command_header({}).has_value());
    const auto short_video = std::span<const std::byte>(video_wire).subspan(
        0, kVideoHeaderBytes - 1);
    assert(!decode_video_header(short_video).has_value());

    const std::vector<std::byte> payload{
        std::byte{0x10}, std::byte{0x20}, std::byte{0x30}};
    const CommandEnvelope payload_command{
        .version = kVersion,
        .type = CommandType::chat,
        .payload_bytes = static_cast<std::uint32_t>(payload.size()),
        .request_id = 7,
    };
    const auto frame_wire = encode_tcp_frame(payload_command, payload);
    assert(!frame_wire.empty());
    TcpFrameParser parser;
    std::vector<TcpFrame> frames;
    assert(parser.feed(std::span<const std::byte>(frame_wire).subspan(0, 2),
                       frames));
    assert(frames.empty());
    assert(parser.feed(std::span<const std::byte>(frame_wire).subspan(2),
                       frames));
    assert(frames.size() == 1);
    assert(frames[0].envelope.type == CommandType::chat);
    assert(frames[0].payload == payload);

    std::vector<std::byte> coalesced = frame_wire;
    coalesced.insert(coalesced.end(), frame_wire.begin(), frame_wire.end());
    TcpFrameParser coalesced_parser;
    std::vector<TcpFrame> coalesced_frames;
    assert(coalesced_parser.feed(coalesced, coalesced_frames));
    assert(coalesced_frames.size() == 2);

    auto malformed = frame_wire;
    malformed[sizeof(std::uint32_t)] = std::byte{0xff};
    TcpFrameParser malformed_parser;
    std::vector<TcpFrame> malformed_frames;
    assert(!malformed_parser.feed(malformed, malformed_frames));

    nstu::net::WinsockRuntime winsock;
#if defined(_WIN32)
    assert(winsock.ready());
#else
    assert(!winsock.ready());
#endif

    nstu::net::DeliveryModeSelector delivery(2, 3);
    assert(delivery.mode() == nstu::net::VideoDeliveryMode::multicast);
    delivery.record_multicast_probe(false);
    assert(delivery.mode() == nstu::net::VideoDeliveryMode::multicast);
    delivery.record_multicast_probe(false);
    assert(delivery.mode() == nstu::net::VideoDeliveryMode::unicast);
    delivery.record_multicast_probe(true);
    assert(delivery.mode() == nstu::net::VideoDeliveryMode::unicast);
    delivery.record_multicast_probe(true);
    assert(delivery.mode() == nstu::net::VideoDeliveryMode::unicast);
    delivery.record_multicast_probe(true);
    assert(delivery.mode() == nstu::net::VideoDeliveryMode::multicast);
    return 0;
}
