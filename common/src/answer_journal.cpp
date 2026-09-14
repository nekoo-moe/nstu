#include "nstu/exam_sync.hpp"

#include "nstu/secret_store.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cwchar>
#include <cerrno>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <type_traits>
#include <thread>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#include <sddl.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace nstu::exam {
namespace {

inline constexpr std::uint32_t kJournalMagic = 0x314a5845u; // "EXJ1"
inline constexpr std::uint32_t kOutboxMagic = 0x3158424fu; // "OBX1"
inline constexpr std::uint32_t kEncryptedOutboxMagic = 0x31424f45u; // "EOB1"
inline constexpr std::uint32_t kStateExportMagic = 0x31584553u; // "SEX1"
inline constexpr std::uint16_t kJournalVersion = 1;
inline constexpr std::uint16_t kStateExportVersion = 1;
inline constexpr std::uint16_t kOutboxVersion = 4;
inline constexpr std::uint16_t kOutboxWatermarkVersion = 2;
inline constexpr std::uint16_t kOutboxAcknowledgementVersion = 3;
inline constexpr std::uint16_t kOutboxFinalizationVersion = 4;
inline constexpr std::uint16_t kOutboxLegacyVersion = 1;
inline constexpr std::size_t kJournalHeaderBytes = 12;
inline constexpr std::size_t kJournalRecordOverhead =
    kJournalHeaderBytes + security::kSha256Bytes;
inline constexpr std::size_t kMaximumWireBytes = kMaximumExamPayloadBytes;
inline constexpr std::size_t kMaximumOutboxWatermarks = 4096;
// DPAPI adds a small provider-dependent wrapper around the logical outbox.
// Keep the on-disk allowance separate from the logical plaintext quota.
inline constexpr std::size_t kMaximumStoredOutboxBytes =
    kMaximumOutboxBytes + 64u * 1024u;
inline constexpr char kOutboxEntropyText[] = "NSTU-EXAM-OUTBOX-V1";

void set_error(std::string* error, const char* message) {
    if (error != nullptr) {
        *error = message;
    }
}

void set_error_code(std::string* error, const char* message,
                    unsigned long code) {
    if (error != nullptr) {
        *error = std::string(message) + " (error " + std::to_string(code) +
                ")";
    }
}

template <typename T>
void append_le(std::vector<std::byte>& output, T value) {
    static_assert(std::is_unsigned_v<T>);
    const auto offset = output.size();
    output.resize(offset + sizeof(T));
    for (std::size_t index = 0; index < sizeof(T); ++index) {
        output[offset + index] = static_cast<std::byte>(value & 0xffu);
        value >>= 8u;
    }
}

template <typename T>
bool read_le(std::span<const std::byte> input, std::size_t& offset,
             T& value) {
    static_assert(std::is_unsigned_v<T>);
    if (offset > input.size() || sizeof(T) > input.size() - offset) {
        return false;
    }
    value = 0;
    for (std::size_t index = 0; index < sizeof(T); ++index) {
        value |= static_cast<T>(std::to_integer<unsigned int>(input[offset++]))
                 << (index * 8u);
    }
    return true;
}

void append_bytes(std::vector<std::byte>& output,
                  std::span<const std::byte> bytes) {
    output.insert(output.end(), bytes.begin(), bytes.end());
}

bool read_bytes(std::span<const std::byte> input, std::size_t& offset,
                std::span<std::byte> output) {
    if (offset > input.size() || output.size() > input.size() - offset) {
        return false;
    }
    std::copy_n(input.begin() + static_cast<std::ptrdiff_t>(offset),
                output.size(), output.begin());
    offset += output.size();
    return true;
}

std::uint64_t now_milliseconds() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<
        std::chrono::milliseconds>(std::chrono::system_clock::now()
                                       .time_since_epoch())
                                           .count());
}

bool all_zero(std::span<const std::byte> bytes) noexcept {
    return std::all_of(bytes.begin(), bytes.end(),
                       [](std::byte value) { return value == std::byte{0}; });
}

std::string session_key(std::string_view package_id,
                        std::span<const std::byte> package_digest,
                        std::span<const std::byte> client_id,
                        std::span<const std::byte> session_id) {
    std::string key;
    key.reserve(package_id.size() + package_digest.size() + client_id.size() +
                session_id.size() + 1);
    key.append(package_id);
    key.push_back('\0');
    key.append(reinterpret_cast<const char*>(package_digest.data()),
               package_digest.size());
    key.append(reinterpret_cast<const char*>(client_id.data()),
               client_id.size());
    key.append(reinterpret_cast<const char*>(session_id.data()),
               session_id.size());
    return key;
}

std::string session_key(const AnswerEvent& event) {
    return session_key(event.package_id, event.package_digest, event.client_id,
                        event.session_id);
}

std::string session_key(const StateRequest& request) {
    return session_key(request.package_id, request.package_digest,
                       request.client_id, request.session_id);
}

bool same_digest(std::span<const std::byte> left,
                 std::span<const std::byte> right) noexcept {
    return left.size() == right.size() &&
           std::equal(left.begin(), left.end(), right.begin());
}

#ifdef _WIN32
// Journal and outbox snapshots contain answer material. Create replacement
// files with an explicit SYSTEM/Administrators-only DACL instead of inheriting
// a potentially permissive directory ACL.
class RestrictedSecurityAttributes {
public:
    RestrictedSecurityAttributes() {
        if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
                L"D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FA;;;OW)", SDDL_REVISION_1,
                &descriptor_, nullptr) != FALSE) {
            attributes_.nLength = sizeof(attributes_);
            attributes_.lpSecurityDescriptor = descriptor_;
            attributes_.bInheritHandle = FALSE;
            valid_ = true;
        }
    }

    ~RestrictedSecurityAttributes() {
        if (descriptor_ != nullptr) {
            LocalFree(descriptor_);
        }
    }

    SECURITY_ATTRIBUTES* get() noexcept { return valid_ ? &attributes_ : nullptr; }
    [[nodiscard]] bool valid() const noexcept { return valid_; }

private:
    PSECURITY_DESCRIPTOR descriptor_ = nullptr;
    SECURITY_ATTRIBUTES attributes_{};
    bool valid_ = false;
};
#endif

// Only one process may own a journal or outbox at a time. The in-process
// mutex protects threads, while this lease closes the gap between loading the
// existing chain and appending the next record in a second process.
class InterprocessFileLease {
public:
    InterprocessFileLease() = default;
    ~InterprocessFileLease() { release(); }

    InterprocessFileLease(const InterprocessFileLease&) = delete;
    InterprocessFileLease& operator=(const InterprocessFileLease&) = delete;

    InterprocessFileLease(InterprocessFileLease&& other) noexcept {
        move_from(std::move(other));
    }

    InterprocessFileLease& operator=(InterprocessFileLease&& other) noexcept {
        if (this != &other) {
            release();
            move_from(std::move(other));
        }
        return *this;
    }

    [[nodiscard]] bool acquire(const std::filesystem::path& path,
                               std::string* error) {
        release();
        auto lock_path = path;
        lock_path += L".lock";
#ifdef _WIN32
        RestrictedSecurityAttributes security_attributes;
        if (!security_attributes.valid()) {
            set_error(error, "journal lock security descriptor creation failed");
            return false;
        }
        handle_ = CreateFileW(lock_path.c_str(), GENERIC_READ | GENERIC_WRITE,
                              0, security_attributes.get(), OPEN_ALWAYS,
                              FILE_ATTRIBUTE_HIDDEN |
                                  FILE_ATTRIBUTE_NOT_CONTENT_INDEXED,
                              nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            handle_ = INVALID_HANDLE_VALUE;
            set_error_code(error, "journal lock acquisition failed",
                           GetLastError());
            return false;
        }
#else
        descriptor_ = ::open(lock_path.c_str(), O_CREAT | O_RDWR, 0600);
        if (descriptor_ < 0) {
            set_error_code(error, "journal lock acquisition failed",
                           static_cast<unsigned long>(errno));
            return false;
        }
        if (::flock(descriptor_, LOCK_EX | LOCK_NB) != 0) {
            const auto lock_error = errno;
            ::close(descriptor_);
            descriptor_ = -1;
            set_error_code(error, "journal lock acquisition failed",
                           static_cast<unsigned long>(lock_error));
            return false;
        }
#endif
        return true;
    }

    void release() noexcept {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
#else
        if (descriptor_ >= 0) {
            (void)::flock(descriptor_, LOCK_UN);
            (void)::close(descriptor_);
            descriptor_ = -1;
        }
#endif
    }

private:
    void move_from(InterprocessFileLease&& other) noexcept {
#ifdef _WIN32
        handle_ = other.handle_;
        other.handle_ = INVALID_HANDLE_VALUE;
#else
        descriptor_ = other.descriptor_;
        other.descriptor_ = -1;
#endif
    }

#ifdef _WIN32
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    int descriptor_ = -1;
#endif
};

