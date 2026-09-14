#pragma once

#include "nstu/exam_sync.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace nstu::exam {

// Exam packages are staged on the client before an authenticated start
// command is sent. Paths are UTF-8 and are revalidated against the installed
// package root by nstu-agent; they are not interpreted by the server.
inline constexpr std::size_t kMaximumExamControlPathBytes = 2048;
// The package identifier is the stable manifest identity used by answer
// events and state requests. Keep the exam-start bound identical to the
// journal/sync protocol so one authenticated session cannot address a second
// namespace through an oversized or ambiguous identifier.
inline constexpr std::size_t kMaximumExamControlPackageIdBytes =
    kMaximumPackageIdBytes;
inline constexpr std::size_t kMaximumExamControlCandidateBytes = 128;
inline constexpr std::size_t kMaximumExamControlPayloadBytes = 16u * 1024u;

struct ExamStartRequest {
    std::string package_root;
    std::string web_root;
    std::string user_data_root;
    std::string package_id;
    std::string candidate_id;
    security::Sha256Digest package_digest{};
    security::ClientId client_id{};
    SessionId session_id{};
};

// The payload is authenticated by the existing control channel. The codec
// still validates all lengths, reserved bits, identities, and UTF-8 fields so
// malformed or oversized messages fail closed before reaching the agent.
[[nodiscard]] std::vector<std::byte> encode_exam_start_request(
    const ExamStartRequest& request);
[[nodiscard]] std::optional<ExamStartRequest> decode_exam_start_request(
    std::span<const std::byte> payload);

[[nodiscard]] bool validate_exam_start_request(
    const ExamStartRequest& request) noexcept;

} // namespace nstu::exam
