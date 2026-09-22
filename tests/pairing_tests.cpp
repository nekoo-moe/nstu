#include "nstu/pairing.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace {

using nstu::pairing::ConfirmationRole;
using nstu::pairing::EphemeralKeyPair;
using nstu::pairing::KeyAgreement;
using nstu::pairing::PairingSecrets;
using nstu::pairing::PairingTranscript;

// The enrolled key becomes the client's long-term protocol key, so the digest
// it is derived into has to be at least as long as one.
static_assert(nstu::security::kSha256Bytes >=
              nstu::security::kMinimumProtocolKeyBytes);

nstu::security::Nonce make_nonce(std::uint8_t seed) {
    nstu::security::Nonce nonce{};
    for (std::size_t index = 0; index < nonce.size(); ++index) {
        nonce[index] = static_cast<std::byte>(seed + index);
    }
    return nonce;
}

std::vector<std::byte> to_vector(std::span<const std::byte> bytes) {
    return {bytes.begin(), bytes.end()};
}

EphemeralKeyPair must_generate() {
    std::string error;
    auto pair = EphemeralKeyPair::generate(&error);
    assert(pair.has_value() && error.empty());
    return std::move(*pair);
}

std::vector<std::byte> must_agree(const EphemeralKeyPair& own,
                                  const EphemeralKeyPair& peer) {
    std::string error;
    auto secret = own.agree(peer.public_key(), &error);
    assert(secret.has_value() && error.empty());
    return std::move(*secret);
}

PairingTranscript make_key_transcript(const EphemeralKeyPair& client,
                                      const EphemeralKeyPair& server) {
    PairingTranscript transcript;
    transcript.agreement = client.agreement();
    transcript.client_uuid = "4C4C4544-0037-5A10-8043-B7C04F565432";
    transcript.client_hostname = "LAB-PC-07";
    transcript.client_public_key = to_vector(client.public_key());
    transcript.server_public_key = to_vector(server.public_key());
    transcript.client_nonce = make_nonce(0x11);
    transcript.server_nonce = make_nonce(0x71);
    return transcript;
}

bool all_decimal_digits(const std::string& value) {
    return std::all_of(value.begin(), value.end(),
                       [](char character) {
                           return character >= '0' && character <= '9';
                       });
}

void test_matched_transcripts_agree() {
    const auto client = must_generate();
    const auto server = must_generate();
    assert(client.agreement() == server.agreement());
    assert(!client.public_key().empty());

    const auto client_secret = must_agree(client, server);
    const auto server_secret = must_agree(server, client);
    assert(client_secret == server_secret);

    const auto transcript = make_key_transcript(client, server);
    const auto client_view =
        nstu::pairing::derive_pairing_secrets(client_secret, transcript);
    const auto server_view =
        nstu::pairing::derive_pairing_secrets(server_secret, transcript);
    assert(client_view.has_value() && server_view.has_value());
    assert(client_view->confirm_key == server_view->confirm_key);
    assert(client_view->enrolled_key == server_view->enrolled_key);
    assert(client_view->short_authentication_string ==
           server_view->short_authentication_string);
    assert(client_view->short_authentication_string.size() ==
           nstu::pairing::kSasDigits);
    assert(all_decimal_digits(client_view->short_authentication_string));

    // The three derived values must be independent of one another.
    assert(client_view->confirm_key != client_view->enrolled_key);
}

// A man in the middle runs a separate exchange with each side. Neither the
// shared secret nor the transcript matches, so the two screens show different
// codes and the operator rejects the pairing.
void test_man_in_the_middle_diverges() {
    const auto client = must_generate();
    const auto server = must_generate();
    const auto attacker_to_client = must_generate();
    const auto attacker_to_server = must_generate();

    const auto client_secret = must_agree(client, attacker_to_client);
    const auto server_secret = must_agree(server, attacker_to_server);
    assert(client_secret != server_secret);

    PairingTranscript client_view =
        make_key_transcript(client, attacker_to_client);
    PairingTranscript server_view =
        make_key_transcript(attacker_to_server, server);

    const auto client_secrets =
        nstu::pairing::derive_pairing_secrets(client_secret, client_view);
    const auto server_secrets =
        nstu::pairing::derive_pairing_secrets(server_secret, server_view);
    assert(client_secrets.has_value() && server_secrets.has_value());
    assert(client_secrets->short_authentication_string !=
           server_secrets->short_authentication_string);
    assert(client_secrets->enrolled_key != server_secrets->enrolled_key);
}