bool write_all_file(const std::filesystem::path& path,
                    std::span<const std::byte> bytes, std::string* error) {
    std::error_code directory_error;
    std::filesystem::create_directories(path.parent_path(), directory_error);
    if (directory_error) {
        set_error(error, "journal directory creation failed");
        return false;
    }
    std::array<std::byte, sizeof(std::uint64_t)> random{};
    if (!security::generate_random(random)) {
        set_error(error, "journal temporary name generation failed");
        return false;
    }
    std::uint64_t suffix = 0;
    for (std::size_t index = 0; index < random.size(); ++index) {
        suffix |= static_cast<std::uint64_t>(
                      std::to_integer<unsigned int>(random[index]))
                  << (index * 8u);
    }
    std::uint64_t process_id = 0;
#ifdef _WIN32
    process_id = static_cast<std::uint64_t>(GetCurrentProcessId());
#else
    process_id = static_cast<std::uint64_t>(
        std::hash<std::thread::id>{}(std::this_thread::get_id()));
#endif
    const auto temporary = path.wstring() + L".tmp-" +
                           std::to_wstring(process_id) + L"-" +
                           std::to_wstring(suffix);
#ifdef _WIN32
    RestrictedSecurityAttributes security_attributes;
    if (!security_attributes.valid()) {
        set_error(error, "journal security descriptor creation failed");
        return false;
    }
    const HANDLE file = CreateFileW(
        temporary.c_str(), GENERIC_WRITE, 0, security_attributes.get(), CREATE_NEW,
        FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        set_error(error, "journal temporary file creation failed");
        return false;
    }
    std::size_t offset = 0;
    bool succeeded = true;
    while (offset < bytes.size()) {
        const auto chunk = static_cast<DWORD>(std::min<std::size_t>(
            bytes.size() - offset, std::numeric_limits<DWORD>::max()));
        DWORD written = 0;
        if (!WriteFile(file, bytes.data() + offset, chunk, &written, nullptr) ||
            written == 0) {
            succeeded = false;
            break;
        }
        offset += written;
    }
    if (succeeded && FlushFileBuffers(file) == FALSE) {
        succeeded = false;
    }
    CloseHandle(file);
    if (succeeded &&
        MoveFileExW(temporary.c_str(), path.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ==
            FALSE) {
        succeeded = false;
    }
#else
    std::ofstream output(std::filesystem::path(temporary),
                         std::ios::binary | std::ios::trunc);
    bool succeeded = output.good() &&
        (bytes.empty() || static_cast<bool>(output.write(
            reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()))));
    output.flush();
    output.close();
    if (succeeded) {
        std::error_code rename_error;
        std::filesystem::rename(temporary, path, rename_error);
        // POSIX rename replaces an existing destination atomically. If it
        // fails, retain the last known-good destination and fail closed; a
        // best-effort remove here could destroy the only durable journal after
        // a transient filesystem or permission error.
        succeeded = !rename_error;
    }
#endif
    if (!succeeded) {
#ifdef _WIN32
        DeleteFileW(temporary.c_str());
#else
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
#endif
        set_error(error, "journal atomic write failed");
    }
    return succeeded;
}

bool append_file(const std::filesystem::path& path,
                 std::span<const std::byte> bytes, std::string* error) {
#ifdef _WIN32
    RestrictedSecurityAttributes security_attributes;
    if (!security_attributes.valid()) {
        set_error(error, "journal security descriptor creation failed");
        return false;
    }
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                    FILE_SHARE_READ, security_attributes.get(),
                                    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        set_error_code(error, "journal append open failed", GetLastError());
        return false;
    }
    OVERLAPPED lock_offset{};
    const bool lock_held = LockFileEx(file, LOCKFILE_EXCLUSIVE_LOCK, 0, MAXDWORD,
                                      MAXDWORD, &lock_offset) != FALSE;
    bool succeeded = lock_held;
    if (!lock_held) {
        set_error_code(error, "journal append lock failed", GetLastError());
    }
    LARGE_INTEGER original_size{};
    bool original_size_known = false;
    if (succeeded && GetFileSizeEx(file, &original_size) == FALSE) {
        succeeded = false;
        set_error_code(error, "journal append size query failed", GetLastError());
    } else if (succeeded) {
        original_size_known = true;
    }
    LARGE_INTEGER end_position{};
    if (succeeded &&
        SetFilePointerEx(file, end_position, nullptr, FILE_END) == FALSE) {
        succeeded = false;
        set_error_code(error, "journal append seek failed", GetLastError());
    }
    std::size_t offset = 0;
    while (succeeded && offset < bytes.size()) {
        const auto chunk = static_cast<DWORD>(std::min<std::size_t>(
            bytes.size() - offset, std::numeric_limits<DWORD>::max()));
        DWORD written = 0;
        if (!WriteFile(file, bytes.data() + offset, chunk, &written, nullptr) ||
            written == 0) {
            succeeded = false;
            set_error_code(error, "journal append write failed", GetLastError());
            break;
        }
        offset += written;
    }
    if (succeeded && FlushFileBuffers(file) == FALSE) {
        succeeded = false;
        set_error_code(error, "journal append flush failed", GetLastError());
    }
    if (!succeeded && lock_held && original_size_known) {
        // A short write or failed flush can leave a partial record behind.
        // Roll it back while the exclusive range lock is still held so the
        // next append cannot start after a corrupt tail.
        if (SetFilePointerEx(file, original_size, nullptr, FILE_BEGIN) == FALSE ||
            SetEndOfFile(file) == FALSE || FlushFileBuffers(file) == FALSE) {
            set_error(error, "journal append rollback failed");
        }
    }
    if (lock_held && UnlockFileEx(file, 0, MAXDWORD, MAXDWORD,
                                 &lock_offset) == FALSE && succeeded) {
        // The bytes have already been flushed. Closing the handle releases an
        // OS-held byte-range lock, so do not report a durable append as a
        // storage failure merely because explicit unlock failed.
    }
    CloseHandle(file);
#else
    std::error_code size_error;
    const auto original_size = std::filesystem::file_size(path, size_error);
    if (size_error) {
        set_error(error, "journal append size query failed");
        return false;
    }
    std::ofstream output(path, std::ios::binary | std::ios::app);
    bool succeeded = output.good() &&
        (bytes.empty() || static_cast<bool>(output.write(
            reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()))));
    output.flush();
    succeeded = succeeded && output.good();
    output.close();
    if (!succeeded) {
        std::error_code rollback_error;
        std::filesystem::resize_file(path, original_size, rollback_error);
        if (rollback_error) {
            set_error(error, "journal append rollback failed");
        }
    }
#endif
    if (!succeeded) {
        if (error == nullptr || error->empty()) {
            set_error(error, "journal append failed");
        }
    }
    return succeeded;
}

std::vector<std::byte> read_file(const std::filesystem::path& path,
                                 std::string* error,
                                 std::size_t maximum_bytes =
                                     kMaximumJournalBytes) {
    std::error_code size_error;
    const auto size = std::filesystem::file_size(path, size_error);
    if (size_error) {
        set_error(error, "journal file size query failed");
        return {};
    }
    if (size > maximum_bytes) {
        set_error(error, "file exceeds the configured quota");
        return {};
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        set_error(error, "journal file open failed");
        return {};
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    if (!bytes.empty() &&
        !input.read(reinterpret_cast<char*>(bytes.data()),
                    static_cast<std::streamsize>(bytes.size()))) {
        set_error(error, "journal file read failed");
        return {};
    }
    return bytes;
}

bool valid_path(const std::filesystem::path& path) {
    return !path.empty() && path.is_absolute() && path != path.root_path();
}

bool aliases_path(const std::filesystem::path& left,
                  const std::filesystem::path& right) {
    if (left == right) {
        return true;
    }
    std::error_code equivalent_error;
    if (std::filesystem::equivalent(left, right, equivalent_error)) {
        return true;
    }

    // `equivalent` cannot compare a destination that has not been created yet.
    // Resolve the existing prefix so a case or `..` alias cannot replace the
    // active journal during an export.
    std::error_code left_error;
    std::error_code right_error;
    const auto left_canonical = std::filesystem::weakly_canonical(left, left_error);
    const auto right_canonical = std::filesystem::weakly_canonical(right, right_error);
    if (left_error || right_error || left_canonical.empty() ||
        right_canonical.empty()) {
        return false;
    }
#ifdef _WIN32
    return _wcsicmp(left_canonical.c_str(), right_canonical.c_str()) == 0;
#else
    return left_canonical == right_canonical;
#endif
}

class SensitiveBytesGuard {
public:
    explicit SensitiveBytesGuard(std::vector<std::byte>& bytes) noexcept
        : bytes_(&bytes) {}

    ~SensitiveBytesGuard() { security::secure_zero(*bytes_); }

    SensitiveBytesGuard(const SensitiveBytesGuard&) = delete;
    SensitiveBytesGuard& operator=(const SensitiveBytesGuard&) = delete;

private:
    std::vector<std::byte>* bytes_;
};

struct OutboxWatermark {
    std::string package_id;
    security::Sha256Digest package_digest{};
    security::ClientId client_id{};
    SessionId session_id{};
    std::string candidate_id;
    std::uint64_t highest_sequence = 0;
    security::Sha256Digest last_hash{};
    std::uint64_t acknowledged_sequence = 0;
    security::Sha256Digest acknowledged_hash{};
    bool finalized = false;
};

bool valid_watermark(const OutboxWatermark& watermark) noexcept {
    return !watermark.package_id.empty() &&
           watermark.package_id.size() <= kMaximumPackageIdBytes &&
           !watermark.candidate_id.empty() &&
           watermark.candidate_id.size() <= kMaximumCandidateIdBytes &&
           !all_zero(watermark.package_digest) &&
           !all_zero(watermark.client_id) && !all_zero(watermark.session_id) &&
           watermark.highest_sequence != 0 && !all_zero(watermark.last_hash) &&
           watermark.acknowledged_sequence <= watermark.highest_sequence &&
           (watermark.acknowledged_sequence == 0
                ? all_zero(watermark.acknowledged_hash)
                : !all_zero(watermark.acknowledged_hash));
}

bool valid_export_answer_state(const AnswerState& answer) noexcept {
    const bool valid_kind = answer.kind == AnswerKind::upsert ||
                            answer.kind == AnswerKind::clear;
    return !answer.question_id.empty() &&
           answer.question_id.size() <= kMaximumQuestionIdBytes &&
           answer.question_id.find('\0') == std::string::npos && valid_kind &&
           answer.question_revision != 0 && answer.sequence != 0 &&
           answer.answer.size() <= kMaximumAnswerBytes &&
           !all_zero(answer.event_hash) &&
           (answer.kind != AnswerKind::clear || answer.answer.empty());
}

} // namespace

struct AnswerJournal::Impl {
    struct SessionData {
        std::string package_id;
        security::Sha256Digest package_digest{};
        security::ClientId client_id{};
        SessionId session_id{};
        std::string candidate_id;
        std::uint64_t highest_sequence = 0;
        security::Sha256Digest last_hash{};
        bool finalized = false;
        std::map<std::uint64_t, security::Sha256Digest> event_hashes;
        std::map<std::string, AnswerState> answers;
    };

