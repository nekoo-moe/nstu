#pragma once

#include "nstu/exam_control.hpp"
#include "nstu/exam_network_lockdown.hpp"
#include "nstu/wfp_allowlist.hpp"

#include <cstdint>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nstu::client {

// Manifest duration plus staging / sync-barrier slack (30 minutes).
inline constexpr std::uint64_t kSlackTimeMs = 30ULL * 60ULL * 1000ULL;

// Absolute safety cap: the firewall block never survives longer than 25 hours.
inline constexpr std::uint64_t kMaxActiveCapMs = 25ULL * 3600ULL * 1000ULL;

// Agent emits exam_alive every 10s; clear block if no heartbeat within 90s.
inline constexpr std::uint64_t kDefaultStaleAfterMs = 90'000ULL;

// Bounded retries for apply_fn before continuing the exam unblocked.
inline constexpr unsigned int kMaxApplyRetries = 5;

// Parse a bare hostname or an http(s):// URL into a bare hostname.
// Rejects userinfo, unsupported schemes, invalid labels, or length > 253.
[[nodiscard]] bool parse_origin_hostname(
    std::string_view origin, std::string& hostname);

// Checks if an IPv4 address (in host byte order) is loopback, link-local,
// unspecified, multicast, reserved, or broadcast.
[[nodiscard]] bool is_disallowed_ipv4(std::uint32_t ip) noexcept;

using OriginResolveFn =
    std::function<std::vector<std::uint32_t>(const std::string& hostname)>;

// Separately testable origin resolver with injectable DNS lookup seam.
// Returns an empty list if nothing resolved or on over-count (> 32 origins).
[[nodiscard]] std::vector<std::uint32_t> resolve_allowed_origins(
    std::span<const std::string> origins,
    OriginResolveFn resolve_fn = nullptr);

struct ExamLockdownArmParams {
    exam::SessionId session_id{};
    security::ClientId client_id{};
    bool package_verified = false;
    std::uint32_t duration_seconds = 0;
    std::vector<std::uint32_t> allowed_ipv4;
};

// ExamLockdownController manages the lifecycle of the Windows Filtering Platform
// website allowlist during an active exam.
//
// Thread safety:
//   - arm() is called from the background worker spawned by command dispatch.
//   - heartbeat() is called on agent named pipe message receipt.
//   - disarm() is called on exam stop, disconnect, or service shutdown.
//   - tick() is called periodically from the 2-second supervisor loop.
// All operations are serialized behind an internal std::mutex.
//
// Known limitations (NOT safety claims):
//   - WFP installs only ALE_AUTH_CONNECT_V4; IPv6 egress is not blocked.
//   - Only ports 80/443 are blocked, with per-IP permits; not default-deny.
//   - Origins resolve to IPv4 at arm time; rotating CDNs may become unreachable.
//   - Terminating nstu-agent.exe stops heartbeats, so the block lifts after 90s.
//   - WfpWebsiteAllowlist::clear() returns true even if individual filter deletes fail.
//   - WebView2 resource filters only permit local package assets; an allowed WFP
//     origin does not automatically allow arbitrary cross-origin page loads in kiosk.
class ExamLockdownController {
public:
    using ApplyFn =
        std::function<bool(const net::WfpAllowlistConfig&, std::string*)>;
    using ClearFn = std::function<bool(std::string*)>;

    ExamLockdownController(ApplyFn apply_fn, ClearFn clear_fn);
    ~ExamLockdownController() = default;

    ExamLockdownController(const ExamLockdownController&) = delete;
    ExamLockdownController& operator=(const ExamLockdownController&) = delete;

    // Issue a monotonic epoch token for an arm attempt.
    [[nodiscard]] std::uint64_t begin_attempt() noexcept;

    // Arm the controller. Refuses unless all guards hold (matching token,
    // non-zero session, matching identity, verified package, non-empty IPv4 set).
    [[nodiscard]] bool arm(
        std::uint64_t token,
        const ExamLockdownArmParams& params,
        const security::ClientId& configured_client_id,
        std::uint64_t now_ms,
        std::string* error = nullptr);

    // Update duration if exam_begin carries an authoritative duration override.
    void update_duration(std::uint32_t duration_seconds) noexcept;

    // Refresh staleness watchdog; no-op if not currently armed.
    void heartbeat(std::uint64_t now_ms) noexcept;

    // Disarm immediately: sets desired to false, bumps epoch, and attempts clear.
    void disarm() noexcept;

    // Reconciliation tick: called periodically from supervisor loop.
    LockdownAction tick(std::uint64_t now_ms);

    // Unconditional startup or uninstall sweep to remove leftover filters.
    bool startup_sweep(std::string* error = nullptr);

    // Inspection queries for unit tests.
    [[nodiscard]] bool is_armed() const noexcept;
    [[nodiscard]] bool is_actually_blocked() const noexcept;
    [[nodiscard]] std::uint64_t current_epoch() const noexcept;
    [[nodiscard]] unsigned int apply_retries() const noexcept;
    [[nodiscard]] bool last_apply_failed() const noexcept;

private:
    ApplyFn apply_fn_;
    ClearFn clear_fn_;
    mutable std::mutex mutex_;
    std::uint64_t current_epoch_ = 0;
    bool armed_ = false;
    bool actually_blocked_ = false;
    unsigned int apply_retries_ = 0;
    bool last_apply_failed_ = false;
    std::uint64_t start_ms_ = 0;
    std::uint64_t last_heartbeat_ms_ = 0;
    std::uint32_t duration_seconds_ = 0;
    ExamNetworkLockdown lockdown_{kMaxActiveCapMs, kDefaultStaleAfterMs};
    std::string last_error_;
};

} // namespace nstu::client
