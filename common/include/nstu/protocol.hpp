#pragma once

#include <cstddef>
#include <cstdint>
#include <array>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace nstu::protocol {

inline constexpr std::uint32_t kMagic = 0x4E535455; // "NSTU"
inline constexpr std::uint16_t kCommandVersion = 1;
inline constexpr std::uint16_t kVideoVersion = 2;
inline constexpr std::uint16_t kVersion = kCommandVersion;
inline constexpr std::size_t kCommandHeaderBytes = 20;
inline constexpr std::size_t kConnectionPreambleBytes = 32;
inline constexpr std::size_t kVideoHeaderBytes = 44;
inline constexpr std::uint32_t kMaxCommandPayload = 64u * 1024u;
inline constexpr std::size_t kMaxTcpBufferedBytes =
    4u * (kCommandHeaderBytes + kMaxCommandPayload);

enum class CommandType : std::uint16_t {
    hello = 1,
    hello_ack = 2,
    heartbeat = 3,
    lock = 4,
    unlock = 5,
    chat = 6,
    keyframe_request = 7,
    auth_hello = 8,
    auth_challenge = 9,
    auth_proof = 10,
    auth_accept = 11,
    enrollment_request = 12,
    enrollment_accept = 13,
    key_rotation = 14,
    key_revocation = 15,
    video_group_key = 16,
    status_request = 17,
    status_report = 18,
    start_stream = 19,
    stop_stream = 20,
    video_nack = 21,
    start_snapshots = 22,
    stop_snapshots = 23,
    snapshot_frame = 24,
    overlay_stroke = 25,
    overlay_clear = 26,
    host_snapshot = 27,
    host_broadcast_stop = 28,
    remote_start = 29,
    remote_input = 30,
    remote_end = 31,
};

enum class ConnectionRole : std::uint8_t {
    client = 1,
    server = 2,
    diagnostic = 3,
};

struct ConnectionPreamble {
    std::uint16_t version = kCommandVersion;
    ConnectionRole role = ConnectionRole::client;
    std::uint8_t flags = 0;
    std::uint16_t preamble_bytes = kConnectionPreambleBytes;
    std::uint16_t reserved = 0;
    std::uint32_t key_id = 0;
    std::array<std::byte, 16> identity{};
};

enum class VideoFlags : std::uint16_t {
    none = 0,
    keyframe = 1 << 0,
    end_of_frame = 1 << 1,
};

struct CommandEnvelope {
    std::uint16_t version = kVersion;
    CommandType type = CommandType::hello;
    std::uint32_t payload_bytes = 0;
    std::uint64_t request_id = 0;
};

struct VideoPacketHeader {
    std::uint16_t version = kVideoVersion;
    VideoFlags flags = VideoFlags::none;
    std::uint32_t stream_id = 0;
    std::uint64_t packet_sequence = 0;
    std::uint64_t frame_id = 0;
    std::uint16_t fragment_index = 0;
    std::uint16_t fragment_count = 0;
    std::uint32_t payload_bytes = 0;
    std::uint64_t capture_time_100ns = 0;
};

[[nodiscard]] std::vector<std::byte> encode_command_header(
    const CommandEnvelope& envelope);

[[nodiscard]] std::optional<CommandEnvelope> decode_command_header(
    std::span<const std::byte> wire);

[[nodiscard]] std::vector<std::byte> encode_connection_preamble(
    const ConnectionPreamble& preamble);

[[nodiscard]] std::optional<ConnectionPreamble> decode_connection_preamble(
    std::span<const std::byte> wire);

[[nodiscard]] std::vector<std::byte> encode_video_header(
    const VideoPacketHeader& header);

[[nodiscard]] std::optional<VideoPacketHeader> decode_video_header(
    std::span<const std::byte> wire);

struct TcpFrame {
    CommandEnvelope envelope;
    std::vector<std::byte> payload;
};

[[nodiscard]] std::vector<std::byte> encode_tcp_frame(
    const CommandEnvelope& envelope, std::span<const std::byte> payload);

class TcpFrameParser {
public:
    // Appends bytes and emits every complete frame currently in the buffer.
    // Returns false if a framing or payload limit is violated.
    bool feed(std::span<const std::byte> bytes, std::vector<TcpFrame>& frames);
    void reset() noexcept;

private:
    std::vector<std::byte> buffer_;
};

} // namespace nstu::protocol
