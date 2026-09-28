#pragma once

#include <chrono>
#include <cstdint>

namespace nstu::server {

// Outcome of one watchdog tick. `none` = leave the stream alone. `first` and
// `repeat` both mean "re-assert set_streaming now"; they differ only so the
// caller logs the re-arm once per contiguous stall episode (`first`) instead of
// every tick (`repeat`), keeping a persistently dark client from churning the
// diagnostics ring buffer.
enum class StreamRearm : std::uint8_t {
    none,
    first,
    repeat,
};

// Level-triggered self-heal for the Focus/remote live stream.
//
// The dashboard arms streaming edge-triggered: set_streaming(true) is sent only
// when the streamed client id changes. That misses the case where a start_stream
// is lost across a client- or agent-side reconnect: the server keeps believing
// the client streams (the id never changed) while the agent, after reconnecting,
// is no longer capturing, so the Focus surface and the remote-control window
// freeze on the last delivered frame with nothing to re-fire the edge.
//
// This watchdog closes that gap. Each tick it is told which client should be
// streaming and that client's latest delivered-frame generation
// (ClientRecord::snapshot_generation, which advances once per received frame).
// While a client is wanted, an unchanged generation for longer than the stall
// window means frames have stopped arriving; update() then returns true exactly
// once per stall window so the caller can re-assert set_streaming. A changing
// generation is treated as liveness and holds the watchdog off; a change of the
// wanted client, or wanting none, re-seeds the baseline without re-arming.
class StreamRearmWatchdog {
public:
    using clock = std::chrono::steady_clock;

    // desired_client_id: the client that should be streaming now (0 == none).
    // current_generation: that client's latest delivered-frame generation.
    // now: monotonic timestamp for this tick.
    // stall: how long frames may be absent before re-arming.
    // Returns whether (and how) the stream command should be re-asserted.
    [[nodiscard]] StreamRearm update(std::uint64_t desired_client_id,
                                     std::uint64_t current_generation,
                                     clock::time_point now,
                                     clock::duration stall) noexcept {
        if (desired_client_id == 0) {
            // Nothing wanted: disarm so the next armed client seeds cleanly.
            client_id_ = 0;
            generation_ = 0;
            armed_this_episode_ = false;
            return StreamRearm::none;
        }
        if (desired_client_id != client_id_) {
            // New target (or first arm): seed the baseline, never re-arm on the
            // same tick the edge-triggered path already issued set_streaming.
            client_id_ = desired_client_id;
            generation_ = current_generation;
            stalled_since_ = now;
            armed_this_episode_ = false;
            return StreamRearm::none;
        }
        if (current_generation != generation_) {
            // Frames are flowing: liveness resets the stall clock and ends any
            // stall episode, so the next stall logs its first re-arm again.
            generation_ = current_generation;
            stalled_since_ = now;
            armed_this_episode_ = false;
            return StreamRearm::none;
        }
        if (now - stalled_since_ >= stall) {
            // Frames have stalled while the client is still wanted. Re-arm once,
            // then wait a full window before re-arming again so a persistently
            // dark client is nudged at a bounded rate, not on every tick. The
            // first re-arm of an episode is distinguished so the caller logs it
            // once rather than every window.
            stalled_since_ = now;
            if (!armed_this_episode_) {
                armed_this_episode_ = true;
                return StreamRearm::first;
            }
            return StreamRearm::repeat;
        }
        return StreamRearm::none;
    }

    void reset() noexcept {
        client_id_ = 0;
        generation_ = 0;
        stalled_since_ = {};
        armed_this_episode_ = false;
    }

    [[nodiscard]] std::uint64_t watched_client_id() const noexcept {
        return client_id_;
    }

private:
    std::uint64_t client_id_ = 0;
    std::uint64_t generation_ = 0;
    clock::time_point stalled_since_{};
    bool armed_this_episode_ = false;
};

} // namespace nstu::server
