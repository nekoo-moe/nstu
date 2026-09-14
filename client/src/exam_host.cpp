#include "nstu/exam_host.hpp"

#include "nstu/exam_bridge.hpp"
#include "nstu/exam_sync.hpp"

#include <windows.h>

#if NSTU_HAS_WEBVIEW2
#include <wrl.h>
#include <unknwn.h>
#if defined(__GNUC__) && !defined(_MSC_VER)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunknown-pragmas"
#endif
#include <WebView2.h>
#if defined(__GNUC__) && !defined(_MSC_VER)
#pragma GCC diagnostic pop
#endif
#endif

#include <atomic>
#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace nstu::client {
namespace {

constexpr wchar_t kExamWindowClass[] = L"NSTU.ExamHost.Window";
constexpr wchar_t kExamWindowTitle[] = L"NSTU Assessment";
constexpr wchar_t kExamVirtualHost[] = L"nstu.exam";
constexpr UINT_PTR kBridgeTimerId = 0x4e535455u;
constexpr UINT kBridgeTimerPeriodMs = 250;
// The initialization timer uses a generation-derived id.  This matters on
// Win32 because a WM_TIMER queued for a previous window operation can arrive
// after KillTimer() and a new exam can already be initializing.  Distinct ids
// let the window procedure discard that stale notification safely.
constexpr UINT_PTR kInitializationTimerBase = 0x4e535600u;
constexpr UINT kInitializationTimerPeriodMs = 250;
constexpr UINT kInitializationTimeoutMs = 15'000;
constexpr std::size_t kMaximumManifestBytes = 1u * 1024u * 1024u;
constexpr std::size_t kMaximumPackageBytes = 128u * 1024u * 1024u;
constexpr std::size_t kMaximumPackageFiles = 4096;
constexpr std::size_t kMaximumJsonBytes = 128u * 1024u;
constexpr std::size_t kMaximumJsonDepth = 16;
constexpr std::size_t kMaximumJsonMembers = 256;
constexpr std::size_t kMaximumJsonStringBytes = 64u * 1024u;
constexpr UINT kHostMessageExamReady = WM_APP + 0x51;
// Private message used by the technician recovery chord. WM_CLOSE is
// intentionally ignored by the kiosk window, so recovery must be explicit.
constexpr UINT kHostMessageExamStop = WM_APP + 0x52;

void set_error(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
}

std::wstring utf8_to_wide(std::string_view value) {
    if (value.empty()) {
        return {};
    }
    const int source_length = static_cast<int>(value.size());
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                           value.data(), source_length,
                                           nullptr, 0);
    if (length <= 0) {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                            source_length, result.data(), length) != length) {
        return {};
    }
    return result;
}

[[maybe_unused]] std::string wide_to_utf8(std::wstring_view value) {
    if (value.empty()) {
        return {};
    }
    const int source_length = static_cast<int>(value.size());
    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                           value.data(), source_length,
                                           nullptr, 0, nullptr, nullptr);
    if (length <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(length), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                            source_length, result.data(), length, nullptr,
                            nullptr) != length) {
        return {};
    }
    return result;
}

#if NSTU_HAS_WEBVIEW2
// Encode host-controlled JSON as a JavaScript string literal before passing it
// to JSON.parse in the document-created bootstrap.  Treating the JSON as a
// raw object expression would make the bootstrap depend on JavaScript object
// literal semantics (notably the special __proto__ key) and would leave
// syntax-sensitive Unicode/control characters at the ABI boundary.
std::wstring javascript_string_literal(std::wstring_view value) {
    std::wstring result;
    result.reserve(value.size() + 2);
    result.push_back(L'"');
    for (const wchar_t character : value) {
        switch (character) {
        case L'"': result += L"\\\""; break;
        case L'\\': result += L"\\\\"; break;
        case L'\b': result += L"\\b"; break;
        case L'\f': result += L"\\f"; break;
        case L'\n': result += L"\\n"; break;
        case L'\r': result += L"\\r"; break;
        case L'\t': result += L"\\t"; break;
        case static_cast<wchar_t>(0x2028): result += L"\\u2028"; break;
        case static_cast<wchar_t>(0x2029): result += L"\\u2029"; break;
        default:
            if (character < 0x20 || character == 0x7f) {
                constexpr wchar_t digits[] = L"0123456789abcdef";
                result += L"\\u00";
                result.push_back(digits[(static_cast<unsigned int>(character) >> 4u) & 0x0fu]);
                result.push_back(digits[static_cast<unsigned int>(character) & 0x0fu]);
            } else {
                result.push_back(character);
            }
            break;
        }
    }
    result.push_back(L'"');
    return result;
}
#endif

template <typename Function>
Function load_function(HMODULE module, const char* name) noexcept {
    const FARPROC raw = module == nullptr ? nullptr : GetProcAddress(module, name);
    Function function{};
    static_assert(sizeof(function) == sizeof(raw));
    std::memcpy(&function, &raw, sizeof(function));
    return function;
}

bool is_zero_digest(std::span<const std::byte> value) noexcept {
    return std::all_of(value.begin(), value.end(),
                       [](std::byte byte) { return byte == std::byte{0}; });
}

