#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace nstu::screen {

struct JpegImage {
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::vector<std::byte> bytes;
};

struct BgraImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t stride = 0;
    std::vector<std::byte> pixels;
};

[[nodiscard]] bool capture_primary_screen_jpeg(
    JpegImage& image, std::uint16_t maximum_width = 480,
    std::uint16_t maximum_height = 270, std::uint8_t quality = 52,
    std::size_t maximum_bytes = 60u * 1024u,
    std::string* error = nullptr);

[[nodiscard]] bool decode_jpeg(const JpegImage& image, BgraImage& decoded,
                               std::string* error = nullptr);

// Decodes one authenticated snapshot from caller-owned immutable bytes. Input
// and metadata dimensions must satisfy the snapshot transport limits, and the
// metadata dimensions are checked against the decoded JPEG dimensions.
[[nodiscard]] bool decode_jpeg(
    std::span<const std::byte> bytes, std::uint16_t expected_width,
    std::uint16_t expected_height, BgraImage& decoded,
    std::string* error = nullptr);

} // namespace nstu::screen
