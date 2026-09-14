#pragma once

#include "nstu/auth.hpp"
#include "nstu/exam_sync.hpp"

#include <filesystem>
#include <optional>
#include <string>

namespace nstu::client {

// Build a stable, filesystem-safe WebView2 profile path for one authenticated
// exam context.  The version prefix makes a future profile-layout migration
// unambiguous; all identity components are encoded as lowercase hexadecimal,
// so no transport-controlled text is interpreted as a path component.
[[nodiscard]] std::optional<std::filesystem::path> derive_exam_profile_path(
    const std::filesystem::path& profile_root,
    const security::Sha256Digest& package_digest,
    const security::ClientId& client_id,
    const exam::SessionId& session_id);

} // namespace nstu::client
