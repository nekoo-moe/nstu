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
    exam_answer_event = 32,
    exam_answer_ack = 33,
    exam_state_request = 34,
    exam_state_response = 35,
    // Authenticated instructor commands for the client-side exam host. The
    // server never hosts or freezes exam data; it only authorizes a client
    // start/stop request over the existing control channel.
    exam_start = 36,
    exam_stop = 37,
    // Verified pairing. An unenrolled client opens the exchange; the server
    // answers with its ephemeral key, both sides show the same six-digit code,
    // and the teacher approves or refuses it in the server UI. These frames
    // are unauthenticated by construction - that is what pairing establishes -
    // so they are only accepted before a connection reaches auth_hello.
    pairing_hello = 38,
    pairing_offer = 39,
    pairing_confirm = 40,
    pairing_accept = 41,
    pairing_reject = 42,
    // Managed mode. The server asks a client to hold itself frozen or to let
    // go; the client answers with what it actually is now, which is not
    // always what it was asked to be.
    freeze_set = 43,
    freeze_report = 44,
    // Reboot-to-restore. The request carries an explicit checkpoint
    // acknowledgement; the client runs readiness checks and reports what it
    // actually changed instead of the server assuming success.
    uwf_configure = 45,
    uwf_report = 46,
    // Fleet reboot-to-restore. uwf_configure/uwf_report only describe a
    // pre-reboot configuration attempt. These carry a server-issued operation
    // id and the client's current boot identity, so the server can tell "I
    // armed UWF and intend to reboot" apart from "I rebooted and am protected
    // right now". Exam authorization depends on that distinction.
    uwf_fleet_configure = 47,
    uwf_fleet_status = 48,
    // Client-side activity log shipped to the teacher machine. Bounded,
    // sanitized, sequence-acknowledged, and rate limited on the server.
    audit_upload = 49,
    audit_ack = 50,
    // Client-to-server chat. The teacher's chat panel keeps a per-client
    // transcript; the client submits its own lines over the authenticated
    // control channel (the reverse direction reuses `chat`).
    client_chat = 51,
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
