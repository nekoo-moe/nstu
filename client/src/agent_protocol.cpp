#include "nstu/agent_protocol.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <type_traits>

namespace nstu::client {
namespace {

inline constexpr std::uint32_t kAgentMagic = 0x4350494eu; // "NIPC"
inline constexpr std::uint16_t kAgentVersion = 1;
inline constexpr std::size_t kAgentHeaderBytes = 12;

void set_error(std::string* error, const char* message) {
    if (error != nullptr) {
        *error = message;
    }
}

template <typename T>
void append_le(std::vector<std::byte>& output, T value) {
    static_assert(std::is_unsigned_v<T>);
    for (std::size_t index = 0; index < sizeof(T); ++index) {
        output.push_back(static_cast<std::byte>(value & 0xffu));
        value >>= 8u;
    }
}

template <typename T>
bool read_le(std::span<const std::byte> input, std::size_t& offset, T& value) {
    static_assert(std::is_unsigned_v<T>);
    if (offset + sizeof(T) > input.size()) {
        return false;
    }
    value = 0;
    for (std::size_t index = 0; index < sizeof(T); ++index) {
        value |= static_cast<T>(std::to_integer<unsigned int>(input[offset++]))
                 << (index * 8u);
    }
    return true;
}

bool valid_type(AgentMessageType type) noexcept {
    return type >= AgentMessageType::lock &&
           type <= AgentMessageType::exam_stop;
}

bool read_exact(const NamedPipe& pipe, std::span<std::byte> output,
                std::string* error) {
    std::size_t offset = 0;
    while (offset < output.size()) {
        const int read = pipe.read(output.subspan(offset), error);
        if (read <= 0) {
            return false;
        }
        offset += static_cast<std::size_t>(read);
    }
    return true;
}

} // namespace

std::vector<std::byte> encode_agent_message(const AgentMessage& message) {
    if (!valid_type(message.type) ||
        message.payload.size() > kMaximumAgentPayloadBytes) {
        return {};
    }
    std::vector<std::byte> wire;
    wire.reserve(kAgentHeaderBytes + message.payload.size());
    append_le(wire, kAgentMagic);
    append_le(wire, kAgentVersion);
    append_le(wire, static_cast<std::uint16_t>(message.type));
    append_le(wire, static_cast<std::uint32_t>(message.payload.size()));
    wire.insert(wire.end(), message.payload.begin(), message.payload.end());
    return wire;
}

std::optional<AgentMessage> decode_agent_message(
    std::span<const std::byte> wire) {
    if (wire.size() < kAgentHeaderBytes) {
        return std::nullopt;
    }
    std::size_t offset = 0;
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    std::uint16_t raw_type = 0;
    std::uint32_t payload_bytes = 0;
    if (!read_le(wire, offset, magic) || !read_le(wire, offset, version) ||
        !read_le(wire, offset, raw_type) ||
        !read_le(wire, offset, payload_bytes) || magic != kAgentMagic ||
        version != kAgentVersion || payload_bytes > kMaximumAgentPayloadBytes ||
        wire.size() - offset != payload_bytes) {
        return std::nullopt;
    }
    const auto type = static_cast<AgentMessageType>(raw_type);
    if (!valid_type(type)) {
        return std::nullopt;
    }
    AgentMessage message;
    message.type = type;
    message.payload.assign(wire.begin() + static_cast<std::ptrdiff_t>(offset),
                           wire.end());
    return message;
}

bool send_agent_message(const NamedPipe& pipe, const AgentMessage& message,
                        std::string* error) {
    const auto wire = encode_agent_message(message);
    if (wire.empty() || pipe.write(wire, error) != static_cast<int>(wire.size())) {
        set_error(error, "agent message write failed");
        return false;
    }
    return true;
}

std::optional<AgentMessage> receive_agent_message(const NamedPipe& pipe,
                                                  std::string* error) {
    std::vector<std::byte> wire(kAgentHeaderBytes);
    if (!read_exact(pipe, wire, error)) {
        return std::nullopt;
    }
    std::size_t offset = sizeof(std::uint32_t) + sizeof(std::uint16_t) * 2;
    std::uint32_t payload_bytes = 0;
    if (!read_le(std::span<const std::byte>(wire), offset, payload_bytes) ||
        payload_bytes > kMaximumAgentPayloadBytes) {
        set_error(error, "invalid agent message length");
        return std::nullopt;
    }
    wire.resize(kAgentHeaderBytes + payload_bytes);
    if (payload_bytes != 0 &&
        !read_exact(pipe,
                    std::span<std::byte>(wire).subspan(kAgentHeaderBytes),
                    error)) {
        return std::nullopt;
    }
    auto message = decode_agent_message(wire);
    if (!message) {
        set_error(error, "invalid agent message");
    }
    return message;
}

std::vector<std::byte> encode_agent_status(const AgentStatus& status) {
    std::vector<std::byte> payload;
    payload.reserve(11);
    append_le(payload, static_cast<std::uint8_t>(status.locked ? 1 : 0));
    append_le(payload, static_cast<std::uint8_t>(status.streaming ? 1 : 0));
    append_le(payload, static_cast<std::uint8_t>(status.snapshotting ? 1 : 0));
    append_le(payload,
              static_cast<std::uint8_t>(status.viewing_broadcast ? 1 : 0));
    append_le(payload, status.frames_per_second);
    append_le(payload, status.snapshot_interval_seconds);
    append_le(payload, status.session_id);
    return payload;
}

std::optional<AgentStatus> decode_agent_status(
    std::span<const std::byte> payload) {
    if (payload.size() != 11) {
        return std::nullopt;
    }
    std::size_t offset = 0;
    std::uint8_t locked = 0;
    std::uint8_t streaming = 0;
    std::uint8_t snapshotting = 0;
    std::uint8_t viewing_broadcast = 0;
    AgentStatus status;
    if (!read_le(payload, offset, locked) || locked > 1 ||
        !read_le(payload, offset, streaming) || streaming > 1 ||
        !read_le(payload, offset, snapshotting) || snapshotting > 1 ||
        !read_le(payload, offset, viewing_broadcast) ||
        viewing_broadcast > 1 ||
        !read_le(payload, offset, status.frames_per_second) ||
        !read_le(payload, offset, status.snapshot_interval_seconds) ||
        !read_le(payload, offset, status.session_id)) {
        return std::nullopt;
    }
    status.locked = locked != 0;
    status.streaming = streaming != 0;
    status.snapshotting = snapshotting != 0;
    status.viewing_broadcast = viewing_broadcast != 0;
    if ((!status.streaming && status.frames_per_second != 0) ||
        (status.streaming && (status.frames_per_second < 5 ||
                              status.frames_per_second > 15)) ||
        (!status.snapshotting && status.snapshot_interval_seconds != 0) ||
        (status.snapshotting &&
         (status.snapshot_interval_seconds < 5 ||
          status.snapshot_interval_seconds > 10))) {
        return std::nullopt;
    }
    return status;
}

std::vector<std::byte> encode_remote_input(
    const wire::RemoteInputPacket& packet) {
    if ((packet.input_type !=
             static_cast<std::uint8_t>(wire::RemoteInputType::mouse) &&
         packet.input_type !=
             static_cast<std::uint8_t>(wire::RemoteInputType::keyboard)) ||
        packet.reserved != 0) {
        return {};
    }
    const auto* bytes = reinterpret_cast<const std::byte*>(&packet);
    return {bytes, bytes + sizeof(packet)};
}

std::optional<wire::RemoteInputPacket> decode_remote_input(
    std::span<const std::byte> payload) {
    if (payload.size() != sizeof(wire::RemoteInputPacket)) {
        return std::nullopt;
    }
    wire::RemoteInputPacket packet{};
    std::memcpy(&packet, payload.data(), sizeof(packet));
    if ((packet.input_type !=
             static_cast<std::uint8_t>(wire::RemoteInputType::mouse) &&
         packet.input_type !=
             static_cast<std::uint8_t>(wire::RemoteInputType::keyboard)) ||
        packet.reserved != 0) {
        return std::nullopt;
    }
    return packet;
}

} // namespace nstu::client