// Even if an attacker somehow held the shared secret, swapping a public key in
// the transcript still changes the code both operators read.
void test_transcript_binds_public_keys() {
    const auto client = must_generate();
    const auto server = must_generate();
    const auto impostor = must_generate();
    const auto secret = must_agree(client, server);

    const auto honest = make_key_transcript(client, server);
    PairingTranscript swapped = honest;
    swapped.server_public_key = to_vector(impostor.public_key());

    const auto honest_secrets =
        nstu::pairing::derive_pairing_secrets(secret, honest);
    const auto swapped_secrets =
        nstu::pairing::derive_pairing_secrets(secret, swapped);
    assert(honest_secrets.has_value() && swapped_secrets.has_value());
    assert(honest_secrets->short_authentication_string !=
           swapped_secrets->short_authentication_string);
    assert(honest_secrets->enrolled_key != swapped_secrets->enrolled_key);
}

void test_transcript_binds_identity_and_nonces() {
    const auto client = must_generate();
    const auto server = must_generate();
    const auto secret = must_agree(client, server);
    const auto honest = make_key_transcript(client, server);
    const auto baseline = nstu::pairing::derive_pairing_secrets(secret, honest);
    assert(baseline.has_value());

    PairingTranscript renamed = honest;
    renamed.client_hostname = "LAB-PC-08";
    const auto renamed_secrets =
        nstu::pairing::derive_pairing_secrets(secret, renamed);
    assert(renamed_secrets.has_value());
    assert(renamed_secrets->short_authentication_string !=
           baseline->short_authentication_string);

    PairingTranscript reidentified = honest;
    reidentified.client_uuid = "4C4C4544-0037-5A10-8043-B7C04F565433";
    const auto reidentified_secrets =
        nstu::pairing::derive_pairing_secrets(secret, reidentified);
    assert(reidentified_secrets.has_value());
    assert(reidentified_secrets->enrolled_key != baseline->enrolled_key);

    PairingTranscript replayed = honest;
    replayed.server_nonce = make_nonce(0x72);
    const auto replayed_secrets =
        nstu::pairing::derive_pairing_secrets(secret, replayed);
    assert(replayed_secrets.has_value());
    assert(replayed_secrets->enrolled_key != baseline->enrolled_key);
    assert(replayed_secrets->short_authentication_string !=
           baseline->short_authentication_string);
}

void test_confirmation_tags() {
    const auto client = must_generate();
    const auto server = must_generate();
    const auto secret = must_agree(client, server);
    const auto transcript = make_key_transcript(client, server);
    const auto secrets =
        nstu::pairing::derive_pairing_secrets(secret, transcript);
    assert(secrets.has_value());

    const auto client_tag = nstu::pairing::confirmation_tag(
        *secrets, transcript, ConfirmationRole::client);
    const auto server_tag = nstu::pairing::confirmation_tag(
        *secrets, transcript, ConfirmationRole::server);
    assert(client_tag.has_value() && server_tag.has_value());
    assert(*client_tag != *server_tag);

    assert(nstu::pairing::verify_confirmation_tag(
        *secrets, transcript, ConfirmationRole::client, *client_tag));
    assert(nstu::pairing::verify_confirmation_tag(
        *secrets, transcript, ConfirmationRole::server, *server_tag));

    // A tag minted for one direction must not be accepted for the other.
    assert(!nstu::pairing::verify_confirmation_tag(
        *secrets, transcript, ConfirmationRole::server, *client_tag));

    // A single flipped bit in the tag is rejected.
    auto corrupted = *client_tag;
    corrupted[0] ^= std::byte{0x01};
    assert(!nstu::pairing::verify_confirmation_tag(
        *secrets, transcript, ConfirmationRole::client, corrupted));

    // So is a tag carried over to a modified transcript.
    PairingTranscript tampered = transcript;
    tampered.client_hostname = "LAB-PC-99";
    assert(!nstu::pairing::verify_confirmation_tag(
        *secrets, tampered, ConfirmationRole::client, *client_tag));

    // And a tag verified against the wrong session key.
    PairingSecrets foreign = *secrets;
    foreign.confirm_key[0] ^= std::byte{0x80};
    assert(!nstu::pairing::verify_confirmation_tag(
        foreign, transcript, ConfirmationRole::client, *client_tag));
}

