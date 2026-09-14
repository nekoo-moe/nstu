#include "nstu/telemetry.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <sstream>
#include <utility>

namespace nstu::telemetry {
namespace {

bool ascii_space(char value) noexcept {
    return std::isspace(static_cast<unsigned char>(value)) != 0;
}

bool ascii_digit(char value) noexcept {
    return std::isdigit(static_cast<unsigned char>(value)) != 0;
}

bool ascii_alpha(char value) noexcept {
    return std::isalpha(static_cast<unsigned char>(value)) != 0;
}

char ascii_lower(char value) noexcept {
    return static_cast<char>(
        std::tolower(static_cast<unsigned char>(value)));
}

std::string lower_ascii(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), ascii_lower);
    return result;
}

std::string_view trim_token(std::string_view token) noexcept {
    constexpr std::string_view punctuation = "\"'()[]{}<>,;";
    while (!token.empty() && punctuation.find(token.front()) !=
                                 std::string_view::npos) {
        token.remove_prefix(1);
    }
    while (!token.empty() &&
           (punctuation.find(token.back()) != std::string_view::npos ||
            token.back() == '.')) {
        token.remove_suffix(1);
    }
    return token;
}

bool sensitive_assignment(std::string_view token) {
    const auto separator = token.find_first_of("=:");
    if (separator == std::string_view::npos) {
        return false;
    }
    auto key = lower_ascii(token.substr(0, separator));
    key.erase(std::remove_if(key.begin(), key.end(), [](char value) {
                  return value == '-' || value == '_';
              }),
              key.end());
    constexpr std::array<std::string_view, 11> keys = {
        "authorization", "clientid", "hostname", "machine",
        "machinename", "password", "psk", "school", "secret",
        "token", "username"};
    return std::find(keys.begin(), keys.end(), key) != keys.end();
}

bool sensitive_key(std::string_view token) {
    auto key = lower_ascii(trim_token(token));
    while (!key.empty() && (key.back() == ':' || key.back() == '=')) {
        key.pop_back();
    }
    key.erase(std::remove_if(key.begin(), key.end(), [](char value) {
                  return value == '-' || value == '_';
              }),
              key.end());
    constexpr std::array<std::string_view, 11> keys = {
        "authorization", "clientid", "hostname", "machine",
        "machinename", "password", "psk", "school", "secret",
        "token", "username"};
    return std::find(keys.begin(), keys.end(), key) != keys.end();
}

bool contains_windows_path(std::string_view token) noexcept {
    if (token.size() >= 2 &&
        ((token[0] == '\\' && token[1] == '\\') ||
         (token[0] == '/' && token[1] == '/'))) {
        return true;
    }
    for (std::size_t index = 0; index + 2 < token.size(); ++index) {
        if (ascii_alpha(token[index]) && token[index + 1] == ':' &&
            (token[index + 2] == '\\' || token[index + 2] == '/')) {
            return true;
        }
    }
    return false;
}

bool contains_email(std::string_view token) noexcept {
    const auto at = token.find('@');
    return at != std::string_view::npos && at != 0 &&
           at + 1 < token.size() &&
           token.find('.', at + 1) != std::string_view::npos;
}

std::size_t ipv4_length(std::string_view value, std::size_t start) noexcept {
    std::size_t cursor = start;
    for (int part = 0; part < 4; ++part) {
        if (cursor >= value.size() || !ascii_digit(value[cursor])) {
            return 0;
        }
        unsigned int octet = 0;
        std::size_t digits = 0;
        while (cursor < value.size() && ascii_digit(value[cursor]) &&
               digits < 3) {
            octet = octet * 10u + static_cast<unsigned int>(value[cursor] - '0');
            ++cursor;
            ++digits;
        }
        if (octet > 255u ||
            (cursor < value.size() && ascii_digit(value[cursor]))) {
            return 0;
        }
        if (part != 3) {
            if (cursor >= value.size() || value[cursor] != '.') {
                return 0;
            }
            ++cursor;
        }
    }
    return cursor - start;
}

bool contains_ip_address(std::string_view token) noexcept {
    for (std::size_t index = 0; index < token.size(); ++index) {
        if (ascii_digit(token[index]) && ipv4_length(token, index) != 0) {
            return true;
        }
    }
    const auto colon_count = static_cast<std::size_t>(
        std::count(token.begin(), token.end(), ':'));
    if (colon_count < 2) {
        return false;
    }
    return std::all_of(token.begin(), token.end(), [](char value) {
        const auto byte = static_cast<unsigned char>(value);
        return std::isxdigit(byte) != 0 || value == ':' || value == '.' ||
               value == '%' || value == '[' || value == ']';
    });
}

bool contains_mac_address(std::string_view token) noexcept {
    for (const char separator : {':', '-'}) {
        std::size_t cursor = 0;
        bool valid = true;
        for (int group = 0; group < 6; ++group) {
            if (cursor + 2 > token.size() ||
                std::isxdigit(static_cast<unsigned char>(token[cursor])) == 0 ||
                std::isxdigit(static_cast<unsigned char>(token[cursor + 1])) == 0) {
                valid = false;
                break;
            }
            cursor += 2;
            if (group != 5) {
                if (cursor >= token.size() || token[cursor] != separator) {
                    valid = false;
                    break;
                }
                ++cursor;
            }
        }
        if (valid && cursor == token.size()) {
            return true;
        }
    }
    return false;
}

