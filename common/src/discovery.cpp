#include "nstu/discovery.hpp"

#include "nstu/multicast.hpp"
#include "nstu/rate_limiter.hpp"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <limits>
#include <mutex>
#include <stop_token>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace nstu::discovery {
namespace {

inline constexpr std::array<std::byte, 4> kMagic{
    static_cast<std::byte>('N'), static_cast<std::byte>('S'),
    static_cast<std::byte>('T'), static_cast<std::byte>('D')};
static_assert(kDiscoveryPacketBytes ==
              kMagic.size() + sizeof(std::uint16_t) * 4 +
                  security::kClientIdBytes + sizeof(std::uint32_t) +
                  security::kNonceBytes + sizeof(std::uint64_t) +
                  security::kSha256Bytes);
inline constexpr std::size_t kMaximumDiscoveryTargets = 32;

// Pairing probes ride the same UDP port but carry their own magic, because
// they are a different trust model: no key exists yet on either side, so the
// trailing digest is an integrity and format check, never authentication.
inline constexpr std::array<std::byte, 4> kPairingMagic{
    static_cast<std::byte>('N'), static_cast<std::byte>('S'),
    static_cast<std::byte>('T'), static_cast<std::byte>('P')};

enum class PairingPacketKind : std::uint16_t {
    probe = 1,
    beacon = 2,
};

struct PairingProbe {
    security::Nonce client_nonce{};
    std::uint64_t unix_time_seconds = 0;
};

struct PairingBeacon {
    security::Nonce client_nonce{};
    std::uint64_t unix_time_seconds = 0;
    std::uint16_t control_port = 0;
    std::string server_name;
};

enum class PacketKind : std::uint16_t {
    request = 1,
    response = 2,
};

struct Packet {
    PacketKind kind = PacketKind::request;
    security::ClientId client_id{};
    std::uint32_t key_id = 0;
    security::Nonce client_nonce{};
    std::uint64_t unix_time_seconds = 0;
    std::uint16_t control_port = 0;
    security::Sha256Digest authenticator{};
};

void set_error(std::string* error, const char* message) {
    if (error != nullptr) {
        *error = message;
    }
}

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

template <typename Range>
bool any_nonzero(const Range& values) noexcept {
    return std::any_of(values.begin(), values.end(),
                       [](std::byte value) { return value != std::byte{0}; });
}

bool valid_request_fields(const DiscoveryRequest& request) noexcept {
    return request.key_id != 0 && request.unix_time_seconds != 0 &&
           any_nonzero(request.client_id) && any_nonzero(request.client_nonce);
}

bool valid_response_fields(const DiscoveryResponse& response) noexcept {
    return response.key_id != 0 && response.unix_time_seconds != 0 &&
           response.control_port != 0 && any_nonzero(response.client_id) &&
           any_nonzero(response.client_nonce);
}

bool time_is_valid(std::uint64_t peer_time, std::uint64_t now,
                   std::chrono::seconds maximum_clock_skew) noexcept {
    const auto allowed = static_cast<std::uint64_t>(
        std::max<std::int64_t>(maximum_clock_skew.count(), 1));
    const auto difference = peer_time > now ? peer_time - now : now - peer_time;
    return difference <= allowed;
}

std::vector<std::byte> authentication_message(
    std::string_view domain, PacketKind kind,
    const security::ClientId& client_id, std::uint32_t key_id,
    const security::Nonce& client_nonce, std::uint64_t unix_time_seconds,
    std::uint16_t control_port) {
    std::vector<std::byte> message;
    message.reserve(domain.size() + 66);
    message.insert(message.end(),
                   reinterpret_cast<const std::byte*>(domain.data()),
                   reinterpret_cast<const std::byte*>(domain.data()) +
                       domain.size());
    append_le(message, kDiscoveryVersion);
    append_le(message, static_cast<std::uint16_t>(kind));
    message.insert(message.end(), client_id.begin(), client_id.end());
    append_le(message, key_id);
    message.insert(message.end(), client_nonce.begin(), client_nonce.end());
    append_le(message, unix_time_seconds);
    append_le(message, control_port);
    return message;
}

std::vector<std::byte> encode_packet(const Packet& packet) {
    std::vector<std::byte> wire;
    wire.reserve(kDiscoveryPacketBytes);
    wire.insert(wire.end(), kMagic.begin(), kMagic.end());
    append_le(wire, kDiscoveryVersion);
    append_le(wire, static_cast<std::uint16_t>(packet.kind));
    wire.insert(wire.end(), packet.client_id.begin(), packet.client_id.end());
    append_le(wire, packet.key_id);
    wire.insert(wire.end(), packet.client_nonce.begin(),
                packet.client_nonce.end());
    append_le(wire, packet.unix_time_seconds);
    append_le(wire, packet.control_port);
    append_le(wire, std::uint16_t{0});
    wire.insert(wire.end(), packet.authenticator.begin(),
                packet.authenticator.end());
    if (wire.size() != kDiscoveryPacketBytes) {
        return {};
    }
    return wire;
}

std::optional<Packet> decode_packet(std::span<const std::byte> wire) {
    if (wire.size() != kDiscoveryPacketBytes ||
        !std::equal(kMagic.begin(), kMagic.end(), wire.begin())) {
        return std::nullopt;
    }
    Packet packet;
    std::size_t offset = kMagic.size();
    std::uint16_t version = 0;
    std::uint16_t raw_kind = 0;
    std::uint16_t reserved = 0;
    if (!read_le(wire, offset, version) || version != kDiscoveryVersion ||
        !read_le(wire, offset, raw_kind) ||
        (raw_kind != static_cast<std::uint16_t>(PacketKind::request) &&
         raw_kind != static_cast<std::uint16_t>(PacketKind::response))) {
        return std::nullopt;
    }
    packet.kind = static_cast<PacketKind>(raw_kind);
    std::copy_n(wire.begin() + static_cast<std::ptrdiff_t>(offset),
                packet.client_id.size(), packet.client_id.begin());
    offset += packet.client_id.size();
    if (!read_le(wire, offset, packet.key_id)) {
        return std::nullopt;
    }
    std::copy_n(wire.begin() + static_cast<std::ptrdiff_t>(offset),
                packet.client_nonce.size(), packet.client_nonce.begin());
    offset += packet.client_nonce.size();
    if (!read_le(wire, offset, packet.unix_time_seconds) ||
        !read_le(wire, offset, packet.control_port) ||
        !read_le(wire, offset, reserved) || reserved != 0 ||
        offset + packet.authenticator.size() != wire.size()) {
        return std::nullopt;
    }
    std::copy_n(wire.begin() + static_cast<std::ptrdiff_t>(offset),
                packet.authenticator.size(), packet.authenticator.begin());
    return packet;
}

std::uint64_t unix_seconds_now() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<
        std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
                                          .count());
}

