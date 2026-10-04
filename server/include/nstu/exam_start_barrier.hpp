#pragma once

#include <cstddef>
#include <cstdint>
#include <set>
#include <vector>

namespace nstu::server {

// Coordinates a synchronized exam start across a cohort of clients. Each client
// reports ready once its package is staged and verified and the kiosk is up
// showing the loading screen; the barrier releases the cohort to begin
// together -- as soon as everyone is ready, or once a quorum is ready and a
// grace period has elapsed, so one stuck machine cannot hold the whole room.
// Clients that become ready after the cohort has begun are released at once
// (they join late). This is pure logic: the caller owns the clock (a monotonic
// millisecond counter) and performs the actual exam_begin sends.
//
// Id is how the caller keys clients (e.g. the client registry id). It only has
// to be ordered, so it can be used as a std::set key.
template <typename Id>
class ExamStartBarrier {
public:
    ExamStartBarrier(const std::vector<Id>& cohort, std::size_t quorum,
                     std::uint64_t grace_period_ms, std::uint64_t start_time_ms)
        : cohort_(cohort.begin(), cohort.end()),
          grace_period_ms_(grace_period_ms),
          start_time_ms_(start_time_ms) {
        // A quorum below 1 or above the cohort size is meaningless; clamping to
        // [1, size] also makes "wait for everyone" the natural default
        // (quorum == size) and keeps a degenerate empty cohort from beginning.
        const std::size_t size = cohort_.size();
        quorum_ = size == 0 ? 1
                            : (quorum < 1 ? 1 : (quorum > size ? size : quorum));
    }

    // Record that a cohort member is ready. Returns true only if the client is
    // in the cohort and was not already ready, so a repeated or spoofed report
    // changes nothing.
    bool mark_ready(const Id& client) {
        if (cohort_.find(client) == cohort_.end()) {
            return false;
        }
        return ready_.insert(client).second;
    }

    // The clients that should receive exam_begin now and have not been released
    // yet. Empty until the begin condition first holds; then every ready
    // client; afterwards, each client that became ready since the last poll.
    std::vector<Id> poll(std::uint64_t now_ms) {
        if (!begun_) {
            if (!should_begin(now_ms)) {
                return {};
            }
            begun_ = true;
        }
        std::vector<Id> releasable;
        for (const Id& client : ready_) {
            if (released_.insert(client).second) {
                releasable.push_back(client);
            }
        }
        return releasable;
    }

    [[nodiscard]] bool begun() const noexcept { return begun_; }
    [[nodiscard]] std::size_t cohort_size() const noexcept {
        return cohort_.size();
    }
    [[nodiscard]] std::size_t ready_count() const noexcept {
        return ready_.size();
    }

private:
    [[nodiscard]] bool should_begin(std::uint64_t now_ms) const noexcept {
        if (!cohort_.empty() && ready_.size() == cohort_.size()) {
            return true;  // everyone is ready -- begin at once
        }
        return ready_.size() >= quorum_ &&
               now_ms >= start_time_ms_ + grace_period_ms_;
    }

    std::set<Id> cohort_;
    std::set<Id> ready_;
    std::set<Id> released_;
    std::size_t quorum_ = 1;
    std::uint64_t grace_period_ms_ = 0;
    std::uint64_t start_time_ms_ = 0;
    bool begun_ = false;
};

} // namespace nstu::server
