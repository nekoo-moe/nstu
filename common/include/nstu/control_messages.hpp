#pragma once

#include "nstu/auth.hpp"
#include "nstu/network.hpp"

#include <array>
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
// The ceiling is 720p so the teacher broadcast (a full desktop shown fullscreen
// on each client) stays legible; monitoring thumbnails choose a far smaller
// capture size at their call sites, so this bound does not grow the wall.
inline constexpr std::uint16_t kMaximumSnapshotWidth = 1280;
inline constexpr std::uint16_t kMaximumSnapshotHeight = 720;
inline constexpr std::size_t kMaximumSnapshotJpegBytes = 512u * 1024u;

enum class UwfConfigureOutcome : std::uint8_t {
    armed = 1,
    already_enabled = 2,
    unsupported_edition = 3,
    feature_missing = 4,
    probe_unavailable = 5,
    provider_unavailable = 6,
    reboot_pending = 7,
    invalid_data_root = 8,
    readiness_failed = 9,
    checkpoint_required = 10,
    access_denied = 11,
    failed = 12,
    busy = 13,
};

struct UwfConfigureReport {
    UwfConfigureOutcome outcome = UwfConfigureOutcome::failed;
    bool reboot_required = false;
    bool data_exclusion_ready = false;
    bool registry_exclusion_ready = false;
    std::string detail;
};

inline constexpr std::size_t kMaximumUwfDetailBytes = 256;

// Fleet reboot-to-restore.
//
// A UwfConfigureReport is produced *before* the restart, so at best it proves
// the client armed UWF for the next boot. Exam authorization needs the
// stronger claim - "this machine is protected in the session it is running
// right now" - which cannot be established without knowing that a restart
// actually happened. Boot identity is what supplies that.
//
// The client service generates a random boot id once at start and keeps it in
// memory only. It therefore necessarily changes across a restart, needs no
// persistence, and does not depend on the client's clock being trustworthy.
inline constexpr std::size_t kBootIdBytes = 16;
using BootId = std::array<std::byte, kBootIdBytes>;

enum class UwfFleetPhase : std::uint8_t {
    // Nothing is in flight for this client.
    idle = 1,
    // The client accepted the operation but has not started applying it.
    requested = 2,
    // Readiness checks and UWF arming are running.
    configuring = 3,
    // UWF is armed for the next boot; the restart has not been issued yet.
    awaiting_restart = 4,
    // The restart has been issued and the client expects to disappear.
    restarting = 5,
    // The client is back and is probing its current-session protection.
    verifying = 6,
    // A fresh probe on a *new* boot id confirmed current-session protection.
    // This is the only phase that authorizes an exam.
    verified_protected = 7,
    // The operation failed; `detail` says how.
    failed = 8,
    // This edition of Windows cannot run UWF at all.
    unsupported = 9,
};

// A read-only observation of what UWF is doing at this instant. Distinct from
// UwfConfigureReport, which describes an attempted mutation.
struct UwfProtectionProbe {
    // False means the probe could not be completed. Every other field is then
    // meaningless and must not be read as a negative observation.
    bool probe_succeeded = false;
    // Current-session state is proof; next-session state is only intent.
    bool filter_current_enabled = false;
    bool filter_next_enabled = false;
    bool system_volume_current_protected = false;
    bool data_exclusion_present = false;
    bool registry_exclusion_present = false;
};

// The client is protected for exam purposes only when a completed probe shows
// the filter enabled *and* the system volume protected in the current session.
// Exclusions are reported for the operator's benefit but are not part of the
// gate: a machine can be genuinely protected while an exclusion is missing,
// and that is a configuration warning rather than a protection failure.
[[nodiscard]] constexpr bool probe_proves_protection(
    const UwfProtectionProbe& probe) noexcept {
    return probe.probe_succeeded && probe.filter_current_enabled &&
           probe.system_volume_current_protected;
}

struct UwfFleetConfigureRequest {
    // Server-issued, non-zero. Binds every later status report to one
    // operator action so a late report from a previous operation cannot be
    // mistaken for the current one.
    std::uint64_t operation_id = 0;
    bool checkpoint_acknowledged = false;
    bool restart_requested = false;
};

struct UwfFleetStatusReport {
    std::uint64_t operation_id = 0;
    // The boot the client is running *now*, not the boot it was configured in.
    BootId boot_id{};
    UwfFleetPhase phase = UwfFleetPhase::idle;
    UwfProtectionProbe probe;
    std::string detail;
};

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

// Managed mode, one byte either way. `freeze_set` carries what the server
// wants; `freeze_report` carries what the client turned out to be.
[[nodiscard]] std::vector<std::byte> encode_freeze_state(bool frozen);
[[nodiscard]] std::optional<bool> decode_freeze_state(
    std::span<const std::byte> payload);

[[nodiscard]] std::vector<std::byte> encode_uwf_configure_request(
    bool checkpoint_acknowledged);
[[nodiscard]] std::optional<bool> decode_uwf_configure_request(
    std::span<const std::byte> payload);
[[nodiscard]] std::vector<std::byte> encode_uwf_configure_report(
    const UwfConfigureReport& report);
[[nodiscard]] std::optional<UwfConfigureReport> decode_uwf_configure_report(
    std::span<const std::byte> payload);

// Fleet variants. Deliberately separate from the codecs above rather than an
// extension of them: the older pair stays strict and unchanged, and a peer
// that only speaks it keeps working.
[[nodiscard]] std::vector<std::byte> encode_uwf_fleet_configure_request(
    const UwfFleetConfigureRequest& request);
[[nodiscard]] std::optional<UwfFleetConfigureRequest>
decode_uwf_fleet_configure_request(std::span<const std::byte> payload);
[[nodiscard]] std::vector<std::byte> encode_uwf_fleet_status_report(
    const UwfFleetStatusReport& report);
[[nodiscard]] std::optional<UwfFleetStatusReport>
decode_uwf_fleet_status_report(std::span<const std::byte> payload);

[[nodiscard]] std::vector<std::byte> encode_video_group_key(
    const VideoGroupKeyMessage& message);
[[nodiscard]] std::optional<VideoGroupKeyMessage> decode_video_group_key(
    std::span<const std::byte> payload);

} // namespace nstu::control