    mutable std::mutex mutex;
    InterprocessFileLease lease;
    std::filesystem::path path;
    bool open = false;
    bool recovered_tail = false;
    std::size_t event_count = 0;
    std::map<std::string, SessionData> sessions;
};

namespace {

template <typename Session>
bool apply_recovered(Session& session, const AnswerEvent& event,
                     const security::Sha256Digest& event_hash) {
    if (session.highest_sequence == std::numeric_limits<std::uint64_t>::max() ||
        event.sequence != session.highest_sequence + 1 ||
        !same_digest(event.previous_event_hash, session.last_hash) ||
        (!session.candidate_id.empty() &&
         session.candidate_id != event.candidate_id) ||
        session.finalized) {
        return false;
    }
    if (session.candidate_id.empty()) {
        session.candidate_id = event.candidate_id;
    }
    session.event_hashes.emplace(event.sequence, event_hash);
    session.highest_sequence = event.sequence;
    session.last_hash = event_hash;
    if (event.kind == AnswerKind::upsert) {
        session.answers.insert_or_assign(
            event.question_id,
            AnswerState{event.question_id, event.question_revision,
                        event.sequence, event.kind, event.answer, event_hash});
    } else if (event.kind == AnswerKind::clear) {
        session.answers.erase(event.question_id);
    } else {
        session.finalized = true;
    }
    return true;
}

template <typename Session>
security::Sha256Digest state_hash(const Session& session) {
    std::vector<std::byte> canonical;
    canonical.reserve(64 + session.answers.size() * 64);
    append_le(canonical, session.highest_sequence);
    append_le(canonical, static_cast<std::uint8_t>(session.finalized ? 1 : 0));
    for (const auto& [question_id, answer] : session.answers) {
        append_le(canonical, static_cast<std::uint16_t>(question_id.size()));
        canonical.insert(canonical.end(),
                         reinterpret_cast<const std::byte*>(question_id.data()),
                         reinterpret_cast<const std::byte*>(question_id.data()) +
                             question_id.size());
        append_le(canonical, answer.question_revision);
        append_le(canonical, answer.sequence);
        append_le(canonical, static_cast<std::uint8_t>(answer.kind));
        append_le(canonical, static_cast<std::uint32_t>(answer.answer.size()));
        canonical.insert(canonical.end(), answer.answer.begin(),
                         answer.answer.end());
        canonical.insert(canonical.end(), answer.event_hash.begin(),
                         answer.event_hash.end());
    }
    const auto digest = security::sha256(canonical);
    return digest.value_or(security::Sha256Digest{});
}

security::Sha256Digest state_response_hash(const StateResponse& response) {
    std::vector<const AnswerState*> answers;
    answers.reserve(response.answers.size());
    for (const auto& answer : response.answers) {
        answers.push_back(&answer);
    }
    std::sort(answers.begin(), answers.end(), [](const auto* left,
                                                 const auto* right) {
        return left->question_id < right->question_id;
    });
    std::vector<std::byte> canonical;
    canonical.reserve(64 + response.answers.size() * 64);
    append_le(canonical, response.highest_contiguous_sequence);
    append_le(canonical,
              static_cast<std::uint8_t>(response.finalized ? 1 : 0));
    for (const auto* answer : answers) {
        append_le(canonical, static_cast<std::uint16_t>(
                                 answer->question_id.size()));
        canonical.insert(
            canonical.end(),
            reinterpret_cast<const std::byte*>(answer->question_id.data()),
            reinterpret_cast<const std::byte*>(answer->question_id.data()) +
                answer->question_id.size());
        append_le(canonical, answer->question_revision);
        append_le(canonical, answer->sequence);
        append_le(canonical, static_cast<std::uint8_t>(answer->kind));
        append_le(canonical,
                  static_cast<std::uint32_t>(answer->answer.size()));
        canonical.insert(canonical.end(), answer->answer.begin(),
                         answer->answer.end());
        canonical.insert(canonical.end(), answer->event_hash.begin(),
                         answer->event_hash.end());
    }
    return security::sha256(canonical).value_or(security::Sha256Digest{});
}

template <typename Session>
AnswerAck make_ack(const Session& session, AnswerAckStatus status,
                   std::uint64_t sequence,
                   const security::Sha256Digest& event_hash) {
    AnswerAck ack;
    ack.status = status;
    ack.session_id = session.session_id;
    ack.sequence = sequence;
    ack.highest_contiguous_sequence = session.highest_sequence;
    ack.server_time_unix_milliseconds = now_milliseconds();
    ack.event_hash = event_hash;
    ack.state_hash = state_hash(session);
    return ack;
}

std::vector<std::byte> encode_journal_record(const AnswerEvent& event,
                                             const security::Sha256Digest& hash) {
    const auto payload = encode_answer_event(event);
    if (payload.empty()) {
        return {};
    }
    std::vector<std::byte> record;
    record.reserve(kJournalRecordOverhead + payload.size());
    append_le(record, kJournalMagic);
    append_le(record, kJournalVersion);
    append_le(record, static_cast<std::uint16_t>(0));
    append_le(record, static_cast<std::uint32_t>(payload.size()));
    record.insert(record.end(), payload.begin(), payload.end());
    record.insert(record.end(), hash.begin(), hash.end());
    return record;
}

std::vector<std::byte> encode_state_export(
    const StateResponse& response) {
    const auto chunks = encode_state_response_chunks(response);
    if (chunks.empty() || chunks.size() > std::numeric_limits<std::uint16_t>::max()) {
        return {};
    }
    std::size_t total = sizeof(std::uint32_t) + sizeof(std::uint16_t) * 2;
    for (const auto& chunk : chunks) {
        if (chunk.empty() ||
            chunk.size() > kMaximumJournalBytes - sizeof(std::uint32_t) ||
            chunk.size() > std::numeric_limits<std::uint32_t>::max() ||
            total > kMaximumJournalBytes - sizeof(std::uint32_t) - chunk.size()) {
            return {};
        }
        total += sizeof(std::uint32_t) + chunk.size();
    }
    std::vector<std::byte> wire;
    wire.reserve(total);
    append_le(wire, kStateExportMagic);
    append_le(wire, kStateExportVersion);
    append_le(wire, static_cast<std::uint16_t>(chunks.size()));
    for (const auto& chunk : chunks) {
        append_le(wire, static_cast<std::uint32_t>(chunk.size()));
        append_bytes(wire, chunk);
    }
    return wire.size() == total ? wire : std::vector<std::byte>{};
}

} // namespace

std::optional<StateResponse> decode_state_export(
    std::span<const std::byte> payload, std::string* error) {
    if (payload.size() < sizeof(std::uint32_t) + sizeof(std::uint16_t) * 2 ||
        payload.size() > kMaximumJournalBytes) {
        set_error(error, "state export is truncated or oversized");
        return std::nullopt;
    }
    std::size_t offset = 0;
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    std::uint16_t chunk_count = 0;
    if (!read_le(payload, offset, magic) || magic != kStateExportMagic ||
        !read_le(payload, offset, version) || version != kStateExportVersion ||
        !read_le(payload, offset, chunk_count) || chunk_count == 0 ||
        chunk_count > kMaximumStateChunks) {
        set_error(error, "state export header is invalid");
        return std::nullopt;
    }

    StateResponse merged;
    bool initialized = false;
    std::set<std::uint16_t> seen_chunks;
    std::set<std::string> question_ids;
    std::set<std::uint64_t> answer_sequences;
    for (std::uint16_t index = 0; index < chunk_count; ++index) {
        std::uint32_t chunk_bytes = 0;
        if (!read_le(payload, offset, chunk_bytes) || chunk_bytes == 0 ||
            chunk_bytes > kMaximumExamPayloadBytes || offset > payload.size() ||
            static_cast<std::size_t>(chunk_bytes) > payload.size() - offset) {
            set_error(error, "state export chunk is truncated or oversized");
            return std::nullopt;
        }
        const auto chunk = decode_state_response(
            payload.subspan(offset, static_cast<std::size_t>(chunk_bytes)));
        offset += chunk_bytes;
        if (!chunk || chunk->chunk_count != chunk_count ||
            !seen_chunks.insert(chunk->chunk_index).second) {
            set_error(error, "state export chunk metadata is invalid");
            return std::nullopt;
        }
        if (!initialized) {
            merged = *chunk;
            merged.answers.clear();
            initialized = true;
        } else if (chunk->package_id != merged.package_id ||
                   chunk->package_digest != merged.package_digest ||
                   chunk->client_id != merged.client_id ||
                   chunk->session_id != merged.session_id ||
                   chunk->candidate_id != merged.candidate_id ||
                   chunk->highest_contiguous_sequence !=
                       merged.highest_contiguous_sequence ||
                   chunk->finalized != merged.finalized ||
                   chunk->state_hash != merged.state_hash ||
                   chunk->last_event_hash != merged.last_event_hash) {
            set_error(error, "state export chunks disagree on metadata");
            return std::nullopt;
        }
        for (const auto& answer : chunk->answers) {
            if (!valid_export_answer_state(answer) ||
                answer.sequence > merged.highest_contiguous_sequence ||
                !question_ids.insert(answer.question_id).second ||
                !answer_sequences.insert(answer.sequence).second ||
                merged.answers.size() >= kMaximumStateEntries) {
                set_error(error, "state export contains an invalid answer set");
                return std::nullopt;
            }
            merged.answers.push_back(answer);
        }
    }
    if (offset != payload.size() || seen_chunks.size() != chunk_count ||
        !initialized) {
        set_error(error, "state export is incomplete");
        return std::nullopt;
    }
    merged.chunk_index = 0;
    merged.chunk_count = 1;
    // Exports may contain several command-sized chunks. Validate the merged
    // snapshot through the chunking path instead of the single-frame encoder;
    // otherwise a perfectly valid large backup would be rejected merely
    // because it cannot fit in one authenticated command.
    if (encode_state_response_chunks(merged).empty() ||
        (merged.highest_contiguous_sequence != 0 &&
         !same_digest(state_response_hash(merged), merged.state_hash))) {
        set_error(error, "state export snapshot is invalid");
        return std::nullopt;
    }
    return merged;
}

