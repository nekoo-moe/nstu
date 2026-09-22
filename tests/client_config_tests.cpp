// The persisted client runtime configuration codec, round-tripped and pinned at
// its two awkward edges: forward/backward compatibility and corruption. The
// version-2 wire format appends a length-prefixed preferred-room string; a
// version-1 blob (an install that paired before the room field existed) carries
// no such field and must still load, simply with no room. The room is a
// non-secret routing hint, so an over-long value is clamped on save rather than
// rejected; but a stored blob whose declared room length exceeds the wire bound,
// or whose lengths do not sum to the payload, is treated as tampering and
// refused. The pre-shared key is read by its explicit length, never "to the
// end", so the trailing room bytes are never folded into the key.
#include "nstu/client_config.hpp"
#include "nstu/secret_store.hpp"

#include <windows.h>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace {

// Mirrors the private wire constants in client_config.cpp. The test builds
// legacy and malformed blobs by hand, so it must agree with the codec byte for
// byte; if the on-disk format changes, these change with it.
constexpr std::uint32_t kConfigMagic = 0x31474643u; // "CFG1"
constexpr std::uint16_t kConfigVersion = 2;
constexpr std::uint16_t kConfigVersionLegacy = 1;
constexpr std::size_t kMaximumRoomBytes = 64;

std::wstring scratch_path(const wchar_t* tag) {
    wchar_t directory[MAX_PATH]{};
    assert(GetTempPathW(MAX_PATH, directory) != 0);
    return (std::filesystem::path(directory) /
            (std::wstring(L"nstu-client-config-") + tag + L"-" +
             std::to_wstring(GetCurrentProcessId()) + L".bin"))
        .wstring();
}

template <typename T>
void append_le(std::vector<std::byte>& out, T value) {
    static_assert(std::is_unsigned_v<T>);
    for (std::size_t index = 0; index < sizeof(T); ++index) {
        out.push_back(static_cast<std::byte>(value & 0xffu));
        value >>= 8u;
    }
}

nstu::client::ClientRuntimeConfig sample_config() {
    nstu::client::ClientRuntimeConfig config;
    config.server_address = "192.168.10.5";
    config.server_port = 47001;
    config.key_id = 7;
    config.client_id.fill(std::byte{0x11});
    config.pre_shared_key.assign(nstu::security::kMinimumProtocolKeyBytes,
                                 std::byte{0x2a});
    return config;
}

// Writes a DPAPI-wrapped blob whose plaintext is exactly `wire`, in the place
// and envelope the codec's loader reads from. This is how the test injects
// hand-built legacy and corrupt payloads without a matching writer in the codec.
void write_raw_blob(const std::wstring& path,
                    const std::vector<std::byte>& wire) {
    std::string error;
    assert(nstu::security::save_machine_secret(path, wire, {}, &error));
}

// The header up to and including client_id is identical across versions; only
// the length block and payloads differ. Sharing it keeps the hand-built blobs
// honest about where they diverge from the real codec.
void append_common_header(std::vector<std::byte>& wire, std::uint16_t version,
                          const nstu::client::ClientRuntimeConfig& config) {
    append_le(wire, kConfigMagic);
    append_le(wire, version);
    append_le(wire, config.server_port);
    append_le(wire, config.key_id);
    wire.insert(wire.end(), config.client_id.begin(), config.client_id.end());
}

void append_bytes(std::vector<std::byte>& wire, std::string_view text) {
    wire.insert(wire.end(), reinterpret_cast<const std::byte*>(text.data()),
                reinterpret_cast<const std::byte*>(text.data()) + text.size());
}

} // namespace

