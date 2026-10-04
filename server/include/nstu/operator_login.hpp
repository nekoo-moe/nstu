#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace nstu::server {

struct LoginDecision {
    bool granted = false;
    bool locked_out = false;
    unsigned attempts_remaining = 0;
};

// Gates operator sign-in to the teacher server. It holds only a derived
// password hash (never the password), compares a caller-supplied candidate hash
// in constant time, and throttles brute force by locking out after too many
// consecutive failures. Pure and clock-injected: the Windows KDF that derives
// the hash from the typed password plus the stored salt, and the DPAPI-backed
// storage of that hash, are wired in separately -- this is the part that
// unit-tests without crypto, storage, or a VM.
class OperatorLoginGate {
public:
    OperatorLoginGate(std::vector<std::byte> stored_hash, unsigned max_attempts,
                      std::uint64_t lockout_ms)
        : stored_hash_(std::move(stored_hash)),
          max_attempts_(max_attempts == 0 ? 1 : max_attempts),
          lockout_ms_(lockout_ms) {}

    [[nodiscard]] bool locked_out(std::uint64_t now_ms) const noexcept {
        return now_ms < locked_until_ms_;
    }

    // candidate_hash is the KDF output the caller derived from the entered
    // password and the stored salt -- never the password itself. While locked
    // out, every attempt is refused without even comparing. A correct hash
    // clears the failure streak; reaching max_attempts consecutive failures
    // starts a lockout window.
    LoginDecision verify(std::span<const std::byte> candidate_hash,
                         std::uint64_t now_ms) {
        if (locked_out(now_ms)) {
            return {false, true, 0};
        }
        if (constant_time_equal(candidate_hash, stored_hash_)) {
            failures_ = 0;
            return {true, false, max_attempts_};
        }
        ++failures_;
        if (failures_ >= max_attempts_) {
            failures_ = 0;
            locked_until_ms_ = now_ms + lockout_ms_;
            return {false, true, 0};
        }
        return {false, false, max_attempts_ - failures_};
    }

private:
    // Length-checked, data-independent comparison: it still runs the full XOR
    // over equal-length inputs so a wrong password cannot be timed out byte by
    // byte. An empty stored hash (no password provisioned) never matches.
    static bool constant_time_equal(std::span<const std::byte> left,
                                    std::span<const std::byte> right) noexcept {
        if (left.size() != right.size() || left.empty()) {
            return false;
        }
        unsigned difference = 0;
        for (std::size_t index = 0; index < left.size(); ++index) {
            difference |= std::to_integer<unsigned>(left[index]) ^
                          std::to_integer<unsigned>(right[index]);
        }
        return difference == 0;
    }

    std::vector<std::byte> stored_hash_;
    unsigned max_attempts_;
    std::uint64_t lockout_ms_;
    unsigned failures_ = 0;
    std::uint64_t locked_until_ms_ = 0;
};

} // namespace nstu::server