std::vector<std::byte> pairing_digest_message(std::string_view domain,
                                              PairingPacketKind kind,
                                              const PairingBeacon& fields) {
    std::vector<std::byte> message;
    message.insert(message.end(),
                   reinterpret_cast<const std::byte*>(domain.data()),
                   reinterpret_cast<const std::byte*>(domain.data() +
                                                      domain.size()));
    append_le(message, kDiscoveryVersion);
    append_le(message, static_cast<std::uint16_t>(kind));
    message.insert(message.end(), fields.client_nonce.begin(),
                   fields.client_nonce.end());
    append_le(message, fields.unix_time_seconds);
    append_le(message, fields.control_port);
    append_le(message, static_cast<std::uint16_t>(fields.server_name.size()));
    message.insert(
        message.end(),
        reinterpret_cast<const std::byte*>(fields.server_name.data()),
        reinterpret_cast<const std::byte*>(fields.server_name.data() +
                                           fields.server_name.size()));
    return message;
}

bool is_pairing_datagram(std::span<const std::byte> wire) noexcept {
    return wire.size() >= kPairingMagic.size() &&
           std::equal(kPairingMagic.begin(), kPairingMagic.end(),
                      wire.begin());
}

std::vector<std::byte> encode_pairing_probe(const PairingProbe& probe) {
    if (probe.unix_time_seconds == 0 || !any_nonzero(probe.client_nonce)) {
        return {};
    }
    PairingBeacon fields;
    fields.client_nonce = probe.client_nonce;
    fields.unix_time_seconds = probe.unix_time_seconds;
    const auto digest = security::sha256(pairing_digest_message(
        "NSTU-PAIRING-PROBE-V1", PairingPacketKind::probe, fields));
    if (!digest) {
        return {};
    }
    std::vector<std::byte> wire;
    wire.reserve(kPairingProbeBytes);
    wire.insert(wire.end(), kPairingMagic.begin(), kPairingMagic.end());
    append_le(wire, kDiscoveryVersion);
    append_le(wire, static_cast<std::uint16_t>(PairingPacketKind::probe));
    wire.insert(wire.end(), probe.client_nonce.begin(),
                probe.client_nonce.end());
    append_le(wire, probe.unix_time_seconds);
    wire.insert(wire.end(), digest->begin(), digest->end());
    return wire.size() == kPairingProbeBytes ? wire
                                             : std::vector<std::byte>{};
}

std::optional<PairingProbe> decode_pairing_probe(
    std::span<const std::byte> wire) {
    if (wire.size() != kPairingProbeBytes || !is_pairing_datagram(wire)) {
        return std::nullopt;
    }
    std::size_t offset = kPairingMagic.size();
    std::uint16_t version = 0;
    std::uint16_t kind = 0;
    PairingProbe probe;
    if (!read_le(wire, offset, version) || version != kDiscoveryVersion ||
        !read_le(wire, offset, kind) ||
        kind != static_cast<std::uint16_t>(PairingPacketKind::probe)) {
        return std::nullopt;
    }
    std::copy_n(wire.begin() + static_cast<std::ptrdiff_t>(offset),
                probe.client_nonce.size(), probe.client_nonce.begin());
    offset += probe.client_nonce.size();
    if (!read_le(wire, offset, probe.unix_time_seconds) ||
        probe.unix_time_seconds == 0 || !any_nonzero(probe.client_nonce)) {
        return std::nullopt;
    }
    PairingBeacon fields;
    fields.client_nonce = probe.client_nonce;
    fields.unix_time_seconds = probe.unix_time_seconds;
    const auto digest = security::sha256(pairing_digest_message(
        "NSTU-PAIRING-PROBE-V1", PairingPacketKind::probe, fields));
    if (!digest ||
        !std::equal(digest->begin(), digest->end(),
                    wire.begin() + static_cast<std::ptrdiff_t>(offset))) {
        return std::nullopt;
    }
    return probe;
}

std::vector<std::byte> encode_pairing_beacon(const PairingBeacon& beacon) {
    if (beacon.unix_time_seconds == 0 || beacon.control_port == 0 ||
        !any_nonzero(beacon.client_nonce) ||
        beacon.server_name.size() > kMaximumServerNameBytes) {
        return {};
    }
    const auto digest = security::sha256(pairing_digest_message(
        "NSTU-PAIRING-BEACON-V1", PairingPacketKind::beacon, beacon));
    if (!digest) {
        return {};
    }
    std::vector<std::byte> wire;
    wire.insert(wire.end(), kPairingMagic.begin(), kPairingMagic.end());
    append_le(wire, kDiscoveryVersion);
    append_le(wire, static_cast<std::uint16_t>(PairingPacketKind::beacon));
    wire.insert(wire.end(), beacon.client_nonce.begin(),
                beacon.client_nonce.end());
    append_le(wire, beacon.unix_time_seconds);
    append_le(wire, beacon.control_port);
    append_le(wire, static_cast<std::uint16_t>(beacon.server_name.size()));
    wire.insert(
        wire.end(),
        reinterpret_cast<const std::byte*>(beacon.server_name.data()),
        reinterpret_cast<const std::byte*>(beacon.server_name.data() +
                                           beacon.server_name.size()));
    wire.insert(wire.end(), digest->begin(), digest->end());
    return wire.size() <= kMaximumDiscoveryDatagramBytes
               ? wire
               : std::vector<std::byte>{};
}

