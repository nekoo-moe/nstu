#include "nstu/pairing.hpp"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cwchar>
#include <type_traits>
#include <utility>

// MinGW's bcrypt.h trails the Windows SDK on the ECDH identifiers.
#ifndef BCRYPT_ECDH_ALGORITHM
#define BCRYPT_ECDH_ALGORITHM L"ECDH"
#endif
#ifndef BCRYPT_ECDH_P256_ALGORITHM
#define BCRYPT_ECDH_P256_ALGORITHM L"ECDH_P256"
#endif
#ifndef BCRYPT_ECC_CURVE_NAME
#define BCRYPT_ECC_CURVE_NAME L"ECCCurveName"
#endif
#ifndef BCRYPT_ECC_CURVE_25519
#define BCRYPT_ECC_CURVE_25519 L"curve25519"
#endif
#ifndef BCRYPT_ECCPUBLIC_BLOB
#define BCRYPT_ECCPUBLIC_BLOB L"ECCPUBLICBLOB"
#endif
#ifndef BCRYPT_KDF_RAW_SECRET
#define BCRYPT_KDF_RAW_SECRET L"TRUNCATE"
#endif

namespace nstu::pairing {
namespace {

using security::constant_time_equal;
using security::hmac_sha256;
using security::Sha256Digest;
using security::sha256;

inline constexpr std::array<std::byte, 4> kTranscriptMagic{
    static_cast<std::byte>('N'), static_cast<std::byte>('S'),
    static_cast<std::byte>('T'), static_cast<std::byte>('P')};

// The CNG public blob starts with a BCRYPT_ECCKEY_BLOB header, read here as
// two little-endian ULONGs so the code does not depend on the struct being
// declared by the toolchain's headers.
inline constexpr std::size_t kEccBlobHeaderBytes = 2 * sizeof(std::uint32_t);

inline constexpr std::string_view kConfirmLabel = "nstu-pair-confirm-v1";
inline constexpr std::string_view kSasLabel = "nstu-pair-sas-v1";
inline constexpr std::string_view kEnrolledKeyLabel = "nstu-pair-key-v1";
inline constexpr std::string_view kClientTagLabel = "nstu-pair-tag-client-v1";
inline constexpr std::string_view kServerTagLabel = "nstu-pair-tag-server-v1";
inline constexpr std::string_view kClientIdLabel = "NSTU-CLIENT-ID-V1";

void set_error(std::string* error, const char* message) {
    if (error != nullptr) {
        *error = message;
    }
}

std::span<const std::byte> as_bytes(std::string_view text) noexcept {
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

template <typename T>
void append_le(std::vector<std::byte>& output, T value) {
    static_assert(std::is_unsigned_v<T>);
    for (std::size_t index = 0; index < sizeof(T); ++index) {
        output.push_back(static_cast<std::byte>(value & 0xffu));
        value >>= 8u;
    }
}

void append_bytes(std::vector<std::byte>& output,
                  std::span<const std::byte> bytes) {
    output.insert(output.end(), bytes.begin(), bytes.end());
}

template <typename Range>
bool any_nonzero(const Range& values) noexcept {
    return std::any_of(values.begin(), values.end(),
                       [](std::byte value) { return value != std::byte{0}; });
}

bool valid_identity(const std::string& value) noexcept {
    if (value.empty() || value.size() > kMaximumIdentityBytes) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](char character) {
        const auto code = static_cast<unsigned char>(character);
        return code >= 0x20u && code < 0x7fu;
    });
}

bool valid_public_key(const std::vector<std::byte>& key) noexcept {
    return !key.empty() && key.size() <= kMaximumPublicKeyBytes;
}

bool known_agreement(KeyAgreement agreement) noexcept {
    return agreement == KeyAgreement::x25519 ||
           agreement == KeyAgreement::nist_p256;
}

std::uint32_t read_u32_le(std::span<const std::byte> input,
                          std::size_t offset) noexcept {
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < sizeof(std::uint32_t); ++index) {
        value |= static_cast<std::uint32_t>(
                     std::to_integer<unsigned int>(input[offset + index]))
                 << (index * 8u);
    }
    return value;
}

bool open_agreement_provider(KeyAgreement agreement,
                             BCRYPT_ALG_HANDLE& handle) noexcept {
    handle = nullptr;
    if (agreement == KeyAgreement::nist_p256) {
        return BCryptOpenAlgorithmProvider(&handle, BCRYPT_ECDH_P256_ALGORITHM,
                                           nullptr, 0) == 0;
    }
    if (BCryptOpenAlgorithmProvider(&handle, BCRYPT_ECDH_ALGORITHM, nullptr,
                                    0) != 0) {
        return false;
    }
    const auto* curve = BCRYPT_ECC_CURVE_25519;
    const auto curve_bytes =
        static_cast<ULONG>((std::wcslen(curve) + 1) * sizeof(wchar_t));
    if (BCryptSetProperty(handle, BCRYPT_ECC_CURVE_NAME,
                          reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(curve)),
                          curve_bytes, 0) != 0) {
        BCryptCloseAlgorithmProvider(handle, 0);
        handle = nullptr;
        return false;
    }
    return true;
}

