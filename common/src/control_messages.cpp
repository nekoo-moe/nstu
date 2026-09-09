#include "nstu/control_messages.hpp"

#include <algorithm>
#include <type_traits>

namespace nstu::control {
namespace {

inline constexpr std::uint16_t kStatusVersion = 2;
inline constexpr std::uint16_t kLockedFlag = 1;
inline constexpr std::uint16_t kStreamingFlag = 2;
inline constexpr std::uint16_t kSnapshottingFlag = 4;
inline constexpr std::uint16_t kViewingBroadcastFlag = 8;
inline constexpr std::uint16_t kSnapshotFrameVersion = 1;
inline constexpr std::size_t kMaximumHostnameBytes = 255;

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

} // namespace

std::vector<std::byte> encode_status_report(
    const ClientStatusReport& report) {
    if (report.hostname.empty() ||
        report.hostname.size() > kMaximumHostnameBytes ||
        report.packet_loss_per_mille > 1000 ||
        (report.streaming && (report.frames_per_second < 5 ||
                              report.frames_per_second > 15)) ||
        (!report.streaming && report.frames_per_second != 0) ||
        (report.snapshotting &&
         (report.snapshot_interval_seconds < kMinimumSnapshotIntervalSeconds ||
          report.snapshot_interval_seconds > kMaximumSnapshotIntervalSeconds)) ||
        (!report.snapshotting && report.snapshot_interval_seconds != 0)) {
        return {};
    }
    std::vector<std::byte> payload;
    payload.reserve(31 + report.hostname.size());
    append_le(payload, kStatusVersion);
    std::uint16_t flags = report.locked ? kLockedFlag : 0;
    if (report.streaming) {
        flags |= kStreamingFlag;
    }
    if (report.snapshotting) {
        flags |= kSnapshottingFlag;
    }
    if (report.viewing_broadcast) {
        flags |= kViewingBroadcastFlag;
    }
    append_le(payload, flags);
    append_le(payload, report.session_id);
    append_le(payload, report.latency_ms);
    append_le(payload, report.packet_loss_per_mille);
    append_le(payload, report.packet_loss_sample_size);
    append_le(payload, static_cast<std::uint8_t>(report.delivery));
    append_le(payload, report.frames_per_second);
    append_le(payload, report.snapshot_interval_seconds);
    append_le(payload, static_cast<std::uint16_t>(report.hostname.size()));
    payload.insert(payload.end(),
                   reinterpret_cast<const std::byte*>(report.hostname.data()),
                   reinterpret_cast<const std::byte*>(report.hostname.data()) +
                       report.hostname.size());
    return payload;
}

std::optional<ClientStatusReport> decode_status_report(
    std::span<const std::byte> payload) {
    std::size_t offset = 0;
    std::uint16_t version = 0;
    std::uint16_t flags = 0;
    std::uint8_t delivery = 0;
    std::uint16_t hostname_bytes = 0;
    ClientStatusReport report;
    if (!read_le(payload, offset, version) ||
        (version != 1 && version != kStatusVersion) ||
        !read_le(payload, offset, flags) ||
        (flags & ~(kLockedFlag | kStreamingFlag | kSnapshottingFlag |
                   kViewingBroadcastFlag)) != 0 ||
        !read_le(payload, offset, report.session_id) ||
        !read_le(payload, offset, report.latency_ms) ||
        !read_le(payload, offset, report.packet_loss_per_mille) ||
        report.packet_loss_per_mille > 1000 ||
        !read_le(payload, offset, report.packet_loss_sample_size) ||
        !read_le(payload, offset, delivery) || delivery > 1 ||
        (version == kStatusVersion &&
         (!read_le(payload, offset, report.frames_per_second) ||
          !read_le(payload, offset, report.snapshot_interval_seconds))) ||
        !read_le(payload, offset, hostname_bytes) || hostname_bytes == 0 ||
        hostname_bytes > kMaximumHostnameBytes ||
        payload.size() - offset != hostname_bytes) {
        return std::nullopt;
    }
    report.locked = (flags & kLockedFlag) != 0;
    report.streaming = (flags & kStreamingFlag) != 0;
    report.snapshotting = (flags & kSnapshottingFlag) != 0;
    report.viewing_broadcast = (flags & kViewingBroadcastFlag) != 0;
    if ((report.streaming && (report.frames_per_second < 5 ||
                              report.frames_per_second > 15)) ||
        (!report.streaming && report.frames_per_second != 0) ||
        (report.snapshotting &&
         (report.snapshot_interval_seconds < kMinimumSnapshotIntervalSeconds ||
          report.snapshot_interval_seconds > kMaximumSnapshotIntervalSeconds)) ||
        (!report.snapshotting && report.snapshot_interval_seconds != 0)) {
        return std::nullopt;
    }
    report.delivery = static_cast<net::VideoDeliveryMode>(delivery);
    report.hostname.assign(
        reinterpret_cast<const char*>(payload.data() + offset), hostname_bytes);
    return report;
}

std::vector<std::byte> encode_start_stream_request(
    std::uint8_t frames_per_second) {
    if (frames_per_second < 5 || frames_per_second > 15) {
        return {};
    }
    return {static_cast<std::byte>(frames_per_second)};
}

std::optional<std::uint8_t> decode_start_stream_request(
    std::span<const std::byte> payload) {
    if (payload.size() != 1) {
        return std::nullopt;
    }
    const auto fps = std::to_integer<std::uint8_t>(payload[0]);
    return fps >= 5 && fps <= 15 ? std::optional{fps} : std::nullopt;
}

std::vector<std::byte> encode_snapshot_schedule(
    std::uint16_t interval_seconds) {
    if (interval_seconds < kMinimumSnapshotIntervalSeconds ||
        interval_seconds > kMaximumSnapshotIntervalSeconds) {
        return {};
    }
    std::vector<std::byte> payload;
    append_le(payload, interval_seconds);
    return payload;
}

std::optional<std::uint16_t> decode_snapshot_schedule(
    std::span<const std::byte> payload) {
    std::size_t offset = 0;
    std::uint16_t interval_seconds = 0;
    if (!read_le(payload, offset, interval_seconds) ||
        offset != payload.size() ||
        interval_seconds < kMinimumSnapshotIntervalSeconds ||
        interval_seconds > kMaximumSnapshotIntervalSeconds) {
        return std::nullopt;
    }
    return interval_seconds;
}

std::vector<std::byte> encode_snapshot_frame(const SnapshotFrame& frame) {
    if (frame.width == 0 || frame.height == 0 ||
        frame.width > kMaximumSnapshotWidth ||
        frame.height > kMaximumSnapshotHeight ||
        frame.captured_at_unix_milliseconds == 0 || frame.jpeg.empty() ||
        frame.jpeg.size() > kMaximumSnapshotJpegBytes) {
        return {};
    }
    std::vector<std::byte> payload;
    payload.reserve(20 + frame.jpeg.size());
    append_le(payload, kSnapshotFrameVersion);
    append_le(payload, frame.width);
    append_le(payload, frame.height);
    append_le(payload, static_cast<std::uint16_t>(0));
    append_le(payload, frame.captured_at_unix_milliseconds);
    append_le(payload, static_cast<std::uint32_t>(frame.jpeg.size()));
    payload.insert(payload.end(), frame.jpeg.begin(), frame.jpeg.end());
    return payload;
}

std::optional<SnapshotFrame> decode_snapshot_frame(
    std::span<const std::byte> payload) {
    SnapshotFrame frame;
    std::size_t offset = 0;
    std::uint16_t version = 0;
    std::uint16_t reserved = 0;
    std::uint32_t jpeg_bytes = 0;
    if (!read_le(payload, offset, version) ||
        version != kSnapshotFrameVersion ||
        !read_le(payload, offset, frame.width) || frame.width == 0 ||
        frame.width > kMaximumSnapshotWidth ||
        !read_le(payload, offset, frame.height) || frame.height == 0 ||
        frame.height > kMaximumSnapshotHeight ||
        !read_le(payload, offset, reserved) || reserved != 0 ||
        !read_le(payload, offset, frame.captured_at_unix_milliseconds) ||
        frame.captured_at_unix_milliseconds == 0 ||
        !read_le(payload, offset, jpeg_bytes) || jpeg_bytes == 0 ||
        jpeg_bytes > kMaximumSnapshotJpegBytes ||
        payload.size() - offset != jpeg_bytes) {
        return std::nullopt;
    }
    frame.jpeg.assign(payload.begin() + static_cast<std::ptrdiff_t>(offset),
                      payload.end());
    return frame;
}

std::vector<std::byte> encode_overlay_stroke(const OverlayStroke& stroke) {
    if (stroke.thickness == 0 || stroke.thickness > 32 ||
        (stroke.rgba & 0xffu) == 0) {
        return {};
    }
    std::vector<std::byte> payload;
    payload.reserve(14);
    append_le(payload, stroke.x0);
    append_le(payload, stroke.y0);
    append_le(payload, stroke.x1);
    append_le(payload, stroke.y1);
    append_le(payload, stroke.thickness);
    append_le(payload, stroke.rgba);
    return payload;
}

std::optional<OverlayStroke> decode_overlay_stroke(
    std::span<const std::byte> payload) {
    OverlayStroke stroke;
    std::size_t offset = 0;
    if (!read_le(payload, offset, stroke.x0) ||
        !read_le(payload, offset, stroke.y0) ||
        !read_le(payload, offset, stroke.x1) ||
        !read_le(payload, offset, stroke.y1) ||
        !read_le(payload, offset, stroke.thickness) ||
        !read_le(payload, offset, stroke.rgba) || offset != payload.size() ||
        stroke.thickness == 0 || stroke.thickness > 32 ||
        (stroke.rgba & 0xffu) == 0) {
        return std::nullopt;
    }
    return stroke;
}

std::vector<std::byte> encode_video_group_key(
    const VideoGroupKeyMessage& message) {
    if (message.stream_id == 0) {
        return {};
    }
    std::vector<std::byte> payload;
    payload.reserve(sizeof(message.stream_id) +
                    sizeof(message.first_packet_sequence) + message.key.size());
    append_le(payload, message.stream_id);
    append_le(payload, message.first_packet_sequence);
    payload.insert(payload.end(), message.key.begin(), message.key.end());
    return payload;
}

std::optional<VideoGroupKeyMessage> decode_video_group_key(
    std::span<const std::byte> payload) {
    if (payload.size() != sizeof(std::uint32_t) + sizeof(std::uint64_t) +
                              security::kSha256Bytes) {
        return std::nullopt;
    }
    VideoGroupKeyMessage message;
    std::size_t offset = 0;
    if (!read_le(payload, offset, message.stream_id) ||
        message.stream_id == 0 ||
        !read_le(payload, offset, message.first_packet_sequence)) {
        return std::nullopt;
    }
    std::copy_n(payload.begin() + static_cast<std::ptrdiff_t>(offset),
                message.key.size(), message.key.begin());
    return message;
}

} // namespace nstu::control