AnswerJournal::AnswerJournal() = default;

AnswerJournal::~AnswerJournal() { close(); }

bool AnswerJournal::open(const std::filesystem::path& path,
                         std::string* error) {
    if (!valid_path(path)) {
        set_error(error, "journal path must be an absolute non-root path");
        return false;
    }
    if (!impl_) {
        impl_ = std::make_unique<Impl>();
    }
    std::scoped_lock lock(impl_->mutex);
    impl_->lease.release();
    impl_->path = path;
    impl_->open = false;
    impl_->sessions.clear();
    impl_->event_count = 0;
    impl_->recovered_tail = false;
    std::error_code directory_error;
    std::filesystem::create_directories(path.parent_path(), directory_error);
    if (directory_error) {
        set_error(error, "journal directory creation failed");
        return false;
    }
    InterprocessFileLease lease;
    if (!lease.acquire(path, error)) {
        return false;
    }
    std::error_code path_status_error;
    const bool path_exists = std::filesystem::exists(path, path_status_error);
    if (path_status_error) {
        set_error(error, "journal file status query failed");
        return false;
    }
    if (!path_exists) {
        if (!write_all_file(path, {}, error)) {
            return false;
        }
    }
    std::string read_error;
    const auto bytes = read_file(path, &read_error);
    if (!read_error.empty()) {
        if (error != nullptr) {
            *error = read_error;
        }
        return false;
    }
    std::size_t offset = 0;
    std::size_t valid_offset = 0;
    while (offset < bytes.size()) {
        if (bytes.size() - offset < kJournalHeaderBytes) {
            impl_->recovered_tail = true;
            break;
        }
        std::size_t header_offset = offset;
        std::uint32_t magic = 0;
        std::uint16_t version = 0;
        std::uint16_t reserved = 0;
        std::uint32_t payload_bytes = 0;
        if (!read_le(bytes, header_offset, magic) ||
            !read_le(bytes, header_offset, version) ||
            !read_le(bytes, header_offset, reserved) ||
            !read_le(bytes, header_offset, payload_bytes) ||
            magic != kJournalMagic || version != kJournalVersion ||
            reserved != 0 || payload_bytes == 0 ||
            payload_bytes > 64u * 1024u) {
            set_error(error, "journal contains a corrupt record header");
            impl_->open = false;
            return false;
        }
        const std::size_t record_bytes = kJournalRecordOverhead + payload_bytes;
        if (bytes.size() - offset < record_bytes) {
            impl_->recovered_tail = true;
            break;
        }
        const auto payload = std::span<const std::byte>(bytes).subspan(
            offset + kJournalHeaderBytes, payload_bytes);
        const auto stored_hash = std::span<const std::byte>(bytes).subspan(
            offset + kJournalHeaderBytes + payload_bytes,
            security::kSha256Bytes);
        std::string decode_error;
        const auto event = decode_answer_event(payload, &decode_error);
        const auto calculated_hash = event ? hash_answer_event(*event)
                                           : std::nullopt;
        if (!event || !calculated_hash ||
            !same_digest(*calculated_hash, stored_hash)) {
            set_error(error, "journal contains a corrupt event record");
            impl_->open = false;
            return false;
        }
        const auto key = session_key(*event);
        if (!impl_->sessions.contains(key) &&
            impl_->sessions.size() >= kMaximumJournalSessions) {
            set_error(error, "journal contains too many sessions");
            impl_->open = false;
            return false;
        }
        auto [found, inserted] = impl_->sessions.try_emplace(key);
        auto& session = found->second;
        if (inserted) {
            session.package_id = event->package_id;
            session.package_digest = event->package_digest;
            session.client_id = event->client_id;
            session.session_id = event->session_id;
            session.candidate_id = event->candidate_id;
        }
        if (session.candidate_id != event->candidate_id) {
            set_error(error, "journal candidate identity changed");
            impl_->open = false;
            return false;
        }
        if (event->kind == AnswerKind::upsert &&
            !session.answers.contains(event->question_id) &&
            session.answers.size() >= kMaximumStateEntries) {
            set_error(error, "journal answer-state quota exceeded");
            impl_->open = false;
            return false;
        }
        if (!apply_recovered(session, *event, *calculated_hash)) {
            set_error(error, "journal event sequence or hash chain is invalid");
            impl_->open = false;
            return false;
        }
        ++impl_->event_count;
        offset += record_bytes;
        valid_offset = offset;
    }
    if (impl_->recovered_tail && valid_offset < bytes.size()) {
        std::error_code truncate_error;
        std::filesystem::resize_file(path, valid_offset, truncate_error);
        if (truncate_error) {
            set_error(error, "journal incomplete tail could not be truncated");
            impl_->open = false;
            return false;
        }
    }
    impl_->open = true;
    impl_->lease = std::move(lease);
    return true;
}

void AnswerJournal::close() noexcept {
    if (!impl_) {
        return;
    }
    std::scoped_lock lock(impl_->mutex);
    impl_->open = false;
    impl_->lease.release();
}

bool AnswerJournal::is_open() const noexcept {
    if (!impl_) {
        return false;
    }
    std::scoped_lock lock(impl_->mutex);
    return impl_->open;
}

bool AnswerJournal::recovered_truncated_tail() const noexcept {
    if (!impl_) {
        return false;
    }
    std::scoped_lock lock(impl_->mutex);
    return impl_->recovered_tail;
}

AppendOutcome AnswerJournal::append(const AnswerEvent& event,
                                    std::string* error) {
    AppendOutcome outcome;
    if (!impl_) {
        outcome.status = AppendStatus::unavailable;
        set_error(error, "answer journal is not open");
        return outcome;
    }
    std::scoped_lock lock(impl_->mutex);
    if (!impl_->open) {
        outcome.status = AppendStatus::unavailable;
        set_error(error, "answer journal is not open");
        return outcome;
    }
    if (!validate_answer_event(event, error)) {
        outcome.status = AppendStatus::rejected;
        return outcome;
    }
    const auto calculated_hash = hash_answer_event(event);
    if (!calculated_hash) {
        outcome.status = AppendStatus::unavailable;
        set_error(error, "answer event hashing failed");
        return outcome;
    }
    const auto key = session_key(event);
    if (!impl_->sessions.contains(key) &&
        impl_->sessions.size() >= kMaximumJournalSessions) {
        outcome.status = AppendStatus::unavailable;
        set_error(error, "journal session quota is exhausted");
        return outcome;
    }
    auto [found, inserted] = impl_->sessions.try_emplace(key);
    auto& session = found->second;
    if (inserted) {
        session.package_id = event.package_id;
        session.package_digest = event.package_digest;
        session.client_id = event.client_id;
        session.session_id = event.session_id;
        session.candidate_id = event.candidate_id;
    }
    const auto discard_inserted_session = [&]() noexcept {
        if (inserted) {
            // A rejected first event must not consume a session quota slot.
            // The entry has no durable record and can be safely discarded.
            impl_->sessions.erase(found);
        }
    };
    if (session.candidate_id != event.candidate_id) {
        outcome.status = AppendStatus::conflict;
        outcome.ack = make_ack(session, AnswerAckStatus::conflict,
                               event.sequence, *calculated_hash);
        set_error(error, "answer candidate identity conflicts with the session");
        discard_inserted_session();
        return outcome;
    }
    const auto existing = session.event_hashes.find(event.sequence);
    if (existing != session.event_hashes.end()) {
        if (!same_digest(existing->second, *calculated_hash)) {
            outcome.status = AppendStatus::conflict;
            outcome.ack = make_ack(session, AnswerAckStatus::conflict,
                                   event.sequence, existing->second);
            set_error(error, "answer sequence conflicts with an existing event");
            return outcome;
        }
        outcome.status = AppendStatus::duplicate;
        outcome.ack = make_ack(session, AnswerAckStatus::duplicate,
                               event.sequence, existing->second);
        return outcome;
    }
    if (event.kind == AnswerKind::upsert &&
        !session.answers.contains(event.question_id) &&
        session.answers.size() >= kMaximumStateEntries) {
        outcome.status = AppendStatus::rejected;
        outcome.ack = make_ack(session, AnswerAckStatus::rejected,
                               event.sequence, *calculated_hash);
        set_error(error, "answer-state quota is exhausted");
        discard_inserted_session();
        return outcome;
    }
    if (session.finalized) {
        outcome.status = AppendStatus::rejected;
        outcome.ack = make_ack(session, AnswerAckStatus::rejected,
                               event.sequence, *calculated_hash);
        set_error(error, "exam session is already finalized");
        discard_inserted_session();
        return outcome;
    }
    if (session.highest_sequence == std::numeric_limits<std::uint64_t>::max() ||
        event.sequence != session.highest_sequence + 1) {
        outcome.status = AppendStatus::gap;
        outcome.ack = make_ack(session, AnswerAckStatus::gap,
                               event.sequence, *calculated_hash);
        set_error(error, "answer event sequence has a gap");
        discard_inserted_session();
        return outcome;
    }
    if (!same_digest(event.previous_event_hash, session.last_hash)) {
        outcome.status = AppendStatus::conflict;
        outcome.ack = make_ack(session, AnswerAckStatus::conflict,
                               event.sequence, *calculated_hash);
        set_error(error, "answer event hash chain does not match the journal");
        discard_inserted_session();
        return outcome;
    }
    const auto record = encode_journal_record(event, *calculated_hash);
    std::error_code file_size_error;
    const auto current_size = std::filesystem::file_size(impl_->path,
                                                          file_size_error);
    if (record.empty() || file_size_error || record.size() >
            kMaximumJournalBytes || current_size >
            kMaximumJournalBytes - record.size() ||
        !append_file(impl_->path, record, error)) {
        // Do not append again after a storage failure. Reopening verifies the
        // on-disk tail and truncates only an incomplete final record.
        impl_->open = false;
        outcome.ack = make_ack(session, AnswerAckStatus::unavailable,
                               event.sequence, *calculated_hash);
        discard_inserted_session();
        outcome.status = AppendStatus::unavailable;
        return outcome;
    }
    if (!apply_recovered(session, event, *calculated_hash)) {
        discard_inserted_session();
        outcome.status = AppendStatus::unavailable;
        set_error(error, "answer event could not be indexed after append");
        return outcome;
    }
    ++impl_->event_count;
    outcome.status = AppendStatus::accepted;
    outcome.ack = make_ack(session, AnswerAckStatus::accepted, event.sequence,
                           *calculated_hash);
    return outcome;
}