template <std::size_t N>
bool parse_hex(std::string_view value, std::array<std::byte, N>& output) {
    if (value.size() != N * 2) {
        return false;
    }
    auto nibble = [](char character) -> int {
        if (character >= '0' && character <= '9') {
            return character - '0';
        }
        if (character >= 'a' && character <= 'f') {
            return character - 'a' + 10;
        }
        if (character >= 'A' && character <= 'F') {
            return character - 'A' + 10;
        }
        return -1;
    };
    for (std::size_t index = 0; index < N; ++index) {
        const int high = nibble(value[index * 2]);
        const int low = nibble(value[index * 2 + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        output[index] = static_cast<std::byte>((high << 4) | low);
    }
    return true;
}

template <typename Array>
std::string hex_string(const Array& bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (const std::byte byte : bytes) {
        const auto value = std::to_integer<unsigned int>(byte);
        result.push_back(digits[(value >> 4) & 0x0f]);
        result.push_back(digits[value & 0x0f]);
    }
    return result;
}

bool read_file_bounded(const std::filesystem::path& path, std::size_t limit,
                       std::vector<std::byte>& output, std::string* error) {
    std::error_code status_error;
    const auto status = std::filesystem::status(path, status_error);
    if (status_error || !std::filesystem::is_regular_file(status)) {
        set_error(error, "exam package file is unavailable");
        return false;
    }
    const auto size = std::filesystem::file_size(path, status_error);
    if (status_error || size > limit || size >
        static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max())) {
        set_error(error, "exam package file is too large");
        return false;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        set_error(error, "exam package file could not be opened");
        return false;
    }
    output.assign(static_cast<std::size_t>(size), std::byte{0});
    if (!output.empty()) {
        input.read(reinterpret_cast<char*>(output.data()),
                   static_cast<std::streamsize>(output.size()));
        if (input.gcount() != static_cast<std::streamsize>(output.size())) {
            set_error(error, "exam package file read failed");
            return false;
        }
    }
    return true;
}

// Check every existing component instead of only the final path. A junction
// or symlink in a parent directory can otherwise be resolved away by
// weakly_canonical() before the containment check runs.
bool path_has_reparse_point(const std::filesystem::path& path) noexcept {
    try {
        if (path.empty()) return false;
        const auto normalized = path.lexically_normal();
        auto has_reparse_attribute = [](const std::filesystem::path& value) {
            const DWORD attributes = GetFileAttributesW(value.c_str());
            return attributes != INVALID_FILE_ATTRIBUTES &&
                   (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        };
        std::filesystem::path prefix = normalized.root_path();
        if (!prefix.empty() && has_reparse_attribute(prefix)) return true;
        for (const auto& component : normalized) {
            if (component == normalized.root_name() ||
                component == normalized.root_directory()) {
                continue;
            }
            prefix /= component;
            if (has_reparse_attribute(prefix)) return true;
        }
    } catch (...) {
        // Callers perform independent existence/type checks. A filesystem
        // exception here must not turn a valid missing-leaf path into an
        // accidental allow; the subsequent canonical/status checks fail
        // closed where the path is required to exist.
    }
    return false;
}

bool path_is_within(const std::filesystem::path& root,
                    const std::filesystem::path& candidate) {
    std::error_code error;
    const auto root_canonical = std::filesystem::weakly_canonical(root, error);
    if (error) {
        return false;
    }
    const auto candidate_canonical =
        std::filesystem::weakly_canonical(candidate, error);
    if (error) {
        return false;
    }
#ifdef _WIN32
    const auto root_text = root_canonical.native();
    const auto candidate_text = candidate_canonical.native();
    if (candidate_text.size() < root_text.size() ||
        _wcsnicmp(candidate_text.c_str(), root_text.c_str(),
                  root_text.size()) != 0) {
        return false;
    }
    return candidate_text.size() == root_text.size() ||
           candidate_text[root_text.size()] == L'\\' ||
           candidate_text[root_text.size()] == L'/';
#else
    const auto relative = std::filesystem::relative(candidate_canonical,
                                                     root_canonical, error);
    return !error && relative.native().find("..") != 0;
#endif
}

// A small bounded JSON reader is used at the native boundary so browser
// messages are parsed structurally rather than by substring matching.  It is
// intentionally limited to the JSON types needed by the exam bridge.
struct JsonValue {
    enum class Kind : std::uint8_t { null_value, boolean, number, string,
                                     object, array };
    Kind kind = Kind::null_value;
    bool boolean = false;
    std::string scalar;
    std::map<std::string, JsonValue> object;
    std::vector<JsonValue> array;
};

void append_codepoint_utf8(std::string& output, std::uint32_t codepoint) {
    if (codepoint <= 0x7fu) {
        output.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7ffu) {
        output.push_back(static_cast<char>(0xc0u | (codepoint >> 6u)));
        output.push_back(static_cast<char>(0x80u | (codepoint & 0x3fu)));
    } else if (codepoint <= 0xffffu) {
        output.push_back(static_cast<char>(0xe0u | (codepoint >> 12u)));
        output.push_back(static_cast<char>(0x80u | ((codepoint >> 6u) & 0x3fu)));
        output.push_back(static_cast<char>(0x80u | (codepoint & 0x3fu)));
    } else if (codepoint <= 0x10ffffu) {
        output.push_back(static_cast<char>(0xf0u | (codepoint >> 18u)));
        output.push_back(static_cast<char>(0x80u | ((codepoint >> 12u) & 0x3fu)));
        output.push_back(static_cast<char>(0x80u | ((codepoint >> 6u) & 0x3fu)));
        output.push_back(static_cast<char>(0x80u | (codepoint & 0x3fu)));
    }
}

class JsonReader {
public:
    explicit JsonReader(std::string_view input) : input_(input) {}

    bool parse(JsonValue& value, std::string* error) {
        if (input_.size() > kMaximumJsonBytes || !parse_value(value, 0) ||
            !consume_whitespace() || position_ != input_.size()) {
            set_error(error, "invalid or oversized JSON message");
            return false;
        }
        return true;
    }

private:
    bool consume_whitespace() noexcept {
        while (position_ < input_.size() &&
               std::isspace(static_cast<unsigned char>(input_[position_]))) {
            ++position_;
        }
        return true;
    }

    bool parse_value(JsonValue& value, std::size_t depth) {
        if (depth > kMaximumJsonDepth) {
            return false;
        }
        consume_whitespace();
        if (position_ >= input_.size()) {
            return false;
        }
        switch (input_[position_]) {
        case '{': return parse_object(value, depth + 1);
        case '[': return parse_array(value, depth + 1);
        case '"':
            value.kind = JsonValue::Kind::string;
            return parse_string(value.scalar);
        case 't':
            if (input_.substr(position_, 4) != "true") return false;
            position_ += 4;
            value.kind = JsonValue::Kind::boolean;
            value.boolean = true;
            return true;
        case 'f':
            if (input_.substr(position_, 5) != "false") return false;
            position_ += 5;
            value.kind = JsonValue::Kind::boolean;
            value.boolean = false;
            return true;
        case 'n':
            if (input_.substr(position_, 4) != "null") return false;
            position_ += 4;
            value.kind = JsonValue::Kind::null_value;
            return true;
        default:
            value.kind = JsonValue::Kind::number;
            return parse_number(value.scalar);
        }
    }

    bool parse_object(JsonValue& value, std::size_t depth) {
        if (input_[position_++] != '{') return false;
        value.kind = JsonValue::Kind::object;
        consume_whitespace();
        if (position_ < input_.size() && input_[position_] == '}') {
            ++position_;
            return true;
        }
        std::size_t members = 0;
        while (position_ < input_.size()) {
            if (++members > kMaximumJsonMembers || input_[position_] != '"') {
                return false;
            }
            std::string key;
            if (!parse_string(key)) return false;
            consume_whitespace();
            if (position_ >= input_.size() || input_[position_++] != ':') {
                return false;
            }
            JsonValue child;
            if (!parse_value(child, depth)) return false;
            if (!value.object.emplace(std::move(key), std::move(child)).second) {
                return false;
            }
            consume_whitespace();
            if (position_ >= input_.size()) return false;
            if (input_[position_] == '}') {
                ++position_;
                return true;
            }
            if (input_[position_++] != ',') return false;
            consume_whitespace();
        }
        return false;
    }

    bool parse_array(JsonValue& value, std::size_t depth) {
        if (input_[position_++] != '[') return false;
        value.kind = JsonValue::Kind::array;
        consume_whitespace();
        if (position_ < input_.size() && input_[position_] == ']') {
            ++position_;
            return true;
        }
        while (position_ < input_.size()) {
            if (value.array.size() >= kMaximumJsonMembers) return false;
            JsonValue child;
            if (!parse_value(child, depth)) return false;
            value.array.push_back(std::move(child));
            consume_whitespace();
            if (position_ >= input_.size()) return false;
            if (input_[position_] == ']') {
                ++position_;
                return true;
            }
            if (input_[position_++] != ',') return false;
            consume_whitespace();
        }
        return false;
    }

    bool parse_string(std::string& output) {
        if (position_ >= input_.size() || input_[position_++] != '"') {
            return false;
        }
        output.clear();
        while (position_ < input_.size()) {
            const unsigned char character =
                static_cast<unsigned char>(input_[position_++]);
            if (character == '"') {
                if (output.size() > kMaximumJsonStringBytes) return false;
                return output.empty() || !utf8_to_wide(output).empty();
            }
            if (character < 0x20u) return false;
            if (character != '\\') {
                output.push_back(static_cast<char>(character));
                if (output.size() > kMaximumJsonStringBytes) return false;
                continue;
            }
            if (position_ >= input_.size()) return false;
            const char escape = input_[position_++];
            switch (escape) {
            case '"': output.push_back('"'); break;
            case '\\': output.push_back('\\'); break;
            case '/': output.push_back('/'); break;
            case 'b': output.push_back('\b'); break;
            case 'f': output.push_back('\f'); break;
            case 'n': output.push_back('\n'); break;
            case 'r': output.push_back('\r'); break;
            case 't': output.push_back('\t'); break;
            case 'u': {
                if (input_.size() - position_ < 4) return false;
                std::uint32_t codepoint = 0;
                for (int index = 0; index < 4; ++index) {
                    const char digit = input_[position_++];
                    codepoint <<= 4u;
                    if (digit >= '0' && digit <= '9') codepoint += digit - '0';
                    else if (digit >= 'a' && digit <= 'f') codepoint += digit - 'a' + 10;
                    else if (digit >= 'A' && digit <= 'F') codepoint += digit - 'A' + 10;
                    else return false;
                }
                append_codepoint_utf8(output, codepoint);
                break;
            }
            default: return false;
            }
            if (output.size() > kMaximumJsonStringBytes) return false;
        }
        return false;
    }

    bool parse_number(std::string& output) {
        const std::size_t begin = position_;
        if (position_ < input_.size() && input_[position_] == '-') ++position_;
        if (position_ >= input_.size()) return false;
        if (input_[position_] == '0') {
            ++position_;
        } else {
            if (input_[position_] < '1' || input_[position_] > '9') return false;
            while (position_ < input_.size() &&
                   std::isdigit(static_cast<unsigned char>(input_[position_]))) {
                ++position_;
            }
        }
        if (position_ < input_.size() && input_[position_] == '.') {
            ++position_;
            if (position_ >= input_.size() ||
                !std::isdigit(static_cast<unsigned char>(input_[position_]))) {
                return false;
            }
            while (position_ < input_.size() &&
                   std::isdigit(static_cast<unsigned char>(input_[position_]))) {
                ++position_;
            }
        }
        if (position_ < input_.size() &&
            (input_[position_] == 'e' || input_[position_] == 'E')) {
            ++position_;
            if (position_ < input_.size() &&
                (input_[position_] == '+' || input_[position_] == '-')) ++position_;
            if (position_ >= input_.size() ||
                !std::isdigit(static_cast<unsigned char>(input_[position_]))) {
                return false;
            }
            while (position_ < input_.size() &&
                   std::isdigit(static_cast<unsigned char>(input_[position_]))) {
                ++position_;
            }
        }
        output.assign(input_.substr(begin, position_ - begin));
        return true;
    }

    std::string_view input_;
    std::size_t position_ = 0;
};

const JsonValue* member(const JsonValue& object, std::string_view name) {
    if (object.kind != JsonValue::Kind::object) return nullptr;
    const auto found = object.object.find(std::string(name));
    return found == object.object.end() ? nullptr : &found->second;
}

bool string_member(const JsonValue& object, std::string_view name,
                  std::string& output, bool required = true) {
    const auto* value = member(object, name);
    if (value == nullptr) return !required;
    if (value->kind != JsonValue::Kind::string) return false;
    output = value->scalar;
    return true;
}

bool uint64_member(const JsonValue& object, std::string_view name,
                   std::uint64_t& output, bool required = true) {
    const auto* value = member(object, name);
    if (value == nullptr) return !required;
    if (value->kind != JsonValue::Kind::number || value->scalar.empty() ||
        value->scalar.front() == '-') return false;
    const auto* begin = value->scalar.data();
    const auto* end = begin + value->scalar.size();
    const auto parsed = std::from_chars(begin, end, output);
    return parsed.ec == std::errc{} && parsed.ptr == end;
}

bool parse_json_object(std::string_view input, JsonValue& output,
                       std::string* error) {
    JsonReader reader(input);
    if (!reader.parse(output, error) ||
        output.kind != JsonValue::Kind::object) {
        set_error(error, "JSON root must be an object");
        return false;
    }
    return true;
}

std::string json_escape(std::string_view value) {
    std::string output;
    output.reserve(value.size() + 8);
    for (const unsigned char character : value) {
        switch (character) {
        case '"': output += "\\\""; break;
        case '\\': output += "\\\\"; break;
        case '\b': output += "\\b"; break;
        case '\f': output += "\\f"; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default:
            if (character < 0x20u) {
                constexpr char digits[] = "0123456789abcdef";
                output += "\\u00";
                output.push_back(digits[(character >> 4) & 0x0f]);
                output.push_back(digits[character & 0x0f]);
            } else {
                output.push_back(static_cast<char>(character));
            }
        }
    }
    return output;
}

bool valid_relative_asset(std::string_view value) {
    if (value.empty() || value.size() > 512 || value.find('\0') != std::string_view::npos) {
        return false;
    }
    if (value.find("://") != std::string_view::npos ||
        value.starts_with("javascript:") || value.starts_with("data:") ||
        value.starts_with('/') || value.starts_with('\\') ||
        (value.size() >= 2 && std::isalpha(static_cast<unsigned char>(value[0])) &&
         value[1] == ':')) {
        return false;
    }
    std::size_t start = 0;
    while (start <= value.size()) {
        const auto slash = value.find_first_of("/\\", start);
        const auto component = value.substr(start, slash == std::string_view::npos
                                                       ? value.size() - start
                                                       : slash - start);
        if (component == "." || component == ".." || component.empty()) {
            return false;
        }
        if (component.back() == '.' || component.back() == ' ') {
            return false;
        }
        for (const unsigned char character : component) {
            if (character < 0x20u || character == 0x7fu ||
                character == ':' || character == '*' || character == '"' ||
                character == '<' || character == '>' || character == '|' ||
                character == '?') {
                return false;
            }
        }
        if (slash == std::string_view::npos) break;
        start = slash + 1;
    }
    return true;
}

bool ascii_case_insensitive_equal(std::string_view left,
                                  std::string_view right) noexcept {
    if (left.size() != right.size()) return false;
    const auto fold = [](unsigned char character) noexcept {
        return character >= static_cast<unsigned char>('A') &&
                       character <= static_cast<unsigned char>('Z')
                   ? static_cast<unsigned char>(character - 'A' + 'a')
                   : character;
    };
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto lhs = static_cast<unsigned char>(left[index]);
        const auto rhs = static_cast<unsigned char>(right[index]);
        if (fold(lhs) != fold(rhs)) return false;
    }
    return true;
}

// These files belong to the deployment boundary, not to the browser-visible
// exam surface. Keep the comparison case-insensitive because the package is
// ultimately unpacked on a Windows case-insensitive filesystem and a manifest
// must not bypass the rule by changing the spelling.
bool is_private_package_asset(std::string_view value) noexcept {
    return ascii_case_insensitive_equal(value, "manifest.json") ||
           ascii_case_insensitive_equal(value, "manifest.p7s");
}

bool valid_declared_asset(std::string_view value) noexcept {
    return valid_relative_asset(value) && !is_private_package_asset(value);
}

int uri_hex_digit(unsigned char character) noexcept {
    if (character >= '0' && character <= '9') return character - '0';
    if (character >= 'a' && character <= 'f') return character - 'a' + 10;
    if (character >= 'A' && character <= 'F') return character - 'A' + 10;
    return -1;
}

[[maybe_unused]] bool decode_uri_path(std::wstring_view encoded,
                                      std::string& output) {
    const auto utf8 = wide_to_utf8(encoded);
    if (utf8.empty()) return false;
    output.clear();
    output.reserve(utf8.size());
    for (std::size_t index = 0; index < utf8.size();) {
        const auto character = static_cast<unsigned char>(utf8[index]);
        if (character == '%') {
            if (index + 2 >= utf8.size()) return false;
            const int high = uri_hex_digit(
                static_cast<unsigned char>(utf8[index + 1]));
            const int low = uri_hex_digit(
                static_cast<unsigned char>(utf8[index + 2]));
            if (high < 0 || low < 0) return false;
            const auto decoded = static_cast<unsigned char>((high << 4) | low);
            // Encoded separators must not be able to create a new path
            // component after validation. Other Windows-invalid characters
            // are rejected by valid_relative_asset below.
            if (decoded == 0 || decoded == '/' || decoded == '\\' ||
                decoded == '?' || decoded == '#') {
                return false;
            }
            output.push_back(static_cast<char>(decoded));
            index += 3;
            continue;
        }
        if (character < 0x20u || character == 0x7fu) return false;
        output.push_back(static_cast<char>(character));
        ++index;
    }
    if (output.empty() || output.find('\0') != std::string::npos ||
        utf8_to_wide(output).empty()) {
        return false;
    }
    return true;
}

bool validate_manifest_shape(const JsonValue& manifest, std::string* error) {
    std::string id;
    std::string title;
    std::uint64_t duration = 0;
    const auto* questions = member(manifest, "questions");
    if (!string_member(manifest, "id", id) || id.empty() || id.size() > 96 ||
        !string_member(manifest, "title", title) || title.empty() ||
        !uint64_member(manifest, "durationSeconds", duration) || duration < 60 ||
        duration > 86400 || questions == nullptr ||
        questions->kind != JsonValue::Kind::array || questions->array.empty() ||
        questions->array.size() > 500) {
        set_error(error, "exam manifest required fields are invalid");
        return false;
    }
    std::set<std::string> question_ids;
    for (const auto& question : questions->array) {
        std::string question_id;
        std::string type;
        std::string prompt;
        if (!string_member(question, "id", question_id) || question_id.empty() ||
            question_id.size() > 96 || !question_ids.insert(question_id).second ||
            !string_member(question, "type", type) ||
            (type != "multiple_choice" && type != "short_answer" &&
             type != "essay" && type != "listening" && type != "reading") ||
            !string_member(question, "prompt", prompt) || prompt.empty() ||
            prompt.size() > 8000) {
            set_error(error, "exam manifest question is invalid");
            return false;
        }
        for (const auto* field : {"audio", "pdf"}) {
            std::string asset;
            if (!string_member(question, field, asset, false)) {
                set_error(error, "exam manifest asset field is invalid");
                return false;
            }
            if (!asset.empty() && !valid_declared_asset(asset)) {
                set_error(error, "exam manifest contains a non-local asset path");
                return false;
            }
        }
        const auto* options = member(question, "options");
        const bool needs_options = type == "multiple_choice" ||
                                    type == "listening" ||
                                    type == "reading";
        if (needs_options &&
            (options == nullptr || options->kind != JsonValue::Kind::array ||
             options->array.size() < 2 || options->array.size() > 12)) {
            set_error(error, "exam manifest question options are invalid");
            return false;
        }
        if (options != nullptr) {
            if (options->kind != JsonValue::Kind::array ||
                options->array.size() > 12) {
                set_error(error, "exam manifest question options are invalid");
                return false;
            }
            for (const auto& option : options->array) {
                if (option.kind != JsonValue::Kind::string ||
                    option.scalar.empty() || option.scalar.size() > 2000) {
                    set_error(error, "exam manifest option text is invalid");
                    return false;
                }
            }
        }
    }
    const auto* documents = member(manifest, "documents");
    if (documents != nullptr) {
        if (documents->kind != JsonValue::Kind::array || documents->array.size() > 128) {
            set_error(error, "exam manifest documents are invalid");
            return false;
        }
        for (const auto& document : documents->array) {
            std::string url;
            if (!string_member(document, "url", url) ||
                !valid_declared_asset(url)) {
                set_error(error, "exam manifest document path is not local");
                return false;
            }
        }
    }
    return true;
}

// The virtual-host mapping points at the unpacked package because the browser
// needs package-relative media paths. Keep a separate allowlist for every
// request so a package page cannot read manifest.json, answer material, or
// other files that are not part of the declared web surface.
bool collect_manifest_assets(const std::string& manifest_json,
                             std::set<std::string>& assets,
                             std::string* error) {
    JsonValue manifest;
    if (!parse_json_object(manifest_json, manifest, error) ||
        !validate_manifest_shape(manifest, error)) {
        return false;
    }
    const auto* questions = member(manifest, "questions");
    if (questions == nullptr || questions->kind != JsonValue::Kind::array) {
        set_error(error, "exam manifest question list is invalid");
        return false;
    }
    for (const auto& question : questions->array) {
        for (const auto* field : {"audio", "pdf"}) {
            std::string asset;
            if (!string_member(question, field, asset, false)) {
                set_error(error, "exam manifest asset field is invalid");
                return false;
            }
            if (!asset.empty()) assets.insert(std::move(asset));
        }
    }
    const auto* documents = member(manifest, "documents");
    if (documents != nullptr) {
        for (const auto& document : documents->array) {
            std::string asset;
            if (!string_member(document, "url", asset) ||
                !valid_declared_asset(asset)) {
                set_error(error, "exam manifest document path is invalid");
                return false;
            }
            assets.insert(std::move(asset));
        }
    }
    return true;
}

bool validate_manifest_asset_files(const std::filesystem::path& package_root,
                                   const std::string& manifest_json,
                                   std::string* error) {
    std::set<std::string> assets;
    if (!collect_manifest_assets(manifest_json, assets, error)) return false;

    for (const auto& asset : assets) {
        // The declaration validator above rejects these names. Keep this
        // check local as well so this helper remains fail-closed if its call
        // sites or manifest parser are changed independently later.
        if (is_private_package_asset(asset)) {
            set_error(error, "exam manifest references a private package file");
            return false;
        }
        const auto wide_asset = utf8_to_wide(asset);
        if (wide_asset.empty()) {
            set_error(error, "exam manifest asset path is not valid UTF-8");
            return false;
        }
        const auto candidate = package_root / std::filesystem::path(wide_asset);
        std::error_code path_error;
        const auto canonical = std::filesystem::weakly_canonical(
            candidate, path_error);
        if (path_error || !path_is_within(package_root, canonical) ||
            path_has_reparse_point(candidate)) {
            set_error(error, "exam manifest asset path is outside the package");
            return false;
        }
        const auto status = std::filesystem::status(candidate, path_error);
        if (path_error || !std::filesystem::is_regular_file(status)) {
            set_error(error, "exam manifest references a missing asset");
            return false;
        }
    }
    return true;
}

struct ContextIdentity {
    std::string package_id;
    security::Sha256Digest package_digest{};
    security::ClientId client_id{};
    exam::SessionId session_id{};
    std::string candidate_id;
};

bool parse_context(std::string_view json, std::string_view manifest_package_id,
                   ContextIdentity& output, std::string* error) {
    JsonValue root;
    if (!parse_json_object(json, root, error)) return false;
    std::string package_id;
    std::string digest;
    std::string client;
    std::string session;
    if (!string_member(root, "packageId", package_id) ||
        !string_member(root, "packageDigestHex", digest) ||
        !string_member(root, "clientIdHex", client) ||
        !string_member(root, "sessionIdHex", session) ||
        !string_member(root, "candidateId", output.candidate_id) ||
        package_id.empty() ||
        package_id.size() > exam::kMaximumPackageIdBytes ||
        output.candidate_id.empty() || output.candidate_id.size() > 128 ||
        !parse_hex(digest, output.package_digest) ||
        !parse_hex(client, output.client_id) ||
        !parse_hex(session, output.session_id) ||
        is_zero_digest(output.package_digest) ||
        is_zero_digest(output.client_id) || is_zero_digest(output.session_id)) {
        set_error(error, "exam context identity is invalid");
        return false;
    }
    if (package_id != manifest_package_id) {
        set_error(error, "exam context package id does not match the manifest");
        return false;
    }
    output.package_id = std::move(package_id);
    return true;
}

bool parse_manifest_and_context(const ExamHostOptions& options,
                                ExamPackageReport& report,
                                ContextIdentity& context,
                                std::string* error) {
    if (!options.allowed_data_root.empty() &&
        !validate_exam_path_policy(options.allowed_data_root,
                                   options.package_root, options.web_root, {},
                                   error)) {
        return false;
    }
    if (!options.allowed_user_data_root.empty() &&
        !validate_exam_user_data_path(options.allowed_user_data_root,
                                      options.user_data_root, error)) {
        return false;
    }
    if (!inspect_exam_package(options.package_root, options.web_root,
                              options.expected_digest_hex,
                              options.require_digest, report, error)) {
        return false;
    }
    JsonValue manifest;
    if (!parse_json_object(report.manifest_json, manifest, error) ||
        !validate_manifest_shape(manifest, error)) {
        return false;
    }
    std::string package_id;
    if (!string_member(manifest, "id", package_id)) {
        set_error(error, "exam manifest id is missing");
        return false;
    }
    if (!parse_context(options.context_json, package_id, context, error)) {
        return false;
    }
    security::Sha256Digest package_digest{};
    if (!parse_hex(report.digest_hex, package_digest) ||
        !security::constant_time_equal(package_digest, context.package_digest)) {
        set_error(error, "exam context package digest does not match the package");
        return false;
    }
    return true;
}

bool same_context(const exam::AnswerEvent& event, const ContextIdentity& context) {
    return event.package_id == context.package_id &&
           event.package_digest == context.package_digest &&
           event.client_id == context.client_id &&
           event.session_id == context.session_id &&
           event.candidate_id == context.candidate_id;
}

bool same_context(const exam::StateRequest& request,
                  const ContextIdentity& context) {
    return request.package_id == context.package_id &&
           request.package_digest == context.package_digest &&
           request.client_id == context.client_id &&
           request.session_id == context.session_id &&
           request.candidate_id == context.candidate_id;
}

[[maybe_unused]] bool parse_answer_event(const JsonValue& root, const ContextIdentity& context,
                        exam::AnswerEvent& output, std::string* error) {
    const auto* value = member(root, "event");
    const auto& event = value != nullptr ? *value : root;
    std::string package_id;
    std::string digest;
    std::string client;
    std::string session;
    std::string kind;
    std::string answer;
    std::string previous_hash;
    std::string question_id;
    std::string candidate;
    std::uint64_t revision = 0;
    std::uint64_t sequence = 0;
    std::uint64_t client_time = 0;
    if (!string_member(event, "packageId", package_id) ||
        !string_member(event, "packageDigestHex", digest) ||
        !string_member(event, "clientIdHex", client) ||
        !string_member(event, "sessionIdHex", session) ||
        !string_member(event, "candidateId", candidate) ||
        !string_member(event, "questionId", question_id, false) ||
        !uint64_member(event, "questionRevision", revision) ||
        !uint64_member(event, "sequence", sequence) ||
        !uint64_member(event, "clientTimeUnixMilliseconds", client_time) ||
        !string_member(event, "kind", kind) ||
        !string_member(event, "answer", answer, false) ||
        !string_member(event, "previousEventHashHex", previous_hash, false)) {
        set_error(error, "exam answer event fields are invalid");
        return false;
    }
    if (revision == 0 ||
        revision > std::numeric_limits<std::uint32_t>::max()) {
        set_error(error, "exam answer question revision is out of range");
        return false;
    }
    // `answer` and `previousEventHashHex` are optional only for finalize/first
    // events; normalize absent values to their protocol defaults.
    if (member(event, "answer") == nullptr) answer.clear();
    if (member(event, "previousEventHashHex") == nullptr) previous_hash.clear();
    output.package_id = std::move(package_id);
    if (!parse_hex(digest, output.package_digest) ||
        !parse_hex(client, output.client_id) ||
        !parse_hex(session, output.session_id)) {
        set_error(error, "exam answer event identity is not valid hex");
        return false;
    }
    output.candidate_id = std::move(candidate);
    output.question_id = std::move(question_id);
    output.question_revision = static_cast<std::uint32_t>(revision);
    output.sequence = sequence;
    output.client_time_unix_milliseconds = client_time;
    if (kind == "upsert") output.kind = exam::AnswerKind::upsert;
    else if (kind == "clear") output.kind = exam::AnswerKind::clear;
    else if (kind == "finalize") output.kind = exam::AnswerKind::finalize;
    else {
        set_error(error, "exam answer event kind is invalid");
        return false;
    }
    output.answer.assign(reinterpret_cast<const std::byte*>(answer.data()),
                         reinterpret_cast<const std::byte*>(answer.data()) +
                             answer.size());
    if (!parse_hex(previous_hash, output.previous_event_hash) &&
        !previous_hash.empty()) {
        set_error(error, "exam answer predecessor hash is invalid");
        return false;
    }
    if (!same_context(output, context) ||
        !exam::validate_answer_event(output, error)) {
        if (error != nullptr && error->empty()) {
            *error = "exam answer event context does not match";
        }
        return false;
    }
    std::string supplied_hash;
    if (!string_member(event, "eventHashHex", supplied_hash, false)) {
        set_error(error, "exam answer event hash field is invalid");
        return false;
    }
    if (const auto calculated = exam::hash_answer_event(output)) {
        if (!supplied_hash.empty()) {
            security::Sha256Digest supplied{};
            if (!parse_hex(supplied_hash, supplied) ||
                !security::constant_time_equal(*calculated, supplied)) {
                set_error(error, "exam answer event hash does not match");
                return false;
            }
        }
    } else {
        set_error(error, "exam answer event could not be hashed");
        return false;
    }
    return true;
}

[[maybe_unused]] bool parse_state_request(const JsonValue& root, const ContextIdentity& context,
                         exam::StateRequest& output, std::string* error) {
    const auto* value = member(root, "request");
    const auto& request = value != nullptr ? *value : root;
    std::string package_id;
    std::string digest;
    std::string client;
    std::string session;
    if (!string_member(request, "packageId", package_id) ||
        !string_member(request, "packageDigestHex", digest) ||
        !string_member(request, "clientIdHex", client) ||
        !string_member(request, "sessionIdHex", session) ||
        !string_member(request, "candidateId", output.candidate_id) ||
        !parse_hex(digest, output.package_digest) ||
        !parse_hex(client, output.client_id) ||
        !parse_hex(session, output.session_id)) {
        set_error(error, "exam state request fields are invalid");
        return false;
    }
    output.package_id = std::move(package_id);
    if (!same_context(output, context)) {
        set_error(error, "exam state request context does not match");
        return false;
    }
    return true;
}

std::string ack_status_name(exam::AnswerAckStatus status) {
    switch (status) {
    case exam::AnswerAckStatus::accepted: return "accepted";
    case exam::AnswerAckStatus::duplicate: return "duplicate";
    case exam::AnswerAckStatus::rejected: return "rejected";
    case exam::AnswerAckStatus::conflict: return "conflict";
    case exam::AnswerAckStatus::gap: return "gap";
    case exam::AnswerAckStatus::unavailable: return "unavailable";
    }
    return "rejected";
}

[[maybe_unused]] std::string make_ack_json(const exam::AnswerAck& ack) {
    return "{\"type\":\"exam_answer_ack\",\"ack\":{\"status\":\"" +
           ack_status_name(ack.status) + "\",\"sessionIdHex\":\"" +
           hex_string(ack.session_id) + "\",\"sequence\":" +
           std::to_string(ack.sequence) +
           ",\"highestContiguousSequence\":" +
           std::to_string(ack.highest_contiguous_sequence) +
           ",\"serverTimeUnixMilliseconds\":" +
           std::to_string(ack.server_time_unix_milliseconds) +
           ",\"eventHashHex\":\"" + hex_string(ack.event_hash) +
           "\",\"stateHashHex\":\"" + hex_string(ack.state_hash) + "\"}}";
}

[[maybe_unused]] std::string make_state_json(const exam::StateResponse& response) {
    std::string json = "{\"type\":\"exam_state_response\",\"response\":{";
    json += "\"packageId\":\"" + json_escape(response.package_id) +
            "\",\"packageDigestHex\":\"" + hex_string(response.package_digest) +
            "\",\"clientIdHex\":\"" + hex_string(response.client_id) +
            "\",\"sessionIdHex\":\"" + hex_string(response.session_id) +
            "\",\"candidateId\":\"" + json_escape(response.candidate_id) +
            "\",\"highestContiguousSequence\":" +
            std::to_string(response.highest_contiguous_sequence) +
            ",\"finalized\":" + (response.finalized ? "true" : "false") +
            ",\"stateHashHex\":\"" + hex_string(response.state_hash) +
            "\",\"lastEventHashHex\":\"" + hex_string(response.last_event_hash) +
            "\",\"chunkIndex\":" + std::to_string(response.chunk_index) +
            ",\"chunkCount\":" + std::to_string(response.chunk_count) +
            ",\"answers\":[";
    for (std::size_t index = 0; index < response.answers.size(); ++index) {
        const auto& answer = response.answers[index];
        if (index != 0) json.push_back(',');
        const std::string bytes(reinterpret_cast<const char*>(answer.answer.data()),
                                 answer.answer.size());
        json += "{\"questionId\":\"" + json_escape(answer.question_id) +
                "\",\"questionRevision\":" +
                std::to_string(answer.question_revision) +
                ",\"sequence\":" + std::to_string(answer.sequence) +
                ",\"kind\":\"" +
                (answer.kind == exam::AnswerKind::clear ? "clear" : "upsert") +
                "\",\"answer\":\"" + json_escape(bytes) +
                "\",\"eventHashHex\":\"" + hex_string(answer.event_hash) +
                "\"}";
    }
    json += "]}}";
    return json;
}

} // namespace

