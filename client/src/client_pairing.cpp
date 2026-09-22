#include "nstu/client_pairing.hpp"

#include "nstu/network.hpp"
#include "nstu/protocol.hpp"

#include <windows.h>

#include <objbase.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <deque>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace nstu::client {
namespace {

constexpr std::size_t kReceiveChunkBytes = 2048;
constexpr std::uint32_t kWaitSliceMs = 250;

void set_error(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
}

std::string narrow_ascii(std::wstring_view wide) {
    std::string text;
    text.reserve(wide.size());
    for (const wchar_t character : wide) {
        if (character >= 0x20 && character < 0x7f) {
            text.push_back(static_cast<char>(character));
        }
    }
    return text;
}

// UTF-8 conversion for a registry REG_SZ value. Unlike narrow_ascii this keeps
// non-ASCII code points as their multi-byte UTF-8 form, so a room label typed
// with diacritics reduces to the same bytes the server produced from its own
// UTF-8 name before either side is sanitised.
std::string wide_to_utf8(std::wstring_view wide) {
    if (wide.empty()) {
        return {};
    }
    const int needed =
        WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                            static_cast<int>(wide.size()), nullptr, 0, nullptr,
                            nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string utf8(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                        static_cast<int>(wide.size()), utf8.data(), needed,
                        nullptr, nullptr);
    return utf8;
}

// Trims leading and trailing ASCII whitespace, returning a view into the input.
std::string_view trim_ascii(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

// ASCII case-insensitive equality. A room label is matched, not authenticated,
// so a byte-wise fold is the right tool; both sides are already reduced to
// printable ASCII by sanitize_server_name before this runs.
bool equals_ignoring_case(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto lhs = static_cast<unsigned char>(left[index]);
        const auto rhs = static_cast<unsigned char>(right[index]);
        if (std::tolower(lhs) != std::tolower(rhs)) {
            return false;
        }
    }
    return true;
}

std::optional<std::wstring> read_machine_string(const wchar_t* subkey,
                                                const wchar_t* value) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, subkey, 0,
                      KEY_QUERY_VALUE | KEY_WOW64_64KEY,
                      &key) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    wchar_t buffer[128]{};
    DWORD bytes = sizeof(buffer);
    DWORD type = 0;
    const LONG read = RegQueryValueExW(key, value, nullptr, &type,
                                       reinterpret_cast<BYTE*>(buffer),
                                       &bytes);
    RegCloseKey(key);
    if (read != ERROR_SUCCESS || type != REG_SZ ||
        bytes < sizeof(wchar_t)) {
        return std::nullopt;
    }
    std::wstring text(buffer, bytes / sizeof(wchar_t));
    while (!text.empty() && text.back() == L'\0') {
        text.pop_back();
    }
    return text.empty() ? std::nullopt : std::optional{std::move(text)};
}

bool write_machine_string(const wchar_t* subkey, const wchar_t* value,
                          const std::wstring& text) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, subkey, 0, nullptr,
                        REG_OPTION_NON_VOLATILE,
                        KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &key,
                        nullptr) != ERROR_SUCCESS) {
        return false;
    }
    const auto bytes = static_cast<DWORD>((text.size() + 1) * sizeof(wchar_t));
    const LONG written = RegSetValueExW(
        key, value, 0, REG_SZ,
        reinterpret_cast<const BYTE*>(text.c_str()), bytes);
    RegCloseKey(key);
    return written == ERROR_SUCCESS;
}

std::uint64_t random_request_id() {
    std::array<std::byte, 8> bytes{};
    if (!security::generate_random(bytes)) {
        return 1;
    }
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        value |= static_cast<std::uint64_t>(
                     std::to_integer<std::uint8_t>(bytes[index]))
            << (index * 8u);
    }
    return value == 0 ? 1 : value;
}

// The server can put an approval and a heartbeat echo in one segment, so
// frames are drained from the buffer before the socket is touched again.
class FrameReader {
public:
    [[nodiscard]] std::optional<protocol::TcpFrame> take() {
        if (ready_.empty()) {
            return std::nullopt;
        }
        auto frame = std::move(ready_.front());
        ready_.pop_front();
        return frame;
    }

