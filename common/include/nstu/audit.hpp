#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// NSTU activity audit log.
//
// This is deliberately distinct from two neighbouring subsystems it must not be
// confused with:
//   * nstu::telemetry - opt-in, public, human-readable diagnostics the operator
//     chooses to share. Audit is always-on operational record keeping.
//   * the exam answer journal - the authoritative record of a candidate's
//     answers. Audit never carries answer bodies, questions, or any exam
//     content; it records that events happened, not what was in them.
//
// Every field is sanitised and bounded before it is ever serialised or written,
// and a hard denylist redacts anything that looks like a secret even if a caller
// passes one by mistake. The rule is simple: an audit record says a thing
// happened, who/what/where at a coarse level, and the outcome - never payloads.
namespace nstu::audit {

enum class Category : std::uint8_t {
    enrollment = 1,
    managed_mode = 2,
    uwf = 3,
    exam = 4,
    session = 5,
    security = 6,
    system = 7,
};

enum class Severity : std::uint8_t {
    info = 1,
    warning = 2,
    error = 3,
};

inline constexpr std::size_t kMaximumFieldBytes = 128;
inline constexpr std::size_t kMaximumDetailBytes = 256;
inline constexpr std::uint16_t kMaximumEventsPerChunk = 64;
inline constexpr std::size_t kMaximumChunkBytes = 32u * 1024u;
// Rotating on-disk sink: eight files of eight mebibytes, oldest dropped.
inline constexpr std::uint64_t kMaximumSinkFileBytes = 8u * 1024u * 1024u;
inline constexpr std::size_t kMaximumSinkFiles = 8;

// One audit event. String fields are free-form at the call site but are forced
// through sanitize_audit_field() the moment they are encoded or written, so a
// caller cannot smuggle control characters, secrets, or unbounded text past the
// boundary.
struct Event {
    Category category = Category::system;
    Severity severity = Severity::info;
    std::string component;
    std::string action;
    std::string result;
    // Ties an event to a server operation when one applies; 0 otherwise.
    std::uint64_t operation_id = 0;
    // Registry id of the client the event concerns; 0 for the local/server side
    // or when not applicable. This is a small integer, never a hostname, MAC,
    // or address.
    std::uint64_t client_id = 0;
    std::string detail;
};

// An event stamped with its monotonic sequence and wall-clock time. Sequence is
// assigned by the emitter and is what an upload is acknowledged against.
struct Record {
    std::uint64_t sequence = 0;
    std::uint64_t timestamp_unix_milliseconds = 0;
    Event event;
};

// sanitize_public_text, then a hard denylist pass that redacts tokens which
// look like secrets (keys, passwords, tokens, SAS codes) and control/tab/newline
// characters, bounded to maximum_bytes. Never throws.
[[nodiscard]] std::string sanitize_audit_field(
    std::string_view text, std::size_t maximum_bytes = kMaximumFieldBytes);

// Returns a copy of the event with every string field sanitised and bounded.
[[nodiscard]] Event sanitize_event(const Event& event);

// One newline-terminated, tab-separated record line for the rotating sink. The
// returned string contains no interior newline, so records stay one-per-line.
[[nodiscard]] std::string format_record_line(const Record& record);

// Wire codecs for audit_upload (a chunk of records) and audit_ack.
[[nodiscard]] std::vector<std::byte> encode_audit_chunk(
    std::span<const Record> records);
[[nodiscard]] std::optional<std::vector<Record>> decode_audit_chunk(
    std::span<const std::byte> payload);
[[nodiscard]] std::vector<std::byte> encode_audit_ack(
    std::uint64_t last_accepted_sequence);
[[nodiscard]] std::optional<std::uint64_t> decode_audit_ack(
    std::span<const std::byte> payload);

// Bounded, drop-oldest in-memory queue of records awaiting upload. When it is
// full the oldest record is discarded and a counter is incremented, so a client
// that cannot reach the server for a long time bounds its memory instead of
// growing without limit. The drop is itself observable.
class Spool {
public:
    explicit Spool(std::size_t capacity = 4096) noexcept;

    // Stamps the event with the next sequence and current time, sanitises it,
    // and enqueues it. Returns the assigned sequence.
    std::uint64_t emit(const Event& event);

    // Up to max_events records, never exceeding kMaximumChunkBytes once encoded,
    // without removing them from the spool. Peeked records stay until acked.
    [[nodiscard]] std::vector<Record> peek_chunk(
        std::size_t max_events = kMaximumEventsPerChunk) const;

    // Removes every record whose sequence is <= last_acked_sequence.
    void acknowledge(std::uint64_t last_acked_sequence);

    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] std::uint64_t dropped_count() const;
    [[nodiscard]] bool empty() const;

private:
    mutable std::mutex mutex_;
    std::deque<Record> records_;
    std::size_t capacity_;
    std::uint64_t next_sequence_ = 1;
    std::uint64_t dropped_ = 0;
};

// Rotating newline-delimited sink. Append is atomic-ish in the same sense as the
// answer journal: a full flush to disk, and a failed write does not leave a torn
// line behind for the next append to build on.
class Sink {
public:
    Sink() = default;

    // Directory that will hold audit.log plus audit.1.log .. audit.N.log.
    [[nodiscard]] bool open(const std::filesystem::path& directory,
                            std::string* error = nullptr);
    [[nodiscard]] bool is_open() const;
    // Appends one record, rotating first if the active file would exceed the
    // size cap. Returns false on I/O failure without partially writing a line.
    [[nodiscard]] bool write(const Record& record, std::string* error = nullptr);
    void close() noexcept;

private:
    [[nodiscard]] bool rotate_locked(std::string* error);

    mutable std::mutex mutex_;
    std::filesystem::path directory_;
    std::filesystem::path active_path_;
    bool open_ = false;
};

// Server-side per-client rate limit for inbound audit chunks. Roughly one chunk
// every five seconds per client, with a small burst allowance, so a client
// cannot flood the central sink.
class UploadRateLimiter {
public:
    // Returns true if a chunk from this client is allowed at now_unix_ms.
    [[nodiscard]] bool allow(std::uint64_t client_id,
                             std::uint64_t now_unix_milliseconds);

private:
    struct Bucket {
        double tokens = 0.0;
        std::uint64_t last_refill_unix_milliseconds = 0;
    };
    mutable std::mutex mutex_;
    std::unordered_map<std::uint64_t, Bucket> buckets_;
};

[[nodiscard]] const char* to_string(Category category) noexcept;
[[nodiscard]] const char* to_string(Severity severity) noexcept;

} // namespace nstu::audit