std::optional<StateResponse> AnswerJournal::state(
    const StateRequest& request, std::string* error) const {
    if (!impl_) {
        set_error(error, "answer journal is not open");
        return std::nullopt;
    }
    std::scoped_lock lock(impl_->mutex);
    if (!impl_->open) {
        set_error(error, "answer journal is not open");
        return std::nullopt;
    }
    if (encode_state_request(request).empty()) {
        set_error(error, "invalid answer state request");
        return std::nullopt;
    }
    const auto found = impl_->sessions.find(session_key(request));
    if (found == impl_->sessions.end()) {
        StateResponse empty;
        empty.package_id = request.package_id;
        empty.package_digest = request.package_digest;
        empty.client_id = request.client_id;
        empty.session_id = request.session_id;
        empty.candidate_id = request.candidate_id;
        empty.state_hash = security::Sha256Digest{};
        return empty;
    }
    const auto& session = found->second;
    if (session.candidate_id != request.candidate_id) {
        set_error(error, "answer state candidate identity does not match");
        return std::nullopt;
    }
    StateResponse response;
    response.package_id = session.package_id;
    response.package_digest = session.package_digest;
    response.client_id = session.client_id;
    response.session_id = session.session_id;
    response.candidate_id = session.candidate_id;
    response.highest_contiguous_sequence = session.highest_sequence;
    response.finalized = session.finalized;
    response.state_hash = state_hash(session);
    response.last_event_hash = session.last_hash;
    response.answers.reserve(session.answers.size());
    for (const auto& [question_id, answer] : session.answers) {
        (void)question_id;
        response.answers.push_back(answer);
    }
    return response;
}

bool AnswerJournal::export_state(const StateRequest& request,
                                 const std::filesystem::path& path,
                                 std::string* error) const {
    if (!valid_path(path)) {
        set_error(error, "state export path must be an absolute non-root path");
        return false;
    }
    if (!impl_) {
        set_error(error, "answer journal is not open");
        return false;
    }
    {
        std::scoped_lock lock(impl_->mutex);
        if (!impl_->open) {
            set_error(error, "answer journal is not open");
            return false;
        }
        if (aliases_path(path, impl_->path)) {
            set_error(error, "state export path must differ from the journal");
            return false;
        }
    }
    const auto snapshot = state(request, error);
    if (!snapshot) {
        return false;
    }
    const auto wire = encode_state_export(*snapshot);
    if (wire.empty()) {
        set_error(error, "answer state export exceeds the configured limit");
        return false;
    }
    return write_all_file(path, wire, error);
}

std::size_t AnswerJournal::event_count() const noexcept {
    if (!impl_) {
        return 0;
    }
    std::scoped_lock lock(impl_->mutex);
    return impl_->event_count;
}

struct AnswerOutbox::Impl {
    mutable std::mutex mutex;
    InterprocessFileLease lease;
    std::filesystem::path path;
    bool open = false;
    std::vector<AnswerEvent> events;
    std::map<std::string, OutboxWatermark> watermarks;
};

namespace {

bool append_bounded_string(std::vector<std::byte>& wire,
                           std::string_view value,
                           std::size_t maximum) {
    if (value.empty() || value.size() > maximum ||
        value.size() > std::numeric_limits<std::uint16_t>::max()) {
        return false;
    }
    append_le(wire, static_cast<std::uint16_t>(value.size()));
    wire.insert(wire.end(), reinterpret_cast<const std::byte*>(value.data()),
                reinterpret_cast<const std::byte*>(value.data()) +
                    value.size());
    return true;
}

bool read_bounded_string(std::span<const std::byte> bytes, std::size_t& offset,
                         std::size_t maximum, std::string& value) {
    std::uint16_t length = 0;
    if (!read_le(bytes, offset, length) || length == 0 ||
        length > maximum || offset > bytes.size() ||
        static_cast<std::size_t>(length) > bytes.size() - offset) {
        return false;
    }
    value.assign(reinterpret_cast<const char*>(bytes.data() + offset), length);
    offset += length;
    return value.find('\0') == std::string::npos;
}

std::vector<std::byte> encode_outbox(
    const std::vector<AnswerEvent>& events,
    const std::map<std::string, OutboxWatermark>& watermarks) {
    if (events.size() > kMaximumOutboxEvents ||
        watermarks.size() > kMaximumOutboxWatermarks) {
        return {};
    }
    std::vector<std::byte> wire;
    wire.reserve(24);
    append_le(wire, kOutboxMagic);
    append_le(wire, kOutboxVersion);
    append_le(wire, static_cast<std::uint16_t>(0));
    append_le(wire, static_cast<std::uint32_t>(events.size()));
    append_le(wire, static_cast<std::uint32_t>(watermarks.size()));
    for (const auto& [key, watermark] : watermarks) {
        (void)key;
        if (!valid_watermark(watermark) ||
            !append_bounded_string(wire, watermark.package_id,
                                   kMaximumPackageIdBytes) ||
            !append_bounded_string(wire, watermark.candidate_id,
                                   kMaximumCandidateIdBytes)) {
            return {};
        }
        append_bytes(wire, watermark.package_digest);
        append_bytes(wire, watermark.client_id);
        append_bytes(wire, watermark.session_id);
        append_le(wire, watermark.highest_sequence);
        append_bytes(wire, watermark.last_hash);
        append_le(wire, watermark.acknowledged_sequence);
        append_bytes(wire, watermark.acknowledged_hash);
        append_le(wire, static_cast<std::uint8_t>(watermark.finalized ? 1 : 0));
    }
    for (const auto& event : events) {
        auto payload = encode_answer_event(event);
        if (payload.empty() || payload.size() > kMaximumWireBytes) {
            security::secure_zero(payload);
            return {};
        }
        append_le(wire, static_cast<std::uint32_t>(payload.size()));
        wire.insert(wire.end(), payload.begin(), payload.end());
        security::secure_zero(payload);
    }
    return wire.size() <= kMaximumOutboxBytes ? wire
                                               : std::vector<std::byte>{};
}

OutboxWatermark watermark_for(const AnswerEvent& event,
                              const security::Sha256Digest& event_hash,
                              std::uint64_t highest_sequence) {
    OutboxWatermark watermark;
    watermark.package_id = event.package_id;
    watermark.package_digest = event.package_digest;
    watermark.client_id = event.client_id;
    watermark.session_id = event.session_id;
    watermark.candidate_id = event.candidate_id;
    watermark.highest_sequence = highest_sequence;
    watermark.last_hash = event_hash;
    watermark.finalized = event.kind == AnswerKind::finalize;
    return watermark;
}

bool same_watermark_context(const OutboxWatermark& watermark,
                            const AnswerEvent& event) noexcept {
    return watermark.package_id == event.package_id &&
           watermark.package_digest == event.package_digest &&
           watermark.client_id == event.client_id &&
           watermark.session_id == event.session_id &&
           watermark.candidate_id == event.candidate_id;
}

// A browser may rebase a pending event when it learns the authoritative
// predecessor hash after reconnecting.  The answer itself must remain
// identical; only the predecessor (and therefore the derived event hash) may
// change.  Keeping this comparison strict prevents a rebase path from being
// used to replace an answer at an already allocated sequence.
bool same_event_body_except_predecessor(const AnswerEvent& left,
                                        const AnswerEvent& right) noexcept {
    return left.package_id == right.package_id &&
           left.package_digest == right.package_digest &&
           left.client_id == right.client_id &&
           left.session_id == right.session_id &&
           left.candidate_id == right.candidate_id &&
           left.question_id == right.question_id &&
           left.question_revision == right.question_revision &&
           left.sequence == right.sequence &&
           left.client_time_unix_milliseconds ==
               right.client_time_unix_milliseconds &&
           left.kind == right.kind && left.answer == right.answer;
}

std::span<const std::byte> outbox_entropy() noexcept {
    return {reinterpret_cast<const std::byte*>(kOutboxEntropyText),
            sizeof(kOutboxEntropyText) - 1};
}

std::vector<std::byte> protect_outbox_wire(std::span<const std::byte> plaintext,
                                           std::string* error) {
    if (plaintext.empty() || plaintext.size() > kMaximumOutboxBytes ||
        plaintext.size() > std::numeric_limits<std::uint32_t>::max()) {
        set_error(error, "outbox plaintext exceeds the configured quota");
        return {};
    }
    auto protected_blob = security::protect_machine_secret(
        plaintext, outbox_entropy(), error);
    if (protected_blob.empty()) {
        return {};
    }
    if (protected_blob.size() > kMaximumStoredOutboxBytes - 12u) {
        security::secure_zero(protected_blob);
        set_error(error, "encrypted outbox exceeds the configured quota");
        return {};
    }
    std::vector<std::byte> wire;
    wire.reserve(12 + protected_blob.size());
    append_le(wire, kEncryptedOutboxMagic);
    append_le(wire, static_cast<std::uint16_t>(1));
    append_le(wire, static_cast<std::uint16_t>(0));
    append_le(wire, static_cast<std::uint32_t>(plaintext.size()));
    append_bytes(wire, protected_blob);
    security::secure_zero(protected_blob);
    if (wire.size() > kMaximumStoredOutboxBytes) {
        security::secure_zero(wire);
        set_error(error, "encrypted outbox exceeds the configured quota");
        return {};
    }
    return wire;
}

std::vector<std::byte> unprotect_outbox_wire(
    std::span<const std::byte> stored, std::string* error) {
    if (stored.size() < 12 || stored.size() > kMaximumStoredOutboxBytes) {
        return {};
    }
    std::size_t offset = 0;
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    std::uint16_t reserved = 0;
    std::uint32_t plaintext_size = 0;
    if (!read_le(stored, offset, magic) || magic != kEncryptedOutboxMagic ||
        !read_le(stored, offset, version) || version != 1 ||
        !read_le(stored, offset, reserved) || reserved != 0 ||
        !read_le(stored, offset, plaintext_size) || plaintext_size == 0 ||
        plaintext_size > kMaximumOutboxBytes || offset >= stored.size()) {
        set_error(error, "encrypted outbox header is invalid");
        return {};
    }
    const auto protected_blob = stored.subspan(offset);
    auto plaintext = security::unprotect_machine_secret(
        protected_blob, outbox_entropy(), error);
    if (plaintext.size() != plaintext_size) {
        security::secure_zero(plaintext);
        set_error(error, "encrypted outbox payload size is invalid");
        return {};
    }
    return plaintext;
}

bool write_outbox_file(const std::filesystem::path& path,
                       std::span<const std::byte> plaintext,
                       std::string* error) {
    auto stored = protect_outbox_wire(plaintext, error);
    if (stored.empty()) {
        return false;
    }
    const bool written = write_all_file(path, stored, error);
    security::secure_zero(stored);
    return written;
}

} // namespace

