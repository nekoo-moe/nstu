#pragma once

#include "nstu/agent_protocol.hpp"

#include <windows.h>

#include <filesystem>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace nstu::client {

// The host consumes an unpacked, already-enrolled package.  It never accepts
// a path supplied by the web page; the service/provisioning layer supplies the
// package root and identity-bound context before the window is created.
struct ExamHostOptions {
    std::filesystem::path package_root;
    std::filesystem::path web_root;
    std::filesystem::path user_data_root;
    // When set, package and WebView2 profile paths must stay below the
    // installed NSTU data root. The agent supplies this value for commands
    // received from the server; leaving it empty preserves the standalone
    // package-inspection API used by diagnostics and tests.
    std::filesystem::path allowed_data_root;
    // WebView2 profile data is user-scoped and normally lives below
    // %LOCALAPPDATA%, because the interactive student account may not be able
    // to write the SYSTEM/admin-protected data root.
    std::filesystem::path allowed_user_data_root;
    std::string context_json;
    std::string expected_digest_hex;
    bool require_digest = true;
};

struct ExamHostCallbacks {
    // Called from the agent UI thread.  The callback must enqueue the message
    // for the service pipe; it must not perform blocking I/O on this thread.
    std::function<void(AgentMessage)> send_to_service;
    std::function<void(std::string)> status;
};

enum class ExamHostState : std::uint8_t {
    idle = 0,
    preparing = 1,
    initializing = 2,
    running = 3,
    degraded = 4,
    failed = 5,
};

struct ExamPackageReport {
    std::filesystem::path package_root;
    std::filesystem::path web_root;
    std::filesystem::path page_path;
    std::string manifest_json;
    std::string digest_hex;
};

// Validates the bounded, unpacked package boundary without starting a browser.
// Manifest-declared media and document paths must resolve to regular files
// inside the package; deployment metadata (manifest.json/manifest.p7s) is not
// a browser asset. The digest covers sorted relative file names and file bytes,
// so a pinned digest detects both manifest and media changes. ZIP extraction
// and release signature verification remain deployment-layer responsibilities.
[[nodiscard]] bool inspect_exam_package(
    const std::filesystem::path& package_root,
    const std::filesystem::path& web_root,
    std::string_view expected_digest_hex,
    bool require_digest,
    ExamPackageReport& report,
    std::string* error = nullptr);

// Enforces the installed client package boundary before a browser is created.
// Packages must live below <data-root>\\exams. The WebView2 profile has its
// own user-writable policy root and is validated with
// validate_exam_user_data_path(). Paths are canonicalized before comparison so
// traversal and sibling-prefix tricks fail closed.
[[nodiscard]] bool validate_exam_path_policy(
    const std::filesystem::path& allowed_data_root,
    const std::filesystem::path& package_root,
    const std::filesystem::path& web_root,
    const std::filesystem::path& user_data_root,
    std::string* error = nullptr);

// Validates a user-scoped WebView2 profile path. The leaf may not be the
// policy root itself, and it may not escape that root through traversal or a
// reparse point. The leaf is allowed not to exist yet; the host creates it.
[[nodiscard]] bool validate_exam_user_data_path(
    const std::filesystem::path& allowed_user_data_root,
    const std::filesystem::path& user_data_root,
    std::string* error = nullptr);

class ExamHost {
public:
    ExamHost();
    ~ExamHost();
    ExamHost(const ExamHost&) = delete;
    ExamHost& operator=(const ExamHost&) = delete;

    // Must be called on the agent's UI/STA thread.  WebView2 initialization is
    // asynchronous; success means the kiosk window and validation completed,
    // while the state may remain `initializing` until the browser is ready.
    [[nodiscard]] bool start(HWND owner, const ExamHostOptions& options,
                             ExamHostCallbacks callbacks,
                             std::string* error = nullptr);
    void stop() noexcept;
    void drain_bridge();
    void enforce_foreground() noexcept;

    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] ExamHostState state() const noexcept;
    [[nodiscard]] HWND window() const noexcept;

    // Routes messages from the agent window procedure.  Returns true when the
    // message belongs to the exam host and sets `result`.
    [[nodiscard]] bool handle_window_message(UINT message, WPARAM wparam,
                                             LPARAM lparam,
                                             LRESULT& result) noexcept;

public:
    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace nstu::client