std::optional<PairingBeacon> decode_pairing_beacon(
    std::span<const std::byte> wire) {
    constexpr std::size_t kFixedBytes = 4 + 2 + 2 + security::kNonceBytes + 8 +
                                        2 + 2 + security::kSha256Bytes;
    if (wire.size() < kFixedBytes ||
        wire.size() > kMaximumDiscoveryDatagramBytes ||
        !is_pairing_datagram(wire)) {
        return std::nullopt;
    }
    std::size_t offset = kPairingMagic.size();
    std::uint16_t version = 0;
    std::uint16_t kind = 0;
    PairingBeacon beacon;
    if (!read_le(wire, offset, version) || version != kDiscoveryVersion ||
        !read_le(wire, offset, kind) ||
        kind != static_cast<std::uint16_t>(PairingPacketKind::beacon)) {
        return std::nullopt;
    }
    std::copy_n(wire.begin() + static_cast<std::ptrdiff_t>(offset),
                beacon.client_nonce.size(), beacon.client_nonce.begin());
    offset += beacon.client_nonce.size();
    std::uint16_t name_bytes = 0;
    if (!read_le(wire, offset, beacon.unix_time_seconds) ||
        !read_le(wire, offset, beacon.control_port) ||
        !read_le(wire, offset, name_bytes) ||
        name_bytes > kMaximumServerNameBytes ||
        wire.size() != offset + name_bytes + security::kSha256Bytes ||
        beacon.unix_time_seconds == 0 || beacon.control_port == 0 ||
        !any_nonzero(beacon.client_nonce)) {
        return std::nullopt;
    }
    beacon.server_name.assign(
        reinterpret_cast<const char*>(wire.data() + offset), name_bytes);
    if (beacon.server_name != sanitize_server_name(beacon.server_name)) {
        return std::nullopt;
    }
    offset += name_bytes;
    const auto digest = security::sha256(pairing_digest_message(
        "NSTU-PAIRING-BEACON-V1", PairingPacketKind::beacon, beacon));
    if (!digest ||
        !std::equal(digest->begin(), digest->end(),
                    wire.begin() + static_cast<std::ptrdiff_t>(offset))) {
        return std::nullopt;
    }
    return beacon;
}

#if defined(_WIN32)

inline constexpr DWORD kSioUdpConnectionReset =
    _WSAIOW(IOC_VENDOR, 12);

void set_winsock_error(std::string* error, const char* operation) {
    if (error != nullptr) {
        *error = std::string(operation) + " failed with Winsock error " +
                 std::to_string(WSAGetLastError());
    }
}

bool disable_udp_connection_reset(SOCKET socket, std::string* error) {
    BOOL disabled = FALSE;
    DWORD returned = 0;
    if (WSAIoctl(socket, kSioUdpConnectionReset, &disabled, sizeof(disabled),
                 nullptr, 0, &returned, nullptr, nullptr) == SOCKET_ERROR) {
        set_winsock_error(error, "UDP connection-reset configuration");
        return false;
    }
    return true;
}

class UniqueSocket {
public:
    UniqueSocket() noexcept = default;
    explicit UniqueSocket(SOCKET socket) noexcept : socket_(socket) {}
    ~UniqueSocket() { reset(); }
    UniqueSocket(const UniqueSocket&) = delete;
    UniqueSocket& operator=(const UniqueSocket&) = delete;
    UniqueSocket(UniqueSocket&& other) noexcept
        : socket_(std::exchange(other.socket_, INVALID_SOCKET)) {}
    UniqueSocket& operator=(UniqueSocket&& other) noexcept {
        if (this != &other) {
            reset();
            socket_ = std::exchange(other.socket_, INVALID_SOCKET);
        }
        return *this;
    }

    [[nodiscard]] SOCKET get() const noexcept { return socket_; }
    [[nodiscard]] bool valid() const noexcept {
        return socket_ != INVALID_SOCKET;
    }
    SOCKET release() noexcept {
        return std::exchange(socket_, INVALID_SOCKET);
    }
    void reset(SOCKET replacement = INVALID_SOCKET) noexcept {
        if (valid()) {
            closesocket(socket_);
        }
        socket_ = replacement;
    }

private:
    SOCKET socket_ = INVALID_SOCKET;
};

std::optional<sockaddr_in> ipv4_endpoint(std::string_view address,
                                         std::uint16_t port) {
    if (address.empty() || address.size() > 255 || port == 0) {
        return std::nullopt;
    }
    std::string terminated(address);
    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_port = htons(port);
    if (InetPtonA(AF_INET, terminated.c_str(), &endpoint.sin_addr) != 1) {
        return std::nullopt;
    }
    return endpoint;
}

void append_unique_target(std::vector<sockaddr_in>& targets,
                          std::unordered_set<std::uint32_t>& seen,
                          std::uint32_t network_order_address,
                          std::uint16_t port) {
    if (targets.size() >= kMaximumDiscoveryTargets) {
        return;
    }
    if (!seen.insert(network_order_address).second) {
        return;
    }
    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_port = htons(port);
    endpoint.sin_addr.s_addr = network_order_address;
    targets.push_back(endpoint);
}