// Curve25519 is selected by name, so the bit length argument is provider
// dependent; try the documented spellings before giving up on the curve.
bool generate_finalized_pair(BCRYPT_ALG_HANDLE algorithm,
                             KeyAgreement agreement,
                             BCRYPT_KEY_HANDLE& key) noexcept {
    const std::array<ULONG, 2> candidate_lengths =
        agreement == KeyAgreement::nist_p256 ? std::array<ULONG, 2>{256u, 0u}
                                             : std::array<ULONG, 2>{0u, 255u};
    for (const ULONG length : candidate_lengths) {
        key = nullptr;
        if (BCryptGenerateKeyPair(algorithm, &key, length, 0) != 0) {
            key = nullptr;
            continue;
        }
        if (BCryptFinalizeKeyPair(key, 0) == 0) {
            return true;
        }
        BCryptDestroyKey(key);
        key = nullptr;
    }
    return false;
}

bool export_public_blob(BCRYPT_KEY_HANDLE key,
                        std::vector<std::byte>& blob) noexcept {
    ULONG required = 0;
    if (BCryptExportKey(key, nullptr, BCRYPT_ECCPUBLIC_BLOB, nullptr, 0,
                        &required, 0) != 0 ||
        required <= kEccBlobHeaderBytes || required > kMaximumPublicKeyBytes) {
        return false;
    }
    blob.assign(required, std::byte{0});
    ULONG written = 0;
    if (BCryptExportKey(key, nullptr, BCRYPT_ECCPUBLIC_BLOB,
                        reinterpret_cast<PUCHAR>(blob.data()), required,
                        &written, 0) != 0 ||
        written != required) {
        blob.clear();
        return false;
    }
    return true;
}

std::string format_sas(const Sha256Digest& digest) {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < sizeof(std::uint64_t); ++index) {
        value = (value << 8u) |
                static_cast<std::uint64_t>(
                    std::to_integer<unsigned int>(digest[index]));
    }
    std::uint64_t modulus = 1;
    for (std::size_t index = 0; index < kSasDigits; ++index) {
        modulus *= 10u;
    }
    value %= modulus;
    std::string digits(kSasDigits, '0');
    for (std::size_t index = kSasDigits; index-- > 0;) {
        digits[index] = static_cast<char>('0' + (value % 10u));
        value /= 10u;
    }
    return digits;
}

std::optional<Sha256Digest> tag_for_role(const PairingSecrets& secrets,
                                         const PairingTranscript& transcript,
                                         ConfirmationRole role) {
    if (role != ConfirmationRole::client && role != ConfirmationRole::server) {
        return std::nullopt;
    }
    auto encoded = encode_transcript(transcript);
    if (!encoded) {
        return std::nullopt;
    }
    const auto label = role == ConfirmationRole::client ? kClientTagLabel
                                                        : kServerTagLabel;
    std::vector<std::byte> message;
    message.reserve(label.size() + encoded->size());
    append_bytes(message, as_bytes(label));
    append_bytes(message, *encoded);
    return hmac_sha256(secrets.confirm_key, message);
}

} // namespace

const char* key_agreement_name(KeyAgreement agreement) noexcept {
    switch (agreement) {
    case KeyAgreement::x25519:
        return "Curve25519";
    case KeyAgreement::nist_p256:
        return "NIST P-256";
    }
    return "unknown";
}

struct EphemeralKeyPair::Impl {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    KeyAgreement agreement = KeyAgreement::x25519;
    std::vector<std::byte> public_blob;

    ~Impl() {
        if (key != nullptr) {
            BCryptDestroyKey(key);
        }
        if (algorithm != nullptr) {
            BCryptCloseAlgorithmProvider(algorithm, 0);
        }
    }
};

EphemeralKeyPair::EphemeralKeyPair() noexcept = default;
EphemeralKeyPair::~EphemeralKeyPair() = default;
EphemeralKeyPair::EphemeralKeyPair(EphemeralKeyPair&&) noexcept = default;
EphemeralKeyPair& EphemeralKeyPair::operator=(EphemeralKeyPair&&) noexcept =
    default;