AnswerOutbox::AnswerOutbox() = default;

AnswerOutbox::~AnswerOutbox() { close(); }

bool AnswerOutbox::open(const std::filesystem::path& path,
                        std::string* error) {
    if (!valid_path(path)) {
        set_error(error, "outbox path must be an absolute non-root path");
        return false;
    }
    if (!impl_) {
        impl_ = std::make_unique<Impl>();
    }
    std::scoped_lock lock(impl_->mutex);
    impl_->lease.release();
    impl_->path = path;
    impl_->open = false;
    impl_->events.clear();
    impl_->watermarks.clear();
    std::error_code directory_error;
    std::filesystem::create_directories(path.parent_path(), directory_error);
    if (directory_error) {
        set_error(error, "outbox directory creation failed");
        return false;
    }
    InterprocessFileLease lease;
    if (!lease.acquire(path, error)) {
        return false;
    }
    std::error_code path_status_error;
    const bool path_exists = std::filesystem::exists(path, path_status_error);
    if (path_status_error) {
        set_error(error, "outbox file status query failed");
        return false;
    }
    if (!path_exists) {
        auto wire = encode_outbox({}, {});
        const bool written = !wire.empty() && write_outbox_file(path, wire, error);
        security::secure_zero(wire);
        if (!written) {
            return false;
        }
    }
    std::string read_error;
    auto stored = read_file(path, &read_error, kMaximumStoredOutboxBytes);
    if (!read_error.empty()) {
        if (error != nullptr) *error = read_error;
        return false;
    }
    if (stored.size() > kMaximumStoredOutboxBytes) {
        set_error(error, "outbox exceeds the configured quota");
        return false;
    }
    std::vector<std::byte> bytes;
    SensitiveBytesGuard bytes_guard(bytes);
    bool encrypted = false;
    if (stored.size() >= sizeof(std::uint32_t)) {
        std::size_t magic_offset = 0;
        std::uint32_t stored_magic = 0;
        if (!read_le(stored, magic_offset, stored_magic)) {
            security::secure_zero(stored);
            set_error(error, "outbox header is invalid");
            return false;
        }
        if (stored_magic == kEncryptedOutboxMagic) {
            std::string decrypt_error;
            bytes = unprotect_outbox_wire(stored, &decrypt_error);
            security::secure_zero(stored);
            if (bytes.empty()) {
                if (error != nullptr) {
                    *error = decrypt_error.empty()
                                 ? "encrypted outbox could not be opened"
                                 : decrypt_error;
                }
                return false;
            }
            encrypted = true;
        } else {
            bytes = std::move(stored);
        }
    } else {
        bytes = std::move(stored);
    }
    std::size_t offset = 0;
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    std::uint16_t reserved = 0;
    std::uint32_t count = 0;
    if (!read_le(bytes, offset, magic) || magic != kOutboxMagic ||
        !read_le(bytes, offset, version) ||
        version < kOutboxLegacyVersion || version > kOutboxVersion ||
        (version >= kOutboxWatermarkVersion &&
         (!read_le(bytes, offset, reserved) || reserved != 0)) ||
        !read_le(bytes, offset, count) || count > kMaximumOutboxEvents) {
        set_error(error, "outbox header is invalid");
        return false;
    }
    std::uint32_t watermark_count = 0;
    const bool has_watermarks = version >= kOutboxWatermarkVersion;
    const bool has_acknowledgement_boundary =
        version >= kOutboxAcknowledgementVersion;
    const bool has_finalization_marker = version >= kOutboxFinalizationVersion;
    if (has_watermarks &&
        (!read_le(bytes, offset, watermark_count) ||
         watermark_count > kMaximumOutboxWatermarks)) {
        set_error(error, "outbox watermark header is invalid");
        return false;
    }
    for (std::uint32_t index = 0; index < watermark_count; ++index) {
        OutboxWatermark watermark;
        if (!read_bounded_string(bytes, offset, kMaximumPackageIdBytes,
                                 watermark.package_id) ||
            !read_bounded_string(bytes, offset, kMaximumCandidateIdBytes,
                                 watermark.candidate_id) ||
            !read_bytes(bytes, offset, watermark.package_digest) ||
            !read_bytes(bytes, offset, watermark.client_id) ||
            !read_bytes(bytes, offset, watermark.session_id) ||
            !read_le(bytes, offset, watermark.highest_sequence) ||
            !read_bytes(bytes, offset, watermark.last_hash)) {
            set_error(error, "outbox watermark is invalid");
            impl_->watermarks.clear();
            return false;
        }
        if (has_acknowledgement_boundary &&
            (!read_le(bytes, offset, watermark.acknowledged_sequence) ||
             !read_bytes(bytes, offset, watermark.acknowledged_hash))) {
            set_error(error, "outbox acknowledgement watermark is invalid");
            impl_->watermarks.clear();
            return false;
        }
        if (has_finalization_marker) {
            std::uint8_t finalized = 0;
            if (!read_le(bytes, offset, finalized) || finalized > 1) {
                set_error(error, "outbox finalization watermark is invalid");
                impl_->watermarks.clear();
                return false;
            }
            watermark.finalized = finalized != 0;
        }
        if (!valid_watermark(watermark)) {
            set_error(error, "outbox watermark is invalid");
            impl_->watermarks.clear();
            return false;
        }
        const auto key = session_key(watermark.package_id,
                                     watermark.package_digest,
                                     watermark.client_id,
                                     watermark.session_id);
        if (!impl_->watermarks.emplace(key, std::move(watermark)).second) {
            set_error(error, "outbox contains duplicate watermark");
            impl_->watermarks.clear();
            return false;
        }
    }
    impl_->events.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        std::uint32_t event_bytes = 0;
        if (!read_le(bytes, offset, event_bytes) || event_bytes == 0 ||
            event_bytes > kMaximumWireBytes || offset > bytes.size() ||
            event_bytes > bytes.size() - offset) {
            set_error(error, "outbox record is truncated");
            impl_->events.clear();
            impl_->watermarks.clear();
            return false;
        }
        const auto event = decode_answer_event(
            std::span<const std::byte>(bytes).subspan(offset, event_bytes));
        if (!event) {
            set_error(error, "outbox record is invalid");
            impl_->events.clear();
            impl_->watermarks.clear();
            return false;
        }
        const auto event_hash = hash_answer_event(*event);
        if (!event_hash || std::any_of(
                impl_->events.begin(), impl_->events.end(), [&](const auto& item) {
                    return session_key(item) == session_key(*event) &&
                           item.sequence == event->sequence;
                })) {
            set_error(error, "outbox contains a duplicate or invalid event");
            impl_->events.clear();
            impl_->watermarks.clear();
            return false;
        }
        const auto key = session_key(*event);
        auto watermark = impl_->watermarks.find(key);
        if (watermark != impl_->watermarks.end() &&
            (!same_watermark_context(watermark->second, *event) ||
             (version != kOutboxLegacyVersion &&
              event->sequence > watermark->second.highest_sequence))) {
            set_error(error, "outbox event exceeds its watermark");
            impl_->events.clear();
            impl_->watermarks.clear();
            return false;
        }
        impl_->events.push_back(*event);
        if (watermark == impl_->watermarks.end()) {
            OutboxWatermark initial;
            initial.package_id = event->package_id;
            initial.package_digest = event->package_digest;
            initial.client_id = event->client_id;
            initial.session_id = event->session_id;
            initial.candidate_id = event->candidate_id;
            initial.highest_sequence = event->sequence;
            initial.last_hash = *event_hash;
            initial.finalized = event->kind == AnswerKind::finalize;
            impl_->watermarks.emplace(key, std::move(initial));
        } else if (version == kOutboxLegacyVersion &&
                   watermark->second.highest_sequence < event->sequence) {
            watermark->second.highest_sequence = event->sequence;
            watermark->second.last_hash = *event_hash;
        }
        if (!has_finalization_marker && event->kind == AnswerKind::finalize &&
            watermark != impl_->watermarks.end()) {
            watermark->second.finalized = true;
        }
        offset += event_bytes;
    }
    std::map<std::string, std::vector<const AnswerEvent*>> pending_by_session;
    for (const auto& event : impl_->events) {
        pending_by_session[session_key(event)].push_back(&event);
    }
    for (auto& [key, pending_events] : pending_by_session) {
        std::sort(pending_events.begin(), pending_events.end(),
                  [](const auto* left, const auto* right) {
                      return left->sequence < right->sequence;
                  });
        auto watermark = impl_->watermarks.find(key);
        if (watermark == impl_->watermarks.end()) {
            set_error(error, "outbox session watermark is missing");
            impl_->events.clear();
            impl_->watermarks.clear();
            return false;
        }
        auto& saved = watermark->second;
        bool pending_finalized = false;
        for (const auto* event : pending_events) {
            if (pending_finalized) {
                set_error(error, "outbox contains an event after finalization");
                impl_->events.clear();
                impl_->watermarks.clear();
                return false;
            }
            if (event->kind == AnswerKind::finalize) {
                pending_finalized = true;
            }
        }
        if (!has_finalization_marker) {
            // Older formats did not persist this boundary. It can be
            // recovered when the final event is still pending.
            saved.finalized = pending_finalized;
        } else if (pending_finalized && !saved.finalized) {
            set_error(error, "outbox finalization marker is missing");
            impl_->events.clear();
            impl_->watermarks.clear();
            return false;
        } else if (saved.finalized && !pending_finalized &&
                   !pending_events.empty()) {
            set_error(error, "outbox finalized watermark has pending events");
            impl_->events.clear();
            impl_->watermarks.clear();
            return false;
        }
        if (!has_acknowledgement_boundary) {
            // Legacy files did not persist the acknowledged boundary. Infer
            // it from the first pending event so they can be upgraded safely.
            if (pending_events.front()->sequence == 1) {
                saved.acknowledged_sequence = 0;
                saved.acknowledged_hash = {};
            } else {
                saved.acknowledged_sequence =
                    pending_events.front()->sequence - 1;
                saved.acknowledged_hash =
                    pending_events.front()->previous_event_hash;
            }
        }
        if (!valid_watermark(saved)) {
            set_error(error, "outbox inferred watermark is invalid");
            impl_->events.clear();
            impl_->watermarks.clear();
            return false;
        }
        if (saved.acknowledged_sequence ==
                std::numeric_limits<std::uint64_t>::max() ||
            saved.acknowledged_sequence + 1 !=
                pending_events.front()->sequence) {
            set_error(error, "outbox pending sequence has a gap");
            impl_->events.clear();
            impl_->watermarks.clear();
            return false;
        }
        if (saved.acknowledged_sequence == 0) {
            if (!all_zero(pending_events.front()->previous_event_hash)) {
                set_error(error, "outbox first event hash boundary is invalid");
                impl_->events.clear();
                impl_->watermarks.clear();
                return false;
            }
        } else if (!same_digest(pending_events.front()->previous_event_hash,
                                saved.acknowledged_hash)) {
            set_error(error, "outbox acknowledged hash boundary is invalid");
            impl_->events.clear();
            impl_->watermarks.clear();
            return false;
        }
        for (std::size_t index = 1; index < pending_events.size(); ++index) {
            const auto& previous = *pending_events[index - 1];
            const auto& current = *pending_events[index];
            if (previous.sequence == std::numeric_limits<std::uint64_t>::max() ||
                current.sequence != previous.sequence + 1) {
                set_error(error, "outbox pending events are not contiguous");
                impl_->events.clear();
                impl_->watermarks.clear();
                return false;
            }
            const auto previous_hash = hash_answer_event(previous);
            if (!previous_hash ||
                !same_digest(current.previous_event_hash, *previous_hash)) {
                set_error(error, "outbox pending hash chain is invalid");
                impl_->events.clear();
                impl_->watermarks.clear();
                return false;
            }
        }
        const auto last_hash = hash_answer_event(*pending_events.back());
        if (!last_hash || pending_events.back()->sequence != saved.highest_sequence ||
            !same_digest(saved.last_hash, *last_hash)) {
            set_error(error, "outbox watermark does not match pending events");
            impl_->events.clear();
            impl_->watermarks.clear();
            return false;
        }
    }
    for (auto& [key, saved] : impl_->watermarks) {
        (void)key;
        if (!pending_by_session.contains(key)) {
            if (saved.acknowledged_sequence != saved.highest_sequence ||
                !same_digest(saved.acknowledged_hash, saved.last_hash)) {
                if (!has_acknowledgement_boundary) {
                    saved.acknowledged_sequence = saved.highest_sequence;
                    saved.acknowledged_hash = saved.last_hash;
                } else {
                    set_error(error, "outbox acknowledged watermark is invalid");
                    impl_->events.clear();
                    impl_->watermarks.clear();
                    return false;
                }
            }
        }
    }
    if (offset != bytes.size()) {
        set_error(error, "outbox has trailing data");
        impl_->events.clear();
        impl_->watermarks.clear();
        return false;
    }
    // New writes are always encrypted v4. Upgrade legacy plaintext files and
    // older serialized versions before exposing the outbox to callers.
    if (!encrypted || version != kOutboxVersion) {
        auto migrated = encode_outbox(impl_->events, impl_->watermarks);
        const bool written = !migrated.empty() &&
            write_outbox_file(path, migrated, error);
        security::secure_zero(migrated);
        if (!written) {
            impl_->events.clear();
            impl_->watermarks.clear();
            return false;
        }
    }
    impl_->open = true;
    impl_->lease = std::move(lease);
    return true;
}

