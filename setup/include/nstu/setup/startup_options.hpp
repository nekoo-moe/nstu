#pragma once

#include "nstu/setup/diagnostics.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace nstu::setup {

// Keep endpoint parsing independent of the registry so command-line overrides
// and missing/malformed saved configuration can be verified without HKLM writes.
[[nodiscard]] inline std::optional<std::uint16_t> parse_diagnostic_server_port(
    std::wstring_view text) noexcept {
    if (text.empty()) return std::nullopt;
    std::uint32_t value = 0;
    for (const auto character : text) {
        if (character < L'0' || character > L'9') return std::nullopt;
        value = value * 10 + static_cast<std::uint32_t>(character - L'0');
        if (value > 65535) return std::nullopt;
    }
    if (value == 0) return std::nullopt;
    return static_cast<std::uint16_t>(value);
}

[[nodiscard]] inline bool valid_diagnostic_server_address(
    std::wstring_view address) noexcept {
    if (address.empty() || address.size() > 253) return false;
    // GetAddrInfoW accepts host names and raw IPv4/IPv6 addresses, including
    // scoped IPv6. Reject URLs, paths, quotes and embedded whitespace/control
    // characters before passing technician-provided data to the resolver.
    for (const auto character : address) {
        if (!((character >= L'a' && character <= L'z') ||
              (character >= L'A' && character <= L'Z') ||
              (character >= L'0' && character <= L'9') ||
              character == L'.' || character == L'-' || character == L'_' ||
              character == L':' || character == L'%')) {
            return false;
        }
    }
    return true;
}

struct SavedDiagnosticEndpoint {
    std::optional<std::wstring> address;
    std::optional<std::uint32_t> port;
};

// The reader is invoked only for a client boot check, and only for values that
// were not explicitly provided. Invalid explicit values must never silently
// select a different, saved endpoint.
template <typename Reader>
[[nodiscard]] bool load_diagnostic_boot_endpoint(
    DiagnosticOptions& options, bool explicit_address, bool explicit_port,
    Reader&& read) {
    if (options.role != DiagnosticRole::client || !options.boot_check ||
        (explicit_address && explicit_port)) {
        return true;
    }
    const auto saved = read(!explicit_address, !explicit_port);
    bool valid = true;
    if (!explicit_address) {
        if (saved.address && valid_diagnostic_server_address(*saved.address)) {
            options.server_address = *saved.address;
        } else {
            options.server_address.clear();
            valid = false;
        }
    }
    if (!explicit_port) {
        if (saved.port && *saved.port != 0 && *saved.port <= 65535) {
            options.server_port = static_cast<std::uint16_t>(*saved.port);
        } else {
            // Zero is never a usable fallback: diagnostics must expose the
            // damaged enrollment instead of probing the default server port.
            options.server_port = 0;
            valid = false;
        }
    }
    return valid;
}

} // namespace nstu::setup