    [[nodiscard]] bool pump(const net::TcpSocket& socket,
                            std::string* error) {
        std::array<std::byte, kReceiveChunkBytes> buffer{};
        const int received = socket.receive(buffer, nullptr);
        if (received <= 0) {
            set_error(error, "the server closed the pairing connection");
            return false;
        }
        std::vector<protocol::TcpFrame> frames;
        if (!parser_.feed(std::span<const std::byte>(buffer).first(
                              static_cast<std::size_t>(received)),
                          frames)) {
            set_error(error, "the server sent a malformed pairing frame");
            return false;
        }
        for (auto& frame : frames) {
            ready_.push_back(std::move(frame));
        }
        return true;
    }

private:
    protocol::TcpFrameParser parser_;
    std::deque<protocol::TcpFrame> ready_;
};

std::optional<protocol::TcpFrame> read_frame(const net::TcpSocket& socket,
                                             FrameReader& reader,
                                             std::string* error) {
    for (;;) {
        if (auto frame = reader.take()) {
            return frame;
        }
        if (!reader.pump(socket, error)) {
            return std::nullopt;
        }
    }
}

bool send_frame(const net::TcpSocket& socket, protocol::CommandType type,
                std::uint64_t request_id, std::span<const std::byte> payload,
                std::string* error) {
    const protocol::CommandEnvelope envelope{
        .version = protocol::kCommandVersion,
        .type = type,
        .payload_bytes = static_cast<std::uint32_t>(payload.size()),
        .request_id = request_id,
    };
    const auto wire = protocol::encode_tcp_frame(envelope, payload);
    std::string send_error;
    if (wire.empty() ||
        socket.send_all(wire, &send_error) != static_cast<int>(wire.size())) {
        set_error(error, send_error.empty()
                             ? std::string("the pairing request was not sent")
                             : send_error);
        return false;
    }
    return true;
}

PairingOutcome outcome_from_reject(
    pairing::PairingRejectReason reason) noexcept {
    switch (reason) {
    case pairing::PairingRejectReason::operator_declined:
        return PairingOutcome::declined;
    case pairing::PairingRejectReason::timed_out:
        return PairingOutcome::timed_out;
    case pairing::PairingRejectReason::unavailable:
        return PairingOutcome::unavailable;
    case pairing::PairingRejectReason::unspecified:
    case pairing::PairingRejectReason::protocol_error:
    case pairing::PairingRejectReason::confirmation_failed:
        break;
    }
    return PairingOutcome::failed;
}

// The derived key lives in this frame until it is either copied into the
// caller's config or thrown away. Neither path may leave it in memory.
class SecretsGuard {
public:
    explicit SecretsGuard(pairing::PairingSecrets& secrets) noexcept
        : secrets_(secrets) {}
    ~SecretsGuard() { pairing::secure_zero(secrets_); }
    SecretsGuard(const SecretsGuard&) = delete;
    SecretsGuard& operator=(const SecretsGuard&) = delete;

private:
    pairing::PairingSecrets& secrets_;
};

} // namespace

const char* pairing_outcome_text(PairingOutcome outcome) noexcept {
    switch (outcome) {
    case PairingOutcome::enrolled:
        return "this computer is now managed";
    case PairingOutcome::declined:
        return "the teacher did not approve this computer";
    case PairingOutcome::timed_out:
        return "nobody approved this computer in time";
    case PairingOutcome::unavailable:
        return "the server is not accepting new computers right now";
    case PairingOutcome::failed:
        break;
    }
    return "pairing did not finish";
}

std::string machine_hostname() {
    char hostname[MAX_COMPUTERNAME_LENGTH + 1]{};
    DWORD length = MAX_COMPUTERNAME_LENGTH + 1;
    if (!GetComputerNameA(hostname, &length) || length == 0) {
        return "NSTU-CLIENT";
    }
    return std::string(hostname, length);
}

