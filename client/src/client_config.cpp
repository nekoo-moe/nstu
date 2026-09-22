#include "nstu/client_config.hpp"

#include "nstu/secret_store.hpp"

#include <algorithm>
#include <limits>
#include <type_traits>

namespace nstu::client {
namespace {

inline constexpr std::uint32_t kConfigMagic = 0x31474643u; // "CFG1"
// Version 2 appends a length-prefixed preferred-room string. Version 1 blobs
// (no room) still load, so an install that paired before this change keeps
// working and simply carries no room preference.
inline constexpr std::uint16_t kConfigVersion = 2;
inline constexpr std::uint16_t kConfigVersionLegacy = 1;
inline constexpr std::size_t kMaximumAddressBytes = 255;
// Mirrors discovery::kMaximumServerNameBytes (the bound the pairing beacon
// enforces on the label this room string is matched against). Kept as a local
// wire limit so the config codec does not depend on the discovery header; the
// room is clamped to it on save and a larger value in a blob is treated as
// corruption on load.
inline constexpr std::size_t kMaximumRoomBytes = 64;

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

bool valid(const ClientRuntimeConfig& config) {
    return !config.server_address.empty() &&
           config.server_address.size() <= kMaximumAddressBytes &&
           config.server_port != 0 && config.key_id != 0 &&
           config.pre_shared_key.size() >=
               security::kMinimumProtocolKeyBytes &&
           std::any_of(config.client_id.begin(), config.client_id.end(),
                       [](std::byte value) { return value != std::byte{0}; });
}

} // namespace

bool save_client_runtime_config(
    const ClientRuntimeConfig& config, std::wstring_view path,
    std::span<const std::byte> optional_entropy, std::string* error) {
    if (!valid(config) ||
        config.pre_shared_key.size() >
            std::numeric_limits<std::uint16_t>::max()) {
        set_error(error, "invalid client runtime configuration");
        return false;
    }
    // The room is a non-secret hint; clamp (not reject) an over-long value to
    // the wire bound so a stray long string never fails an otherwise valid
    // save. Empty is fine and common (no room preference).
    const std::string_view room(
        config.preferred_room.data(),
        std::min(config.preferred_room.size(), kMaximumRoomBytes));
    std::vector<std::byte> wire;
    wire.reserve(34 + config.server_address.size() +
                 config.pre_shared_key.size() + room.size());
    append_le(wire, kConfigMagic);
    append_le(wire, kConfigVersion);
    append_le(wire, config.server_port);
    append_le(wire, config.key_id);
    wire.insert(wire.end(), config.client_id.begin(), config.client_id.end());
    append_le(wire, static_cast<std::uint16_t>(config.server_address.size()));
    append_le(wire, static_cast<std::uint16_t>(config.pre_shared_key.size()));
    append_le(wire, static_cast<std::uint16_t>(room.size()));
    wire.insert(wire.end(),
                reinterpret_cast<const std::byte*>(config.server_address.data()),
                reinterpret_cast<const std::byte*>(config.server_address.data()) +
                    config.server_address.size());
    wire.insert(wire.end(), config.pre_shared_key.begin(),
                config.pre_shared_key.end());
    wire.insert(wire.end(), reinterpret_cast<const std::byte*>(room.data()),
                reinterpret_cast<const std::byte*>(room.data()) + room.size());
    const bool succeeded = security::save_machine_secret(
        path, wire, optional_entropy, error);
    security::secure_zero(wire);
    return succeeded;
}

bool load_client_runtime_config(
    ClientRuntimeConfig& config, std::wstring_view path,
    std::span<const std::byte> optional_entropy, std::string* error) {
    auto wire = security::load_machine_secret(path, optional_entropy, error);
    if (wire.empty()) {
        return false;
    }
    std::size_t offset = 0;
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    std::uint16_t address_bytes = 0;
    std::uint16_t key_bytes = 0;
    std::uint16_t room_bytes = 0;
    ClientRuntimeConfig loaded;
    const bool header_valid =
        read_le(wire, offset, magic) && magic == kConfigMagic &&
        read_le(wire, offset, version) &&
        (version == kConfigVersion || version == kConfigVersionLegacy) &&
        read_le(wire, offset, loaded.server_port) &&
        read_le(wire, offset, loaded.key_id) &&
        wire.size() - offset >= loaded.client_id.size();
    if (!header_valid) {
        security::secure_zero(wire);
        set_error(error, "invalid client configuration header");
        return false;
    }
    std::copy_n(wire.begin() + static_cast<std::ptrdiff_t>(offset),
                loaded.client_id.size(), loaded.client_id.begin());
    offset += loaded.client_id.size();
    // Version 2 carries an extra room-length field; version 1 has none and
    // leaves the preferred room empty. The pre-shared key is read by its
    // explicit length (not "to the end") so the trailing room bytes in a
    // version-2 blob are never folded into the key.
    const bool has_room = version == kConfigVersion;
    if (!read_le(wire, offset, address_bytes) ||
        !read_le(wire, offset, key_bytes) ||
        (has_room && !read_le(wire, offset, room_bytes)) ||
        address_bytes == 0 || address_bytes > kMaximumAddressBytes ||
        key_bytes < security::kMinimumProtocolKeyBytes ||
        room_bytes > kMaximumRoomBytes ||
        wire.size() - offset != static_cast<std::size_t>(address_bytes) +
                                    key_bytes + room_bytes) {
        security::secure_zero(wire);
        set_error(error, "invalid client configuration body");
        return false;
    }
    loaded.server_address.assign(
        reinterpret_cast<const char*>(wire.data() + offset), address_bytes);
    offset += address_bytes;
    loaded.pre_shared_key.assign(
        wire.begin() + static_cast<std::ptrdiff_t>(offset),
        wire.begin() + static_cast<std::ptrdiff_t>(offset + key_bytes));
    offset += key_bytes;
    if (room_bytes != 0) {
        loaded.preferred_room.assign(
            reinterpret_cast<const char*>(wire.data() + offset), room_bytes);
    }
    security::secure_zero(wire);
    if (!valid(loaded)) {
        clear_client_runtime_config(loaded);
        set_error(error, "invalid client runtime configuration");
        return false;
    }
    clear_client_runtime_config(config);
    config = std::move(loaded);
    return true;
}

void clear_client_runtime_config(ClientRuntimeConfig& config) noexcept {
    security::secure_zero(config.pre_shared_key);
    config = {};
}

} // namespace nstu::client