std::optional<EphemeralKeyPair> EphemeralKeyPair::generate(
    std::string* error) {
    constexpr std::array<KeyAgreement, 2> kPreference{KeyAgreement::x25519,
                                                      KeyAgreement::nist_p256};
    for (const KeyAgreement agreement : kPreference) {
        auto impl = std::make_unique<Impl>();
        impl->agreement = agreement;
        if (!open_agreement_provider(agreement, impl->algorithm)) {
            continue;
        }
        if (!generate_finalized_pair(impl->algorithm, agreement, impl->key)) {
            continue;
        }
        if (!export_public_blob(impl->key, impl->public_blob)) {
            continue;
        }
        EphemeralKeyPair pair;
        pair.impl_ = std::move(impl);
        return pair;
    }
    set_error(error, "no usable elliptic-curve key agreement provider");
    return std::nullopt;
}

KeyAgreement EphemeralKeyPair::agreement() const noexcept {
    return impl_ == nullptr ? KeyAgreement::x25519 : impl_->agreement;
}

std::span<const std::byte> EphemeralKeyPair::public_key() const noexcept {
    if (impl_ == nullptr) {
        return {};
    }
    return impl_->public_blob;
}

std::optional<std::vector<std::byte>> EphemeralKeyPair::agree(
    std::span<const std::byte> peer_public_key, std::string* error) const {
    if (impl_ == nullptr || impl_->public_blob.size() <= kEccBlobHeaderBytes) {
        set_error(error, "pairing key pair is not initialised");
        return std::nullopt;
    }
    if (peer_public_key.size() != impl_->public_blob.size()) {
        set_error(error, "peer public key has an unexpected length");
        return std::nullopt;
    }
    const std::span<const std::byte> own{impl_->public_blob};
    if (read_u32_le(peer_public_key, 0) != read_u32_le(own, 0) ||
        read_u32_le(peer_public_key, sizeof(std::uint32_t)) !=
            read_u32_le(own, sizeof(std::uint32_t))) {
        set_error(error, "peer public key uses a different curve");
        return std::nullopt;
    }
    if (std::equal(peer_public_key.begin(), peer_public_key.end(),
                   own.begin())) {
        set_error(error, "peer echoed our own public key");
        return std::nullopt;
    }

    BCRYPT_KEY_HANDLE peer_key = nullptr;
    if (BCryptImportKeyPair(
            impl_->algorithm, nullptr, BCRYPT_ECCPUBLIC_BLOB, &peer_key,
            reinterpret_cast<PUCHAR>(
                const_cast<std::byte*>(peer_public_key.data())),
            static_cast<ULONG>(peer_public_key.size()), 0) != 0) {
        set_error(error, "peer public key was rejected by the provider");
        return std::nullopt;
    }

    BCRYPT_SECRET_HANDLE agreed = nullptr;
    if (BCryptSecretAgreement(impl_->key, peer_key, &agreed, 0) != 0) {
        BCryptDestroyKey(peer_key);
        set_error(error, "key agreement failed");
        return std::nullopt;
    }

    ULONG required = 0;
    std::optional<std::vector<std::byte>> result;
    if (BCryptDeriveKey(agreed, BCRYPT_KDF_RAW_SECRET, nullptr, nullptr, 0,
                        &required, 0) == 0 &&
        required >= security::kMinimumProtocolKeyBytes &&
        required <= kMaximumPublicKeyBytes) {
        std::vector<std::byte> secret(required, std::byte{0});
        ULONG written = 0;
        if (BCryptDeriveKey(agreed, BCRYPT_KDF_RAW_SECRET, nullptr,
                            reinterpret_cast<PUCHAR>(secret.data()), required,
                            &written, 0) == 0 &&
            written == required && any_nonzero(secret)) {
            result = std::move(secret);
        } else {
            security::secure_zero(secret);
        }
    }

    BCryptDestroySecret(agreed);
    BCryptDestroyKey(peer_key);
    if (!result) {
        set_error(error, "shared secret derivation failed");
    }
    return result;
}

bool valid_transcript(const PairingTranscript& transcript) noexcept {
    return transcript.version != 0 && known_agreement(transcript.agreement) &&
           valid_identity(transcript.client_uuid) &&
           valid_identity(transcript.client_hostname) &&
           valid_public_key(transcript.client_public_key) &&
           valid_public_key(transcript.server_public_key) &&
           any_nonzero(transcript.client_nonce) &&
           any_nonzero(transcript.server_nonce);
}

