#pragma once

#include "nstu/named_pipe.hpp"
#include "nstu/protocol.hpp"
#include "nstu/protocol_headers.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace nstu::client {

enum class AgentMessageType : std::uint16_t {
    lock = 1,
    unlock = 2,
    chat = 3,
    status_request = 4,
    status_report = 5,
    start_stream = 6,
    stop_stream = 7,
    keyframe_request = 8,
    start_snapshots = 9,
    stop_snapshots = 10,
    snapshot_frame = 11,
    overlay_stroke = 12,
    overlay_clear = 13,
    host_snapshot = 14,
    host_broadcast_stop = 15,
    remote_start = 16,
    remote_input = 17,
    remote_end = 18,
    // Client-to-service bridge for durable exam answer events. The service
    // validates and persists the payload before forwarding it to the server.
    exam_answer_event = 19,
    exam_state_request = 20,
    exam_answer_ack = 21,
    exam_state_response = 22,
    exam_start = 23,
    exam_stop = 24,
};

struct AgentMessage {
    AgentMessageType type = AgentMessageType::status_request;
    std::vector<std::byte> payload;
};

struct AgentStatus {
    bool locked = false;
    bool streaming = false;
    bool snapshotting = false;
    bool viewing_broadcast = false;
    std::uint8_t frames_per_second = 0;
    std::uint16_t snapshot_interval_seconds = 0;
    std::uint32_t session_id = 0;
};

inline constexpr std::size_t kMaximumAgentPayloadBytes =
    protocol::kMaxCommandPayload;

[[nodiscard]] std::vector<std::byte> encode_agent_message(
    const AgentMessage& message);
[[nodiscard]] std::optional<AgentMessage> decode_agent_message(
    std::span<const std::byte> wire);
[[nodiscard]] bool send_agent_message(const NamedPipe& pipe,
                                      const AgentMessage& message,
                                      std::string* error = nullptr);
[[nodiscard]] std::optional<AgentMessage> receive_agent_message(
    const NamedPipe& pipe, std::string* error = nullptr);

[[nodiscard]] std::vector<std::byte> encode_agent_status(
    const AgentStatus& status);
[[nodiscard]] std::optional<AgentStatus> decode_agent_status(
    std::span<const std::byte> payload);

[[nodiscard]] std::vector<std::byte> encode_remote_input(
    const wire::RemoteInputPacket& packet);
[[nodiscard]] std::optional<wire::RemoteInputPacket> decode_remote_input(
    std::span<const std::byte> payload);

} // namespace nstu::client