std::vector<sockaddr_in> adapter_broadcast_targets(std::uint16_t port) {
    std::vector<sockaddr_in> targets;
    std::unordered_set<std::uint32_t> seen;
    ULONG buffer_bytes = 16 * 1024;
    std::vector<std::max_align_t> buffer;
    PIP_ADAPTER_ADDRESSES adapters = nullptr;
    ULONG result = ERROR_BUFFER_OVERFLOW;
    for (int attempt = 0; attempt < 3 && result == ERROR_BUFFER_OVERFLOW;
         ++attempt) {
        const auto elements =
            (static_cast<std::size_t>(buffer_bytes) +
             sizeof(std::max_align_t) - 1) /
            sizeof(std::max_align_t);
        buffer.resize(elements);
        adapters = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
        ULONG supplied_bytes = static_cast<ULONG>(
            buffer.size() * sizeof(std::max_align_t));
        result = GetAdaptersAddresses(
            AF_INET,
            GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                GAA_FLAG_SKIP_DNS_SERVER,
            nullptr, adapters, &supplied_bytes);
        buffer_bytes = supplied_bytes;
    }
    if (result == NO_ERROR) {
        for (auto* adapter = adapters; adapter != nullptr;
             adapter = adapter->Next) {
            if (adapter->OperStatus != IfOperStatusUp ||
                adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK ||
                adapter->IfType == IF_TYPE_TUNNEL) {
                continue;
            }
            for (auto* unicast = adapter->FirstUnicastAddress;
                 unicast != nullptr; unicast = unicast->Next) {
                if (unicast->Address.lpSockaddr == nullptr ||
                    unicast->Address.lpSockaddr->sa_family != AF_INET ||
                    unicast->OnLinkPrefixLength == 0 ||
                    unicast->OnLinkPrefixLength > 30) {
                    continue;
                }
                const auto* address = reinterpret_cast<const sockaddr_in*>(
                    unicast->Address.lpSockaddr);
                const auto host_address = ntohl(address->sin_addr.s_addr);
                const auto prefix = unicast->OnLinkPrefixLength;
                const auto mask = 0xffffffffu << (32u - prefix);
                const auto broadcast = htonl((host_address & mask) | ~mask);
                append_unique_target(targets, seen, broadcast, port);
            }
        }
    }
    append_unique_target(targets, seen, htonl(INADDR_BROADCAST), port);
    return targets;
}

std::vector<sockaddr_in> discovery_targets(
    const ClientDiscoveryOptions& options, std::string* error) {
    if (options.target_addresses.empty()) {
        return adapter_broadcast_targets(options.discovery_port);
    }
    if (options.target_addresses.size() > kMaximumDiscoveryTargets) {
        set_error(error, "too many explicit IPv4 discovery targets");
        return {};
    }
    std::vector<sockaddr_in> targets;
    std::unordered_set<std::uint32_t> seen;
    targets.reserve(options.target_addresses.size());
    for (const auto& address : options.target_addresses) {
        const auto endpoint = ipv4_endpoint(address, options.discovery_port);
        if (!endpoint) {
            set_error(error, "discovery target is not a valid IPv4 address");
            return {};
        }
        append_unique_target(targets, seen, endpoint->sin_addr.s_addr,
                             options.discovery_port);
    }
    return targets;
}

std::optional<std::string> address_text(const sockaddr_in& address) {
    std::array<char, INET_ADDRSTRLEN> buffer{};
    if (InetNtopA(AF_INET, &address.sin_addr, buffer.data(),
                  static_cast<DWORD>(buffer.size())) == nullptr) {
        return std::nullopt;
    }
    return std::string(buffer.data());
}

#endif

} // namespace

std::string sanitize_server_name(std::string_view name) {
    std::string sanitized;
    sanitized.reserve(std::min(name.size(), kMaximumServerNameBytes));
    for (const char character : name) {
        if (sanitized.size() == kMaximumServerNameBytes) {
            break;
        }
        const auto code = static_cast<unsigned char>(character);
        sanitized.push_back(code >= 0x20u && code < 0x7fu
                                ? character
                                : '?');
    }
    return sanitized;
}

std::optional<DiscoveryRequest> create_request(
    const security::ClientId& client_id, std::uint32_t key_id,
    std::uint64_t unix_time_seconds,
    std::span<const std::byte> pre_shared_key) noexcept {
    if (key_id == 0 || unix_time_seconds == 0 ||
        pre_shared_key.size() < security::kMinimumProtocolKeyBytes ||
        !any_nonzero(client_id)) {
        return std::nullopt;
    }
    DiscoveryRequest request;
    request.client_id = client_id;
    request.key_id = key_id;
    request.unix_time_seconds = unix_time_seconds;
    if (!security::generate_random(request.client_nonce)) {
        return std::nullopt;
    }
    const auto message = authentication_message(
        "NSTU-DISCOVERY-REQUEST-V1", PacketKind::request, request.client_id,
        request.key_id, request.client_nonce, request.unix_time_seconds, 0);
    auto authenticator = security::hmac_sha256(pre_shared_key, message);
    if (!authenticator) {
        return std::nullopt;
    }
    request.authenticator = *authenticator;
    security::secure_zero(*authenticator);
    return request;
}

std::vector<std::byte> encode_request(const DiscoveryRequest& request) {
    if (!valid_request_fields(request) || !any_nonzero(request.authenticator)) {
        return {};
    }
    Packet packet;
    packet.kind = PacketKind::request;
    packet.client_id = request.client_id;
    packet.key_id = request.key_id;
    packet.client_nonce = request.client_nonce;
    packet.unix_time_seconds = request.unix_time_seconds;
    packet.authenticator = request.authenticator;
    return encode_packet(packet);
}

std::optional<DiscoveryRequest> decode_request(
    std::span<const std::byte> wire) {
    const auto packet = decode_packet(wire);
    if (!packet || packet->kind != PacketKind::request ||
        packet->control_port != 0) {
        return std::nullopt;
    }
    DiscoveryRequest request;
    request.client_id = packet->client_id;
    request.key_id = packet->key_id;
    request.client_nonce = packet->client_nonce;
    request.unix_time_seconds = packet->unix_time_seconds;
    request.authenticator = packet->authenticator;
    return valid_request_fields(request) && any_nonzero(request.authenticator)
               ? std::optional{request}
               : std::nullopt;
}