int main() {
    // Version-2 round-trip: a config carrying a room name comes back identical,
    // room included.
    {
        const auto path = scratch_path(L"v2");
        auto config = sample_config();
        config.preferred_room = "Room 7";
        std::string error;
        assert(nstu::client::save_client_runtime_config(config, path, {},
                                                        &error));
        nstu::client::ClientRuntimeConfig loaded;
        assert(nstu::client::load_client_runtime_config(loaded, path, {},
                                                        &error));
        assert(loaded.server_address == config.server_address);
        assert(loaded.server_port == config.server_port);
        assert(loaded.key_id == config.key_id);
        assert(loaded.client_id == config.client_id);
        assert(loaded.pre_shared_key == config.pre_shared_key);
        assert(loaded.preferred_room == "Room 7");
        DeleteFileW(path.c_str());
    }

    // A version-2 config with no room round-trips to an empty room, the same
    // observable result a version-1 blob produces.
    {
        const auto path = scratch_path(L"v2-noroom");
        const auto config = sample_config();
        std::string error;
        assert(nstu::client::save_client_runtime_config(config, path, {},
                                                        &error));
        nstu::client::ClientRuntimeConfig loaded;
        assert(nstu::client::load_client_runtime_config(loaded, path, {},
                                                        &error));
        assert(loaded.preferred_room.empty());
        DeleteFileW(path.c_str());
    }

    // Version-1 back-compat: a hand-built legacy blob with no room-length field
    // at all still loads, with an empty preferred room and no error reported.
    {
        const auto path = scratch_path(L"v1");
        const auto config = sample_config();
        std::vector<std::byte> wire;
        append_common_header(wire, kConfigVersionLegacy, config);
        append_le(wire,
                  static_cast<std::uint16_t>(config.server_address.size()));
        append_le(wire,
                  static_cast<std::uint16_t>(config.pre_shared_key.size()));
        // No room-length field: its absence is precisely what marks this a
        // version-1 blob to the loader.
        append_bytes(wire, config.server_address);
        wire.insert(wire.end(), config.pre_shared_key.begin(),
                    config.pre_shared_key.end());
        write_raw_blob(path, wire);

        nstu::client::ClientRuntimeConfig loaded;
        std::string error;
        assert(nstu::client::load_client_runtime_config(loaded, path, {},
                                                        &error));
        assert(error.empty());
        assert(loaded.preferred_room.empty());
        assert(loaded.server_address == config.server_address);
        assert(loaded.key_id == config.key_id);
        assert(loaded.pre_shared_key == config.pre_shared_key);
        DeleteFileW(path.c_str());
    }

    // Clamp on save: a room longer than the wire bound is truncated to the
    // bound (never rejected), and loads back at exactly the bound.
    {
        const auto path = scratch_path(L"clamp");
        auto config = sample_config();
        config.preferred_room.assign(kMaximumRoomBytes + 40, 'x');
        std::string error;
        assert(nstu::client::save_client_runtime_config(config, path, {},
                                                        &error));
        nstu::client::ClientRuntimeConfig loaded;
        assert(nstu::client::load_client_runtime_config(loaded, path, {},
                                                        &error));
        assert(loaded.preferred_room.size() == kMaximumRoomBytes);
        DeleteFileW(path.c_str());
    }

    // Corruption: a blob that declares a room longer than the wire bound is
    // refused, not clamped. Clamping is a save-time courtesy for a live config;
    // on load an oversized length is a sign of truncation or tampering.
    {
        const auto path = scratch_path(L"badroom");
        const auto config = sample_config();
        const auto oversized_room =
            static_cast<std::uint16_t>(kMaximumRoomBytes + 1);
        std::vector<std::byte> wire;
        append_common_header(wire, kConfigVersion, config);
        append_le(wire,
                  static_cast<std::uint16_t>(config.server_address.size()));
        append_le(wire,
                  static_cast<std::uint16_t>(config.pre_shared_key.size()));
        append_le(wire, oversized_room);
        append_bytes(wire, config.server_address);
        wire.insert(wire.end(), config.pre_shared_key.begin(),
                    config.pre_shared_key.end());
        // Match the declared length so only the bound, not the total size, is
        // what trips the rejection.
        wire.insert(wire.end(), static_cast<std::size_t>(oversized_room),
                    std::byte{'x'});
        write_raw_blob(path, wire);

        nstu::client::ClientRuntimeConfig loaded;
        std::string error;
        assert(!nstu::client::load_client_runtime_config(loaded, path, {},
                                                         &error));
        assert(!error.empty());
        DeleteFileW(path.c_str());
    }

    // Corruption: lengths that do not sum to the payload are refused. Here the
    // declared address length is one byte longer than the body actually holds.
    {
        const auto path = scratch_path(L"badlen");
        const auto config = sample_config();
        std::vector<std::byte> wire;
        append_common_header(wire, kConfigVersion, config);
        append_le(wire, static_cast<std::uint16_t>(
                            config.server_address.size() + 1));
        append_le(wire,
                  static_cast<std::uint16_t>(config.pre_shared_key.size()));
        append_le(wire, static_cast<std::uint16_t>(0));
        append_bytes(wire, config.server_address);
        wire.insert(wire.end(), config.pre_shared_key.begin(),
                    config.pre_shared_key.end());
        write_raw_blob(path, wire);

        nstu::client::ClientRuntimeConfig loaded;
        std::string error;
        assert(!nstu::client::load_client_runtime_config(loaded, path, {},
                                                         &error));
        assert(!error.empty());
        DeleteFileW(path.c_str());
    }

    return 0;
}
