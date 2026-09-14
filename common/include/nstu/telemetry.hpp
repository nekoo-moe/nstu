#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace nstu::telemetry {

inline constexpr std::size_t kMaximumEvents = 64;
inline constexpr std::size_t kMaximumPublicTextBytes = 512;

enum class Severity : std::uint8_t {
    information,
    warning,
    error,
};

struct ConsentPolicy {
    bool collect_in_background = false;
    bool prompt_on_error = false;
};

struct Field {
    std::string name;
    std::string value;
};

struct Event {
    Severity severity = Severity::information;
    std::string component;
    std::string detail;
};

struct PublicReport {
    std::string application;
    std::string version;
    std::string build_channel;
    std::vector<Field> fields;
    std::deque<Event> events;
};

[[nodiscard]] constexpr bool collection_enabled(
    const ConsentPolicy& policy) noexcept {
    return policy.collect_in_background;
}

[[nodiscard]] constexpr bool should_prompt_for_error(
    const ConsentPolicy& policy) noexcept {
    return policy.collect_in_background && policy.prompt_on_error;
}

[[nodiscard]] std::string sanitize_public_text(
    std::string_view text,
    std::size_t maximum_bytes = kMaximumPublicTextBytes);

void append_bounded_event(std::deque<Event>& events, Event event,
                          std::size_t capacity = kMaximumEvents);

[[nodiscard]] std::string build_public_markdown(const PublicReport& report);

} // namespace nstu::telemetry