bool verify_request(const DiscoveryRequest& request,
                    std::span<const std::byte> pre_shared_key,
                    std::uint64_t now_unix_seconds,
                    std::chrono::seconds maximum_clock_skew) noexcept {
    if (!valid_request_fields(request) ||
        pre_shared_key.size() < security::kMinimumProtocolKeyBytes ||
        !time_is_valid(request.unix_time_seconds, now_unix_seconds,
                       maximum_clock_skew)) {
        return false;
    }
    const auto message = authentication_message(
        "NSTU-DISCOVERY-REQUEST-V1", PacketKind::request, request.client_id,
        request.key_id, request.client_nonce, request.unix_time_seconds, 0);
    auto expected = security::hmac_sha256(pre_shared_key, message);
    const bool verified = expected && security::constant_time_equal(
                                          *expected, request.authenticator);
    if (expected) {
        security::secure_zero(*expected);
    }
    return verified;
}

std::optional<DiscoveryResponse> create_response(
    const DiscoveryRequest& request, std::uint16_t control_port,
    std::uint64_t unix_time_seconds,
    std::span<const std::byte> pre_shared_key) noexcept {
    if (!valid_request_fields(request) || control_port == 0 ||
        unix_time_seconds == 0 ||
        pre_shared_key.size() < security::kMinimumProtocolKeyBytes) {
        return std::nullopt;
    }
    DiscoveryResponse response;
    response.client_id = request.client_id;
    response.key_id = request.key_id;
    response.client_nonce = request.client_nonce;
    response.unix_time_seconds = unix_time_seconds;
    response.control_port = control_port;
    const auto message = authentication_message(
        "NSTU-DISCOVERY-RESPONSE-V1", PacketKind::response,
        response.client_id, response.key_id, response.client_nonce,
        response.unix_time_seconds, response.control_port);
    auto authenticator = security::hmac_sha256(pre_shared_key, message);
    if (!authenticator) {
        return std::nullopt;
    }
    response.authenticator = *authenticator;
    security::secure_zero(*authenticator);
    return response;
}

std::vector<std::byte> encode_response(const DiscoveryResponse& response) {
    if (!valid_response_fields(response) ||
        !any_nonzero(response.authenticator)) {
        return {};
    }
    Packet packet;
    packet.kind = PacketKind::response;
    packet.client_id = response.client_id;
    packet.key_id = response.key_id;
    packet.client_nonce = response.client_nonce;
    packet.unix_time_seconds = response.unix_time_seconds;
    packet.control_port = response.control_port;
    packet.authenticator = response.authenticator;
    return encode_packet(packet);
}

std::optional<DiscoveryResponse> decode_response(
    std::span<const std::byte> wire) {
    const auto packet = decode_packet(wire);
    if (!packet || packet->kind != PacketKind::response ||
        packet->control_port == 0) {
        return std::nullopt;
    }
    DiscoveryResponse response;
    response.client_id = packet->client_id;
    response.key_id = packet->key_id;
    response.client_nonce = packet->client_nonce;
    response.unix_time_seconds = packet->unix_time_seconds;
    response.control_port = packet->control_port;
    response.authenticator = packet->authenticator;
    return valid_response_fields(response) &&
                   any_nonzero(response.authenticator)
               ? std::optional{response}
               : std::nullopt;
}

bool verify_response(const DiscoveryResponse& response,
                     const DiscoveryRequest& request,
                     std::span<const std::byte> pre_shared_key,
                     std::uint64_t now_unix_seconds,
                     std::chrono::seconds maximum_clock_skew) noexcept {
    if (!valid_response_fields(response) ||
        response.client_id != request.client_id ||
        response.key_id != request.key_id ||
        response.client_nonce != request.client_nonce ||
        pre_shared_key.size() < security::kMinimumProtocolKeyBytes ||
        !time_is_valid(response.unix_time_seconds, now_unix_seconds,
                       maximum_clock_skew)) {
        return false;
    }
    const auto message = authentication_message(
        "NSTU-DISCOVERY-RESPONSE-V1", PacketKind::response,
        response.client_id, response.key_id, response.client_nonce,
        response.unix_time_seconds, response.control_port);
    auto expected = security::hmac_sha256(pre_shared_key, message);
    const bool verified = expected && security::constant_time_equal(
                                          *expected, response.authenticator);
    if (expected) {
        security::secure_zero(*expected);
    }
    return verified;
}

