#pragma once

#include "nstu/auth.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace nstu::exam {

// The exam event sequence is independent from the authenticated TCP sequence.
// TCP sessions can be recreated after a reboot or a network interruption while
// this sequence must remain stable for replay and deduplication.
using SessionId = std::array<std::byte, 16>;

inline constexpr std::size_t kMaximumPackageIdBytes = 128;
inline constexpr std::size_t kMaximumCandidateIdBytes = 128;
inline constexpr std::size_t kMaximumQuestionIdBytes = 128;
inline constexpr std::size_t kMaximumAnswerBytes = 16u * 1024u;
// The payload is wrapped by an 8-byte sequence and a 16-byte MAC tag on the
// authenticated command channel. Keep exam payloads below the outer protocol
// limit so every encoded chunk can actually be transmitted.
static_assert(protocol::kMaxCommandPayload >
                  sizeof(std::uint64_t) + security::kControlAuthTagBytes,
              "authenticated command overhead must fit inside the payload");
inline constexpr std::size_t kMaximumExamPayloadBytes =
    protocol::kMaxCommandPayload - sizeof(std::uint64_t) -
    security::kControlAuthTagBytes;
inline constexpr std::size_t kMaximumJournalBytes = 256u * 1024u * 1024u;
inline constexpr std::size_t kMaximumJournalSessions = 4096;
inline constexpr std::size_t kMaximumOutboxEvents = 512;
inline constexpr std::size_t kMaximumOutboxBytes = 8u * 1024u * 1024u;
inline constexpr std::size_t kMaximumStateEntries = 512;
// Keep native responses bounded to the same number of chunks retained by the
// client ExamBridge and browser reassembler. Larger snapshots must be
// requested in a future paginated protocol revision.
inline constexpr std::size_t kMaximumStateChunks = 64;

enum class AnswerKind : std::uint8_t {
    upsert = 1,
    clear = 2,
    finalize = 3,
};

enum class AnswerAckStatus : std::uint8_t {
    accepted = 1,
    duplicate = 2,
    rejected = 3,
    conflict = 4,
    gap = 5,
    unavailable = 6,
};

struct AnswerEvent {
    std::string package_id;
    security::Sha256Digest package_digest{};
    security::ClientId client_id{};
    SessionId session_id{};
    std::string candidate_id;
    std::string question_id;
    std::uint32_t question_revision = 1;
    std::uint64_t sequence = 0;
    std::uint64_t client_time_unix_milliseconds = 0;
    AnswerKind kind = AnswerKind::upsert;
    std::vector<std::byte> answer;
    security::Sha256Digest previous_event_hash{};
};

struct AnswerAck {
    AnswerAckStatus status = AnswerAckStatus::rejected;
    SessionId session_id{};
    std::uint64_t sequence = 0;
    std::uint64_t highest_contiguous_sequence = 0;
    std::uint64_t server_time_unix_milliseconds = 0;
    security::Sha256Digest event_hash{};
    security::Sha256Digest state_hash{};
};

struct StateRequest {
    std::string package_id;
    security::Sha256Digest package_digest{};
    security::ClientId client_id{};
    SessionId session_id{};
    std::string candidate_id;
};

struct AnswerState {
    std::string question_id;
    std::uint32_t question_revision = 1;
    std::uint64_t sequence = 0;
    AnswerKind kind = AnswerKind::upsert;
    std::vector<std::byte> answer;
    security::Sha256Digest event_hash{};
};

struct StateResponse {
    std::string package_id;
    security::Sha256Digest package_digest{};
    security::ClientId client_id{};
    SessionId session_id{};
    std::string candidate_id;
    std::uint64_t highest_contiguous_sequence = 0;
    bool finalized = false;
    security::Sha256Digest state_hash{};
    // Hash of the event at highest_contiguous_sequence.  Unlike state_hash,
    // this is the chain cursor needed to construct the next event after a
    // reconnect, including sessions whose latest event cleared an answer.
    security::Sha256Digest last_event_hash{};
    // State responses are chunked when the answer set would exceed the
    // authenticated command payload limit.  A response with no chunking is
    // represented as index 0/count 1.
    std::uint16_t chunk_index = 0;
    std::uint16_t chunk_count = 1;
    std::vector<AnswerState> answers;
};

[[nodiscard]] bool validate_answer_event(const AnswerEvent& event,
                                         std::string* error = nullptr);