std::string machine_uuid(std::string* error) {
    // MachineGuid is written once when Windows is installed and survives
    // everything short of re-imaging - which is exactly the lifetime a lab
    // machine's identity should have, since a re-imaged machine should pair
    // again rather than inherit the old one's key.
    if (const auto installed = read_machine_string(
            L"SOFTWARE\\Microsoft\\Cryptography", L"MachineGuid")) {
        return narrow_ascii(*installed);
    }
    if (const auto stored =
            read_machine_string(L"Software\\NSTU", L"MachineId")) {
        return narrow_ascii(*stored);
    }
    GUID generated{};
    wchar_t text[40]{};
    if (FAILED(CoCreateGuid(&generated)) ||
        StringFromGUID2(generated, text,
                        static_cast<int>(std::size(text))) == 0) {
        set_error(error, "this computer has no readable identity");
        return {};
    }
    std::wstring identity(text);
    if (identity.size() >= 2 && identity.front() == L'{') {
        identity = identity.substr(1, identity.size() - 2);
    }
    // Generating a fresh identity on every attempt would leave a trail of
    // abandoned key-store entries, so a machine that cannot store one has no
    // identity at all.
    if (!write_machine_string(L"Software\\NSTU", L"MachineId", identity)) {
        set_error(error, "this computer's identity could not be stored");
        return {};
    }
    return narrow_ascii(identity);
}

PairingAttemptResult pair_with_server(
    const discovery::PairingCandidate& candidate,
    std::string_view client_uuid, std::string_view hostname,
    const PairingCodeObserver& code_observer, std::stop_token stop_token,
    const PairingAttemptOptions& options, std::string* error) {
    PairingAttemptResult result;
    const auto client_id = pairing::client_id_from_uuid(client_uuid);
    if (!client_id) {
        set_error(error, "this computer has no usable identity");
        return result;
    }
    auto key_pair = pairing::EphemeralKeyPair::generate(error);
    if (!key_pair) {
        return result;
    }
    pairing::PairingHello hello;
    hello.agreement = key_pair->agreement();
    hello.client_uuid = std::string(client_uuid);
    hello.client_hostname = std::string(hostname);
    const auto public_key = key_pair->public_key();
    hello.client_public_key.assign(public_key.begin(), public_key.end());
    if (!security::generate_random(hello.client_nonce)) {
        set_error(error, "the pairing nonce could not be generated");
        return result;
    }
    const auto hello_payload = pairing::encode_pairing_hello(hello);
    if (hello_payload.empty()) {
        set_error(error, "the pairing request could not be encoded");
        return result;
    }

    net::TcpSocket socket;
    if (!socket.connect_with_timeout(candidate.address, candidate.port,
                                     options.connect_timeout_ms, error) ||
        !socket.set_io_timeouts(5000, 5000, error)) {
        return result;
    }
    const auto request_id = random_request_id();
    FrameReader reader;
    if (!send_frame(socket, protocol::CommandType::pairing_hello, request_id,
                    hello_payload, error)) {
        return result;
    }
    const auto offered = read_frame(socket, reader, error);
    if (!offered) {
        return result;
    }
    if (offered->envelope.type == protocol::CommandType::pairing_reject) {
        const auto reason = pairing::decode_pairing_reject(offered->payload);
        result.outcome =
            reason ? outcome_from_reject(*reason) : PairingOutcome::failed;
        set_error(error, pairing_outcome_text(result.outcome));
        return result;
    }
    if (offered->envelope.type != protocol::CommandType::pairing_offer) {
        set_error(error, "the server did not answer the pairing request");
        return result;
    }
    const auto offer = pairing::decode_pairing_offer(offered->payload);
    if (!offer) {
        set_error(error, "the server's pairing offer was malformed");
        return result;
    }
    auto transcript = pairing::make_transcript(hello, *offer);
    if (!transcript) {
        set_error(error, "the pairing transcript was rejected");
        return result;
    }
    auto shared = key_pair->agree(offer->server_public_key, error);
    if (!shared) {
        return result;
    }
    auto secrets = pairing::derive_pairing_secrets(*shared, *transcript);
    security::secure_zero(*shared);
    if (!secrets) {
        set_error(error, "the pairing secrets could not be derived");
        return result;
    }
    const SecretsGuard guard(*secrets);

    // Showing the code before the confirmation goes out means the operator is
    // already reading it while the server is still deciding what to display.
    if (code_observer) {
        code_observer(secrets->short_authentication_string,
                      candidate.server_name);
    }
    const auto tag = pairing::confirmation_tag(
        *secrets, *transcript, pairing::ConfirmationRole::client);
    if (!tag) {
        set_error(error, "the pairing confirmation could not be computed");
        return result;
    }
    const pairing::PairingConfirm confirm{.client_tag = *tag};
    if (!send_frame(socket, protocol::CommandType::pairing_confirm, request_id,
                    pairing::encode_pairing_confirm(confirm), error)) {
        return result;
    }

    const auto deadline =
        std::chrono::steady_clock::now() + options.approval_timeout;
    auto next_heartbeat = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(options.heartbeat_interval_ms);
    while (!stop_token.stop_requested()) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            result.outcome = PairingOutcome::timed_out;
            set_error(error, pairing_outcome_text(result.outcome));
            return result;
        }
        auto frame = reader.take();
        if (!frame) {
            const auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - now)
                    .count();
            const auto slice = static_cast<std::uint32_t>(
                std::min<std::int64_t>(remaining, kWaitSliceMs));
            if (socket.wait_readable(slice, nullptr)) {
                if (!reader.pump(socket, error)) {
                    return result;
                }
                frame = reader.take();
            }
        }
        if (!frame) {
            // Waiting on a human outlasts the server's idle timeout, so this
            // connection has to keep saying it is still here.
            if (std::chrono::steady_clock::now() >= next_heartbeat) {
                if (!send_frame(socket, protocol::CommandType::heartbeat,
                                request_id, {}, error)) {
                    return result;
                }
                next_heartbeat = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(options.heartbeat_interval_ms);
            }
            continue;
        }
        if (frame->envelope.type == protocol::CommandType::heartbeat) {
            continue;
        }
        if (frame->envelope.type == protocol::CommandType::pairing_reject) {
            const auto reason = pairing::decode_pairing_reject(frame->payload);
            result.outcome =
                reason ? outcome_from_reject(*reason) : PairingOutcome::failed;
            set_error(error, pairing_outcome_text(result.outcome));
            return result;
        }
        if (frame->envelope.type != protocol::CommandType::pairing_accept) {
            set_error(error, "the server sent an unexpected pairing reply");
            return result;
        }
        const auto accept = pairing::decode_pairing_accept(frame->payload);
        if (!accept || accept->key_id == 0) {
            set_error(error, "the pairing approval was malformed");
            return result;
        }
        // The server tag is what ties the approval back to the machine that
        // ran the exchange, rather than to whoever holds the socket now.
        if (!pairing::verify_confirmation_tag(*secrets, *transcript,
                                              pairing::ConfirmationRole::server,
                                              accept->server_tag)) {
            set_error(error, "the server's approval did not verify");
            return result;
        }
        result.config.server_address = candidate.address;
        result.config.server_port = candidate.port;
        result.config.client_id = *client_id;
        result.config.key_id = accept->key_id;
        result.config.pre_shared_key.assign(secrets->enrolled_key.begin(),
                                            secrets->enrolled_key.end());
        result.outcome = PairingOutcome::enrolled;
        return result;
    }
    set_error(error, "pairing was cancelled");
    return result;
}