void AnswerOutbox::close() noexcept {
    if (!impl_) return;
    std::scoped_lock lock(impl_->mutex);
    impl_->open = false;
    impl_->lease.release();
}

bool AnswerOutbox::is_open() const noexcept {
    if (!impl_) {
        return false;
    }
    std::scoped_lock lock(impl_->mutex);
    return impl_->open;
}

bool AnswerOutbox::enqueue(const AnswerEvent& event, std::string* error) {
    if (!impl_) {
        set_error(error, "outbox is not open");
        return false;
    }
    std::scoped_lock lock(impl_->mutex);
    if (!impl_->open || !validate_answer_event(event, error)) {
        if (!impl_->open) set_error(error, "outbox is not open");
        return false;
    }
    const auto event_hash = hash_answer_event(event);
    if (!event_hash) {
        set_error(error, "outbox event hashing failed");
        return false;
    }
    const auto key = session_key(event);
    auto watermark = impl_->watermarks.find(key);
    for (std::size_t index = 0; index < impl_->events.size(); ++index) {
        const auto& existing = impl_->events[index];
        if (session_key(existing) != key || existing.sequence != event.sequence) {
            continue;
        }
        const auto existing_hash = hash_answer_event(existing);
        if (existing_hash && same_digest(*existing_hash, *event_hash)) {
            return true;
        }
        if (!same_event_body_except_predecessor(existing, event) ||
            watermark == impl_->watermarks.end()) {
            set_error(error, "outbox sequence conflicts with an existing event");
            return false;
        }
        if (watermark->second.finalized ||
            event.sequence <= watermark->second.acknowledged_sequence) {
            set_error(error, "outbox sequence was already acknowledged");
            return false;
        }

        // Rebase the existing record and every later pending record as one
        // atomic write.  Browser recovery can learn a new predecessor hash
        // after a reconnect; retaining the old serialized record would make
        // the same logical event fail forever on the next enqueue.  Only the
        // first pending event may change the acknowledged boundary, and its
        // answer/body remains byte-for-byte identical.
        auto candidate_events = impl_->events;
        auto candidate_watermarks = impl_->watermarks;
        auto& candidate_watermark = candidate_watermarks.find(key)->second;
        std::vector<std::size_t> session_indices;
        for (std::size_t candidate_index = 0;
             candidate_index < candidate_events.size(); ++candidate_index) {
            if (session_key(candidate_events[candidate_index]) == key) {
                session_indices.push_back(candidate_index);
            }
        }
        std::sort(session_indices.begin(), session_indices.end(),
                  [&](std::size_t left, std::size_t right) {
                      return candidate_events[left].sequence <
                             candidate_events[right].sequence;
                  });
        const auto target_position = std::find(
            session_indices.begin(), session_indices.end(), index);
        if (target_position == session_indices.end()) {
            set_error(error, "outbox rebase target is unavailable");
            return false;
        }
        const auto target_offset = static_cast<std::size_t>(
            std::distance(session_indices.begin(), target_position));
        if (target_offset == 0) {
            if (candidate_watermark.acknowledged_sequence ==
                    std::numeric_limits<std::uint64_t>::max() ||
                event.sequence != candidate_watermark.acknowledged_sequence + 1 ||
                (event.sequence == 1
                     ? !all_zero(event.previous_event_hash)
                     : all_zero(event.previous_event_hash))) {
                set_error(error, "outbox rebase predecessor is invalid");
                return false;
            }
            candidate_watermark.acknowledged_hash = event.previous_event_hash;
        } else {
            const auto previous_hash = hash_answer_event(
                candidate_events[session_indices[target_offset - 1]]);
            if (!previous_hash ||
                !same_digest(event.previous_event_hash, *previous_hash)) {
                set_error(error, "outbox rebase predecessor is invalid");
                return false;
            }
        }

        candidate_events[index] = event;
        auto previous_hash = hash_answer_event(candidate_events[index]);
        if (!previous_hash) {
            set_error(error, "outbox rebase hashing failed");
            return false;
        }
        for (std::size_t position = target_offset + 1;
             position < session_indices.size(); ++position) {
            auto& current = candidate_events[session_indices[position]];
            const auto& previous =
                candidate_events[session_indices[position - 1]];
            if (previous.sequence == std::numeric_limits<std::uint64_t>::max() ||
                current.sequence != previous.sequence + 1) {
                set_error(error, "outbox rebase sequence is not contiguous");
                return false;
            }
            current.previous_event_hash = *previous_hash;
            previous_hash = hash_answer_event(current);
            if (!previous_hash) {
                set_error(error, "outbox rebase hashing failed");
                return false;
            }
        }
        candidate_watermark.last_hash = *previous_hash;
        auto wire = encode_outbox(candidate_events, candidate_watermarks);
        const bool written = !wire.empty() &&
            write_outbox_file(impl_->path, wire, error);
        security::secure_zero(wire);
        if (!written) {
            return false;
        }
        impl_->events = std::move(candidate_events);
        impl_->watermarks = std::move(candidate_watermarks);
        return true;
    }
    if (watermark != impl_->watermarks.end() &&
        !same_watermark_context(watermark->second, event)) {
        set_error(error, "outbox event identity conflicts with its session");
        return false;
    }
    if (watermark != impl_->watermarks.end()) {
        const auto& saved = watermark->second;
        if (saved.finalized) {
            if (event.sequence == saved.highest_sequence &&
                same_digest(*event_hash, saved.last_hash)) {
                return true; // Retry of the durably finalized event.
            }
            set_error(error, "exam session is already finalized");
            return false;
        }
        if (event.sequence <= saved.highest_sequence) {
            if (event.sequence == saved.highest_sequence &&
                same_digest(*event_hash, saved.last_hash)) {
                return true; // already durably queued/acknowledged
            }
            set_error(error, "outbox sequence was already acknowledged");
            return false;
        }
        if (saved.highest_sequence == std::numeric_limits<std::uint64_t>::max() ||
            event.sequence != saved.highest_sequence + 1) {
            set_error(error, "outbox event sequence has a gap");
            return false;
        }
        if (!same_digest(event.previous_event_hash, saved.last_hash)) {
            set_error(error, "outbox event hash chain does not match");
            return false;
        }
    } else if (event.sequence != 1 || !all_zero(event.previous_event_hash)) {
        set_error(error, "first outbox event must start at sequence one");
        return false;
    }
    if (impl_->events.size() >= kMaximumOutboxEvents) {
        set_error(error, "outbox event quota is exhausted");
        return false;
    }
    auto candidate = impl_->events;
    candidate.push_back(event);
    auto candidate_watermarks = impl_->watermarks;
    if (watermark == impl_->watermarks.end()) {
        candidate_watermarks.emplace(
            key, watermark_for(event, *event_hash, event.sequence));
    } else {
        auto& saved = candidate_watermarks.find(key)->second;
        saved.highest_sequence = event.sequence;
        saved.last_hash = *event_hash;
        if (event.kind == AnswerKind::finalize) {
            saved.finalized = true;
        }
    }
    auto wire = encode_outbox(candidate, candidate_watermarks);
    const bool written = !wire.empty() && wire.size() <= kMaximumOutboxBytes &&
        write_outbox_file(impl_->path, wire, error);
    security::secure_zero(wire);
    if (!written) {
        return false;
    }
    impl_->events = std::move(candidate);
    impl_->watermarks = std::move(candidate_watermarks);
    return true;
}

