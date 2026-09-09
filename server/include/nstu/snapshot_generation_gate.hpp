#pragma once

#include <cstdint>

namespace nstu::server {

// Allows exactly one processing attempt per non-zero snapshot generation.
// Both success and failure remain cached until a newer generation arrives or
// the graphics resource owner explicitly resets the gate.
class SnapshotGenerationGate {
public:
    void reset() noexcept {
        generation_ = 0;
        state_ = State::unattempted;
    }

    [[nodiscard]] bool begin(std::uint64_t generation) noexcept {
        if (generation == 0) {
            return false;
        }
        if (generation != generation_) {
            generation_ = generation;
            state_ = State::unattempted;
        }
        if (state_ != State::unattempted) {
            return false;
        }
        state_ = State::attempting;
        return true;
    }

    void mark_succeeded(std::uint64_t generation) noexcept {
        if (generation == generation_ && state_ == State::attempting) {
            state_ = State::succeeded;
        }
    }

    void mark_failed(std::uint64_t generation) noexcept {
        if (generation == generation_ && state_ == State::attempting) {
            state_ = State::failed;
        }
    }

    [[nodiscard]] bool succeeded(std::uint64_t generation) const noexcept {
        return generation != 0 && generation == generation_ &&
               state_ == State::succeeded;
    }

    [[nodiscard]] bool failed(std::uint64_t generation) const noexcept {
        return generation != 0 && generation == generation_ &&
               state_ == State::failed;
    }

private:
    enum class State : std::uint8_t {
        unattempted,
        attempting,
        succeeded,
        failed,
    };

    std::uint64_t generation_ = 0;
    State state_ = State::unattempted;
};

} // namespace nstu::server
