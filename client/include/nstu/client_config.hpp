#pragma once

#include "nstu/auth.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nstu::client {

struct ClientRuntimeConfig {
    // Last mutually authenticated endpoint; it is a reconnect cache, not a
    // server identity credential.
    std::string server_address;
    std::uint16_t server_port = 47001;
    security::ClientId client_id{};
    std::uint32_t key_id = 0;
    std::vector<std::byte> pre_shared_key;
    // Optional classroom label this machine prefers when several servers answer
    // the pre-enrollment sweep on a shared VLAN. A routing hint, never a
    // credential: the six-digit SAS still gates every pairing. Empty means "no
    // preference" (today's sole-candidate/menu behavior). Seeded from the
    // installer's registry value on first pair, then carried in this blob.
    std::string preferred_room;
};

[[nodiscard]] bool save_client_runtime_config(
    const ClientRuntimeConfig& config, std::wstring_view path,
    std::span<const std::byte> optional_entropy = {},
    std::string* error = nullptr);

[[nodiscard]] bool load_client_runtime_config(
    ClientRuntimeConfig& config, std::wstring_view path,
    std::span<const std::byte> optional_entropy = {},
    std::string* error = nullptr);

void clear_client_runtime_config(ClientRuntimeConfig& config) noexcept;

} // namespace nstu::client