std::vector<AnswerEvent> AnswerOutbox::pending(std::size_t maximum) const {
    if (!impl_ || maximum == 0) return {};
    std::scoped_lock lock(impl_->mutex);
    if (!impl_->open) return {};
    auto result = impl_->events;
    std::stable_sort(result.begin(), result.end(), [](const auto& left,
                                                      const auto& right) {
        const auto left_key = session_key(left);
        const auto right_key = session_key(right);
        if (left_key != right_key) {
            return left_key < right_key;
        }
        return left.sequence < right.sequence;
    });
    if (result.size() > maximum) {
        result.resize(maximum);
    }
    return result;
}

std::vector<AnswerEvent> AnswerOutbox::pending_for_client(
    const security::ClientId& client_id, std::size_t maximum) const {
    if (!impl_ || maximum == 0 || all_zero(client_id)) return {};
    std::scoped_lock lock(impl_->mutex);
    if (!impl_->open) return {};
    std::vector<AnswerEvent> result;
    result.reserve(std::min(maximum, impl_->events.size()));
    for (const auto& event : impl_->events) {
        if (event.client_id == client_id) {
            result.push_back(event);
        }
    }
    std::stable_sort(result.begin(), result.end(), [](const auto& left,
                                                      const auto& right) {
        const auto left_key = session_key(left);
        const auto right_key = session_key(right);
        if (left_key != right_key) {
            return left_key < right_key;
        }
        return left.sequence < right.sequence;
    });
    if (result.size() > maximum) result.resize(maximum);
    return result;
}

bool AnswerOutbox::acknowledge(const SessionId& session_id,
                               std::uint64_t sequence,
                               std::span<const std::byte> event_hash,
                               std::string* error) {
    if (!impl_) {
        set_error(error, "outbox is not open");
        return false;
    }
    std::scoped_lock lock(impl_->mutex);
    if (!impl_->open || event_hash.size() != security::kSha256Bytes ||
        sequence == 0) {
        set_error(error, "invalid outbox acknowledgement");
        return false;
    }
    // A SessionId is expected to be unique, but the durable context also
    // includes package/client/candidate identity. Resolve the exact event
    // hash first so a valid ACK cannot be rejected merely because two
    // independent contexts reused the same session identifier.
    std::set<std::string> matching_session_keys;
    std::set<std::string> exact_session_keys;
    for (const auto& event : impl_->events) {
        if (event.session_id == session_id) {
            const auto key = session_key(event);
            matching_session_keys.insert(key);
            if (event.sequence == sequence) {
                const auto calculated_hash = hash_answer_event(event);
                if (calculated_hash &&
                    same_digest(*calculated_hash, event_hash)) {
                    exact_session_keys.insert(key);
                }
            }
        }
    }
    for (const auto& [key, watermark] : impl_->watermarks) {
        if (watermark.session_id == session_id) {
            matching_session_keys.insert(key);
            if ((sequence == watermark.acknowledged_sequence &&
                 same_digest(event_hash, watermark.acknowledged_hash)) ||
                (sequence == watermark.highest_sequence &&
                 same_digest(event_hash, watermark.last_hash))) {
                exact_session_keys.insert(key);
            }
        }
    }
    if (matching_session_keys.empty()) {
        set_error(error, "outbox acknowledgement is unknown");
        return false;
    }
    if (exact_session_keys.size() > 1) {
        set_error(error, "outbox acknowledgement is ambiguous");
        return false;
    }
    std::string selected_session_key;
    if (exact_session_keys.size() == 1) {
        selected_session_key = *exact_session_keys.begin();
    } else if (matching_session_keys.size() == 1) {
        selected_session_key = *matching_session_keys.begin();
    } else {
        set_error(error, "outbox acknowledgement session is ambiguous");
        return false;
    }
    auto target = std::find_if(
        impl_->events.begin(), impl_->events.end(),
        [&](const auto& event) {
            return session_key(event) == selected_session_key &&
                   event.sequence == sequence;
        });
    if (target == impl_->events.end()) {
        auto watermark = impl_->watermarks.find(selected_session_key);
        if (watermark == impl_->watermarks.end()) {
            set_error(error, "outbox acknowledgement is unknown");
            return false;
        }
        const auto& saved = watermark->second;
        if (sequence > saved.highest_sequence) {
            set_error(error, "outbox acknowledgement is unknown");
            return false;
        }
        if (sequence == saved.acknowledged_sequence) {
            if (same_digest(event_hash, saved.acknowledged_hash)) {
                return true; // Idempotent replay of the durable boundary.
            }
            set_error(error, "outbox acknowledgement hash mismatch");
            return false;
        }
        if (sequence < saved.acknowledged_sequence ||
            sequence != saved.highest_sequence) {
            // The event was removed and its hash is no longer available. Only
            // the exact acknowledged boundary or the highest watermark can be
            // verified safely; never persist an unverifiable hash.
            set_error(error, "outbox acknowledgement cannot be verified");
            return false;
        }
        if (!same_digest(event_hash, saved.last_hash)) {
            set_error(error, "outbox acknowledgement hash mismatch");
            return false;
        }
        if (sequence > saved.acknowledged_sequence) {
            auto candidate_watermarks = impl_->watermarks;
            auto& candidate_saved =
                candidate_watermarks.find(selected_session_key)->second;
            candidate_saved.acknowledged_sequence = sequence;
            candidate_saved.acknowledged_hash = saved.last_hash;
            auto wire = encode_outbox(impl_->events, candidate_watermarks);
            const bool written = !wire.empty() &&
                write_outbox_file(impl_->path, wire, error);
            security::secure_zero(wire);
            if (!written) {
                return false;
            }
            impl_->watermarks = std::move(candidate_watermarks);
        }
        return true; // idempotent acknowledgement after a reconnect.
    }
    const auto target_hash = hash_answer_event(*target);
    if (!target_hash || !same_digest(*target_hash, event_hash)) {
        set_error(error, "outbox acknowledgement hash mismatch");
        return false;
    }
    const auto key = session_key(*target);
    auto watermark = impl_->watermarks.find(key);
    if (watermark == impl_->watermarks.end() ||
        sequence > watermark->second.highest_sequence) {
        set_error(error, "outbox watermark is missing");
        return false;
    }
    if (sequence <= watermark->second.acknowledged_sequence) {
        set_error(error, "outbox acknowledgement is stale");
        return false;
    }
    std::vector<AnswerEvent> remaining;
    remaining.reserve(impl_->events.size());
    for (const auto& event : impl_->events) {
        if (session_key(event) == key && event.sequence <= sequence) {
            continue;
        }
        remaining.push_back(event);
    }
    auto candidate_watermarks = impl_->watermarks;
    auto& candidate_saved = candidate_watermarks.find(key)->second;
    if (sequence > candidate_saved.acknowledged_sequence) {
        candidate_saved.acknowledged_sequence = sequence;
        candidate_saved.acknowledged_hash = *target_hash;
    }
    auto wire = encode_outbox(remaining, candidate_watermarks);
    const bool written = !wire.empty() &&
        write_outbox_file(impl_->path, wire, error);
    security::secure_zero(wire);
    if (!written) {
        return false;
    }
    impl_->events = std::move(remaining);
    impl_->watermarks = std::move(candidate_watermarks);
    return true;
}

std::size_t AnswerOutbox::size() const noexcept {
    if (!impl_) return 0;
    std::scoped_lock lock(impl_->mutex);
    return impl_->events.size();
}

} // namespace nstu::exam
