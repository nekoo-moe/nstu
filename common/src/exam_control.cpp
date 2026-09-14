#include "nstu/exam_control.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <type_traits>

namespace nstu::exam {
namespace {

constexpr std::uint32_t kExamStartMagic = 0x31535845u; // "EXS1"
// Version 2 adds the required manifest package ID to the fixed header and
// variable text section. Older payloads are rejected rather than authorized
// without an identity-bound package namespace.
constexpr std::uint16_t kExamStartVersion = 2;
constexpr std::uint16_t kExamStartFlags = 0;
constexpr std::size_t kFixedHeaderBytes =
    sizeof(std::uint32_t) + sizeof(std::uint16_t) * 7 +
    security::kSha256Bytes + security::kClientIdBytes + sizeof(SessionId);

template <typename T>
void append_le(std::vector<std::byte>& output, T value) {
    static_assert(std::is_unsigned_v<T>);
    for (std::size_t index = 0; index < sizeof(T); ++index) {
        output.push_back(static_cast<std::byte>(value & 0xffu));
        value >>= 8u;
    }
}

template <typename T>
bool read_le(std::span<const std::byte> input, std::size_t& offset, T& value) {
    static_assert(std::is_unsigned_v<T>);
    if (offset > input.size() || input.size() - offset < sizeof(T)) {
        return false;
    }
    value = 0;
    for (std::size_t index = 0; index < sizeof(T); ++index) {
        value |= static_cast<T>(std::to_integer<unsigned int>(input[offset++]))
                 << (index * 8u);
    }
    return true;
}

bool non_zero(std::span<const std::byte> value) noexcept {
    return !value.empty() && std::any_of(
        value.begin(), value.end(), [](std::byte byte) {
            return byte != std::byte{0};
        });
}

bool valid_utf8(std::string_view value, std::size_t maximum) noexcept {
    if (value.empty() || value.size() > maximum ||
        std::find(value.begin(), value.end(), '\0') != value.end()) {
        return false;
    }
    for (std::size_t index = 0; index < value.size();) {
        const auto lead = static_cast<unsigned char>(value[index]);
        std::size_t width = 0;
        std::uint32_t codepoint = 0;
        if (lead <= 0x7fu) {
            if (lead < 0x20u || lead == 0x7fu) return false;
            width = 1;
            codepoint = lead;
        } else if (lead >= 0xc2u && lead <= 0xdfu) {
            width = 2;
            codepoint = lead & 0x1fu;
        } else if (lead >= 0xe0u && lead <= 0xefu) {
            width = 3;
            codepoint = lead & 0x0fu;
        } else if (lead >= 0xf0u && lead <= 0xf4u) {
            width = 4;
            codepoint = lead & 0x07u;
        } else {
            return false;
        }
        if (index + width > value.size()) return false;
        for (std::size_t continuation = 1; continuation < width;
             ++continuation) {
            const auto byte = static_cast<unsigned char>(
                value[index + continuation]);
            if ((byte & 0xc0u) != 0x80u) return false;
            codepoint = (codepoint << 6u) | (byte & 0x3fu);
        }
        if ((width == 3 && codepoint < 0x800u) ||
            (width == 4 && codepoint < 0x10000u) ||
            codepoint > 0x10ffffu ||
            (codepoint >= 0xd800u && codepoint <= 0xdfffu)) {
            return false;
        }
        index += width;
    }
    return true;
}

void append_text(std::vector<std::byte>& output, std::string_view value) {
    output.insert(output.end(),
                  reinterpret_cast<const std::byte*>(value.data()),
                  reinterpret_cast<const std::byte*>(value.data()) +
                      value.size());
}

bool read_text(std::span<const std::byte> input, std::size_t& offset,
               std::uint16_t length, std::size_t maximum,
               std::string& output) {
    if (length == 0 || length > maximum || offset > input.size() ||
        input.size() - offset < length) {
        return false;
    }
    output.assign(reinterpret_cast<const char*>(input.data() + offset), length);
    offset += length;
    return valid_utf8(output, maximum);
}

} // namespace

bool validate_exam_start_request(const ExamStartRequest& request) noexcept {
    return valid_utf8(request.package_root, kMaximumExamControlPathBytes) &&
           (request.web_root.empty() ||
            valid_utf8(request.web_root, kMaximumExamControlPathBytes)) &&
           (request.user_data_root.empty() ||
            valid_utf8(request.user_data_root, kMaximumExamControlPathBytes)) &&
           valid_utf8(request.package_id,
                      kMaximumExamControlPackageIdBytes) &&
           valid_utf8(request.candidate_id,
                      kMaximumExamControlCandidateBytes) &&
           non_zero(request.package_digest) && non_zero(request.client_id) &&
           non_zero(request.session_id);
}

std::vector<std::byte> encode_exam_start_request(
    const ExamStartRequest& request) {
    if (!validate_exam_start_request(request) ||
        request.package_root.size() > UINT16_MAX ||
        request.web_root.size() > UINT16_MAX ||
        request.user_data_root.size() > UINT16_MAX ||
        request.package_id.size() > UINT16_MAX ||
        request.candidate_id.size() > UINT16_MAX) {
        return {};
    }
    const auto total = kFixedHeaderBytes + request.package_root.size() +
                       request.web_root.size() + request.user_data_root.size() +
                       request.package_id.size() +
                       request.candidate_id.size();
    if (total > kMaximumExamControlPayloadBytes) return {};

    std::vector<std::byte> output;
    output.reserve(total);
    append_le(output, kExamStartMagic);
    append_le(output, kExamStartVersion);
    append_le(output, kExamStartFlags);
    append_le(output, static_cast<std::uint16_t>(request.package_root.size()));
    append_le(output, static_cast<std::uint16_t>(request.web_root.size()));
    append_le(output,
              static_cast<std::uint16_t>(request.user_data_root.size()));
    append_le(output, static_cast<std::uint16_t>(request.package_id.size()));
    append_le(output, static_cast<std::uint16_t>(request.candidate_id.size()));
    output.insert(output.end(), request.package_digest.begin(),
                  request.package_digest.end());
    output.insert(output.end(), request.client_id.begin(), request.client_id.end());
    output.insert(output.end(), request.session_id.begin(), request.session_id.end());
    append_text(output, request.package_root);
    if (!request.web_root.empty()) append_text(output, request.web_root);
    if (!request.user_data_root.empty()) append_text(output, request.user_data_root);
    append_text(output, request.package_id);
    append_text(output, request.candidate_id);
    return output;
}

std::optional<ExamStartRequest> decode_exam_start_request(
    std::span<const std::byte> payload) {
    if (payload.size() < kFixedHeaderBytes ||
        payload.size() > kMaximumExamControlPayloadBytes) {
        return std::nullopt;
    }
    std::size_t offset = 0;
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    std::uint16_t flags = 0;
    std::uint16_t package_bytes = 0;
    std::uint16_t web_bytes = 0;
    std::uint16_t user_data_bytes = 0;
    std::uint16_t package_id_bytes = 0;
    std::uint16_t candidate_bytes = 0;
    ExamStartRequest request;
    if (!read_le(payload, offset, magic) || magic != kExamStartMagic ||
        !read_le(payload, offset, version) || version != kExamStartVersion ||
        !read_le(payload, offset, flags) || flags != kExamStartFlags ||
        !read_le(payload, offset, package_bytes) ||
        !read_le(payload, offset, web_bytes) ||
        !read_le(payload, offset, user_data_bytes) ||
        !read_le(payload, offset, package_id_bytes) ||
        !read_le(payload, offset, candidate_bytes) ||
        offset + request.package_digest.size() > payload.size()) {
        return std::nullopt;
    }
    std::copy_n(payload.begin() + static_cast<std::ptrdiff_t>(offset),
                request.package_digest.size(), request.package_digest.begin());
    offset += request.package_digest.size();
    if (offset + request.client_id.size() > payload.size()) return std::nullopt;
    std::copy_n(payload.begin() + static_cast<std::ptrdiff_t>(offset),
                request.client_id.size(), request.client_id.begin());
    offset += request.client_id.size();
    if (offset + request.session_id.size() > payload.size()) return std::nullopt;
    std::copy_n(payload.begin() + static_cast<std::ptrdiff_t>(offset),
                request.session_id.size(), request.session_id.begin());
    offset += request.session_id.size();
    if (!read_text(payload, offset, package_bytes,
                   kMaximumExamControlPathBytes, request.package_root)) {
        return std::nullopt;
    }
    if (web_bytes != 0 &&
        !read_text(payload, offset, web_bytes, kMaximumExamControlPathBytes,
                   request.web_root)) {
        return std::nullopt;
    }
    if (user_data_bytes != 0 &&
        !read_text(payload, offset, user_data_bytes,
                   kMaximumExamControlPathBytes, request.user_data_root)) {
        return std::nullopt;
    }
    if (!read_text(payload, offset, package_id_bytes,
                   kMaximumExamControlPackageIdBytes, request.package_id)) {
        return std::nullopt;
    }
    if (!read_text(payload, offset, candidate_bytes,
                   kMaximumExamControlCandidateBytes, request.candidate_id) ||
        offset != payload.size() || !validate_exam_start_request(request)) {
        return std::nullopt;
    }
    return request;
}

} // namespace nstu::exam