std::vector<ServerEndpoint> discover_authenticated_servers(
    const security::ClientId& client_id, std::uint32_t key_id,
    std::span<const std::byte> pre_shared_key,
    const ClientDiscoveryOptions& options, std::string* error) {
#if defined(_WIN32)
    net::WinsockRuntime winsock;
    if (!winsock.ready()) {
        set_error(error, "Winsock initialization failed");
        return {};
    }
    if (options.discovery_port == 0 || options.maximum_endpoints == 0 ||
        pre_shared_key.size() < security::kMinimumProtocolKeyBytes) {
        set_error(error, "invalid authenticated discovery configuration");
        return {};
    }
    const auto request = create_request(client_id, key_id, unix_seconds_now(),
                                        pre_shared_key);
    if (!request) {
        set_error(error, "authenticated discovery request generation failed");
        return {};
    }
    const auto wire = encode_request(*request);
    if (wire.size() != kDiscoveryPacketBytes) {
        set_error(error, "authenticated discovery request encoding failed");
        return {};
    }
    UniqueSocket socket(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    if (!socket.valid()) {
        set_winsock_error(error, "UDP discovery socket creation");
        return {};
    }
    if (!disable_udp_connection_reset(socket.get(), error)) {
        return {};
    }
    const BOOL broadcast = TRUE;
    if (setsockopt(socket.get(), SOL_SOCKET, SO_BROADCAST,
                   reinterpret_cast<const char*>(&broadcast),
                   sizeof(broadcast)) == SOCKET_ERROR) {
        set_winsock_error(error, "UDP discovery broadcast setup");
        return {};
    }
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = 0;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(socket.get(), reinterpret_cast<const sockaddr*>(&local),
             sizeof(local)) == SOCKET_ERROR) {
        set_winsock_error(error, "UDP discovery bind");
        return {};
    }
    auto targets = discovery_targets(options, error);
    if (targets.empty()) {
        if (error != nullptr && error->empty()) {
            set_error(error, "no IPv4 discovery target is available");
        }
        return {};
    }
    bool sent = false;
    for (const auto& target : targets) {
        const int result = sendto(
            socket.get(), reinterpret_cast<const char*>(wire.data()),
            static_cast<int>(wire.size()), 0,
            reinterpret_cast<const sockaddr*>(&target), sizeof(target));
        sent = sent || result == static_cast<int>(wire.size());
    }
    if (!sent) {
        set_winsock_error(error, "UDP discovery send");
        return {};
    }

    const auto timeout = std::clamp(options.timeout,
                                    std::chrono::milliseconds(100),
                                    std::chrono::milliseconds(5000));
    const auto endpoint_limit =
        std::clamp<std::size_t>(options.maximum_endpoints, 1, 32);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::vector<ServerEndpoint> endpoints;
    std::unordered_set<std::string> seen;
    std::array<std::byte, kDiscoveryPacketBytes> buffer{};
    while (std::chrono::steady_clock::now() < deadline &&
           endpoints.size() < endpoint_limit) {
        const auto remaining = std::chrono::duration_cast<
            std::chrono::milliseconds>(deadline -
                                      std::chrono::steady_clock::now());
        const auto remaining_count = std::max<std::int64_t>(
            remaining.count(), 1);
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(socket.get(), &readable);
        timeval wait{
            static_cast<long>(remaining_count / 1000),
            static_cast<long>((remaining_count % 1000) * 1000),
        };
        const int ready = select(0, &readable, nullptr, nullptr, &wait);
        if (ready == 0) {
            break;
        }
        if (ready == SOCKET_ERROR) {
            set_winsock_error(error, "UDP discovery wait");
            return {};
        }
        sockaddr_in source{};
        int source_bytes = sizeof(source);
        const int received = recvfrom(
            socket.get(), reinterpret_cast<char*>(buffer.data()),
            static_cast<int>(buffer.size()), 0,
            reinterpret_cast<sockaddr*>(&source), &source_bytes);
        if (received != static_cast<int>(buffer.size()) ||
            source.sin_family != AF_INET ||
            ntohs(source.sin_port) != options.discovery_port) {
            continue;
        }
        const auto response = decode_response(buffer);
        if (!response ||
            !verify_response(*response, *request, pre_shared_key,
                             unix_seconds_now())) {
            continue;
        }
        const auto address = address_text(source);
        if (!address) {
            continue;
        }
        const auto lookup = *address + ":" +
                            std::to_string(response->control_port);
        if (seen.insert(lookup).second) {
            endpoints.push_back({*address, response->control_port});
        }
    }
    if (endpoints.empty()) {
        set_error(error, "no authenticated NSTU discovery response received");
    } else if (error != nullptr) {
        error->clear();
    }
    return endpoints;
#else
    (void)client_id;
    (void)key_id;
    (void)pre_shared_key;
    (void)options;
    set_error(error, "authenticated discovery requires Windows");
    return {};
#endif
}

std::vector<PairingCandidate> discover_pairing_candidates(
    const ClientDiscoveryOptions& options, std::string* error) {
#if defined(_WIN32)
    net::WinsockRuntime winsock;
    if (!winsock.ready()) {
        set_error(error, "Winsock initialization failed");
        return {};
    }
    if (options.discovery_port == 0 || options.maximum_endpoints == 0) {
        set_error(error, "invalid pairing discovery configuration");
        return {};
    }
    PairingProbe probe;
    probe.unix_time_seconds = unix_seconds_now();
    if (!security::generate_random(probe.client_nonce)) {
        set_error(error, "pairing probe nonce generation failed");
        return {};
    }
    const auto wire = encode_pairing_probe(probe);
    if (wire.size() != kPairingProbeBytes) {
        set_error(error, "pairing probe encoding failed");
        return {};
    }
    UniqueSocket socket(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    if (!socket.valid()) {
        set_winsock_error(error, "UDP pairing socket creation");
        return {};
    }
    if (!disable_udp_connection_reset(socket.get(), error)) {
        return {};
    }
    const BOOL broadcast = TRUE;
    if (setsockopt(socket.get(), SOL_SOCKET, SO_BROADCAST,
                   reinterpret_cast<const char*>(&broadcast),
                   sizeof(broadcast)) == SOCKET_ERROR) {
        set_winsock_error(error, "UDP pairing broadcast setup");
        return {};
    }
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = 0;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(socket.get(), reinterpret_cast<const sockaddr*>(&local),
             sizeof(local)) == SOCKET_ERROR) {
        set_winsock_error(error, "UDP pairing bind");
        return {};
    }
    auto targets = discovery_targets(options, error);
    if (targets.empty()) {
        if (error != nullptr && error->empty()) {
            set_error(error, "no IPv4 discovery target is available");
        }
        return {};
    }
    bool sent = false;
    for (const auto& target : targets) {
        const int result = sendto(
            socket.get(), reinterpret_cast<const char*>(wire.data()),
            static_cast<int>(wire.size()), 0,
            reinterpret_cast<const sockaddr*>(&target), sizeof(target));
        sent = sent || result == static_cast<int>(wire.size());
    }
    if (!sent) {
        set_winsock_error(error, "UDP pairing probe send");
        return {};
    }

    const auto timeout = std::clamp(options.timeout,
                                    std::chrono::milliseconds(100),
                                    std::chrono::milliseconds(5000));
    const auto candidate_limit =
        std::clamp<std::size_t>(options.maximum_endpoints, 1, 32);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::vector<PairingCandidate> candidates;
    std::unordered_set<std::string> seen;
    std::array<std::byte, kMaximumDiscoveryDatagramBytes> buffer{};
    while (std::chrono::steady_clock::now() < deadline &&
           candidates.size() < candidate_limit) {
        const auto remaining = std::chrono::duration_cast<
            std::chrono::milliseconds>(deadline -
                                      std::chrono::steady_clock::now());
        const auto remaining_count = std::max<std::int64_t>(
            remaining.count(), 1);
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(socket.get(), &readable);
        timeval wait{
            static_cast<long>(remaining_count / 1000),
            static_cast<long>((remaining_count % 1000) * 1000),
        };
        const int ready = select(0, &readable, nullptr, nullptr, &wait);
        if (ready == 0) {
            break;
        }
        if (ready == SOCKET_ERROR) {
            set_winsock_error(error, "UDP pairing wait");
            return {};
        }
        sockaddr_in source{};
        int source_bytes = sizeof(source);
        const int received = recvfrom(
            socket.get(), reinterpret_cast<char*>(buffer.data()),
            static_cast<int>(buffer.size()), 0,
            reinterpret_cast<sockaddr*>(&source), &source_bytes);
        if (received <= 0 || source.sin_family != AF_INET ||
            ntohs(source.sin_port) != options.discovery_port) {
            continue;
        }
        const std::span<const std::byte> datagram{
            buffer.data(), static_cast<std::size_t>(received)};
        const auto beacon = decode_pairing_beacon(datagram);
        // The nonce echo only pairs the reply with this sweep; it proves
        // nothing about who sent it, which is why the operator still has to
        // compare the code afterwards.
        if (!beacon || beacon->client_nonce != probe.client_nonce) {
            continue;
        }
        const auto address = address_text(source);
        if (!address) {
            continue;
        }
        const auto lookup = *address + ":" +
                            std::to_string(beacon->control_port);
        if (seen.insert(lookup).second) {
            candidates.push_back(
                {*address, beacon->control_port, beacon->server_name});
        }
    }
    if (candidates.empty()) {
        set_error(error, "no NSTU server answered the pairing probe");
    } else if (error != nullptr) {
        error->clear();
    }
    return candidates;
#else
    (void)options;
    set_error(error, "pairing discovery requires Windows");
    return {};
#endif
}

