#include "nstu/audit.hpp"

#include "nstu/telemetry.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <string>

namespace nstu::audit {
namespace {

std::uint64_t now_unix_milliseconds() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

void append_u8(std::vector<std::byte>& out, std::uint8_t value) {
    out.push_back(static_cast<std::byte>(value));
}

void append_u16(std::vector<std::byte>& out, std::uint16_t value) {
    out.push_back(static_cast<std::byte>(value & 0xff));
    out.push_back(static_cast<std::byte>((value >> 8) & 0xff));
}

void append_u64(std::vector<std::byte>& out, std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
        out.push_back(static_cast<std::byte>((value >> shift) & 0xff));
    }
}

void append_string(std::vector<std::byte>& out, const std::string& value) {
    append_u16(out, static_cast<std::uint16_t>(value.size()));
    for (const char byte : value) {
        out.push_back(static_cast<std::byte>(static_cast<unsigned char>(byte)));
    }
}

class Reader {
public:
    explicit Reader(std::span<const std::byte> data) noexcept : data_(data) {}

    [[nodiscard]] bool u8(std::uint8_t& value) noexcept {
        if (offset_ + 1 > data_.size()) {
            return false;
        }
        value = static_cast<std::uint8_t>(data_[offset_]);
        offset_ += 1;
        return true;
    }

    [[nodiscard]] bool u16(std::uint16_t& value) noexcept {
        if (offset_ + 2 > data_.size()) {
            return false;
        }
        value = static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(data_[offset_]) |
            (static_cast<std::uint16_t>(data_[offset_ + 1]) << 8));
        offset_ += 2;
        return true;
    }

    [[nodiscard]] bool u64(std::uint64_t& value) noexcept {
        if (offset_ + 8 > data_.size()) {
            return false;
        }
        value = 0;
        for (int index = 0; index < 8; ++index) {
            value |= static_cast<std::uint64_t>(
                         static_cast<std::uint8_t>(data_[offset_ + index]))
                     << (index * 8);
        }
        offset_ += 8;
        return true;
    }

    [[nodiscard]] bool string(std::string& value, std::size_t maximum) noexcept {
        std::uint16_t length = 0;
        if (!u16(length)) {
            return false;
        }
        if (length > maximum || offset_ + length > data_.size()) {
            return false;
        }
        value.assign(reinterpret_cast<const char*>(data_.data() + offset_),
                     length);
        offset_ += length;
        return true;
    }

    [[nodiscard]] bool at_end() const noexcept {
        return offset_ == data_.size();
    }

private:
    std::span<const std::byte> data_;
    std::size_t offset_ = 0;
};

[[nodiscard]] bool category_in_range(std::uint8_t value) noexcept {
    return value >= static_cast<std::uint8_t>(Category::enrollment) &&
           value <= static_cast<std::uint8_t>(Category::system);
}

[[nodiscard]] bool severity_in_range(std::uint8_t value) noexcept {
    return value >= static_cast<std::uint8_t>(Severity::info) &&
           value <= static_cast<std::uint8_t>(Severity::error);
}

std::wstring rotated_path(const std::filesystem::path& directory,
                          std::size_t index) {
    return (directory / ("audit." + std::to_string(index) + ".log")).wstring();
}

} // namespace

std::string sanitize_audit_field(std::string_view text,
                                 std::size_t maximum_bytes) {
    // sanitize_public_text already redacts Windows paths, keys, passwords,
    // tokens, secret-looking values, emails, IPs, MACs and client identifiers,
    // and turns control characters into a placeholder. The extra pass here is
    // the belt to that braces: guarantee no tab, newline or carriage return
    // survives, so every record stays exactly one line in the sink.
    std::string sanitized = telemetry::sanitize_public_text(text, maximum_bytes);
    for (char& value : sanitized) {
        if (value == '\t' || value == '\n' || value == '\r') {
            value = ' ';
        }
    }
    return sanitized;
}

