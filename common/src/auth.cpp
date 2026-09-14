#include "nstu/auth.hpp"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <type_traits>

namespace nstu::security {
namespace {

template <typename T>
void append_le(std::vector<std::byte>& output, T value) {
    static_assert(std::is_unsigned_v<T>);
    for (std::size_t index = 0; index < sizeof(T); ++index) {
        output.push_back(static_cast<std::byte>(value & 0xffu));
        value >>= 8u;
    }
}

template <typename T>
bool read_le(std::span<const std::byte> input, std::size_t& offset, T& value) {
    static_assert(std::is_unsigned_v<T>);
    if (offset + sizeof(T) > input.size()) {
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

std::vector<std::byte> make_transcript(const char* label,
                                       const AuthHello& hello,
                                       const AuthChallenge& challenge) {
    std::vector<std::byte> transcript;
    const auto label_length = std::strlen(label);
    transcript.reserve(label_length + kAuthHelloBytes + kAuthChallengeBytes);
    append_bytes(transcript, {reinterpret_cast<const std::byte*>(label),
                              label_length});
    append_bytes(transcript, encode_auth_hello(hello));
    append_bytes(transcript, encode_auth_challenge(challenge));
    return transcript;
}

std::string replay_key(const AuthHello& hello) {
    std::string key;
    key.resize(kClientIdBytes + kNonceBytes);
    std::memcpy(key.data(), hello.client_id.data(), kClientIdBytes);
    std::memcpy(key.data() + kClientIdBytes, hello.client_nonce.data(),
                kNonceBytes);
    return key;
}

} // namespace

std::vector<std::byte> encode_auth_hello(const AuthHello& hello) {
    std::vector<std::byte> wire;
    wire.reserve(kAuthHelloBytes);
    append_bytes(wire, hello.client_id);
    append_bytes(wire, hello.client_nonce);
    append_le(wire, hello.unix_time_seconds);
    append_le(wire, hello.key_id);
    return wire;
}

std::optional<AuthHello> decode_auth_hello(std::span<const std::byte> wire) {
    if (wire.size() != kAuthHelloBytes) {
        return std::nullopt;
    }
    AuthHello hello;
    std::size_t offset = 0;
    std::copy_n(wire.begin(), kClientIdBytes, hello.client_id.begin());
    offset += kClientIdBytes;
    std::copy_n(wire.begin() + static_cast<std::ptrdiff_t>(offset), kNonceBytes,
                hello.client_nonce.begin());
    offset += kNonceBytes;
    if (!read_le(wire, offset, hello.unix_time_seconds) ||
        !read_le(wire, offset, hello.key_id)) {
        return std::nullopt;
    }
    return hello;
}

std::vector<std::byte> encode_auth_challenge(const AuthChallenge& challenge) {
    std::vector<std::byte> wire;
    wire.reserve(kAuthChallengeBytes);
    append_bytes(wire, challenge.server_nonce);
    append_le(wire, challenge.unix_time_seconds);
    return wire;
}

std::optional<AuthChallenge> decode_auth_challenge(
    std::span<const std::byte> wire) {
    if (wire.size() != kAuthChallengeBytes) {
        return std::nullopt;
    }
    AuthChallenge challenge;
    std::copy_n(wire.begin(), kNonceBytes, challenge.server_nonce.begin());
    std::size_t offset = kNonceBytes;
    if (!read_le(wire, offset, challenge.unix_time_seconds)) {
        return std::nullopt;
    }
    return challenge;
}

bool validate_auth_hello(const AuthHello& hello) noexcept {
    const bool client_id_present = std::any_of(
        hello.client_id.begin(), hello.client_id.end(),
        [](std::byte value) { return value != std::byte{0}; });
    const bool nonce_present = std::any_of(
        hello.client_nonce.begin(), hello.client_nonce.end(),
        [](std::byte value) { return value != std::byte{0}; });
    return client_id_present && nonce_present && hello.unix_time_seconds != 0 &&
           hello.key_id != 0;
}

bool validate_auth_challenge(const AuthChallenge& challenge) noexcept {
    const bool nonce_present = std::any_of(
        challenge.server_nonce.begin(), challenge.server_nonce.end(),
        [](std::byte value) { return value != std::byte{0}; });
    return nonce_present && challenge.unix_time_seconds != 0;
}

bool generate_random(std::span<std::byte> output) noexcept {
    if (output.empty()) {
        return true;
    }
    if (output.size() > std::numeric_limits<ULONG>::max()) {
        return false;
    }
    return BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(output.data()),
                           static_cast<ULONG>(output.size()),
                           BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
}

std::optional<Sha256Digest> sha256(
    std::span<const std::byte> message) noexcept {
    if (message.size() > std::numeric_limits<ULONG>::max()) {
        return std::nullopt;
    }
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM,
                                    nullptr, 0) != 0) {
        return std::nullopt;
    }
    DWORD object_bytes = 0;
    DWORD copied = 0;
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                          reinterpret_cast<PUCHAR>(&object_bytes),
                          sizeof(object_bytes), &copied, 0) != 0 ||
        object_bytes == 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return std::nullopt;
    }
    std::vector<UCHAR> hash_object(object_bytes);
    BCRYPT_HASH_HANDLE hash = nullptr;
    const NTSTATUS create_status = BCryptCreateHash(
        algorithm, &hash, hash_object.data(), object_bytes, nullptr, 0, 0);
    Sha256Digest digest{};
    const bool succeeded =
        create_status == 0 &&
        BCryptHashData(hash,
                       const_cast<PUCHAR>(reinterpret_cast<const UCHAR*>(
                           message.data())),
                       static_cast<ULONG>(message.size()), 0) == 0 &&
        BCryptFinishHash(hash, reinterpret_cast<PUCHAR>(digest.data()),
                         static_cast<ULONG>(digest.size()), 0) == 0;
    if (hash != nullptr) {
        BCryptDestroyHash(hash);
    }
    secure_zero({reinterpret_cast<std::byte*>(hash_object.data()),
                 hash_object.size()});
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!succeeded) {
        secure_zero(digest);
        return std::nullopt;
    }
    return digest;
}