[[nodiscard]] std::vector<std::byte> encode_answer_event(
    const AnswerEvent& event);
[[nodiscard]] std::optional<AnswerEvent> decode_answer_event(
    std::span<const std::byte> payload, std::string* error = nullptr);
[[nodiscard]] std::optional<security::Sha256Digest> hash_answer_event(
    const AnswerEvent& event);

[[nodiscard]] std::vector<std::byte> encode_answer_ack(const AnswerAck& ack);
[[nodiscard]] std::optional<AnswerAck> decode_answer_ack(
    std::span<const std::byte> payload);

[[nodiscard]] std::vector<std::byte> encode_state_request(
    const StateRequest& request);
[[nodiscard]] std::optional<StateRequest> decode_state_request(
    std::span<const std::byte> payload);

[[nodiscard]] std::vector<std::byte> encode_state_response(
    const StateResponse& response);
// Split a complete state snapshot into independently authenticated wire
// payloads.  Every returned payload carries the same session/state metadata;
// the caller sends them with the same request id and the receiver reassembles
// by (session_id, chunk_index, chunk_count).
[[nodiscard]] std::vector<std::vector<std::byte>> encode_state_response_chunks(
    const StateResponse& response);
[[nodiscard]] std::optional<StateResponse> decode_state_response(
    std::span<const std::byte> payload);

enum class AppendStatus : std::uint8_t {
    accepted,
    duplicate,
    conflict,
    gap,
    rejected,
    unavailable,
};

struct AppendOutcome {
    AppendStatus status = AppendStatus::rejected;
    AnswerAck ack{};
};

// Append-only server journal. A record is made visible to callers only after
// its bytes and the file metadata have been flushed. Reopening the journal
// verifies the hash chain and discards only an incomplete tail record.
class AnswerJournal {
public:
    AnswerJournal();
    ~AnswerJournal();
    AnswerJournal(const AnswerJournal&) = delete;
    AnswerJournal& operator=(const AnswerJournal&) = delete;

    [[nodiscard]] bool open(const std::filesystem::path& path,
                            std::string* error = nullptr);
    void close() noexcept;
    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] bool recovered_truncated_tail() const noexcept;

    [[nodiscard]] AppendOutcome append(const AnswerEvent& event,
                                       std::string* error = nullptr);
    [[nodiscard]] std::optional<StateResponse> state(
        const StateRequest& request, std::string* error = nullptr) const;
    [[nodiscard]] bool export_state(const StateRequest& request,
                                    const std::filesystem::path& path,
                                    std::string* error = nullptr) const;
    [[nodiscard]] std::size_t event_count() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Offline answer exports use a framed SEX1 container so a large state can be
// copied and verified without relying on the authenticated command channel.
// The decoder validates all frames and returns one reassembled snapshot.
[[nodiscard]] std::optional<StateResponse> decode_state_export(
    std::span<const std::byte> payload, std::string* error = nullptr);

// Client-side write-ahead outbox. It is intentionally separate from the
// authoritative server journal: an event is removed only after the server
// acknowledges the matching durable sequence and hash.
class AnswerOutbox {
public:
    AnswerOutbox();
    ~AnswerOutbox();
    AnswerOutbox(const AnswerOutbox&) = delete;
    AnswerOutbox& operator=(const AnswerOutbox&) = delete;

    [[nodiscard]] bool open(const std::filesystem::path& path,
                            std::string* error = nullptr);
    void close() noexcept;
    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] bool enqueue(const AnswerEvent& event,
                               std::string* error = nullptr);
    [[nodiscard]] std::vector<AnswerEvent> pending(
        std::size_t maximum = kMaximumOutboxEvents) const;
    // Return only events provisioned for the supplied client identity.  A
    // machine may retain an encrypted outbox across reprovisioning; callers
    // must never send records belonging to a previous client identity over a
    // newly authenticated session.
    [[nodiscard]] std::vector<AnswerEvent> pending_for_client(
        const security::ClientId& client_id,
        std::size_t maximum = kMaximumOutboxEvents) const;
    [[nodiscard]] bool acknowledge(const SessionId& session_id,
                                   std::uint64_t sequence,
                                   std::span<const std::byte> event_hash,
                                   std::string* error = nullptr);
    [[nodiscard]] std::size_t size() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nstu::exam