Event sanitize_event(const Event& event) {
    Event clean = event;
    clean.component = sanitize_audit_field(event.component, kMaximumFieldBytes);
    clean.action = sanitize_audit_field(event.action, kMaximumFieldBytes);
    clean.result = sanitize_audit_field(event.result, kMaximumFieldBytes);
    clean.detail = sanitize_audit_field(event.detail, kMaximumDetailBytes);
    return clean;
}

std::string format_record_line(const Record& record) {
    const Event clean = sanitize_event(record.event);
    std::string line;
    line += std::to_string(record.timestamp_unix_milliseconds);
    line += '\t';
    line += std::to_string(record.sequence);
    line += '\t';
    line += to_string(clean.category);
    line += '\t';
    line += to_string(clean.severity);
    line += '\t';
    line += clean.component;
    line += '\t';
    line += clean.action;
    line += '\t';
    line += clean.result;
    line += "\top=";
    line += std::to_string(clean.operation_id);
    line += "\tclient=";
    line += std::to_string(clean.client_id);
    line += '\t';
    line += clean.detail;
    line += '\n';
    return line;
}

std::vector<std::byte> encode_audit_chunk(std::span<const Record> records) {
    std::vector<std::byte> out;
    append_u8(out, 1);
    std::vector<std::byte> body;
    std::uint16_t count = 0;
    for (const auto& record : records) {
        if (count >= kMaximumEventsPerChunk) {
            break;
        }
        const Event clean = sanitize_event(record.event);
        std::vector<std::byte> encoded;
        append_u64(encoded, record.sequence);
        append_u64(encoded, record.timestamp_unix_milliseconds);
        append_u8(encoded, static_cast<std::uint8_t>(clean.category));
        append_u8(encoded, static_cast<std::uint8_t>(clean.severity));
        append_u64(encoded, clean.operation_id);
        append_u64(encoded, clean.client_id);
        append_string(encoded, clean.component);
        append_string(encoded, clean.action);
        append_string(encoded, clean.result);
        append_string(encoded, clean.detail);
        // 3 header bytes (version + count) plus the accumulated body.
        if (3 + body.size() + encoded.size() > kMaximumChunkBytes) {
            break;
        }
        body.insert(body.end(), encoded.begin(), encoded.end());
        ++count;
    }
    append_u16(out, count);
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

std::optional<std::vector<Record>> decode_audit_chunk(
    std::span<const std::byte> payload) {
    if (payload.size() > kMaximumChunkBytes) {
        return std::nullopt;
    }
    Reader reader(payload);
    std::uint8_t version = 0;
    std::uint16_t count = 0;
    if (!reader.u8(version) || version != 1 || !reader.u16(count) ||
        count > kMaximumEventsPerChunk) {
        return std::nullopt;
    }
    std::vector<Record> records;
    records.reserve(count);
    for (std::uint16_t index = 0; index < count; ++index) {
        Record record;
        std::uint8_t category = 0;
        std::uint8_t severity = 0;
        if (!reader.u64(record.sequence) ||
            !reader.u64(record.timestamp_unix_milliseconds) ||
            !reader.u8(category) || !category_in_range(category) ||
            !reader.u8(severity) || !severity_in_range(severity) ||
            !reader.u64(record.event.operation_id) ||
            !reader.u64(record.event.client_id) ||
            !reader.string(record.event.component, kMaximumFieldBytes) ||
            !reader.string(record.event.action, kMaximumFieldBytes) ||
            !reader.string(record.event.result, kMaximumFieldBytes) ||
            !reader.string(record.event.detail, kMaximumDetailBytes)) {
            return std::nullopt;
        }
        record.event.category = static_cast<Category>(category);
        record.event.severity = static_cast<Severity>(severity);
        // Re-sanitise on receipt. A peer is not trusted to have done it, and a
        // record is about to be persisted centrally.
        record.event = sanitize_event(record.event);
        records.push_back(std::move(record));
    }
    if (!reader.at_end()) {
        return std::nullopt;
    }
    return records;
}

std::vector<std::byte> encode_audit_ack(std::uint64_t last_accepted_sequence) {
    std::vector<std::byte> out;
    append_u8(out, 1);
    append_u64(out, last_accepted_sequence);
    return out;
}

std::optional<std::uint64_t> decode_audit_ack(
    std::span<const std::byte> payload) {
    Reader reader(payload);
    std::uint8_t version = 0;
    std::uint64_t sequence = 0;
    if (!reader.u8(version) || version != 1 || !reader.u64(sequence) ||
        !reader.at_end()) {
        return std::nullopt;
    }
    return sequence;
}

Spool::Spool(std::size_t capacity) noexcept
    : capacity_(capacity == 0 ? 1 : capacity) {}

std::uint64_t Spool::emit(const Event& event) {
    std::scoped_lock lock(mutex_);
    while (records_.size() >= capacity_) {
        records_.pop_front();
        ++dropped_;
    }
    Record record;
    record.sequence = next_sequence_++;
    record.timestamp_unix_milliseconds = now_unix_milliseconds();
    record.event = sanitize_event(event);
    records_.push_back(std::move(record));
    return records_.back().sequence;
}

std::vector<Record> Spool::peek_chunk(std::size_t max_events) const {
    std::scoped_lock lock(mutex_);
    std::vector<Record> chunk;
    const std::size_t limit = std::min(max_events, records_.size());
    chunk.reserve(limit);
    for (std::size_t index = 0; index < limit; ++index) {
        chunk.push_back(records_[index]);
    }
    return chunk;
}

void Spool::acknowledge(std::uint64_t last_acked_sequence) {
    std::scoped_lock lock(mutex_);
    while (!records_.empty() &&
           records_.front().sequence <= last_acked_sequence) {
        records_.pop_front();
    }
}

std::size_t Spool::size() const {
    std::scoped_lock lock(mutex_);
    return records_.size();
}

std::uint64_t Spool::dropped_count() const {
    std::scoped_lock lock(mutex_);
    return dropped_;
}

bool Spool::empty() const {
    std::scoped_lock lock(mutex_);
    return records_.empty();
}

bool Sink::open(const std::filesystem::path& directory, std::string* error) {
    std::scoped_lock lock(mutex_);
    std::error_code create_error;
    std::filesystem::create_directories(directory, create_error);
    if (create_error) {
        if (error != nullptr) {
            *error = "audit sink directory could not be created";
        }
        return false;
    }
    directory_ = directory;
    active_path_ = directory / "audit.log";
    open_ = true;
    return true;
}

bool Sink::is_open() const {
    std::scoped_lock lock(mutex_);
    return open_;
}

bool Sink::rotate_locked(std::string* error) {
    // Drop the oldest kept file, then shift each rotated file up by one, then
    // move the active file to index 1. Keeps audit.log plus indices 1..N-1.
    std::error_code ignored;
    std::filesystem::remove(rotated_path(directory_, kMaximumSinkFiles - 1),
                            ignored);
    for (std::size_t index = kMaximumSinkFiles - 1; index > 1; --index) {
        const std::filesystem::path from = rotated_path(directory_, index - 1);
        if (std::filesystem::exists(from, ignored)) {
            std::filesystem::rename(from, rotated_path(directory_, index),
                                    ignored);
        }
    }
    if (std::filesystem::exists(active_path_, ignored)) {
        std::error_code rename_error;
        std::filesystem::rename(active_path_, rotated_path(directory_, 1),
                                rename_error);
        if (rename_error) {
            if (error != nullptr) {
                *error = "audit sink rotation failed";
            }
            return false;
        }
    }
    return true;
}

bool Sink::write(const Record& record, std::string* error) {
    std::scoped_lock lock(mutex_);
    if (!open_) {
        if (error != nullptr) {
            *error = "audit sink is not open";
        }
        return false;
    }
    const std::string line = format_record_line(record);

    std::error_code size_error;
    const auto current = std::filesystem::exists(active_path_, size_error)
                             ? std::filesystem::file_size(active_path_,
                                                          size_error)
                             : 0;
    if (!size_error && current + line.size() > kMaximumSinkFileBytes) {
        if (!rotate_locked(error)) {
            return false;
        }
    }

    // FILE_APPEND_DATA makes every write land at end of file regardless of the
    // file pointer, which is exactly the append semantics wanted. FILE_WRITE_DATA
    // is also requested so the torn-write rollback below (SetEndOfFile) has the
    // right it needs; with append-only access SetEndOfFile would be denied and a
    // torn tail could survive.
    const HANDLE file = CreateFileW(
        active_path_.wstring().c_str(), FILE_APPEND_DATA | FILE_WRITE_DATA,
        FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (error != nullptr) {
            *error = "audit sink open failed";
        }
        return false;
    }
    LARGE_INTEGER original{};
    original.QuadPart = 0;
    (void)GetFileSizeEx(file, &original);
    bool succeeded = true;
    std::size_t offset = 0;
    while (offset < line.size()) {
        const DWORD chunk = static_cast<DWORD>(
            std::min<std::size_t>(line.size() - offset, 1u << 20));
        DWORD written = 0;
        if (!WriteFile(file, line.data() + offset, chunk, &written, nullptr) ||
            written == 0) {
            succeeded = false;
            break;
        }
        offset += written;
    }
    if (succeeded && FlushFileBuffers(file) == FALSE) {
        succeeded = false;
    }
    if (!succeeded) {
        // Roll the file back to its size before this line so a torn tail can
        // never become the prefix of the next record.
        if (SetFilePointerEx(file, original, nullptr, FILE_BEGIN) != FALSE) {
            (void)SetEndOfFile(file);
            (void)FlushFileBuffers(file);
        }
        if (error != nullptr) {
            *error = "audit sink write failed";
        }
    }
    CloseHandle(file);
    return succeeded;
}

void Sink::close() noexcept {
    std::scoped_lock lock(mutex_);
    open_ = false;
    directory_.clear();
    active_path_.clear();
}

bool UploadRateLimiter::allow(std::uint64_t client_id,
                              std::uint64_t now_unix_milliseconds) {
    // One chunk per five seconds, burst of three. Enough to drain a backlog
    // after a reconnect without letting a client flood the central sink.
    constexpr double refill_per_millisecond = 1.0 / 5000.0;
    constexpr double burst = 3.0;
    std::scoped_lock lock(mutex_);
    auto& bucket = buckets_[client_id];
    if (bucket.last_refill_unix_milliseconds == 0) {
        bucket.tokens = burst;
        bucket.last_refill_unix_milliseconds = now_unix_milliseconds;
    } else if (now_unix_milliseconds > bucket.last_refill_unix_milliseconds) {
        const auto elapsed =
            now_unix_milliseconds - bucket.last_refill_unix_milliseconds;
        bucket.tokens = std::min(
            burst, bucket.tokens +
                       static_cast<double>(elapsed) * refill_per_millisecond);
        bucket.last_refill_unix_milliseconds = now_unix_milliseconds;
    }
    if (bucket.tokens >= 1.0) {
        bucket.tokens -= 1.0;
        return true;
    }
    return false;
}

const char* to_string(Category category) noexcept {
    switch (category) {
    case Category::enrollment:
        return "enrollment";
    case Category::managed_mode:
        return "managed_mode";
    case Category::uwf:
        return "uwf";
    case Category::exam:
        return "exam";
    case Category::session:
        return "session";
    case Category::security:
        return "security";
    case Category::system:
        return "system";
    }
    return "unknown";
}

const char* to_string(Severity severity) noexcept {
    switch (severity) {
    case Severity::info:
        return "info";
    case Severity::warning:
        return "warning";
    case Severity::error:
        return "error";
    }
    return "unknown";
}

} // namespace nstu::audit