std::optional<Sha256Digest> hmac_sha256(
    std::span<const std::byte> key,
    std::span<const std::byte> message) noexcept {
    if (key.empty() || key.size() > std::numeric_limits<ULONG>::max() ||
        message.size() > std::numeric_limits<ULONG>::max()) {
        return std::nullopt;
    }
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr,
                                    BCRYPT_ALG_HANDLE_HMAC_FLAG) != 0) {
        return std::nullopt;
    }
    DWORD object_bytes = 0;
    DWORD copied = 0;
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                          reinterpret_cast<PUCHAR>(&object_bytes),
                          sizeof(object_bytes), &copied, 0) != 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return std::nullopt;
    }
    std::vector<UCHAR> hash_object(object_bytes);
    BCRYPT_HASH_HANDLE hash = nullptr;
    const NTSTATUS create_status = BCryptCreateHash(
        algorithm, &hash, hash_object.data(), object_bytes,
        const_cast<PUCHAR>(reinterpret_cast<const UCHAR*>(key.data())),
        static_cast<ULONG>(key.size()), 0);
    Sha256Digest digest{};
    const bool succeeded = create_status == 0 &&
        BCryptHashData(hash,
                       const_cast<PUCHAR>(reinterpret_cast<const UCHAR*>(
                           message.data())),
                       static_cast<ULONG>(message.size()), 0) == 0 &&
        BCryptFinishHash(hash, reinterpret_cast<PUCHAR>(digest.data()),
                         static_cast<ULONG>(digest.size()), 0) == 0;
    if (hash != nullptr) {
        BCryptDestroyHash(hash);
    }
    secure_zero({reinterpret_cast<std::byte*>(hash_object.data()),
                 hash_object.size()});
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!succeeded) {
        secure_zero(digest);
        return std::nullopt;
    }
    return digest;
}

bool constant_time_equal(std::span<const std::byte> left,
                         std::span<const std::byte> right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    unsigned int difference = 0;
    for (std::size_t index = 0; index < left.size(); ++index) {
        difference |= std::to_integer<unsigned int>(left[index] ^ right[index]);
    }
    return difference == 0;
}

void secure_zero(std::span<std::byte> bytes) noexcept {
    if (!bytes.empty()) {
        SecureZeroMemory(bytes.data(), bytes.size());
    }
}

