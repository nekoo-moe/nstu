#include "nstu/screen_snapshot.hpp"

#include "nstu/control_messages.hpp"

#include <windows.h>
#include <wincrypt.h>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace {

std::vector<std::byte> fixture_jpeg() {
    constexpr char encoded[] =
        "/9j/4AAQSkZJRgABAQEAYABgAAD/2wBDAAMCAgMCAgMDAwMEAwMEBQgFBQQE"
        "BQoHBwYIDAoMDAsKCwsNDhIQDQ4RDgsLEBYQERMUFRUVDA8XGBYUGBIUFRT/"
        "2wBDAQMEBAUEBQkFBQkUDQsNFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQU"
        "FBQUFBQUFBQUFBQUFBQUFBQUFBQUFBT/wAARCAACAAIDASIAAhEBAxEB/8QA"
        "HwAAAQUBAQEBAQEAAAAAAAAAAAECAwQFBgcICQoL/8QAtRAAAgEDAwIEAwUFB"
        "AQAAAF9AQIDAAQRBRIhMUEGE1FhByJxFDKBkaEII0KxwRVS0fAkM2JyggkKF"
        "hcYGRolJicoKSo0NTY3ODk6Q0RFRkdISUpTVFVWV1hZWmNkZWZnaGlqc3R1dn"
        "d4eXqDhIWGh4iJipKTlJWWl5iZmqKjpKWmp6ipqrKztLW2t7i5usLDxMXGx8"
        "jJytLT1NXW19jZ2uHi4+Tl5ufo6erx8vP09fb3+Pn6/8QAHwEAAwEBAQEBAQE"
        "BAQAAAAAAAAECAwQFBgcICQoL/8QAtREAAgECBAQDBAcFBAQAAQJ3AAECAxEE"
        "BSExBhJBUQdhcRMiMoEIFEKRobHBCSMzUvAVYnLRChYkNOEl8RcYGRomJygpK"
        "jU2Nzg5OkNERUZHSElKU1RVVldYWVpjZGVmZ2hpanN0dXZ3eHl6goOEhYaHiI"
        "mKkpOUlZaXmJmaoqOkpaanqKmqsrO0tba3uLm6wsPExcbHyMnK0tPU1dbX2Nn"
        "a4uPk5ebn6Onq8vP09fb3+Pn6/9oADAMBAAIRAxEAPwD7V/Z2+C3w91v9n74Z"
        "ajqPgTwzf6heeF9MuLm7utHt5JZ5XtImd3dkJZmJJJJySSTRRRXyOL/3ip/if"
        "5nwmO/3qr/il+bP/9k=";

    DWORD size = 0;
    const DWORD encoded_size = static_cast<DWORD>(sizeof(encoded) - 1);
    assert(CryptStringToBinaryA(encoded, encoded_size, CRYPT_STRING_BASE64,
                                nullptr, &size, nullptr, nullptr));
    std::vector<std::byte> bytes(size);
    assert(CryptStringToBinaryA(
        encoded, encoded_size, CRYPT_STRING_BASE64,
        reinterpret_cast<BYTE*>(bytes.data()), &size, nullptr, nullptr));
    bytes.resize(size);
    return bytes;
}

} // namespace

int main() {
    const auto jpeg = fixture_jpeg();
    nstu::screen::BgraImage decoded;
    std::string error;
    assert(nstu::screen::decode_jpeg(
        std::span<const std::byte>(jpeg), 2, 2, decoded, &error));
    assert(decoded.width == 2);
    assert(decoded.height == 2);
    assert(decoded.stride == 8);
    assert(decoded.pixels.size() == 16);

    assert(!nstu::screen::decode_jpeg(
        std::span<const std::byte>(jpeg), 3, 2, decoded, &error));
    assert(decoded.pixels.empty());

    assert(!nstu::screen::decode_jpeg(
        std::span<const std::byte>(jpeg), 0, 0, decoded, &error));
    assert(decoded.pixels.empty());

    std::vector<std::byte> oversized(
        nstu::control::kMaximumSnapshotJpegBytes + 1, std::byte{0});
    assert(!nstu::screen::decode_jpeg(
        std::span<const std::byte>(oversized), 2, 2, decoded, &error));
    assert(decoded.pixels.empty());

    constexpr std::byte not_jpeg[] = {
        std::byte{0x89}, std::byte{'P'}, std::byte{'N'}, std::byte{'G'}};
    assert(!nstu::screen::decode_jpeg(
        std::span<const std::byte>(not_jpeg), 2, 2, decoded, &error));
    assert(decoded.pixels.empty());

    nstu::screen::JpegImage capture;
    assert(!nstu::screen::capture_primary_screen_jpeg(
        capture,
        static_cast<std::uint16_t>(
            nstu::control::kMaximumSnapshotWidth + 1),
        nstu::control::kMaximumSnapshotHeight, 52,
        nstu::control::kMaximumSnapshotJpegBytes, &error));
    return 0;
}
