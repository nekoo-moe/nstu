#pragma once

#include "nstu/wfp_allowlist.hpp"

#include <cstdint>

namespace nstu::client {

enum class LockdownAction { none, apply, clear };

// Decides WHEN the exam network block should be active; it never touches the
// firewall itself -- the caller maps apply/clear onto nstu::net::
// WfpWebsiteAllowlist. The whole point is fail-safe teardown: the block clears
// on an explicit stop, but ALSO once a hard safety cap elapses or heartbeats
// stop arriving, so a hung or wedged exam can never leave a student machine
// cut off. Pure and clock-injected, so it unit-tests without admin, WFP, or a
// VM; the real firewall and the heartbeat source are wired in on the client.
class ExamNetworkLockdown {
public:
    // max_active_ms: the longest the block may stay up even if no stop ever
    //   arrives. stale_after_ms: clear the block when no heartbeat has arrived
    //   within this window (the exam process is wedged). Both are safety nets,
    //   not the normal teardown path.
    ExamNetworkLockdown(std::uint64_t max_active_ms,
                        std::uint64_t stale_after_ms) noexcept
        : max_active_ms_(max_active_ms), stale_after_ms_(stale_after_ms) {}

    // Begin blocking, allowing only `config`. Records the start instant and the
    // first heartbeat at now_ms.
    void arm(const net::WfpAllowlistConfig& config, std::uint64_t now_ms) {
        config_ = config;
        desired_ = true;
        start_ms_ = now_ms;
        last_heartbeat_ms_ = now_ms;
    }

    // The exam is still alive; refresh the staleness timer.
    void heartbeat(std::uint64_t now_ms) noexcept {
        last_heartbeat_ms_ = now_ms;
    }

    // Request teardown (the normal stop path).
    void disarm() noexcept { desired_ = false; }

    // The action to take now, updating the tracked applied state. An apply
    // carries the allowlist via config(). Idempotent: once applied it returns
    // none until the desired state actually changes.
    LockdownAction poll(std::uint64_t now_ms) {
        const bool want = should_block(now_ms);
        if (want && !applied_) {
            applied_ = true;
            return LockdownAction::apply;
        }
        if (!want && applied_) {
            applied_ = false;
            return LockdownAction::clear;
        }
        return LockdownAction::none;
    }

    [[nodiscard]] bool applied() const noexcept { return applied_; }
    [[nodiscard]] const net::WfpAllowlistConfig& config() const noexcept {
        return config_;
    }

private:
    [[nodiscard]] bool should_block(std::uint64_t now_ms) const noexcept {
        if (!desired_) {
            return false;
        }
        // A clock that went backwards must never extend the block, so treat a
        // now_ms before the recorded instant as zero elapsed.
        const std::uint64_t active_for =
            now_ms >= start_ms_ ? now_ms - start_ms_ : 0;
        const std::uint64_t since_heartbeat =
            now_ms >= last_heartbeat_ms_ ? now_ms - last_heartbeat_ms_ : 0;
        return active_for <= max_active_ms_ &&
               since_heartbeat <= stale_after_ms_;
    }

    net::WfpAllowlistConfig config_{};
    std::uint64_t max_active_ms_ = 0;
    std::uint64_t stale_after_ms_ = 0;
    std::uint64_t start_ms_ = 0;
    std::uint64_t last_heartbeat_ms_ = 0;
    bool desired_ = false;
    bool applied_ = false;
};

} // namespace nstu::client
