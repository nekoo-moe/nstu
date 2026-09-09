#pragma once

#include "nstu/auth.hpp"
#include "nstu/network.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace nstu::control {

struct ClientStatusReport {
    std::string hostname;
    bool locked = false;
    bool streaming = false;
    bool snapshotting = false;
    bool viewing_broadcast = false;
    std::uint8_t frames_per_second = 0;
    std::uint16_t snapshot_interval_seconds = 0;
    std::uint32_t session_id = 0;
    std::uint32_t latency_ms = 0;
    std::uint32_t packet_loss_per_mille = 0;
    std::uint64_t packet_loss_sample_size = 0;
    net::VideoDeliveryMode delivery = net::VideoDeliveryMode::multicast;
};

struct SnapshotFrame {
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::uint64_t captured_at_unix_milliseconds = 0;
    std::vector<std::byte> jpeg;
};

struct OverlayStroke {
    std::uint16_t x0 = 0;
    std::uint16_t y0 = 0;
    std::uint16_t x1 = 0;
    std::uint16_t y1 = 0;
    std::uint16_t thickness = 0;
    std::uint32_t rgba = 0;
};

inline constexpr std::uint16_t kMinimumSnapshotIntervalSeconds = 5;
inline constexpr std::uint16_t kMaximumSnapshotIntervalSeconds = 10;
// Snapshot dimensions are part of the application contract.  Keeping this
// bound at the wire boundary prevents a tiny compressed payload from forcing
// an unexpectedly large decode/texture allocation on the teacher machine.
inline constexpr std::uint16_t kMaximumSnapshotWidth = 480;
inline constexpr std::uint16_t kMaximumSnapshotHeight = 270;
inline constexpr std::size_t kMaximumSnapshotJpegBytes = 60u * 1024u;

struct VideoGroupKeyMessage {
    std::uint32_t stream_id = 0;
    std::uint64_t first_packet_sequence = 0;
    security::Sha256Digest key{};
};

[[nodiscard]] std::vector<std::byte> encode_status_report(
    const ClientStatusReport& report);
[[nodiscard]] std::optional<ClientStatusReport> decode_status_report(
    std::span<const std::byte> payload);

[[nodiscard]] std::vector<std::byte> encode_start_stream_request(
    std::uint8_t frames_per_second);
[[nodiscard]] std::optional<std::uint8_t> decode_start_stream_request(
    std::span<const std::byte> payload);

[[nodiscard]] std::vector<std::byte> encode_snapshot_schedule(
    std::uint16_t interval_seconds);
[[nodiscard]] std::optional<std::uint16_t> decode_snapshot_schedule(
    std::span<const std::byte> payload);

[[nodiscard]] std::vector<std::byte> encode_snapshot_frame(
    const SnapshotFrame& frame);
[[nodiscard]] std::optional<SnapshotFrame> decode_snapshot_frame(
    std::span<const std::byte> payload);

[[nodiscard]] std::vector<std::byte> encode_overlay_stroke(
    const OverlayStroke& stroke);
[[nodiscard]] std::optional<OverlayStroke> decode_overlay_stroke(
    std::span<const std::byte> payload);

[[nodiscard]] std::vector<std::byte> encode_video_group_key(
    const VideoGroupKeyMessage& message);
[[nodiscard]] std::optional<VideoGroupKeyMessage> decode_video_group_key(
    std::span<const std::byte> payload);

} // namespace nstu::control
