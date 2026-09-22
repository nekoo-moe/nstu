#include "nstu/agent_protocol.hpp"

#include "nstu/pairing.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <string_view>
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
           type <= AgentMessageType::managed_state;
}

// Pairing text reaches the agent from the network and lands directly on a
// screen, so it is held to printable ASCII here rather than trusted to be
// harmless. The discovery beacon sanitizes names too; this is the side that
// has to be right even if it did not.
bool printable_text(std::string_view text, std::size_t maximum) noexcept {
    if (text.empty() || text.size() > maximum) {
        return false;
    }
    return std::all_of(text.begin(), text.end(), [](char character) {
        const auto value = static_cast<unsigned char>(character);
        return value >= 0x20 && value < 0x7f;
    });
}

void append_text(std::vector<std::byte>& output, std::string_view text) {
    append_le(output, static_cast<std::uint16_t>(text.size()));
    const auto* bytes = reinterpret_cast<const std::byte*>(text.data());
    output.insert(output.end(), bytes, bytes + text.size());
}

bool read_text(std::span<const std::byte> input, std::size_t& offset,
               std::string& text) {
    std::uint16_t length = 0;
    if (!read_le(input, offset, length) ||
        length > kMaximumPairingTextBytes ||
        offset + length > input.size()) {
        return false;
    }
    text.assign(reinterpret_cast<const char*>(input.data() + offset), length);
    offset += length;
    return true;
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

std::vector<std::byte> encode_agent_pairing_choices(
    std::span<const AgentPairingChoice> choices) {
    if (choices.empty() || choices.size() > kMaximumPairingChoices) {
        return {};
    }
    std::vector<std::byte> payload;
    append_le(payload, static_cast<std::uint8_t>(choices.size()));
    for (const auto& choice : choices) {
        if (!printable_text(choice.server_name, kMaximumPairingTextBytes) ||
            !printable_text(choice.address, kMaximumPairingTextBytes) ||
            choice.port == 0) {
            return {};
        }
        append_text(payload, choice.server_name);
        append_text(payload, choice.address);
        append_le(payload, choice.port);
    }
    return payload;
}

std::optional<std::vector<AgentPairingChoice>> decode_agent_pairing_choices(
    std::span<const std::byte> payload) {
    std::size_t offset = 0;
    std::uint8_t count = 0;
    if (!read_le(payload, offset, count) || count == 0 ||
        count > kMaximumPairingChoices) {
        return std::nullopt;
    }
    std::vector<AgentPairingChoice> choices;
    choices.reserve(count);
    for (std::uint8_t index = 0; index < count; ++index) {
        AgentPairingChoice choice;
        if (!read_text(payload, offset, choice.server_name) ||
            !read_text(payload, offset, choice.address) ||
            !read_le(payload, offset, choice.port) || choice.port == 0 ||
            !printable_text(choice.server_name, kMaximumPairingTextBytes) ||
            !printable_text(choice.address, kMaximumPairingTextBytes)) {
            return std::nullopt;
        }
        choices.push_back(std::move(choice));
    }
    return offset == payload.size() ? std::optional{std::move(choices)}
                                    : std::nullopt;
}

std::vector<std::byte> encode_agent_pairing_selection(
    std::uint16_t choice_index) {
    if (choice_index >= kMaximumPairingChoices) {
        return {};
    }
    std::vector<std::byte> payload;
    append_le(payload, choice_index);
    return payload;
}

std::optional<std::uint16_t> decode_agent_pairing_selection(
    std::span<const std::byte> payload) {
    std::size_t offset = 0;
    std::uint16_t choice_index = 0;
    if (payload.size() != sizeof(std::uint16_t) ||
        !read_le(payload, offset, choice_index) ||
        choice_index >= kMaximumPairingChoices) {
        return std::nullopt;
    }
    return choice_index;
}

std::vector<std::byte> encode_agent_pairing_code(const AgentPairingCode& code) {
    if (code.code.size() != pairing::kSasDigits ||
        !std::all_of(code.code.begin(), code.code.end(),
                     [](char digit) { return digit >= '0' && digit <= '9'; }) ||
        !printable_text(code.server_name, kMaximumPairingTextBytes)) {
        return {};
    }
    std::vector<std::byte> payload;
    append_text(payload, code.code);
    append_text(payload, code.server_name);
    return payload;
}

std::optional<AgentPairingCode> decode_agent_pairing_code(
    std::span<const std::byte> payload) {
    std::size_t offset = 0;
    AgentPairingCode code;
    if (!read_text(payload, offset, code.code) ||
        !read_text(payload, offset, code.server_name) ||
        offset != payload.size() ||
        code.code.size() != pairing::kSasDigits ||
        !std::all_of(code.code.begin(), code.code.end(),
                     [](char digit) { return digit >= '0' && digit <= '9'; }) ||
        !printable_text(code.server_name, kMaximumPairingTextBytes)) {
        return std::nullopt;
    }
    return code;
}

std::vector<std::byte> encode_agent_pairing_status(
    const AgentPairingStatus& status) {
    if (!printable_text(status.detail, kMaximumPairingTextBytes)) {
        return {};
    }
    std::vector<std::byte> payload;
    append_le(payload, status.outcome);
    append_text(payload, status.detail);
    return payload;
}

std::optional<AgentPairingStatus> decode_agent_pairing_status(
    std::span<const std::byte> payload) {
    std::size_t offset = 0;
    AgentPairingStatus status;
    if (!read_le(payload, offset, status.outcome) ||
        !read_text(payload, offset, status.detail) ||
        offset != payload.size() ||
        !printable_text(status.detail, kMaximumPairingTextBytes)) {
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