void test_transcript_validation() {
    const auto client = must_generate();
    const auto server = must_generate();
    const auto valid = make_key_transcript(client, server);
    assert(nstu::pairing::valid_transcript(valid));
    assert(nstu::pairing::encode_transcript(valid).has_value());

    PairingTranscript no_uuid = valid;
    no_uuid.client_uuid.clear();
    assert(!nstu::pairing::valid_transcript(no_uuid));

    PairingTranscript long_hostname = valid;
    long_hostname.client_hostname.assign(
        nstu::pairing::kMaximumIdentityBytes + 1, 'a');
    assert(!nstu::pairing::valid_transcript(long_hostname));

    PairingTranscript control_characters = valid;
    control_characters.client_hostname = std::string("LAB\001PC");
    assert(!nstu::pairing::valid_transcript(control_characters));

    PairingTranscript zero_nonce = valid;
    zero_nonce.client_nonce = nstu::security::Nonce{};
    assert(!nstu::pairing::valid_transcript(zero_nonce));

    PairingTranscript no_key = valid;
    no_key.server_public_key.clear();
    assert(!nstu::pairing::valid_transcript(no_key));

    PairingTranscript unknown_agreement = valid;
    unknown_agreement.agreement = static_cast<KeyAgreement>(0x4242);
    assert(!nstu::pairing::valid_transcript(unknown_agreement));

    PairingTranscript zero_version = valid;
    zero_version.version = 0;
    assert(!nstu::pairing::valid_transcript(zero_version));

    // Derivation refuses an invalid transcript rather than producing a key.
    const auto secret = must_agree(client, server);
    assert(!nstu::pairing::derive_pairing_secrets(secret, no_uuid).has_value());

    // A short shared secret is refused too.
    const std::vector<std::byte> short_secret(8, std::byte{0x5a});
    assert(!nstu::pairing::derive_pairing_secrets(short_secret, valid)
                .has_value());
}

// Length-prefixed fields: moving a byte from one identity to the next must not
// produce the same encoding.
void test_transcript_encoding_is_unambiguous() {
    const auto client = must_generate();
    const auto server = must_generate();

    PairingTranscript left = make_key_transcript(client, server);
    left.client_uuid = "AB";
    left.client_hostname = "CDE";

    PairingTranscript right = left;
    right.client_uuid = "ABC";
    right.client_hostname = "DE";

    const auto left_encoded = nstu::pairing::encode_transcript(left);
    const auto right_encoded = nstu::pairing::encode_transcript(right);
    assert(left_encoded.has_value() && right_encoded.has_value());
    assert(*left_encoded != *right_encoded);
}

void test_agree_rejects_malformed_peer_keys() {
    const auto client = must_generate();
    const auto server = must_generate();

    std::string error;
    assert(!client.agree({}, &error).has_value());
    assert(!error.empty());

    const auto peer = to_vector(server.public_key());
    std::vector<std::byte> truncated(peer.begin(), peer.end() - 1);
    error.clear();
    assert(!client.agree(truncated, &error).has_value());
    assert(!error.empty());

    // Reflecting our own public key back at us is not a valid exchange.
    error.clear();
    assert(!client.agree(client.public_key(), &error).has_value());
    assert(!error.empty());

    // Garbage of the right length fails the curve check before import.
    std::vector<std::byte> garbage(peer.size(), std::byte{0xcd});
    error.clear();
    assert(!client.agree(garbage, &error).has_value());
    assert(!error.empty());
}

void test_hkdf_separates_labels() {
    const std::vector<std::byte> ikm(32, std::byte{0x2b});
    const std::vector<std::byte> salt(32, std::byte{0x7c});
    const std::string first = "label-one";
    const std::string second = "label-two";

    const auto a = nstu::pairing::hkdf_sha256(
        ikm, salt,
        {reinterpret_cast<const std::byte*>(first.data()), first.size()});
    const auto b = nstu::pairing::hkdf_sha256(
        ikm, salt,
        {reinterpret_cast<const std::byte*>(second.data()), second.size()});
    assert(a.has_value() && b.has_value());
    assert(*a != *b);

    // Deterministic for the same inputs.
    const auto again = nstu::pairing::hkdf_sha256(
        ikm, salt,
        {reinterpret_cast<const std::byte*>(first.data()), first.size()});
    assert(again.has_value() && *again == *a);

    // A different salt gives a different key even with the same label.
    const std::vector<std::byte> other_salt(32, std::byte{0x7d});
    const auto resalted = nstu::pairing::hkdf_sha256(
        ikm, other_salt,
        {reinterpret_cast<const std::byte*>(first.data()), first.size()});
    assert(resalted.has_value() && *resalted != *a);

    assert(!nstu::pairing::hkdf_sha256({}, salt, {}).has_value());
}