std::optional<std::vector<std::byte>> encode_transcript(
    const PairingTranscript& transcript) {
    if (!valid_transcript(transcript)) {
        return std::nullopt;
    }
    std::vector<std::byte> encoded;
    encoded.reserve(kTranscriptMagic.size() + sizeof(std::uint16_t) * 6 +
                    transcript.client_uuid.size() +
                    transcript.client_hostname.size() +
                    transcript.client_public_key.size() +
                    transcript.server_public_key.size() +
                    transcript.client_nonce.size() +
                    transcript.server_nonce.size());
    append_bytes(encoded, kTranscriptMagic);
    append_le(encoded, transcript.version);
    append_le(encoded, static_cast<std::uint16_t>(transcript.agreement));
    append_le(encoded,
              static_cast<std::uint16_t>(transcript.client_uuid.size()));
    append_bytes(encoded, as_bytes(transcript.client_uuid));
    append_le(encoded,
              static_cast<std::uint16_t>(transcript.client_hostname.size()));
    append_bytes(encoded, as_bytes(transcript.client_hostname));
    append_le(encoded,
              static_cast<std::uint16_t>(transcript.client_public_key.size()));
    append_bytes(encoded, transcript.client_public_key);
    append_le(encoded,
              static_cast<std::uint16_t>(transcript.server_public_key.size()));
    append_bytes(encoded, transcript.server_public_key);
    append_bytes(encoded, transcript.client_nonce);
    append_bytes(encoded, transcript.server_nonce);
    return encoded;
}

std::optional<Sha256Digest> hkdf_sha256(
    std::span<const std::byte> input_key_material,
    std::span<const std::byte> salt,
    std::span<const std::byte> info) noexcept {
    if (input_key_material.empty()) {
        return std::nullopt;
    }
    auto pseudo_random_key = hmac_sha256(salt, input_key_material);
    if (!pseudo_random_key) {
        return std::nullopt;
    }
    std::vector<std::byte> block;
    block.reserve(info.size() + 1);
    block.insert(block.end(), info.begin(), info.end());
    block.push_back(std::byte{0x01});
    auto output = hmac_sha256(*pseudo_random_key, block);
    security::secure_zero(*pseudo_random_key);
    return output;
}

std::optional<PairingSecrets> derive_pairing_secrets(
    std::span<const std::byte> shared_secret,
    const PairingTranscript& transcript) {
    if (shared_secret.size() < security::kMinimumProtocolKeyBytes) {
        return std::nullopt;
    }
    auto encoded = encode_transcript(transcript);
    if (!encoded) {
        return std::nullopt;
    }
    const auto salt = sha256(*encoded);
    if (!salt) {
        return std::nullopt;
    }
    auto confirm = hkdf_sha256(shared_secret, *salt, as_bytes(kConfirmLabel));
    auto sas_material = hkdf_sha256(shared_secret, *salt, as_bytes(kSasLabel));
    auto enrolled =
        hkdf_sha256(shared_secret, *salt, as_bytes(kEnrolledKeyLabel));
    if (!confirm || !sas_material || !enrolled) {
        if (confirm) {
            security::secure_zero(*confirm);
        }
        if (sas_material) {
            security::secure_zero(*sas_material);
        }
        if (enrolled) {
            security::secure_zero(*enrolled);
        }
        return std::nullopt;
    }
    PairingSecrets secrets;
    secrets.confirm_key = *confirm;
    secrets.enrolled_key = *enrolled;
    secrets.short_authentication_string = format_sas(*sas_material);
    security::secure_zero(*confirm);
    security::secure_zero(*sas_material);
    security::secure_zero(*enrolled);
    return secrets;
}

std::optional<Sha256Digest> confirmation_tag(
    const PairingSecrets& secrets, const PairingTranscript& transcript,
    ConfirmationRole role) {
    return tag_for_role(secrets, transcript, role);
}

bool verify_confirmation_tag(const PairingSecrets& secrets,
                             const PairingTranscript& transcript,
                             ConfirmationRole role,
                             const Sha256Digest& received) {
    const auto expected = tag_for_role(secrets, transcript, role);
    return expected.has_value() && constant_time_equal(*expected, received);
}

std::optional<security::ClientId> client_id_from_uuid(
    std::string_view uuid) noexcept {
    if (uuid.empty() || uuid.size() > kMaximumIdentityBytes) {
        return std::nullopt;
    }
    std::vector<std::byte> message;
    message.reserve(kClientIdLabel.size() + uuid.size());
    append_bytes(message, as_bytes(kClientIdLabel));
    append_bytes(message, as_bytes(uuid));
    const auto digest = sha256(message);
    if (!digest) {
        return std::nullopt;
    }
    security::ClientId client_id{};
    std::copy_n(digest->begin(), client_id.size(), client_id.begin());
    return client_id;
}

void secure_zero(PairingSecrets& secrets) noexcept {
    security::secure_zero(secrets.confirm_key);
    security::secure_zero(secrets.enrolled_key);
    if (!secrets.short_authentication_string.empty()) {
        security::secure_zero(std::as_writable_bytes(
            std::span{secrets.short_authentication_string.data(),
                      secrets.short_authentication_string.size()}));
        secrets.short_authentication_string.clear();
    }
}

} // namespace nstu::pairing