RoomSelectionResult select_preferred_room_candidate(
    std::span<const discovery::PairingCandidate> candidates,
    std::string_view preferred_room) {
    // Normalise the wanted label exactly as the beacon normalises the name it
    // advertises, then trim. If nothing is left there is no preference, and the
    // caller keeps today's sole-candidate/menu behaviour.
    const std::string normalized =
        discovery::sanitize_server_name(preferred_room);
    const std::string_view wanted = trim_ascii(normalized);
    if (wanted.empty()) {
        return {RoomSelection::no_preference, 0};
    }
    RoomSelectionResult result{RoomSelection::unmatched, 0};
    bool found = false;
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        // Candidate names arrive already sanitised on the wire, so only the
        // trim is needed before the case-insensitive compare.
        if (!equals_ignoring_case(trim_ascii(candidates[index].server_name),
                                  wanted)) {
            continue;
        }
        if (found) {
            // A second server claims the same room. The SAS is per-server, so
            // guessing between them would just waste an operator's approval;
            // refuse and let the caller fall back to the menu.
            return {RoomSelection::unmatched, 0};
        }
        found = true;
        result = {RoomSelection::matched, index};
    }
    return result;
}

std::string read_preferred_room_seed() {
    if (const auto seed =
            read_machine_string(L"Software\\NSTU", L"PreferredRoom")) {
        return wide_to_utf8(*seed);
    }
    return {};
}

} // namespace nstu::client