std::optional<Sha256Digest> compute_client_proof(
    std::span<const std::byte> pre_shared_key, const AuthHello& hello,
    const AuthChallenge& challenge) noexcept {
    if (pre_shared_key.size() < kMinimumProtocolKeyBytes ||
        !validate_auth_hello(hello) || !validate_auth_challenge(challenge)) {
        return std::nullopt;
    }
    const auto transcript = make_transcript("NSTU-CLIENT-PROOF-V1", hello,
                                            challenge);
    return hmac_sha256(pre_shared_key, transcript);
}

std::optional<Sha256Digest> derive_session_key(
    std::span<const std::byte> pre_shared_key, const AuthHello& hello,
    const AuthChallenge& challenge) noexcept {
    if (pre_shared_key.size() < kMinimumProtocolKeyBytes ||
        !validate_auth_hello(hello) || !validate_auth_challenge(challenge)) {
        return std::nullopt;
    }
    const auto transcript = make_transcript("NSTU-SESSION-KEY-V1", hello,
                                            challenge);
    return hmac_sha256(pre_shared_key, transcript);
}

std::optional<Sha256Digest> compute_server_proof(
    std::span<const std::byte> session_key, const AuthHello& hello,
    const AuthChallenge& challenge) noexcept {
    if (session_key.size() < kMinimumProtocolKeyBytes ||
        !validate_auth_hello(hello) || !validate_auth_challenge(challenge)) {
        return std::nullopt;
    }
    const auto transcript = make_transcript("NSTU-SERVER-PROOF-V1", hello,
                                            challenge);
    return hmac_sha256(session_key, transcript);
}

std::optional<VideoAuthTag> compute_video_auth_tag(
    std::span<const std::byte> key,
    const protocol::VideoPacketHeader& header,
    std::span<const std::byte> payload) noexcept {
    if (key.size() < kMinimumProtocolKeyBytes) {
        return std::nullopt;
    }
    constexpr char label[] = "NSTU-VIDEO-PACKET-V1";
    std::vector<std::byte> message{
        reinterpret_cast<const std::byte*>(label),
        reinterpret_cast<const std::byte*>(label) + sizeof(label) - 1};
    const auto header_wire = protocol::encode_video_header(header);
    message.insert(message.end(), header_wire.begin(), header_wire.end());
    message.insert(message.end(), payload.begin(), payload.end());
    const auto digest = hmac_sha256(key, message);
    if (!digest) {
        return std::nullopt;
    }
    VideoAuthTag tag{};
    std::copy_n(digest->begin(), tag.size(), tag.begin());
    return tag;
}

bool verify_video_auth_tag(std::span<const std::byte> key,
                           const protocol::VideoPacketHeader& header,
                           std::span<const std::byte> payload,
                           std::span<const std::byte> tag) noexcept {
    if (tag.size() != kVideoAuthTagBytes) {
        return false;
    }
    const auto expected = compute_video_auth_tag(key, header, payload);
    return expected && constant_time_equal(*expected, tag);
}

std::optional<ControlAuthTag> compute_control_auth_tag(
    std::span<const std::byte> session_key,
    const protocol::CommandEnvelope& envelope, std::uint64_t sequence,
    std::span<const std::byte> payload) noexcept {
    if (session_key.size() < kMinimumProtocolKeyBytes) {
        return std::nullopt;
    }
    constexpr char label[] = "NSTU-CONTROL-FRAME-V1";
    std::vector<std::byte> message{
        reinterpret_cast<const std::byte*>(label),
        reinterpret_cast<const std::byte*>(label) + sizeof(label) - 1};
    const auto header = protocol::encode_command_header(envelope);
    message.insert(message.end(), header.begin(), header.end());
    append_le(message, sequence);
    message.insert(message.end(), payload.begin(), payload.end());
    const auto digest = hmac_sha256(session_key, message);
    if (!digest) {
        return std::nullopt;
    }
    ControlAuthTag tag{};
    std::copy_n(digest->begin(), tag.size(), tag.begin());
    return tag;
}