bool validate_exam_user_data_path(
    const std::filesystem::path& allowed_user_data_root,
    const std::filesystem::path& user_data_root,
    std::string* error) {
    if (allowed_user_data_root.empty() ||
        !allowed_user_data_root.is_absolute()) {
        set_error(error, "exam user-data policy root must be absolute");
        return false;
    }
    if (user_data_root.empty() || !user_data_root.is_absolute()) {
        set_error(error, "exam user-data root must be an absolute path");
        return false;
    }
    if (path_has_reparse_point(allowed_user_data_root) ||
        path_has_reparse_point(user_data_root)) {
        set_error(error, "exam user-data path is reparse-backed");
        return false;
    }

    std::error_code fs_error;
    const auto policy_root = std::filesystem::weakly_canonical(
        allowed_user_data_root, fs_error);
    if (fs_error || policy_root.empty() || path_has_reparse_point(policy_root)) {
        set_error(error, "exam user-data policy root is invalid");
        return false;
    }
    const auto user_data =
        std::filesystem::weakly_canonical(user_data_root, fs_error);
    if (fs_error || user_data.empty() || user_data == policy_root ||
        !path_is_within(policy_root, user_data)) {
        set_error(error, "exam user-data root is outside its policy root");
        return false;
    }
    std::error_code status_error;
    const auto status = std::filesystem::status(user_data, status_error);
    if (!status_error && std::filesystem::exists(status) &&
        !std::filesystem::is_directory(status)) {
        set_error(error, "exam user-data root is not a directory");
        return false;
    }
    if (!status_error && path_has_reparse_point(user_data)) {
        set_error(error, "exam user-data root is reparse-backed");
        return false;
    }
    return true;
}