class AuthenticatedDiscoveryResponder::Impl {
public:
    Impl()
        : rate_limiter_(security::HandshakeRateLimitPolicy{
              .maximum_sources = 2048,
              .maximum_failures = 8,
              .window = std::chrono::seconds(30),
              .block = std::chrono::seconds(60)}) {}

    ~Impl() { stop(); }

    bool start(std::uint16_t discovery_port, std::uint16_t control_port,
               const security::KeyStore& key_store, std::string* error) {
#if defined(_WIN32)
        if (running_.load()) {
            set_error(error, "authenticated discovery is already running");
            return false;
        }
        if (control_port == 0 || !winsock_.ready()) {
            set_error(error, "invalid authenticated discovery listener");
            return false;
        }
        UniqueSocket socket(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
        if (!socket.valid()) {
            set_winsock_error(error, "UDP discovery responder creation");
            return false;
        }
        if (!disable_udp_connection_reset(socket.get(), error)) {
            return false;
        }
        const BOOL exclusive = TRUE;
        if (setsockopt(socket.get(), SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                       reinterpret_cast<const char*>(&exclusive),
                       sizeof(exclusive)) == SOCKET_ERROR) {
            set_winsock_error(error, "UDP discovery exclusive bind setup");
            return false;
        }
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_port = htons(discovery_port);
        local.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(socket.get(), reinterpret_cast<const sockaddr*>(&local),
                 sizeof(local)) == SOCKET_ERROR) {
            set_winsock_error(error, "UDP discovery responder bind");
            return false;
        }
        int local_bytes = sizeof(local);
        if (getsockname(socket.get(), reinterpret_cast<sockaddr*>(&local),
                        &local_bytes) == SOCKET_ERROR) {
            set_winsock_error(error, "UDP discovery responder getsockname");
            return false;
        }
        key_store_ = &key_store;
        control_port_ = control_port;
        local_port_ = ntohs(local.sin_port);
        socket_ = socket.release();
        replay_protector_.clear();
        running_ = true;
        try {
            worker_ = std::jthread(
                [this](std::stop_token stop_token) { run(stop_token); });
        } catch (const std::system_error&) {
            closesocket(socket_);
            socket_ = INVALID_SOCKET;
            running_ = false;
            local_port_ = 0;
            control_port_ = 0;
            key_store_ = nullptr;
            set_error(error, "UDP discovery responder thread creation failed");
            return false;
        }
        return true;
#else
        (void)discovery_port;
        (void)control_port;
        (void)key_store;
        set_error(error, "authenticated discovery requires Windows");
        return false;
#endif
    }

    void stop() noexcept {
#if defined(_WIN32)
        if (worker_.joinable()) {
            worker_.request_stop();
            worker_.join();
        }
        if (socket_ != INVALID_SOCKET) {
            closesocket(socket_);
            socket_ = INVALID_SOCKET;
        }
#endif
        running_ = false;
        local_port_ = 0;
        control_port_ = 0;
        key_store_ = nullptr;
        replay_protector_.clear();
    }

    [[nodiscard]] bool running() const noexcept { return running_.load(); }
    [[nodiscard]] std::uint16_t local_port() const noexcept {
        return local_port_.load();
    }

    void set_pairing_beacon(bool enabled, std::string_view server_name) {
        auto sanitized = sanitize_server_name(server_name);
        std::scoped_lock lock(beacon_mutex_);
        beacon_name_ = std::move(sanitized);
        beacon_enabled_ = enabled && !beacon_name_.empty();
    }