std::optional<ControlAuthTag> compute_control_auth_tag(
    std::span<const std::byte> session_key,
    const protocol::CommandEnvelope& envelope, std::uint64_t sequence,
    std::span<const std::byte> payload, ControlDirection direction) noexcept {
    if (session_key.size() < kMinimumProtocolKeyBytes ||
        (direction != ControlDirection::client_to_server &&
         direction != ControlDirection::server_to_client)) {
        return std::nullopt;
    }
    // V2 is intentionally an opt-in domain. The live v1 transport continues
    // to use the neutral label until capability negotiation is deployed.
    constexpr char label[] = "NSTU-CONTROL-FRAME-V2";
    std::vector<std::byte> message{
        reinterpret_cast<const std::byte*>(label),
        reinterpret_cast<const std::byte*>(label) + sizeof(label) - 1};
    message.push_back(static_cast<std::byte>(direction));
    const auto header = protocol::encode_command_header(envelope);
    message.insert(message.end(), header.begin(), header.end());
    append_le(message, sequence);
    message.insert(message.end(), payload.begin(), payload.end());
    const auto digest = hmac_sha256(session_key, message);
    if (!digest) {
        return std::nullopt;
    }
    ControlAuthTag tag{};
    std::copy_n(digest->begin(), tag.size(), tag.begin());
    return tag;
}

bool verify_control_auth_tag(std::span<const std::byte> session_key,
                             const protocol::CommandEnvelope& envelope,
                             std::uint64_t sequence,
                             std::span<const std::byte> payload,
                             std::span<const std::byte> tag) noexcept {
    if (tag.size() != kControlAuthTagBytes) {
        return false;
    }
    const auto expected = compute_control_auth_tag(session_key, envelope,
                                                   sequence, payload);
    return expected && constant_time_equal(*expected, tag);
}

bool verify_control_auth_tag(std::span<const std::byte> session_key,
                             const protocol::CommandEnvelope& envelope,
                             std::uint64_t sequence,
                             std::span<const std::byte> payload,
                             ControlDirection direction,
                             std::span<const std::byte> tag) noexcept {
    if (tag.size() != kControlAuthTagBytes) {
        return false;
    }
    const auto expected = compute_control_auth_tag(
        session_key, envelope, sequence, payload, direction);
    return expected && constant_time_equal(*expected, tag);
}

void ControlSequenceGuard::reset(std::uint64_t first_expected_sequence) noexcept {
    next_expected_ = first_expected_sequence;
    exhausted_ = false;
}

bool ControlSequenceGuard::accept(std::uint64_t sequence) noexcept {
    if (exhausted_ || sequence != next_expected_) {
        return false;
    }
    if (next_expected_ == std::numeric_limits<std::uint64_t>::max()) {
        exhausted_ = true;
    } else {
        ++next_expected_;
    }
    return true;
}

std::uint64_t ControlSequenceGuard::next_expected_sequence() const noexcept {
    return next_expected_;
}

ReplayProtector::ReplayProtector(std::size_t capacity,
                                 std::chrono::seconds maximum_clock_skew)
    : capacity_(std::max<std::size_t>(capacity, 1)),
      maximum_clock_skew_(static_cast<std::uint64_t>(
          std::max<std::int64_t>(maximum_clock_skew.count(), 1))) {}

bool ReplayProtector::accept(const AuthHello& hello,
                             std::uint64_t now_unix_seconds) {
    if (!validate_auth_hello(hello)) {
        return false;
    }
    const auto difference = hello.unix_time_seconds > now_unix_seconds
                                ? hello.unix_time_seconds - now_unix_seconds
                                : now_unix_seconds - hello.unix_time_seconds;
    if (difference > maximum_clock_skew_) {
        return false;
    }
    const auto key = replay_key(hello);
    std::scoped_lock lock(mutex_);
    while (!entries_.empty() && entries_.front().expires_at < now_unix_seconds) {
        keys_.erase(entries_.front().key);
        entries_.pop_front();
    }
    if (keys_.contains(key)) {
        return false;
    }
    while (entries_.size() >= capacity_) {
        keys_.erase(entries_.front().key);
        entries_.pop_front();
    }
    const auto expiry_base =
        std::max(now_unix_seconds, hello.unix_time_seconds);
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    const auto expires_at = maximum_clock_skew_ > maximum - expiry_base
                                ? maximum
                                : expiry_base + maximum_clock_skew_;
    keys_.insert(key);
    entries_.push_back({key, expires_at});
    return true;
}

void ReplayProtector::clear() {
    std::scoped_lock lock(mutex_);
    entries_.clear();
    keys_.clear();
}

} // namespace nstu::security