void test_client_id_from_uuid() {
    const auto first = nstu::pairing::client_id_from_uuid(
        "4C4C4544-0037-5A10-8043-B7C04F565432");
    const auto again = nstu::pairing::client_id_from_uuid(
        "4C4C4544-0037-5A10-8043-B7C04F565432");
    const auto other = nstu::pairing::client_id_from_uuid(
        "4C4C4544-0037-5A10-8043-B7C04F565433");
    assert(first.has_value() && again.has_value() && other.has_value());
    assert(*first == *again);
    assert(*first != *other);
    assert(std::any_of(first->begin(), first->end(), [](std::byte value) {
        return value != std::byte{0};
    }));

    assert(!nstu::pairing::client_id_from_uuid("").has_value());
    assert(!nstu::pairing::client_id_from_uuid(
                std::string(nstu::pairing::kMaximumIdentityBytes + 1, 'a'))
                .has_value());
}

nstu::pairing::PairingHello make_hello(const EphemeralKeyPair& client) {
    nstu::pairing::PairingHello hello;
    hello.agreement = client.agreement();
    hello.client_uuid = "4C4C4544-0037-5A10-8043-B7C04F565432";
    hello.client_hostname = "LAB-PC-07";
    hello.client_public_key = to_vector(client.public_key());
    hello.client_nonce = make_nonce(0x11);
    return hello;
}

nstu::pairing::PairingOffer make_offer(const EphemeralKeyPair& server) {
    nstu::pairing::PairingOffer offer;
    offer.agreement = server.agreement();
    offer.server_public_key = to_vector(server.public_key());
    offer.server_nonce = make_nonce(0x71);
    return offer;
}

void test_wire_messages_round_trip() {
    const auto client = must_generate();
    const auto server = must_generate();

    const auto hello = make_hello(client);
    const auto hello_wire = nstu::pairing::encode_pairing_hello(hello);
    assert(!hello_wire.empty());
    assert(hello_wire.size() <= nstu::pairing::kMaximumPairingMessageBytes);
    const auto decoded_hello = nstu::pairing::decode_pairing_hello(hello_wire);
    assert(decoded_hello.has_value());
    assert(decoded_hello->version == hello.version);
    assert(decoded_hello->agreement == hello.agreement);
    assert(decoded_hello->client_uuid == hello.client_uuid);
    assert(decoded_hello->client_hostname == hello.client_hostname);
    assert(decoded_hello->client_public_key == hello.client_public_key);
    assert(decoded_hello->client_nonce == hello.client_nonce);

    const auto offer = make_offer(server);
    const auto offer_wire = nstu::pairing::encode_pairing_offer(offer);
    assert(!offer_wire.empty());
    const auto decoded_offer = nstu::pairing::decode_pairing_offer(offer_wire);
    assert(decoded_offer.has_value());
    assert(decoded_offer->server_public_key == offer.server_public_key);
    assert(decoded_offer->server_nonce == offer.server_nonce);

    nstu::pairing::PairingConfirm confirm;
    confirm.client_tag = make_nonce(0x31);
    const auto confirm_wire = nstu::pairing::encode_pairing_confirm(confirm);
    const auto decoded_confirm =
        nstu::pairing::decode_pairing_confirm(confirm_wire);
    assert(decoded_confirm.has_value());
    assert(decoded_confirm->client_tag == confirm.client_tag);

    nstu::pairing::PairingAccept accept;
    accept.key_id = 0x2ca5f001u;
    accept.server_tag = make_nonce(0x41);
    const auto accept_wire = nstu::pairing::encode_pairing_accept(accept);
    assert(!accept_wire.empty());
    const auto decoded_accept =
        nstu::pairing::decode_pairing_accept(accept_wire);
    assert(decoded_accept.has_value());
    assert(decoded_accept->key_id == accept.key_id);
    assert(decoded_accept->server_tag == accept.server_tag);

    const auto reject_wire = nstu::pairing::encode_pairing_reject(
        nstu::pairing::PairingRejectReason::operator_declined);
    const auto decoded_reject =
        nstu::pairing::decode_pairing_reject(reject_wire);
    assert(decoded_reject.has_value());
    assert(*decoded_reject ==
           nstu::pairing::PairingRejectReason::operator_declined);
}

