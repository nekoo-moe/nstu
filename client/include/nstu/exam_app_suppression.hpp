#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace nstu::client {

// Processes that must never be terminated while an exam is suppressing apps:
// killing any of these would crash the machine, lock the student out of the
// desktop, or take down NSTU itself -- including the WebView2 process that *is*
// the exam. The exam's own allowlist is additive on top of this; this built-in
// set is the floor and cannot be overridden.
inline constexpr std::string_view kProtectedImages[] = {
    // Windows core -- terminating these bluescreens or wedges the session.
    "System", "Registry", "smss.exe", "csrss.exe", "wininit.exe",
    "winlogon.exe", "services.exe", "lsass.exe", "lsaiso.exe", "svchost.exe",
    "fontdrvhost.exe", "dwm.exe", "LogonUI.exe", "WUDFHost.exe",
    // Shell / desktop UX -- without these the student has no usable screen.
    "explorer.exe", "ctfmon.exe", "sihost.exe", "taskhostw.exe",
    "RuntimeBroker.exe", "ApplicationFrameHost.exe", "ShellExperienceHost.exe",
    "StartMenuExperienceHost.exe", "TextInputHost.exe", "SearchHost.exe",
    // NSTU's own processes, and the WebView2 host that renders the exam.
    "nstu-agent.exe", "nstu-service.exe", "nstu-server.exe",
    "msedgewebview2.exe",
};

// Decides which running processes an exam should terminate. Pure policy: the
// caller enumerates processes and performs the actual termination; this only
// classifies, so it unit-tests without touching a single real process. The
// built-in protected set guarantees the suppression can never take down the OS,
// the shell, NSTU, or the exam kiosk itself.
class ExamAppSuppression {
public:
    explicit ExamAppSuppression(std::vector<std::string> allowed_images)
        : allowed_images_(std::move(allowed_images)) {}

    void arm() noexcept { active_ = true; }
    void disarm() noexcept { active_ = false; }
    [[nodiscard]] bool active() const noexcept { return active_; }

    // Whether this process image should be terminated right now. Only ever
    // true while armed, and never for a protected or exam-allowed image.
    [[nodiscard]] bool should_suppress(std::string_view image) const {
        if (!active_ || image.empty() || is_protected(image)) {
            return false;
        }
        for (const auto& allowed : allowed_images_) {
            if (iequals(allowed, image)) {
                return false;
            }
        }
        return true;
    }

    // Exposed so the always-keep floor can be asserted independently of the
    // arm state and the per-exam allowlist.
    [[nodiscard]] static bool is_protected(std::string_view image) {
        for (const std::string_view protected_image : kProtectedImages) {
            if (iequals(protected_image, image)) {
                return true;
            }
        }
        return false;
    }

private:
    static char ascii_lower(char value) noexcept {
        return (value >= 'A' && value <= 'Z')
                   ? static_cast<char>(value - 'A' + 'a')
                   : value;
    }

    // Windows process image names are case-insensitive; compare them that way
    // so "CSRSS.EXE" is still recognized as protected.
    static bool iequals(std::string_view left, std::string_view right) noexcept {
        if (left.size() != right.size()) {
            return false;
        }
        for (std::size_t index = 0; index < left.size(); ++index) {
            if (ascii_lower(left[index]) != ascii_lower(right[index])) {
                return false;
            }
        }
        return true;
    }

    std::vector<std::string> allowed_images_;
    bool active_ = false;
};

} // namespace nstu::client
