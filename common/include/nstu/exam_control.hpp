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

// Synchronized-start barrier. Distribution and the kiosk loading screen let a
// cohort of clients reach a locked, ready-to-begin state at different moments;
// the server holds the reveal until the cohort is ready so no student sees the
// questions (or starts the clock) before the others. These two payloads ride
// the same authenticated control channel as the exam start/stop commands and
// are validated identically (fixed identity tuple + bounded UTF-8 text).

// The exam timer is authored in the manifest (durationSeconds, 60s..24h). A
// begin command may override it per start, or carry 0 to keep the manifest's
// own value. Bounds mirror the native host's manifest validation so an
// authored exam and a per-start override are held to the same limits.
inline constexpr std::uint32_t kMinimumExamDurationSeconds = 60u;
inline constexpr std::uint32_t kMaximumExamDurationSeconds = 86400u;

// Sent client -> server (relayed by the service) when a client has staged and
// verified the package, locked the kiosk, and is showing the loading screen.
// The identity tuple must match the server's active-exam context for the
// report to be counted toward the cohort.
struct ExamReadyReport {
    std::string package_id;
    std::string candidate_id;
    security::Sha256Digest package_digest{};
    security::ClientId client_id{};
    SessionId session_id{};
};

// Sent server -> client (relayed by the service) once the cohort may begin.
// Carries the authoritative start instant so every machine anchors its
// countdown to the same server clock rather than to whenever it finished
// loading. duration_seconds is 0 to use the manifest's durationSeconds, or a
// per-start override within [kMinimumExamDurationSeconds, kMaximumExamDurationSeconds].
struct ExamBeginCommand {
    std::string package_id;
    std::string candidate_id;
    security::Sha256Digest package_digest{};
    security::ClientId client_id{};
    SessionId session_id{};
    std::uint64_t server_start_unix_milliseconds = 0;
    std::uint32_t duration_seconds = 0;
};

[[nodiscard]] std::vector<std::byte> encode_exam_ready_report(
    const ExamReadyReport& report);
[[nodiscard]] std::optional<ExamReadyReport> decode_exam_ready_report(
    std::span<const std::byte> payload);
[[nodiscard]] bool validate_exam_ready_report(
    const ExamReadyReport& report) noexcept;

[[nodiscard]] std::vector<std::byte> encode_exam_begin_command(
    const ExamBeginCommand& command);
[[nodiscard]] std::optional<ExamBeginCommand> decode_exam_begin_command(
    std::span<const std::byte> payload);
[[nodiscard]] bool validate_exam_begin_command(
    const ExamBeginCommand& command) noexcept;

} // namespace nstu::exam