bool validate_exam_path_policy(
    const std::filesystem::path& allowed_data_root,
    const std::filesystem::path& package_root,
    const std::filesystem::path& web_root,
    const std::filesystem::path& user_data_root,
    std::string* error) {
    if (allowed_data_root.empty() || !allowed_data_root.is_absolute()) {
        set_error(error, "NSTU data root must be an absolute path");
        return false;
    }
    if (path_has_reparse_point(allowed_data_root) ||
        path_has_reparse_point(package_root) ||
        (!web_root.empty() && path_has_reparse_point(web_root))) {
        set_error(error, "NSTU exam path is reparse-backed");
        return false;
    }

    std::error_code fs_error;
    const auto data_root =
        std::filesystem::weakly_canonical(allowed_data_root, fs_error);
    if (fs_error || !std::filesystem::is_directory(data_root) ||
        path_has_reparse_point(data_root)) {
        set_error(error, "NSTU data root is unavailable or reparse-backed");
        return false;
    }

    const auto package_area =
        std::filesystem::weakly_canonical(data_root / L"exams", fs_error);
    if (fs_error || !std::filesystem::is_directory(package_area) ||
        path_has_reparse_point(package_area)) {
        set_error(error, "NSTU exam package directory is unavailable");
        return false;
    }

    if (package_root.empty() || !package_root.is_absolute()) {
        set_error(error, "exam package root must be an absolute path");
        return false;
    }
    const auto package =
        std::filesystem::weakly_canonical(package_root, fs_error);
    if (fs_error || !std::filesystem::is_directory(package) ||
        path_has_reparse_point(package) || package == package_area ||
        !path_is_within(package_area, package)) {
        set_error(error, "exam package root is outside the NSTU data root");
        return false;
    }

    const auto effective_web_root =
        web_root.empty() ? package / L"exam" / L"web" : web_root;
    if (!effective_web_root.is_absolute()) {
        set_error(error, "exam web root must be an absolute path");
        return false;
    }
    const auto web =
        std::filesystem::weakly_canonical(effective_web_root, fs_error);
    if (fs_error || !std::filesystem::is_directory(web) ||
        path_has_reparse_point(web) || web == package ||
        !path_is_within(package, web)) {
        set_error(error, "exam web root is outside the package");
        return false;
    }

    if (!user_data_root.empty() &&
        !validate_exam_user_data_path(data_root / L"exam-user-data",
                                      user_data_root, error)) {
        return false;
    }
    return true;
}

bool inspect_exam_package(const std::filesystem::path& package_root,
                          const std::filesystem::path& web_root,
                          std::string_view expected_digest_hex,
                          bool require_digest, ExamPackageReport& report,
                          std::string* error) {
    if (package_root.empty() || !package_root.is_absolute()) {
        set_error(error, "exam package root must be an absolute path");
        return false;
    }
    if (path_has_reparse_point(package_root) ||
        (!web_root.empty() && path_has_reparse_point(web_root))) {
        set_error(error, "exam package path is reparse-backed");
        return false;
    }
    std::error_code fs_error;
    const auto root = std::filesystem::weakly_canonical(package_root, fs_error);
    if (fs_error || !std::filesystem::is_directory(root) ||
        path_has_reparse_point(root)) {
        set_error(error, "exam package root is unavailable or reparse-backed");
        return false;
    }
    const auto effective_web_root = web_root.empty() ? root / L"exam" / L"web"
                                                      : web_root;
    if (!effective_web_root.is_absolute() ||
        !path_is_within(root, effective_web_root)) {
        set_error(error, "exam web root is outside the package");
        return false;
    }
    const auto canonical_web_root =
        std::filesystem::weakly_canonical(effective_web_root, fs_error);
    if (fs_error || canonical_web_root == root ||
        !std::filesystem::is_directory(canonical_web_root) ||
        path_has_reparse_point(canonical_web_root)) {
        set_error(error, "exam web root is outside the package");
        return false;
    }
    const auto manifest_path = root / L"manifest.json";
    const auto page_path = effective_web_root / L"index.html";
    std::vector<std::byte> page_bytes;
    if (!read_file_bounded(page_path, kMaximumManifestBytes, page_bytes, error)) {
        set_error(error, "exam web entry point is unavailable");
        return false;
    }
    std::vector<std::byte> manifest_data;
    if (!read_file_bounded(manifest_path, kMaximumManifestBytes, manifest_data,
                           error)) {
        return false;
    }
    report.manifest_json.assign(reinterpret_cast<const char*>(manifest_data.data()),
                                manifest_data.size());
    if (!validate_manifest_asset_files(root, report.manifest_json, error)) {
        return false;
    }

    struct PackageFile {
        std::string relative;
        std::filesystem::path path;
        std::uintmax_t size = 0;
    };
    std::vector<PackageFile> files;
    std::uintmax_t total_bytes = 0;
    std::filesystem::recursive_directory_iterator iterator(
        root, std::filesystem::directory_options::none, fs_error);
    const std::filesystem::recursive_directory_iterator end;
    for (; !fs_error && iterator != end; iterator.increment(fs_error)) {
        const auto current = iterator->path();
        if (path_has_reparse_point(current)) {
            set_error(error, "exam package contains a reparse point");
            return false;
        }
        if (!iterator->is_regular_file(fs_error)) {
            if (fs_error) break;
            continue;
        }
        const auto relative = std::filesystem::relative(current, root, fs_error);
        if (fs_error || relative.empty() ||
            relative.generic_string().find("..") == 0) {
            set_error(error, "exam package contains an invalid relative path");
            return false;
        }
        const auto size = iterator->file_size(fs_error);
        if (fs_error || size > kMaximumPackageBytes ||
            total_bytes > kMaximumPackageBytes - size ||
            files.size() >= kMaximumPackageFiles) {
            set_error(error, "exam package exceeds its bounded file quota");
            return false;
        }
        total_bytes += size;
        files.push_back({relative.generic_string(), current, size});
    }
    if (fs_error || files.empty()) {
        set_error(error, "exam package enumeration failed");
        return false;
    }
    std::sort(files.begin(), files.end(),
              [](const auto& left, const auto& right) {
                  return left.relative < right.relative;
              });
    std::vector<std::byte> canonical;
    canonical.reserve(static_cast<std::size_t>(total_bytes) + files.size() * 32);
    auto append_u64 = [&canonical](std::uint64_t value) {
        for (unsigned index = 0; index < 8; ++index) {
            canonical.push_back(static_cast<std::byte>(value & 0xffu));
            value >>= 8u;
        }
    };
    for (const auto& file : files) {
        std::vector<std::byte> bytes;
        if (!read_file_bounded(file.path, kMaximumPackageBytes, bytes, error)) {
            return false;
        }
        append_u64(file.relative.size());
        canonical.insert(canonical.end(),
                         reinterpret_cast<const std::byte*>(file.relative.data()),
                         reinterpret_cast<const std::byte*>(file.relative.data()) +
                             file.relative.size());
        append_u64(bytes.size());
        canonical.insert(canonical.end(), bytes.begin(), bytes.end());
    }
    const auto digest = security::sha256(canonical);
    if (!digest) {
        set_error(error, "exam package digest calculation failed");
        return false;
    }
    report.digest_hex = hex_string(*digest);
    if (require_digest && expected_digest_hex.empty()) {
        set_error(error, "exam package digest pin is required");
        return false;
    }
    if (!expected_digest_hex.empty()) {
        security::Sha256Digest expected{};
        if (!parse_hex(expected_digest_hex, expected) ||
            !security::constant_time_equal(expected, *digest)) {
            set_error(error, "exam package digest does not match its pin");
            return false;
        }
    }
    report.package_root = root;
    report.web_root = canonical_web_root;
    report.page_path = std::filesystem::weakly_canonical(page_path);
    return true;
}