void test_wire_messages_reject_malformed_input() {
    const auto client = must_generate();
    const auto hello = make_hello(client);
    const auto wire = nstu::pairing::encode_pairing_hello(hello);

    // Truncation, trailing junk and an oversized payload are all refused.
    assert(!nstu::pairing::decode_pairing_hello(
                std::span<const std::byte>(wire).first(wire.size() - 1))
                .has_value());
    auto padded = wire;
    padded.push_back(std::byte{0});
    assert(!nstu::pairing::decode_pairing_hello(padded).has_value());
    const std::vector<std::byte> oversized(
        nstu::pairing::kMaximumPairingMessageBytes + 1, std::byte{0});
    assert(!nstu::pairing::decode_pairing_hello(oversized).has_value());
    assert(!nstu::pairing::decode_pairing_hello({}).has_value());

    // A length prefix that runs past the end of the buffer must not read out
    // of bounds; it is simply rejected.
    auto overlong_uuid = wire;
    overlong_uuid[4] = std::byte{0xff};
    overlong_uuid[5] = std::byte{0x00};
    assert(!nstu::pairing::decode_pairing_hello(overlong_uuid).has_value());

    // Encoding refuses to emit a message the peer would have to reject.
    nstu::pairing::PairingHello anonymous = hello;
    anonymous.client_uuid.clear();
    assert(nstu::pairing::encode_pairing_hello(anonymous).empty());
    nstu::pairing::PairingHello zero_nonce = hello;
    zero_nonce.client_nonce = nstu::security::Nonce{};
    assert(nstu::pairing::encode_pairing_hello(zero_nonce).empty());
    nstu::pairing::PairingHello unknown_curve = hello;
    unknown_curve.agreement = static_cast<KeyAgreement>(0x4242);
    assert(nstu::pairing::encode_pairing_hello(unknown_curve).empty());

    nstu::pairing::PairingAccept unkeyed;
    unkeyed.key_id = 0;
    assert(nstu::pairing::encode_pairing_accept(unkeyed).empty());
    assert(!nstu::pairing::decode_pairing_confirm({}).has_value());
    assert(!nstu::pairing::decode_pairing_accept({}).has_value());

    const std::vector<std::byte> unknown_reason{std::byte{0xff},
                                                std::byte{0xff}};
    assert(!nstu::pairing::decode_pairing_reject(unknown_reason).has_value());
}

// The transcript both sides derive from must come out of the same builder, or
// the codes diverge for a reason no operator could diagnose.
void test_make_transcript_matches_manual_build() {
    const auto client = must_generate();
    const auto server = must_generate();
    const auto hello = make_hello(client);
    const auto offer = make_offer(server);

    const auto built = nstu::pairing::make_transcript(hello, offer);
    assert(built.has_value());
    const auto expected = make_key_transcript(client, server);
    const auto built_encoded = nstu::pairing::encode_transcript(*built);
    const auto expected_encoded = nstu::pairing::encode_transcript(expected);
    assert(built_encoded.has_value() && expected_encoded.has_value());
    assert(*built_encoded == *expected_encoded);

    // A curve or version disagreement is caught here rather than surfacing as
    // an unexplained code mismatch.
    auto mismatched = offer;
    mismatched.agreement = hello.agreement == KeyAgreement::x25519
                               ? KeyAgreement::nist_p256
                               : KeyAgreement::x25519;
    assert(!nstu::pairing::make_transcript(hello, mismatched).has_value());

    auto downgraded = offer;
    downgraded.version = static_cast<std::uint16_t>(hello.version + 1);
    assert(!nstu::pairing::make_transcript(hello, downgraded).has_value());
}

void test_secure_zero_clears_secrets() {    const auto client = must_generate();
    const auto server = must_generate();
    const auto secret = must_agree(client, server);
    const auto transcript = make_key_transcript(client, server);
    auto secrets = nstu::pairing::derive_pairing_secrets(secret, transcript);
    assert(secrets.has_value());

    nstu::pairing::secure_zero(*secrets);
    assert(secrets->short_authentication_string.empty());
    assert(std::all_of(secrets->enrolled_key.begin(),
                       secrets->enrolled_key.end(), [](std::byte value) {
                           return value == std::byte{0};
                       }));
    assert(std::all_of(secrets->confirm_key.begin(),
                       secrets->confirm_key.end(), [](std::byte value) {
                           return value == std::byte{0};
                       }));
}

} // namespace

int main() {
    test_matched_transcripts_agree();
    test_man_in_the_middle_diverges();
    test_transcript_binds_public_keys();
    test_transcript_binds_identity_and_nonces();
    test_confirmation_tags();
    test_transcript_validation();
    test_transcript_encoding_is_unambiguous();
    test_agree_rejects_malformed_peer_keys();
    test_hkdf_separates_labels();
    test_client_id_from_uuid();
    test_wire_messages_round_trip();
    test_wire_messages_reject_malformed_input();
    test_make_transcript_matches_manual_build();
    test_secure_zero_clears_secrets();
    return 0;
}
