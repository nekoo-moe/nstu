#include "nstu/exam_profile.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <string_view>

namespace nstu::client {
namespace {

bool all_zero(std::span<const std::byte> bytes) noexcept {
    return std::all_of(bytes.begin(), bytes.end(),
                       [](const std::byte value) {
                           return value == std::byte{0};
                       });
}

void append_hex(std::string& output, std::span<const std::byte> bytes) {
    constexpr std::array<char, 16> digits{
        '0', '1', '2', '3', '4', '5', '6', '7',
        '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    output.reserve(output.size() + bytes.size() * 2);
    for (const auto value : bytes) {
        const auto byte = std::to_integer<unsigned int>(value);
        output.push_back(digits[(byte >> 4u) & 0x0fu]);
        output.push_back(digits[byte & 0x0fu]);
    }
}

} // namespace

std::optional<std::filesystem::path> derive_exam_profile_path(
    const std::filesystem::path& profile_root,
    const security::Sha256Digest& package_digest,
    const security::ClientId& client_id,
    const exam::SessionId& session_id) {
    if (profile_root.empty() || !profile_root.is_absolute() ||
        all_zero(package_digest) || all_zero(client_id) ||
        all_zero(session_id)) {
        return std::nullopt;
    }

    std::string leaf = "v2-";
    append_hex(leaf, package_digest);
    leaf.push_back('-');
    append_hex(leaf, client_id);
    leaf.push_back('-');
    append_hex(leaf, session_id);
    return profile_root / std::filesystem::path(leaf);
}

} // namespace nstu::client