    [[nodiscard]] bool pairing_beacon_enabled() const noexcept {
        std::scoped_lock lock(beacon_mutex_);
        return beacon_enabled_;
    }

private:
#if defined(_WIN32)
    // Unauthenticated by design: the peer has no key yet. The reply discloses
    // only the display name and control port, and only while the operator has
    // the enrollment window open.
    void answer_pairing_probe(std::span<const std::byte> datagram,
                              const sockaddr_in& source, int source_bytes,
                              const std::string& source_text) noexcept {
        const auto probe = decode_pairing_probe(datagram);
        if (!probe) {
            rate_limiter_.record_failure(source_text);
            return;
        }
        const auto now = unix_seconds_now();
        if (!time_is_valid(probe->unix_time_seconds, now,
                           std::chrono::seconds(120))) {
            rate_limiter_.record_failure(source_text);
            return;
        }
        PairingBeacon beacon;
        beacon.client_nonce = probe->client_nonce;
        beacon.unix_time_seconds = now;
        beacon.control_port = control_port_;
        {
            std::scoped_lock lock(beacon_mutex_);
            if (!beacon_enabled_) {
                // A well-formed probe against a closed window is not an
                // attack; stay silent rather than penalising the source.
                return;
            }
            beacon.server_name = beacon_name_;
        }
        const auto wire = encode_pairing_beacon(beacon);
        if (wire.empty()) {
            return;
        }
        const int sent = sendto(
            socket_, reinterpret_cast<const char*>(wire.data()),
            static_cast<int>(wire.size()), 0,
            reinterpret_cast<const sockaddr*>(&source), source_bytes);
        if (sent == static_cast<int>(wire.size())) {
            rate_limiter_.record_success(source_text);
        }
    }

    void run(std::stop_token stop_token) noexcept {
        std::array<std::byte, kMaximumDiscoveryDatagramBytes> buffer{};
        while (!stop_token.stop_requested()) {
            fd_set readable;
            FD_ZERO(&readable);
            FD_SET(socket_, &readable);
            timeval wait{0, 200000};
            const int ready = select(0, &readable, nullptr, nullptr, &wait);
            if (ready == 0) {
                continue;
            }
            if (ready == SOCKET_ERROR) {
                break;
            }
            sockaddr_in source{};
            int source_bytes = sizeof(source);
            const int received = recvfrom(
                socket_, reinterpret_cast<char*>(buffer.data()),
                static_cast<int>(buffer.size()), 0,
                reinterpret_cast<sockaddr*>(&source), &source_bytes);
            if (source.sin_family != AF_INET) {
                continue;
            }
            const auto source_text = address_text(source);
            if (!source_text || !rate_limiter_.allow(*source_text)) {
                continue;
            }
            if (received <= 0) {
                rate_limiter_.record_failure(*source_text);
                continue;
            }
            const std::span<const std::byte> datagram{
                buffer.data(), static_cast<std::size_t>(received)};
            if (is_pairing_datagram(datagram)) {
                answer_pairing_probe(datagram, source, source_bytes,
                                     *source_text);
                continue;
            }
            if (received != static_cast<int>(kDiscoveryPacketBytes)) {
                rate_limiter_.record_failure(*source_text);
                continue;
            }
            const auto request = decode_request(datagram);
            if (!request || key_store_ == nullptr) {
                rate_limiter_.record_failure(*source_text);
                continue;
            }
            auto key = key_store_->resolve(request->client_id,
                                           request->key_id);
            const auto now = unix_seconds_now();
            if (!key || !verify_request(*request, *key, now)) {
                if (key) {
                    security::secure_zero(*key);
                }
                rate_limiter_.record_failure(*source_text);
                continue;
            }
            security::AuthHello replay_identity;
            replay_identity.client_id = request->client_id;
            replay_identity.client_nonce = request->client_nonce;
            replay_identity.unix_time_seconds = request->unix_time_seconds;
            replay_identity.key_id = request->key_id;
            if (!replay_protector_.accept(replay_identity, now)) {
                security::secure_zero(*key);
                // Directed and limited broadcasts can deliver the same valid
                // datagram twice. Drop replays without teaching an attacker
                // how to block a legitimate source through captured packets.
                continue;
            }
            const auto response = create_response(*request, control_port_, now,
                                                  *key);
            security::secure_zero(*key);
            if (!response) {
                rate_limiter_.record_failure(*source_text);
                continue;
            }
            const auto wire = encode_response(*response);
            if (wire.size() != kDiscoveryPacketBytes) {
                rate_limiter_.record_failure(*source_text);
                continue;
            }
            const int sent = sendto(
                socket_, reinterpret_cast<const char*>(wire.data()),
                static_cast<int>(wire.size()), 0,
                reinterpret_cast<const sockaddr*>(&source), source_bytes);
            if (sent == static_cast<int>(wire.size())) {
                rate_limiter_.record_success(*source_text);
            }
        }
        running_ = false;
    }
#endif

    net::WinsockRuntime winsock_;
    const security::KeyStore* key_store_ = nullptr;
    security::HandshakeRateLimiter rate_limiter_;
    security::ReplayProtector replay_protector_;
    mutable std::mutex beacon_mutex_;
    std::string beacon_name_;
    bool beacon_enabled_ = false;
    std::jthread worker_;
    std::atomic_bool running_{false};
    std::atomic<std::uint16_t> local_port_{0};
    std::uint16_t control_port_ = 0;
#if defined(_WIN32)
    SOCKET socket_ = INVALID_SOCKET;
#endif
};

AuthenticatedDiscoveryResponder::AuthenticatedDiscoveryResponder()
    : impl_(std::make_unique<Impl>()) {}

AuthenticatedDiscoveryResponder::~AuthenticatedDiscoveryResponder() = default;

bool AuthenticatedDiscoveryResponder::start(
    std::uint16_t discovery_port, std::uint16_t control_port,
    const security::KeyStore& key_store, std::string* error) {
    return impl_->start(discovery_port, control_port, key_store, error);
}

void AuthenticatedDiscoveryResponder::stop() noexcept { impl_->stop(); }

void AuthenticatedDiscoveryResponder::set_pairing_beacon(
    bool enabled, std::string_view server_name) {
    impl_->set_pairing_beacon(enabled, server_name);
}

bool AuthenticatedDiscoveryResponder::pairing_beacon_enabled() const noexcept {
    return impl_->pairing_beacon_enabled();
}

bool AuthenticatedDiscoveryResponder::running() const noexcept {
    return impl_->running();
}

std::uint16_t AuthenticatedDiscoveryResponder::local_port() const noexcept {
    return impl_->local_port();
}

} // namespace nstu::discovery
