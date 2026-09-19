#pragma once

#include "nstu/auth.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Verified pairing: an unenrolled client and the server agree on a fresh
// protocol key over an ephemeral ECDH exchange, and both screens show the same
// six-digit short authentication string (SAS). An operator who compares the two
// codes rules out a look-alike client and a man in the middle on the LAN, which
// is what replaces the hand-copied enrollment secret.
namespace nstu::pairing {

inline constexpr std::uint16_t kPairingVersion = 1;
inline constexpr std::size_t kSasDigits = 6;
inline constexpr std::size_t kMaximumIdentityBytes = 128;
inline constexpr std::size_t kMaximumPublicKeyBytes = 256;

enum class KeyAgreement : std::uint16_t {
    x25519 = 1,
    nist_p256 = 2,
};

[[nodiscard]] const char* key_agreement_name(KeyAgreement agreement) noexcept;

// One-shot ephemeral key pair. The private key never leaves the CNG provider,
// and the object is destroyed as soon as the pairing attempt ends.
class EphemeralKeyPair {
public:
    // Prefers Curve25519 and falls back to NIST P-256 when the platform
    // provider does not offer it.
    [[nodiscard]] static std::optional<EphemeralKeyPair> generate(
        std::string* error = nullptr);

    ~EphemeralKeyPair();
    EphemeralKeyPair(EphemeralKeyPair&&) noexcept;
    EphemeralKeyPair& operator=(EphemeralKeyPair&&) noexcept;
    EphemeralKeyPair(const EphemeralKeyPair&) = delete;
    EphemeralKeyPair& operator=(const EphemeralKeyPair&) = delete;

    [[nodiscard]] KeyAgreement agreement() const noexcept;

    // Wire encoding of the public key. This is the CNG public blob, kept whole
    // so the header stays canonical: import rejects anything whose magic, key
    // length or total size differs from what this side exports.
    [[nodiscard]] std::span<const std::byte> public_key() const noexcept;

    // Raw shared secret. Never used as a key directly - feed it to
    // derive_pairing_secrets so the transcript is bound in.
    [[nodiscard]] std::optional<std::vector<std::byte>> agree(
        std::span<const std::byte> peer_public_key,
        std::string* error = nullptr) const;

private:
    struct Impl;
    EphemeralKeyPair() noexcept;
    std::unique_ptr<Impl> impl_;
};

// Everything both sides must agree on bit for bit. A mismatch anywhere makes
// the derived SAS diverge, which is what the operator sees.
struct PairingTranscript {
    std::uint16_t version = kPairingVersion;
    KeyAgreement agreement = KeyAgreement::x25519;
    std::string client_uuid;
    std::string client_hostname;
    std::vector<std::byte> client_public_key;
    std::vector<std::byte> server_public_key;
    security::Nonce client_nonce{};
    security::Nonce server_nonce{};
};

struct PairingSecrets {
    security::Sha256Digest confirm_key{};
    security::Sha256Digest enrolled_key{};
    std::string short_authentication_string;
};

enum class ConfirmationRole : std::uint16_t {
    client = 1,
    server = 2,
};

// Identities are limited to printable ASCII so that the transcript has exactly
// one encoding for a given pair of machines.
[[nodiscard]] bool valid_transcript(const PairingTranscript& transcript) noexcept;

[[nodiscard]] std::optional<std::vector<std::byte>> encode_transcript(
    const PairingTranscript& transcript);

[[nodiscard]] std::optional<security::Sha256Digest> hkdf_sha256(
    std::span<const std::byte> input_key_material,
    std::span<const std::byte> salt, std::span<const std::byte> info) noexcept;

[[nodiscard]] std::optional<PairingSecrets> derive_pairing_secrets(
    std::span<const std::byte> shared_secret,
    const PairingTranscript& transcript);

[[nodiscard]] std::optional<security::Sha256Digest> confirmation_tag(
    const PairingSecrets& secrets, const PairingTranscript& transcript,
    ConfirmationRole role);

[[nodiscard]] bool verify_confirmation_tag(
    const PairingSecrets& secrets, const PairingTranscript& transcript,
    ConfirmationRole role, const security::Sha256Digest& received);

// Stable client identity derived from the machine UUID, so a client that
// re-pairs keeps its place in the key store instead of accumulating entries.
[[nodiscard]] std::optional<security::ClientId> client_id_from_uuid(
    std::string_view uuid) noexcept;

void secure_zero(PairingSecrets& secrets) noexcept;

} // namespace nstu::pairing