struct ExamHost::Impl {
    ExamHost* api = nullptr;
    HWND owner = nullptr;
    HWND window = nullptr;
    ExamHostOptions options;
    ExamPackageReport package;
    ContextIdentity context;
    ExamHostCallbacks callbacks;
    ExamHostState state = ExamHostState::idle;
    HHOOK keyboard_hook = nullptr;
    bool stopping = false;
    std::set<std::string> allowed_package_assets;
    // Incremented whenever an exam host operation is stopped/restarted.  The
    // WebView2 API completes asynchronously, so callbacks from an older
    // operation must never mutate a newly-started host.
    std::uint64_t operation_generation = 0;

#if NSTU_HAS_WEBVIEW2
    HMODULE loader = nullptr;
    Microsoft::WRL::ComPtr<ICoreWebView2Environment> environment;
    Microsoft::WRL::ComPtr<ICoreWebView2Controller> controller;
    Microsoft::WRL::ComPtr<ICoreWebView2> webview;
    Microsoft::WRL::ComPtr<ICoreWebView2NavigationStartingEventHandler>
        navigation_handler;
    Microsoft::WRL::ComPtr<ICoreWebView2NewWindowRequestedEventHandler>
        new_window_handler;
    Microsoft::WRL::ComPtr<ICoreWebView2NavigationCompletedEventHandler>
        navigation_completed_handler;
    Microsoft::WRL::ComPtr<ICoreWebView2WebMessageReceivedEventHandler>
        message_handler;
    Microsoft::WRL::ComPtr<ICoreWebView2ProcessFailedEventHandler>
        process_failed_handler;
    EventRegistrationToken navigation_token{};
    EventRegistrationToken new_window_token{};
    EventRegistrationToken navigation_completed_token{};
    EventRegistrationToken message_token{};
    EventRegistrationToken process_failed_token{};
    Microsoft::WRL::ComPtr<ICoreWebView2WebResourceRequestedEventHandler>
        web_resource_handler;
    EventRegistrationToken web_resource_token{};
    bool web_resource_filter_added = false;
    bool webview_ready = false;
    UINT_PTR initialization_timer_id = 0;
    std::uint64_t initialization_generation = 0;
    ULONGLONG initialization_deadline_ms = 0;
    ULONGLONG last_blocked_resource_notice_ms = 0;
#endif
};

namespace {

ExamHost::Impl* g_keyboard_host = nullptr;

[[maybe_unused]] bool callback_is_current(const ExamHost::Impl& host,
                                          std::uint64_t generation) noexcept {
    return !host.stopping && host.state != ExamHostState::failed &&
           host.operation_generation == generation;
}

void notify_status(ExamHost::Impl& host, std::string message) {
    if (host.callbacks.status) {
        try {
            host.callbacks.status(std::move(message));
        } catch (...) {
            // Status reporting is an observer callback. It must never unwind
            // through a Win32 message loop or a WebView2 COM ABI boundary.
            OutputDebugStringA("NSTU exam status callback threw an exception.\n");
        }
    }
}

void cancel_initialization_watchdog(ExamHost::Impl& host) noexcept {
#if NSTU_HAS_WEBVIEW2
    if (host.window != nullptr && host.initialization_timer_id != 0) {
        (void)KillTimer(host.window, host.initialization_timer_id);
    }
    host.initialization_timer_id = 0;
    host.initialization_generation = 0;
    host.initialization_deadline_ms = 0;
#else
    (void)host;
#endif
}

[[maybe_unused]] bool begin_initialization_watchdog(
    ExamHost::Impl& host, std::uint64_t generation) noexcept {
#if NSTU_HAS_WEBVIEW2
    if (host.window == nullptr || generation == 0) return false;
    cancel_initialization_watchdog(host);
    // Keep the id in a private range and derive it from the operation. A
    // queued WM_TIMER from an older operation can therefore be rejected by
    // both its id and the generation check in the window procedure.
    const auto id = static_cast<UINT_PTR>(
        kInitializationTimerBase | (generation & 0x0fffu));
    if (id == 0 || SetTimer(host.window, id, kInitializationTimerPeriodMs,
                            nullptr) == 0) {
        return false;
    }
    host.initialization_timer_id = id;
    host.initialization_generation = generation;
    host.initialization_deadline_ms = GetTickCount64() +
                                      kInitializationTimeoutMs;
    return true;
#else
    (void)host;
    (void)generation;
    return true;
#endif
}

void set_host_state(ExamHost::Impl& host, ExamHostState state,
                    std::string message = {}) {
    const bool entering_failed = state == ExamHostState::failed &&
                                 host.state != ExamHostState::failed;
    host.state = state;
    if (state == ExamHostState::failed) {
#if NSTU_HAS_WEBVIEW2
        host.webview_ready = false;
#endif
    }
    if (state == ExamHostState::running || state == ExamHostState::failed) {
        cancel_initialization_watchdog(host);
    }
    if (!message.empty()) notify_status(host, std::move(message));
    if (entering_failed && host.window != nullptr && !host.stopping) {
        // The normal agent callback posts a status message to its owner.  A
        // standalone caller may not have an owner, so also queue a host-local
        // cleanup request.  The window procedure only honors this internal
        // form while the host is still failed, preventing stale messages from
        // stopping a later exam operation.
        (void)PostMessageW(host.window, kHostMessageExamStop, 1, 0);
    }
}

LRESULT CALLBACK exam_keyboard_proc(int code, WPARAM wparam, LPARAM lparam) {
    if (code == HC_ACTION && g_keyboard_host != nullptr &&
        (wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN)) {
        const auto* key = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lparam);
        const bool alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0 ||
                         (key->flags & LLKHF_ALTDOWN) != 0;
        const bool control = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        const bool recovery = alt && control && shift && key->vkCode == VK_F12;
        if (recovery) {
            // WM_CLOSE is intentionally ignored by the kiosk window so an
            // accidental close request cannot end an active exam. The
            // technician recovery chord uses the explicit UI stop message.
            PostMessageW(g_keyboard_host->window, kHostMessageExamStop, 0, 0);
            return 1;
        }
        const bool blocked = key->vkCode == VK_LWIN || key->vkCode == VK_RWIN ||
                             (alt && (key->vkCode == VK_TAB ||
                                      key->vkCode == VK_ESCAPE ||
                                      key->vkCode == VK_F4)) ||
                             (control && key->vkCode == VK_ESCAPE);
        if (blocked) return 1;
    }
    return CallNextHookEx(nullptr, code, wparam, lparam);
}

