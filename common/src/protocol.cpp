#include "nstu/protocol.hpp"

#include <algorithm>
#include <cstring>
#include <utility>
#include <type_traits>

namespace nstu::protocol {
namespace {

template <typename T>
void write_le(std::span<std::byte> output, std::size_t& offset, T value) {
    static_assert(std::is_unsigned_v<T>);
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        output[offset++] = static_cast<std::byte>(value & 0xffu);
        value >>= 8u;
    }
}

template <typename T>
[[nodiscard]] bool read_le(std::span<const std::byte> input,
                           std::size_t& offset, T& value) {
    static_assert(std::is_unsigned_v<T>);
    if (offset + sizeof(T) > input.size()) {
        return false;
    }

    value = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        value |= static_cast<T>(std::to_integer<unsigned int>(input[offset++]))
                 << (8u * i);
    }
    return true;
}

} // namespace

std::vector<std::byte> encode_command_header(const CommandEnvelope& envelope) {
    std::vector<std::byte> wire(kCommandHeaderBytes);
    std::size_t offset = 0;
    write_le<std::uint32_t>(wire, offset, kMagic);
    write_le<std::uint16_t>(wire, offset, envelope.version);
    write_le<std::uint16_t>(wire, offset,
                            static_cast<std::uint16_t>(envelope.type));
    write_le<std::uint32_t>(wire, offset, envelope.payload_bytes);
    write_le<std::uint64_t>(wire, offset, envelope.request_id);
    return wire;
}

std::optional<CommandEnvelope> decode_command_header(
    std::span<const std::byte> wire) {
    if (wire.size() < kCommandHeaderBytes) {
        return std::nullopt;
    }

    std::size_t offset = 0;
    std::uint32_t magic = 0;
    CommandEnvelope envelope;
    std::uint16_t type = 0;
    if (!read_le(wire, offset, magic) || magic != kMagic ||
        !read_le(wire, offset, envelope.version) ||
        !read_le(wire, offset, type) ||
        !read_le(wire, offset, envelope.payload_bytes) ||
        !read_le(wire, offset, envelope.request_id) ||
        envelope.version != kVersion) {
        return std::nullopt;
    }
    envelope.type = static_cast<CommandType>(type);
    return envelope;
}

std::vector<std::byte> encode_connection_preamble(
    const ConnectionPreamble& preamble) {
    if (preamble.preamble_bytes != kConnectionPreambleBytes ||
        preamble.reserved != 0 ||
        (preamble.role != ConnectionRole::client &&
         preamble.role != ConnectionRole::server &&
         preamble.role != ConnectionRole::diagnostic)) {
        return {};
    }
    std::vector<std::byte> wire(kConnectionPreambleBytes);
    std::size_t offset = 0;
    write_le<std::uint32_t>(wire, offset, kMagic);
    write_le<std::uint16_t>(wire, offset, preamble.version);
    wire[offset++] = static_cast<std::byte>(preamble.role);
    wire[offset++] = static_cast<std::byte>(preamble.flags);
    write_le<std::uint16_t>(wire, offset, preamble.preamble_bytes);
    write_le<std::uint16_t>(wire, offset, preamble.reserved);
    write_le<std::uint32_t>(wire, offset, preamble.key_id);
    std::copy(preamble.identity.begin(), preamble.identity.end(),
              wire.begin() + static_cast<std::ptrdiff_t>(offset));
    return wire;
}

std::optional<ConnectionPreamble> decode_connection_preamble(
    std::span<const std::byte> wire) {
    if (wire.size() != kConnectionPreambleBytes) {
        return std::nullopt;
    }
    std::size_t offset = 0;
    std::uint32_t magic = 0;
    ConnectionPreamble preamble;
    std::uint8_t role = 0;
    if (!read_le(wire, offset, magic) || magic != kMagic ||
        !read_le(wire, offset, preamble.version) ||
        offset + 2 > wire.size()) {
        return std::nullopt;
    }
    role = std::to_integer<std::uint8_t>(wire[offset++]);
    preamble.role = static_cast<ConnectionRole>(role);
    preamble.flags = std::to_integer<std::uint8_t>(wire[offset++]);
    if (!read_le(wire, offset, preamble.preamble_bytes) ||
        !read_le(wire, offset, preamble.reserved) ||
        !read_le(wire, offset, preamble.key_id) ||
        offset + preamble.identity.size() != wire.size() ||
        preamble.version != kCommandVersion ||
        preamble.preamble_bytes != kConnectionPreambleBytes ||
        preamble.reserved != 0 ||
        (preamble.role != ConnectionRole::client &&
         preamble.role != ConnectionRole::server &&
         preamble.role != ConnectionRole::diagnostic)) {
        return std::nullopt;
    }
    std::copy_n(wire.begin() + static_cast<std::ptrdiff_t>(offset),
                preamble.identity.size(), preamble.identity.begin());
    const bool identity_present = std::any_of(
        preamble.identity.begin(), preamble.identity.end(),
        [](std::byte value) { return value != std::byte{0}; });
    if (preamble.role == ConnectionRole::client &&
        (!identity_present || preamble.key_id == 0)) {
        return std::nullopt;
    }
    if ((preamble.role == ConnectionRole::server ||
         preamble.role == ConnectionRole::diagnostic) &&
        (identity_present || preamble.key_id != 0)) {
        return std::nullopt;
    }
    return preamble;
}

