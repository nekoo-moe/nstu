#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace nstu::client {

inline constexpr wchar_t kControlPipeName[] = L"\\\\.\\pipe\\nstu-control-v1";

class NamedPipe {
public:
    NamedPipe() noexcept;
    ~NamedPipe();
    NamedPipe(NamedPipe&& other) noexcept;
    NamedPipe& operator=(NamedPipe&& other) noexcept;
    NamedPipe(const NamedPipe&) = delete;
    NamedPipe& operator=(const NamedPipe&) = delete;

    [[nodiscard]] bool create_server(std::wstring_view name,
                                     std::string* error = nullptr);
    [[nodiscard]] bool wait_for_client(std::string* error = nullptr) const;
    [[nodiscard]] bool validate_client_process(
        std::wstring_view expected_image_path,
        std::uint32_t* session_id = nullptr,
        std::string* error = nullptr) const;
    [[nodiscard]] bool connect_client(std::wstring_view name,
                                      std::uint32_t timeout_ms,
                                      std::string* error = nullptr);
    [[nodiscard]] int write(std::span<const std::byte> bytes,
                            std::string* error = nullptr) const;
    [[nodiscard]] int read(std::span<std::byte> bytes,
                           std::string* error = nullptr) const;
    [[nodiscard]] bool available_bytes(std::uint32_t& bytes,
                                       std::string* error = nullptr) const;
    [[nodiscard]] bool is_open() const noexcept;
    void close() noexcept;

private:
    std::uintptr_t handle_ = 0;
};

} // namespace nstu::client