void resize_exam_window(ExamHost::Impl& host) {
    if (host.window == nullptr) return;
    const int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int width = std::max(1, GetSystemMetrics(SM_CXVIRTUALSCREEN));
    const int height = std::max(1, GetSystemMetrics(SM_CYVIRTUALSCREEN));
    SetWindowPos(host.window, HWND_TOPMOST, x, y, width, height,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
#if NSTU_HAS_WEBVIEW2
    if (host.controller != nullptr) {
        RECT bounds{0, 0, width, height};
        (void)host.controller->put_Bounds(bounds);
        (void)host.controller->NotifyParentWindowPositionChanged();
    }
#endif
}

void enable_per_monitor_dpi_awareness() noexcept {
    using SetDpiAwarenessContextFn = DPI_AWARENESS_CONTEXT(WINAPI*) (
        DPI_AWARENESS_CONTEXT);
    const auto user32 = GetModuleHandleW(L"user32.dll");
    if (user32 == nullptr) return;
    const auto set_context =
        load_function<SetDpiAwarenessContextFn>(
            user32, "SetThreadDpiAwarenessContext");
    if (set_context != nullptr) {
        (void)set_context(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    }
}

LRESULT CALLBACK exam_window_proc(HWND window, UINT message, WPARAM wparam,
                                  LPARAM lparam) {
    auto* host = reinterpret_cast<ExamHost::Impl*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        host = static_cast<ExamHost::Impl*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(host));
        if (host != nullptr) host->window = window;
    }
    if (host == nullptr) return DefWindowProcW(window, message, wparam, lparam);
    switch (message) {
    case WM_TIMER:
        if (wparam == kBridgeTimerId) {
            if (host->api != nullptr) {
                host->api->drain_bridge();
                host->api->enforce_foreground();
            }
            return 0;
        }
#if NSTU_HAS_WEBVIEW2
        if (wparam == host->initialization_timer_id) {
            if (host->initialization_generation == host->operation_generation &&
                host->state == ExamHostState::initializing &&
                host->initialization_deadline_ms != 0 &&
                GetTickCount64() >= host->initialization_deadline_ms) {
                set_host_state(
                    *host, ExamHostState::failed,
                    "WebView2 initialization timed out; the exam host was stopped.");
            }
            return 0;
        }
#endif
        break;
    case WM_DISPLAYCHANGE:
    case WM_DPICHANGED:
        resize_exam_window(*host);
        return 0;
    case WM_ACTIVATE:
    case WM_KILLFOCUS:
        if (!host->stopping && host->state == ExamHostState::running) {
            SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            SetForegroundWindow(window);
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wparam & 0xfff0u) == SC_CLOSE ||
            (wparam & 0xfff0u) == SC_MINIMIZE ||
            (wparam & 0xfff0u) == SC_MAXIMIZE) {
            return 0;
        }
        break;
    case WM_CLOSE:
        // Only the authenticated host/service path calls ExamHost::stop().
        return 0;
    case kHostMessageExamStop:
        // wParam==0 is the explicit technician/service/recovery request. The
        // internal failure-cleanup request uses a nonzero marker and is
        // ignored if a new operation has already replaced the failed one.
        if (wparam == 0 || host->state == ExamHostState::failed) {
            if (host->api != nullptr) host->api->stop();
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_NCDESTROY:
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        host->window = nullptr;
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

bool register_exam_window_class(HINSTANCE instance) {
    WNDCLASSEXW klass{};
    klass.cbSize = sizeof(klass);
    klass.hInstance = instance;
    klass.lpfnWndProc = exam_window_proc;
    klass.lpszClassName = kExamWindowClass;
    klass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    klass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    if (RegisterClassExW(&klass) != 0) return true;
    return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

#if NSTU_HAS_WEBVIEW2

using CreateEnvironmentWithOptionsFn = HRESULT(STDAPICALLTYPE*) (
    PCWSTR, PCWSTR, ICoreWebView2EnvironmentOptions*,
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*);
using GetAvailableBrowserVersionFn = HRESULT(STDAPICALLTYPE*) (
    PCWSTR, LPWSTR*);

// MinGW does not emit __uuidof specializations for the SDK's C++ interface
// declarations.  Use the GUID constants published by WebView2.h explicitly
// so the optional host links with both MSVC and MinGW.
template <typename Interface>
const IID& webview_interface_iid() noexcept;

#define NSTU_WEBVIEW_IID(interface_name)                                      \
    template <>                                                               \
    const IID& webview_interface_iid<interface_name>() noexcept {             \
        return IID_##interface_name;                                          \
    }

NSTU_WEBVIEW_IID(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler)
NSTU_WEBVIEW_IID(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler)
NSTU_WEBVIEW_IID(ICoreWebView2NavigationStartingEventHandler)
NSTU_WEBVIEW_IID(ICoreWebView2NewWindowRequestedEventHandler)
NSTU_WEBVIEW_IID(ICoreWebView2WebMessageReceivedEventHandler)
NSTU_WEBVIEW_IID(ICoreWebView2WebResourceRequestedEventHandler)
NSTU_WEBVIEW_IID(ICoreWebView2NavigationCompletedEventHandler)
NSTU_WEBVIEW_IID(ICoreWebView2ProcessFailedEventHandler)
NSTU_WEBVIEW_IID(ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler)

#undef NSTU_WEBVIEW_IID

// The Microsoft WRL package supplies Callback<> on MSVC, but the MinGW
// compatibility headers intentionally expose only ComPtr.  Keep the host
// portable by providing the small COM adapter needed by WebView2 event
// interfaces directly.  The adapter owns its callable through COM reference
// counting and therefore has the same lifetime semantics as WRL::Callback.
template <typename Interface, typename... Arguments>
class WebViewCallback final : public Interface {
public:
    explicit WebViewCallback(std::function<HRESULT(Arguments...)> function)
        : function_(std::move(function)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                              void** object) override {
        if (object == nullptr) return E_POINTER;
        *object = nullptr;
        if (riid == IID_IUnknown || riid == webview_interface_iid<Interface>()) {
            *object = static_cast<Interface*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return references_.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG previous = references_.fetch_sub(
            1, std::memory_order_acq_rel);
        if (previous == 1) {
            delete this;
            return 0;
        }
        return previous - 1;
    }

    HRESULT STDMETHODCALLTYPE Invoke(Arguments... arguments) override {
        if (!function_) return E_UNEXPECTED;
        try {
            return function_(std::forward<Arguments>(arguments)...);
        } catch (...) {
            // Never allow a C++ exception to cross the COM callback ABI. A
            // failing callback is reported to WebView2 as a failed HRESULT;
            // the host's process-failure/status path performs cleanup.
            return E_FAIL;
        }
    }

private:
    std::atomic<ULONG> references_{1};
    std::function<HRESULT(Arguments...)> function_;
};

template <typename Interface, typename... Arguments, typename Callable>
Microsoft::WRL::ComPtr<Interface> make_webview_callback(Callable&& function) {
    std::function<HRESULT(Arguments...)> adapted(
        std::forward<Callable>(function));
    auto* callback = new (std::nothrow)
        WebViewCallback<Interface, Arguments...>(std::move(adapted));
    if (callback == nullptr) return {};
    Microsoft::WRL::ComPtr<Interface> result;
    result.Attach(callback);
    return result;
}

bool relative_is_web_path(const ExamHost::Impl& host,
                          std::string_view relative) {
    std::error_code relative_error;
    const auto web_relative = std::filesystem::relative(
        host.package.web_root, host.package.package_root, relative_error);
    if (relative_error || web_relative.empty() || web_relative == ".") {
        return false;
    }
    auto web_prefix = web_relative.generic_string();
    if (web_prefix.empty() || web_prefix == "." ||
        web_prefix.starts_with("..")) {
        return false;
    }
    if (!web_prefix.ends_with('/')) web_prefix.push_back('/');
    return relative.size() > web_prefix.size() &&
           relative.starts_with(web_prefix);
}

bool allowed_exam_web_uri(const ExamHost::Impl& host,
                          std::wstring_view uri) {
    constexpr std::wstring_view prefix = L"https://nstu.exam/";
    if (uri.size() <= prefix.size() ||
        _wcsnicmp(uri.data(), prefix.data(), prefix.size()) != 0) {
        return false;
    }
    const auto path = uri.substr(prefix.size());
    // Navigation and message origins must be a package web path, not merely
    // an arbitrary file mapped by the virtual host. Query/fragment suffixes
    // are intentionally rejected so the browser and native resource policy
    // observe the same canonical path.
    if (path.find_first_of(L"?#\\") != std::wstring_view::npos) {
        return false;
    }
    std::string relative;
    if (!decode_uri_path(path, relative) || !valid_relative_asset(relative)) {
        return false;
    }
    return relative_is_web_path(host, relative);
}

bool allowed_package_resource(const ExamHost::Impl& host,
                              std::wstring_view uri) {
    constexpr std::wstring_view prefix = L"https://nstu.exam/";
    if (uri.size() <= prefix.size() ||
        _wcsnicmp(uri.data(), prefix.data(), prefix.size()) != 0) {
        return false;
    }
    const auto path = uri.substr(prefix.size());
    // Query strings, fragments, and backslashes are rejected at this boundary.
    // Percent escapes are decoded once below, then validated as ordinary
    // relative paths so encoded traversal and alternate separators cannot
    // bypass the allowlist.
    if (path.find_first_of(L"?#\\") != std::wstring_view::npos) {
        return false;
    }
    std::string relative;
    if (!decode_uri_path(path, relative) ||
        !valid_relative_asset(relative)) {
        return false;
    }
    if (is_private_package_asset(relative)) return false;
    if (relative_is_web_path(host, relative)) return true;
    return host.allowed_package_assets.contains(relative);
}

HRESULT block_web_resource(ExamHost::Impl& host,
                           ICoreWebView2WebResourceRequestedEventArgs* args) {
    if (args == nullptr || host.environment == nullptr) return E_INVALIDARG;
    Microsoft::WRL::ComPtr<ICoreWebView2WebResourceResponse> response;
    constexpr wchar_t headers[] =
        L"Content-Length: 0\r\nCache-Control: no-store\r\n";
    const HRESULT create_result = host.environment->CreateWebResourceResponse(
        nullptr, 403, L"Blocked", headers, &response);
    if (FAILED(create_result) || response == nullptr) return create_result;
    return args->put_Response(response.Get());
}

HRESULT on_web_resource_requested(
    ExamHost::Impl& host,
    ICoreWebView2WebResourceRequestedEventArgs* args) {
    if (args == nullptr) return E_INVALIDARG;
    Microsoft::WRL::ComPtr<ICoreWebView2WebResourceRequest> request;
    if (FAILED(args->get_Request(&request)) || request == nullptr) {
        return block_web_resource(host, args);
    }
    LPWSTR raw_uri = nullptr;
    const HRESULT uri_result = request->get_Uri(&raw_uri);
    const std::wstring uri = raw_uri != nullptr ? raw_uri : L"";
    if (raw_uri != nullptr) CoTaskMemFree(raw_uri);
    if (FAILED(uri_result) || !allowed_package_resource(host, uri)) {
        // A page can issue many rejected subresource requests at once. Keep
        // the diagnostic useful without turning the status queue into a
        // request-amplification channel.
        const auto now = GetTickCount64();
        if (host.last_blocked_resource_notice_ms == 0 ||
            now - host.last_blocked_resource_notice_ms >= 1000) {
            host.last_blocked_resource_notice_ms = now;
            notify_status(host,
                          "Exam network or private-package request blocked.");
        }
        return block_web_resource(host, args);
    }
    return S_OK;
}

void post_webview_json(ExamHost::Impl& host, std::string_view json) {
    if (host.webview == nullptr || json.empty()) return;
    const auto wide = utf8_to_wide(json);
    if (wide.empty()) return;
    (void)host.webview->PostWebMessageAsJson(wide.c_str());
}

HRESULT on_navigation_starting(ExamHost::Impl& host,
                               ICoreWebView2NavigationStartingEventArgs* args) {
    if (args == nullptr) {
        notify_status(host, "WebView2 supplied an invalid navigation event.");
        return E_INVALIDARG;
    }
    LPWSTR raw_uri = nullptr;
    const HRESULT result = args->get_Uri(&raw_uri);
    const std::wstring uri = raw_uri != nullptr ? raw_uri : L"";
    if (raw_uri != nullptr) CoTaskMemFree(raw_uri);
    if (FAILED(result) || !allowed_exam_web_uri(host, uri)) {
        (void)args->put_Cancel(TRUE);
        notify_status(host, "Exam navigation was blocked by the local policy.");
        return S_OK;
    }
    return S_OK;
}

HRESULT on_web_message(ExamHost::Impl& host,
                       ICoreWebView2WebMessageReceivedEventArgs* args) {
    if (args == nullptr) {
        notify_status(host, "WebView2 supplied an invalid message event.");
        return E_INVALIDARG;
    }
    LPWSTR raw_source = nullptr;
    LPWSTR raw_json = nullptr;
    if (FAILED(args->get_Source(&raw_source)) ||
        FAILED(args->get_WebMessageAsJson(&raw_json))) {
        if (raw_source != nullptr) CoTaskMemFree(raw_source);
        if (raw_json != nullptr) CoTaskMemFree(raw_json);
        return S_OK;
    }
    const std::wstring source = raw_source != nullptr ? raw_source : L"";
    const std::wstring json_wide = raw_json != nullptr ? raw_json : L"";
    if (raw_source != nullptr) CoTaskMemFree(raw_source);
    if (raw_json != nullptr) CoTaskMemFree(raw_json);
    if (!allowed_exam_web_uri(host, source)) {
        notify_status(host, "Exam message from an untrusted source was ignored.");
        return S_OK;
    }
    const auto json = wide_to_utf8(json_wide);
    if (json.empty() || json.size() > kMaximumJsonBytes) return S_OK;
    JsonValue root;
    std::string parse_error;
    if (!parse_json_object(json, root, &parse_error)) {
        notify_status(host, "Exam message JSON was rejected.");
        return S_OK;
    }
    std::string type;
    if (!string_member(root, "type", type)) return S_OK;
    if (type == "exam_answer_event") {
        exam::AnswerEvent event;
        if (!parse_answer_event(root, host.context, event, &parse_error)) {
            notify_status(host, "Exam answer event was rejected: " + parse_error);
            return S_OK;
        }
        const auto payload = exam::encode_answer_event(event);
        if (payload.empty()) {
            notify_status(host, "Exam answer event could not be encoded.");
            return S_OK;
        }
        if (host.callbacks.send_to_service) {
            host.callbacks.send_to_service(
                {AgentMessageType::exam_answer_event, payload});
        }
    } else if (type == "exam_state_request") {
        exam::StateRequest request;
        if (!parse_state_request(root, host.context, request, &parse_error)) {
            notify_status(host, "Exam state request was rejected: " + parse_error);
            return S_OK;
        }
        const auto payload = exam::encode_state_request(request);
        if (payload.empty()) return S_OK;
        if (host.callbacks.send_to_service) {
            host.callbacks.send_to_service(
                {AgentMessageType::exam_state_request, payload});
        }
    } else if (type == "exam_ready") {
        post_webview_json(host, "{\"type\":\"exam_host_ready\"}");
    }
    return S_OK;
}

HRESULT on_new_window_requested(
    ExamHost::Impl& host, ICoreWebView2NewWindowRequestedEventArgs* args) {
    if (args == nullptr) {
        notify_status(host, "WebView2 supplied an invalid popup event.");
        return E_INVALIDARG;
    }
    // Exam content is confined to the single authenticated host window.  Do
    // not let target=_blank/window.open create an unmanaged browser surface.
    (void)args->put_Handled(TRUE);
    notify_status(host, "Exam popup was blocked by the local policy.");
    return S_OK;
}

HRESULT on_process_failed(ExamHost::Impl& host,
                          ICoreWebView2ProcessFailedEventArgs* args) {
    (void)args;
    // A browser/render/GPU process failure invalidates the kiosk boundary.
    // Mark the host failed and let the agent UI thread perform orderly
    // shutdown through its status-message path.
    host.webview_ready = false;
    set_host_state(host, ExamHostState::failed,
                   "WebView2 process failure detected; exam host stopped.");
    return S_OK;
}

void configure_webview(ExamHost::Impl& host) {
    Microsoft::WRL::ComPtr<ICoreWebView2Settings> settings;
    if (host.webview != nullptr && SUCCEEDED(host.webview->get_Settings(&settings)) &&
        settings != nullptr) {
        (void)settings->put_IsScriptEnabled(TRUE);
        (void)settings->put_AreDefaultScriptDialogsEnabled(FALSE);
        (void)settings->put_IsStatusBarEnabled(FALSE);
        (void)settings->put_AreDevToolsEnabled(FALSE);
        (void)settings->put_AreDefaultContextMenusEnabled(FALSE);
        (void)settings->put_IsZoomControlEnabled(FALSE);
        (void)settings->put_IsBuiltInErrorPageEnabled(FALSE);
    }
}

void begin_webview(ExamHost::Impl& host, std::uint64_t generation);

bool package_snapshot_is_current(ExamHost::Impl& host) {
    ExamPackageReport refreshed;
    std::string error;
    if (!inspect_exam_package(host.package.package_root,
                              host.package.web_root,
                              host.package.digest_hex,
                              true, refreshed, &error)) {
        notify_status(host,
                      "Exam package changed or failed validation before launch.");
        return false;
    }
    // The digest covers every regular file and relative name in the package.
    // Rechecking immediately before virtual-host mapping closes the common
    // validation-to-navigation gap; deployment staging remains responsible
    // for eliminating races against an untrusted writable package directory.
    if (refreshed.digest_hex != host.package.digest_hex ||
        refreshed.package_root != host.package.package_root ||
        refreshed.web_root != host.package.web_root ||
        refreshed.page_path != host.package.page_path) {
        notify_status(host,
                      "Exam package changed after validation; launch aborted.");
        return false;
    }
    return true;
}

void on_environment_created(ExamHost::Impl& host, std::uint64_t generation,
                            HRESULT error, ICoreWebView2Environment* environment) {
    if (!callback_is_current(host, generation)) return;
    if (FAILED(error) || environment == nullptr || host.window == nullptr) {
        set_host_state(host, ExamHostState::failed,
                       "WebView2 environment initialization failed.");
        return;
    }
    host.environment = environment;
    host.controller = nullptr;
    auto callback = make_webview_callback<
        ICoreWebView2CreateCoreWebView2ControllerCompletedHandler,
        HRESULT, ICoreWebView2Controller*>(
        [&host, generation](HRESULT result,
                            ICoreWebView2Controller* controller) -> HRESULT {
            if (!callback_is_current(host, generation)) return S_OK;
            if (FAILED(result) || controller == nullptr) {
                set_host_state(host, ExamHostState::failed,
                               "WebView2 controller initialization failed.");
                return S_OK;
            }
            host.controller = controller;
            RECT bounds{};
            GetClientRect(host.window, &bounds);
            if (FAILED(host.controller->put_Bounds(bounds))) {
                set_host_state(host, ExamHostState::failed,
                               "WebView2 bounds could not be configured.");
                return S_OK;
            }
            if (FAILED(host.controller->get_CoreWebView2(&host.webview)) ||
                host.webview == nullptr) {
                set_host_state(host, ExamHostState::failed,
                               "WebView2 core object could not be created.");
                return S_OK;
            }
            configure_webview(host);
            if (!package_snapshot_is_current(host)) {
                set_host_state(host, ExamHostState::failed,
                               "Exam package changed during host initialization.");
                return S_OK;
            }
            Microsoft::WRL::ComPtr<ICoreWebView2_3> core3;
            const HRESULT core3_result = host.webview->QueryInterface(
                IID_ICoreWebView2_3,
                reinterpret_cast<void**>(core3.ReleaseAndGetAddressOf()));
            if (FAILED(core3_result) || core3 == nullptr ||
                FAILED(core3->SetVirtualHostNameToFolderMapping(
                    kExamVirtualHost, host.package.package_root.c_str(),
                    COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW))) {
                set_host_state(host, ExamHostState::failed,
                               "WebView2 local package mapping failed.");
                return S_OK;
            }
            host.navigation_handler = make_webview_callback<
                ICoreWebView2NavigationStartingEventHandler,
                ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs*>(
                [&host, generation](ICoreWebView2*,
                                    ICoreWebView2NavigationStartingEventArgs* args)
                    -> HRESULT {
                    if (!callback_is_current(host, generation)) return S_OK;
                    return on_navigation_starting(host, args);
                });
            host.new_window_handler = make_webview_callback<
                ICoreWebView2NewWindowRequestedEventHandler,
                ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs*>(
                [&host, generation](ICoreWebView2*,
                                    ICoreWebView2NewWindowRequestedEventArgs* args)
                    -> HRESULT {
                    if (!callback_is_current(host, generation)) return S_OK;
                    return on_new_window_requested(host, args);
                });
            host.message_handler = make_webview_callback<
                ICoreWebView2WebMessageReceivedEventHandler,
                ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs*>(
                [&host, generation](ICoreWebView2*,
                                    ICoreWebView2WebMessageReceivedEventArgs* args)
                    -> HRESULT {
                    if (!callback_is_current(host, generation)) return S_OK;
                    return on_web_message(host, args);
                });
            host.web_resource_handler = make_webview_callback<
                ICoreWebView2WebResourceRequestedEventHandler,
                ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs*>(
                [&host, generation](ICoreWebView2*,
                                    ICoreWebView2WebResourceRequestedEventArgs*
                                        args) -> HRESULT {
                    if (!callback_is_current(host, generation)) return S_OK;
                    return on_web_resource_requested(host, args);
                });
            if (host.web_resource_handler == nullptr) {
                set_host_state(host, ExamHostState::failed,
                               "WebView2 resource policy handler could not be created.");
                return S_OK;
            }
            host.navigation_completed_handler = make_webview_callback<
                ICoreWebView2NavigationCompletedEventHandler,
                ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*>(
                [&host, generation](ICoreWebView2*,
                                    ICoreWebView2NavigationCompletedEventArgs* args)
                    -> HRESULT {
                    if (!callback_is_current(host, generation)) return S_OK;
                    BOOL success = FALSE;
                    if (args != nullptr) (void)args->get_IsSuccess(&success);
                    if (!success) {
                        host.webview_ready = false;
                        set_host_state(host, ExamHostState::failed,
                                       "Exam page navigation failed.");
                        return S_OK;
                    }
                    host.webview_ready = true;
                    set_host_state(host, ExamHostState::running,
                                   "Exam host is ready.");
                    post_webview_json(host, "{\"type\":\"exam_host_ready\"}");
                    return S_OK;
                });
            host.process_failed_handler = make_webview_callback<
                ICoreWebView2ProcessFailedEventHandler,
                ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs*>(
                [&host, generation](ICoreWebView2*,
                                    ICoreWebView2ProcessFailedEventArgs* args)
                    -> HRESULT {
                    if (!callback_is_current(host, generation)) return S_OK;
                    return on_process_failed(host, args);
                });
            if (FAILED(host.webview->add_NavigationStarting(
                    host.navigation_handler.Get(), &host.navigation_token)) ||
                FAILED(host.webview->add_NewWindowRequested(
                    host.new_window_handler.Get(), &host.new_window_token)) ||
                FAILED(host.webview->add_WebMessageReceived(
                    host.message_handler.Get(), &host.message_token)) ||
                FAILED(host.webview->add_WebResourceRequested(
                    host.web_resource_handler.Get(), &host.web_resource_token)) ||
                FAILED(host.webview->add_NavigationCompleted(
                    host.navigation_completed_handler.Get(),
                    &host.navigation_completed_token)) ||
                FAILED(host.webview->add_ProcessFailed(
                    host.process_failed_handler.Get(),
                    &host.process_failed_token))) {
                set_host_state(host, ExamHostState::failed,
                               "WebView2 event handlers could not be installed.");
                return S_OK;
            }
            if (FAILED(host.webview->AddWebResourceRequestedFilter(
                    L"*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL))) {
                set_host_state(host, ExamHostState::failed,
                               "WebView2 resource policy filter could not be installed.");
                return S_OK;
            }
            host.web_resource_filter_added = true;
            const auto manifest = utf8_to_wide(host.package.manifest_json);
            const auto context = utf8_to_wide(host.options.context_json);
            if (manifest.empty() || context.empty()) {
                set_host_state(host, ExamHostState::failed,
                               "Exam bootstrap data is not valid UTF-8.");
                return S_OK;
            }
            // Bootstrap values are host-authenticated inputs.  Install them as
            // non-configurable, deeply frozen properties so package JavaScript
            // cannot replace the trust anchor before app.js captures it.  JSON
            // parsing also avoids interpreting package keys as object-literal
            // syntax at this native-to-browser boundary.
            const auto manifest_literal = javascript_string_literal(manifest);
            const auto context_literal = javascript_string_literal(context);
            const auto asset_base_literal = javascript_string_literal(
                L"https://nstu.exam/");
            const std::wstring script =
                L"(function(){"
                L"function deepFreeze(value){"
                L"if(value===null||typeof value!==\"object\"||Object.isFrozen(value))return value;"
                L"for(const key of Object.getOwnPropertyNames(value))deepFreeze(value[key]);"
                L"return Object.freeze(value);"
                L"}"
                L"const manifest=deepFreeze(JSON.parse(" + manifest_literal + L"));"
                L"const context=deepFreeze(JSON.parse(" + context_literal + L"));"
                L"const assetBase=" + asset_base_literal + L";"
                L"Object.defineProperty(window,\"NSTU_EXAM_MANIFEST\",{value:manifest,writable:false,configurable:false,enumerable:false});"
                L"Object.defineProperty(window,\"NSTU_EXAM_CONTEXT\",{value:context,writable:false,configurable:false,enumerable:false});"
                L"Object.defineProperty(window,\"NSTU_EXAM_ASSET_BASE\",{value:assetBase,writable:false,configurable:false,enumerable:false});"
                L"})();";
            auto script_callback = make_webview_callback<
                ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler,
                HRESULT, LPCWSTR>(
                [](HRESULT, LPCWSTR) -> HRESULT { return S_OK; });
            if (FAILED(host.webview->AddScriptToExecuteOnDocumentCreated(
                    script.c_str(), script_callback.Get()))) {
                set_host_state(host, ExamHostState::failed,
                               "Exam bootstrap script could not be installed.");
                return S_OK;
            }
            std::error_code relative_error;
            const auto relative = std::filesystem::relative(
                host.package.page_path, host.package.package_root, relative_error);
            if (relative_error || relative.empty()) {
                set_host_state(host, ExamHostState::failed,
                               "Exam page path could not be resolved.");
                return S_OK;
            }
            std::wstring uri = L"https://nstu.exam/" + relative.generic_wstring();
            if (FAILED(host.webview->Navigate(uri.c_str()))) {
                set_host_state(host, ExamHostState::failed,
                               "Exam page navigation could not be started.");
            }
            return S_OK;
        });
    if (FAILED(host.environment->CreateCoreWebView2Controller(
            host.window, callback.Get()))) {
        set_host_state(host, ExamHostState::failed,
                       "WebView2 controller request failed.");
    }
}

void begin_webview(ExamHost::Impl& host, std::uint64_t generation) {
    if (!callback_is_current(host, generation)) return;
    std::filesystem::path user_data = host.options.user_data_root;
    if (user_data.empty()) {
        if (!host.options.allowed_user_data_root.empty()) {
            set_host_state(host, ExamHostState::failed,
                           "WebView2 user-data path is missing.");
            return;
        }
        wchar_t temporary[MAX_PATH]{};
        const DWORD length = GetTempPathW(MAX_PATH, temporary);
        if (length == 0 || length >= MAX_PATH) {
            set_host_state(host, ExamHostState::failed,
                           "WebView2 user-data path could not be resolved.");
            return;
        }
        user_data = std::filesystem::path(temporary) / L"NSTU" / L"exam-webview";
    }
    if (!host.options.allowed_user_data_root.empty()) {
        std::string path_error;
        if (!validate_exam_user_data_path(host.options.allowed_user_data_root,
                                          user_data, &path_error)) {
            set_host_state(host, ExamHostState::failed,
                           path_error.empty()
                               ? "WebView2 user-data path is outside its policy."
                               : path_error);
            return;
        }
    }
    std::error_code directory_error;
    std::filesystem::create_directories(user_data, directory_error);
    if (directory_error ||
        (!host.options.allowed_user_data_root.empty() &&
         !validate_exam_user_data_path(host.options.allowed_user_data_root,
                                       user_data, nullptr))) {
        set_host_state(host, ExamHostState::failed,
                       "WebView2 user-data directory could not be prepared.");
        return;
    }
    const auto executable = std::filesystem::path([&] {
        wchar_t buffer[MAX_PATH]{};
        const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
        return std::wstring(buffer, length);
    }());
    const auto local_loader = executable.parent_path() / L"WebView2Loader.dll";
    // Only load the loader shipped beside the signed NSTU agent. Falling back
    // to an arbitrary system search path would allow an unrelated DLL (or a
    // stale runtime copy) to decide which browser implementation is started.
    const DWORD loader_attributes =
        GetFileAttributesW(local_loader.c_str());
    if (local_loader.empty() || loader_attributes == INVALID_FILE_ATTRIBUTES ||
        (loader_attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
        path_has_reparse_point(local_loader)) {
        set_host_state(host, ExamHostState::failed,
                       "WebView2Loader.dll is missing or reparse-backed beside NSTU.");
        return;
    }
    host.loader = LoadLibraryExW(
        local_loader.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
            LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (host.loader == nullptr) {
        set_host_state(host, ExamHostState::failed,
                       "WebView2Loader.dll is not installed with NSTU.");
        return;
    }
    const auto create = load_function<CreateEnvironmentWithOptionsFn>(
        host.loader, "CreateCoreWebView2EnvironmentWithOptions");
    if (create == nullptr) {
        set_host_state(host, ExamHostState::failed,
                       "WebView2 loader does not expose the required API.");
        return;
    }
    const auto get_browser_version =
        load_function<GetAvailableBrowserVersionFn>(
            host.loader, "GetAvailableCoreWebView2BrowserVersionString");
    if (get_browser_version == nullptr) {
        set_host_state(host, ExamHostState::failed,
                       "WebView2 loader cannot probe the installed runtime.");
        return;
    }
    LPWSTR browser_version_raw = nullptr;
    const HRESULT browser_version_result =
        get_browser_version(nullptr, &browser_version_raw);
    const std::wstring browser_version =
        browser_version_raw != nullptr ? browser_version_raw : L"";
    if (browser_version_raw != nullptr) CoTaskMemFree(browser_version_raw);
    if (FAILED(browser_version_result) || browser_version.empty()) {
        set_host_state(host, ExamHostState::failed,
                       "A compatible WebView2 Runtime is not installed.");
        return;
    }
    notify_status(host, "WebView2 Runtime detected: " +
                           wide_to_utf8(browser_version));
    auto callback = make_webview_callback<
        ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler,
        HRESULT, ICoreWebView2Environment*>(
        [&host, generation](HRESULT result,
                            ICoreWebView2Environment* environment) -> HRESULT {
            if (!callback_is_current(host, generation)) return S_OK;
            on_environment_created(host, generation, result, environment);
            return S_OK;
        });
    if (FAILED(create(nullptr, user_data.c_str(), nullptr, callback.Get()))) {
        set_host_state(host, ExamHostState::failed,
                       "WebView2 environment request failed.");
    }
}

#endif // NSTU_HAS_WEBVIEW2

} // namespace

ExamHost::ExamHost() : impl_(std::make_unique<Impl>()) {
    impl_->api = this;
}

ExamHost::~ExamHost() { stop(); }

bool ExamHost::start(HWND owner, const ExamHostOptions& options,
                     ExamHostCallbacks callbacks, std::string* error) {
    stop();
    if (impl_ == nullptr) impl_ = std::make_unique<Impl>();
    impl_->api = this;
    [[maybe_unused]] const std::uint64_t generation =
        ++impl_->operation_generation;
    impl_->owner = owner;
    impl_->options = options;
    impl_->callbacks = std::move(callbacks);
#if NSTU_HAS_WEBVIEW2
    impl_->webview_ready = false;
    impl_->last_blocked_resource_notice_ms = 0;
#endif
    set_host_state(*impl_, ExamHostState::preparing);
    if (!parse_manifest_and_context(options, impl_->package, impl_->context,
                                    error)) {
        set_host_state(*impl_, ExamHostState::failed,
                       error != nullptr ? *error : "Exam package validation failed.");
        return false;
    }
    impl_->allowed_package_assets.clear();
    if (!collect_manifest_assets(impl_->package.manifest_json,
                                 impl_->allowed_package_assets, error)) {
        set_host_state(*impl_, ExamHostState::failed,
                       error != nullptr ? *error :
                           "Exam package asset policy validation failed.");
        return false;
    }
    set_host_state(*impl_, ExamHostState::initializing);
    enable_per_monitor_dpi_awareness();
    const auto instance = GetModuleHandleW(nullptr);
    if (!register_exam_window_class(instance)) {
        set_host_state(*impl_, ExamHostState::failed,
                       "Exam host window class registration failed.");
        set_error(error, "exam host window class registration failed");
        return false;
    }
    const int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int width = std::max(1, GetSystemMetrics(SM_CXVIRTUALSCREEN));
    const int height = std::max(1, GetSystemMetrics(SM_CYVIRTUALSCREEN));
    impl_->window = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kExamWindowClass, kExamWindowTitle,
        WS_POPUP, x, y, width, height, owner, nullptr, instance, impl_.get());
    if (impl_->window == nullptr) {
        set_host_state(*impl_, ExamHostState::failed,
                       "Exam host window creation failed.");
        set_error(error, "exam host window creation failed");
        return false;
    }
    if (SetTimer(impl_->window, kBridgeTimerId, kBridgeTimerPeriodMs, nullptr) == 0) {
        set_host_state(*impl_, ExamHostState::failed,
                       "Exam host bridge timer could not be installed.");
        set_error(error, "exam host bridge timer could not be installed");
        stop();
        return false;
    }
#if NSTU_HAS_WEBVIEW2
    if (!begin_initialization_watchdog(*impl_, generation)) {
        set_host_state(*impl_, ExamHostState::failed,
                       "WebView2 initialization watchdog could not be installed.");
        set_error(error, "WebView2 initialization watchdog could not be installed");
        stop();
        return false;
    }
#endif
    exam_bridge().clear();
    impl_->keyboard_hook = SetWindowsHookExW(
        WH_KEYBOARD_LL, exam_keyboard_proc, instance, 0);
    if (impl_->keyboard_hook == nullptr) {
        set_host_state(*impl_, ExamHostState::failed,
                       "Exam keyboard hook could not be installed.");
        set_error(error, "exam keyboard hook could not be installed");
        stop();
        return false;
    }
    g_keyboard_host = impl_.get();
    resize_exam_window(*impl_);
    ShowWindow(impl_->window, SW_SHOW);
    SetForegroundWindow(impl_->window);
    SetFocus(impl_->window);
#if NSTU_HAS_WEBVIEW2
    begin_webview(*impl_, generation);
    if (impl_->state == ExamHostState::failed) {
        if (error != nullptr && error->empty()) {
            *error = "WebView2 host initialization failed";
        }
        stop();
        return false;
    }
#else
    set_host_state(*impl_, ExamHostState::failed,
                   "This build does not include the WebView2 host module.");
    notify_status(*impl_, "Install the WebView2-enabled internal build to run exams.");
    set_error(error, "this build does not include the WebView2 host module");
    stop();
    return false;
#endif
    return true;
}

void ExamHost::stop() noexcept {
    if (impl_ == nullptr) return;
    ++impl_->operation_generation;
    impl_->stopping = true;
#if NSTU_HAS_WEBVIEW2
    impl_->webview_ready = false;
    impl_->last_blocked_resource_notice_ms = 0;
    // Kill the watchdog before releasing the browser objects.  DestroyWindow
    // also removes window timers, but doing this explicitly keeps the timer
    // state coherent if teardown is interrupted or the window is already
    // gone.
    cancel_initialization_watchdog(*impl_);
#endif
    if (impl_->keyboard_hook != nullptr) {
        UnhookWindowsHookEx(impl_->keyboard_hook);
        impl_->keyboard_hook = nullptr;
    }
    if (g_keyboard_host == impl_.get()) g_keyboard_host = nullptr;
#if NSTU_HAS_WEBVIEW2
    if (impl_->webview != nullptr) {
        if (impl_->navigation_token.value != 0)
            (void)impl_->webview->remove_NavigationStarting(impl_->navigation_token);
        if (impl_->new_window_token.value != 0)
            (void)impl_->webview->remove_NewWindowRequested(
                impl_->new_window_token);
        if (impl_->navigation_completed_token.value != 0)
            (void)impl_->webview->remove_NavigationCompleted(
                impl_->navigation_completed_token);
        if (impl_->message_token.value != 0)
            (void)impl_->webview->remove_WebMessageReceived(impl_->message_token);
        if (impl_->process_failed_token.value != 0)
            (void)impl_->webview->remove_ProcessFailed(
                impl_->process_failed_token);
        if (impl_->web_resource_token.value != 0)
            (void)impl_->webview->remove_WebResourceRequested(
                impl_->web_resource_token);
        if (impl_->web_resource_filter_added) {
            (void)impl_->webview->RemoveWebResourceRequestedFilter(
                L"*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
        }
    }
    impl_->navigation_token = {};
    impl_->new_window_token = {};
    impl_->navigation_completed_token = {};
    impl_->message_token = {};
    impl_->process_failed_token = {};
    impl_->web_resource_token = {};
    impl_->web_resource_filter_added = false;
    if (impl_->controller != nullptr) (void)impl_->controller->Close();
    impl_->webview.Reset();
    impl_->controller.Reset();
    impl_->environment.Reset();
    impl_->navigation_handler.Reset();
    impl_->new_window_handler.Reset();
    impl_->navigation_completed_handler.Reset();
    impl_->message_handler.Reset();
    impl_->process_failed_handler.Reset();
    impl_->web_resource_handler.Reset();
    if (impl_->loader != nullptr) {
        FreeLibrary(impl_->loader);
        impl_->loader = nullptr;
    }
#endif
    exam_bridge().clear();
    if (impl_->window != nullptr) {
        KillTimer(impl_->window, kBridgeTimerId);
        DestroyWindow(impl_->window);
        impl_->window = nullptr;
    }
    impl_->callbacks = {};
    impl_->state = ExamHostState::idle;
    impl_->stopping = false;
}

void ExamHost::drain_bridge() {
    if (impl_ == nullptr) return;
#if NSTU_HAS_WEBVIEW2
    if (!impl_->webview_ready || impl_->webview == nullptr) return;
    while (const auto message = exam_bridge().try_pop()) {
        if (message->type == AgentMessageType::exam_answer_ack) {
            const auto ack = exam::decode_answer_ack(message->payload);
            if (ack.has_value() && ack->session_id == impl_->context.session_id) {
                post_webview_json(*impl_, make_ack_json(*ack));
            }
        } else if (message->type == AgentMessageType::exam_state_response) {
            const auto response = exam::decode_state_response(message->payload);
            if (response.has_value() &&
                response->package_id == impl_->context.package_id &&
                response->package_digest == impl_->context.package_digest &&
                response->client_id == impl_->context.client_id &&
                response->session_id == impl_->context.session_id &&
                response->candidate_id == impl_->context.candidate_id) {
                post_webview_json(*impl_, make_state_json(*response));
            }
        }
    }
#else
    exam_bridge().clear();
#endif
}

void ExamHost::enforce_foreground() noexcept {
    if (impl_ == nullptr || impl_->window == nullptr || impl_->stopping ||
        impl_->state != ExamHostState::running) return;
    SetWindowPos(impl_->window, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (GetForegroundWindow() != impl_->window) SetForegroundWindow(impl_->window);
}

bool ExamHost::active() const noexcept {
    return impl_ != nullptr && impl_->state == ExamHostState::running;
}

ExamHostState ExamHost::state() const noexcept {
    return impl_ == nullptr ? ExamHostState::idle : impl_->state;
}

HWND ExamHost::window() const noexcept {
    return impl_ == nullptr ? nullptr : impl_->window;
}

bool ExamHost::handle_window_message(UINT message, WPARAM wparam,
                                     LPARAM lparam, LRESULT& result) noexcept {
    if (impl_ == nullptr) return false;
    if (message == kHostMessageExamReady) {
        drain_bridge();
        enforce_foreground();
        result = 0;
        return true;
    }
    if (message == kHostMessageExamStop) {
        stop();
        result = 0;
        return true;
    }
    (void)wparam;
    (void)lparam;
    return false;
}

} // namespace nstu::client