std::vector<std::byte> encode_video_header(const VideoPacketHeader& header) {
    std::vector<std::byte> wire(kVideoHeaderBytes);
    std::size_t offset = 0;
    write_le<std::uint32_t>(wire, offset, kMagic);
    write_le<std::uint16_t>(wire, offset, header.version);
    write_le<std::uint16_t>(wire, offset,
                            static_cast<std::uint16_t>(header.flags));
    write_le<std::uint32_t>(wire, offset, header.stream_id);
    write_le<std::uint64_t>(wire, offset, header.packet_sequence);
    write_le<std::uint64_t>(wire, offset, header.frame_id);
    write_le<std::uint16_t>(wire, offset, header.fragment_index);
    write_le<std::uint16_t>(wire, offset, header.fragment_count);
    write_le<std::uint32_t>(wire, offset, header.payload_bytes);
    write_le<std::uint64_t>(wire, offset, header.capture_time_100ns);
    return wire;
}

std::optional<VideoPacketHeader> decode_video_header(
    std::span<const std::byte> wire) {
    if (wire.size() < kVideoHeaderBytes) {
        return std::nullopt;
    }

    std::size_t offset = 0;
    std::uint32_t magic = 0;
    std::uint16_t flags = 0;
    VideoPacketHeader header;
    if (!read_le(wire, offset, magic) || magic != kMagic ||
        !read_le(wire, offset, header.version) ||
        !read_le(wire, offset, flags) ||
        !read_le(wire, offset, header.stream_id) ||
        !read_le(wire, offset, header.packet_sequence) ||
        !read_le(wire, offset, header.frame_id) ||
        !read_le(wire, offset, header.fragment_index) ||
        !read_le(wire, offset, header.fragment_count) ||
        !read_le(wire, offset, header.payload_bytes) ||
        !read_le(wire, offset, header.capture_time_100ns) ||
        header.version != kVideoVersion || header.fragment_count == 0 ||
        header.fragment_index >= header.fragment_count) {
        return std::nullopt;
    }
    header.flags = static_cast<VideoFlags>(flags);
    return header;
}

std::vector<std::byte> encode_tcp_frame(
    const CommandEnvelope& envelope, std::span<const std::byte> payload) {
    if (payload.size() > kMaxCommandPayload ||
        payload.size() != envelope.payload_bytes) {
        return {};
    }

    const auto header = encode_command_header(envelope);
    const auto body_bytes = header.size() + payload.size();
    std::vector<std::byte> wire(sizeof(std::uint32_t) + body_bytes);
    std::size_t offset = 0;
    write_le<std::uint32_t>(wire, offset,
                            static_cast<std::uint32_t>(body_bytes));
    std::memcpy(wire.data() + offset, header.data(), header.size());
    offset += header.size();
    if (!payload.empty()) {
        std::memcpy(wire.data() + offset, payload.data(), payload.size());
    }
    return wire;
}

bool TcpFrameParser::feed(std::span<const std::byte> bytes,
                          std::vector<TcpFrame>& frames) {
    if (bytes.size() > kMaxTcpBufferedBytes -
                          std::min(buffer_.size(), kMaxTcpBufferedBytes)) {
        reset();
        return false;
    }
    if (!bytes.empty()) {
        buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
    }

    while (true) {
        if (buffer_.size() < sizeof(std::uint32_t)) {
            return true;
        }

        std::size_t offset = 0;
        std::uint32_t body_bytes = 0;
        if (!read_le(std::span<const std::byte>(buffer_), offset, body_bytes) ||
            body_bytes < kCommandHeaderBytes ||
            body_bytes > kCommandHeaderBytes + kMaxCommandPayload) {
            reset();
            return false;
        }

        const auto total_bytes = sizeof(std::uint32_t) + body_bytes;
        if (buffer_.size() < total_bytes) {
            return true;
        }

        const auto body = std::span<const std::byte>(buffer_).subspan(
            sizeof(std::uint32_t), body_bytes);
        const auto envelope = decode_command_header(body);
        if (!envelope || envelope->payload_bytes !=
                             body_bytes - kCommandHeaderBytes) {
            reset();
            return false;
        }

        TcpFrame frame{*envelope, {}};
        const auto payload = body.subspan(kCommandHeaderBytes);
        frame.payload.assign(payload.begin(), payload.end());
        frames.push_back(std::move(frame));
        buffer_.erase(buffer_.begin(), buffer_.begin() + total_bytes);
    }
}

void TcpFrameParser::reset() noexcept {
    buffer_.clear();
}

} // namespace nstu::protocol