bool looks_like_secret(std::string_view token) noexcept {
    if (token.size() < 32) {
        return false;
    }
    std::size_t encoded = 0;
    std::size_t hyphens = 0;
    for (const char value : token) {
        const auto byte = static_cast<unsigned char>(value);
        if (std::isalnum(byte) != 0 || value == '+' || value == '/' ||
            value == '_' || value == '=' || value == '-') {
            ++encoded;
        }
        if (value == '-') {
            ++hyphens;
        }
    }
    return encoded == token.size() &&
           (hyphens >= 4 || token.size() >= 40);
}

bool contains_client_identifier(std::string_view text,
                                std::size_t index,
                                std::size_t& consumed) {
    constexpr std::string_view prefix = "client ";
    if (index + prefix.size() > text.size()) {
        return false;
    }
    for (std::size_t offset = 0; offset < prefix.size(); ++offset) {
        if (ascii_lower(text[index + offset]) != prefix[offset]) {
            return false;
        }
    }
    auto cursor = index + prefix.size();
    const auto digits_begin = cursor;
    while (cursor < text.size() && ascii_digit(text[cursor])) {
        ++cursor;
    }
    if (cursor == digits_begin) {
        return false;
    }
    consumed = cursor - index;
    return true;
}

void append_limited(std::string& output, std::string_view value,
                    std::size_t maximum_bytes) {
    if (output.size() >= maximum_bytes) {
        return;
    }
    const auto available = maximum_bytes - output.size();
    output.append(value.substr(0, available));
}

const char* severity_text(Severity severity) noexcept {
    switch (severity) {
    case Severity::information: return "info";
    case Severity::warning: return "warning";
    case Severity::error: return "error";
    }
    return "unknown";
}

} // namespace

std::string sanitize_public_text(std::string_view text,
                                 std::size_t maximum_bytes) {
    if (maximum_bytes == 0) {
        return {};
    }
    std::string output;
    output.reserve(std::min(text.size(), maximum_bytes));
    std::size_t index = 0;
    bool truncated = false;
    while (index < text.size() && output.size() < maximum_bytes) {
        std::size_t consumed = 0;
        if (contains_client_identifier(text, index, consumed)) {
            append_limited(output, "client <id>", maximum_bytes);
            index += consumed;
            continue;
        }
        if (ascii_space(text[index])) {
            while (index < text.size() && ascii_space(text[index])) {
                ++index;
            }
            if (!output.empty() && output.back() != ' ') {
                append_limited(output, " ", maximum_bytes);
            }
            continue;
        }
        auto end = index;
        while (end < text.size() && !ascii_space(text[end])) {
            ++end;
        }
        const auto token = text.substr(index, end - index);
        const auto core = trim_token(token);
        const bool redact_tail = sensitive_assignment(core) ||
            sensitive_key(core) || contains_windows_path(core);
        if (redact_tail) {
            append_limited(output, "<redacted>", maximum_bytes);
            index = text.size();
            break;
        }
        if (contains_email(core) || contains_ip_address(core) ||
            contains_mac_address(core) || looks_like_secret(core)) {
            append_limited(output, "<redacted>", maximum_bytes);
        } else {
            for (const char value : token) {
                if (output.size() >= maximum_bytes) {
                    truncated = true;
                    break;
                }
                const auto byte = static_cast<unsigned char>(value);
                if (std::iscntrl(byte) != 0) {
                    append_limited(output, "?", maximum_bytes);
                } else {
                    output.push_back(value);
                }
            }
        }
        index = end;
    }
    if ((truncated || index < text.size()) && maximum_bytes >= 3) {
        output.resize(std::min(output.size(), maximum_bytes - 3));
        output += "...";
    }
    return output;
}

void append_bounded_event(std::deque<Event>& events, Event event,
                          std::size_t capacity) {
    if (capacity == 0) {
        return;
    }
    event.component = sanitize_public_text(event.component, 64);
    event.detail = sanitize_public_text(event.detail);
    if (event.component.empty() || event.detail.empty()) {
        return;
    }
    if (!events.empty() && events.back().severity == event.severity &&
        events.back().component == event.component &&
        events.back().detail == event.detail) {
        return;
    }
    while (events.size() >= capacity) {
        events.pop_front();
    }
    events.push_back(std::move(event));
}

std::string build_public_markdown(const PublicReport& report) {
    std::ostringstream output;
    output << "# NSTU privacy-filtered diagnostic report\n\n"
           << "> Review this report before posting. It intentionally excludes "
              "IP/MAC addresses, host and user names, file paths, secrets, "
              "screenshots, chat, and exam content.\n\n"
           << "## Build\n\n"
           << "- Application: " << sanitize_public_text(report.application, 96)
           << '\n'
           << "- Version: " << sanitize_public_text(report.version, 64) << '\n'
           << "- Channel: " << sanitize_public_text(report.build_channel, 64)
           << "\n\n";
    if (!report.fields.empty()) {
        output << "## Configuration\n\n";
        for (const auto& field : report.fields) {
            output << "- " << sanitize_public_text(field.name, 64) << ": "
                   << sanitize_public_text(field.value, 192) << '\n';
        }
        output << '\n';
    }
    output << "## Recent diagnostic events\n\n";
    if (report.events.empty()) {
        output << "No events were collected.\n";
        return output.str();
    }
    const auto first = report.events.size() > kMaximumEvents
        ? report.events.size() - kMaximumEvents : 0;
    for (std::size_t index = first; index < report.events.size(); ++index) {
        const auto& event = report.events[index];
        output << "- `" << severity_text(event.severity) << "` `"
               << sanitize_public_text(event.component, 64) << "`: "
               << sanitize_public_text(event.detail) << '\n';
    }
    return output.str();
}

} // namespace nstu::telemetry
