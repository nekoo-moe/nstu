#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace nstu::net {

inline constexpr std::uintptr_t kInvalidNativeHandle =
    static_cast<std::uintptr_t>(-1);

class TcpSocket {
public:
    TcpSocket() noexcept;
    explicit TcpSocket(std::uintptr_t native_handle) noexcept;
    ~TcpSocket();
    TcpSocket(TcpSocket&& other) noexcept;
    TcpSocket& operator=(TcpSocket&& other) noexcept;
    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;

    [[nodiscard]] bool connect(const std::string& address, std::uint16_t port,
                               std::string* error = nullptr);
    [[nodiscard]] bool connect_with_timeout(
        const std::string& address, std::uint16_t port,
        std::uint32_t timeout_ms, std::string* error = nullptr);
    [[nodiscard]] bool listen(std::uint16_t port, int backlog = 64,
                              std::string* error = nullptr);
    [[nodiscard]] TcpSocket accept(std::string* error = nullptr) const;
    [[nodiscard]] bool enable_keepalive(std::uint32_t idle_ms,
                                        std::uint32_t interval_ms,
                                        std::string* error = nullptr) const;
    [[nodiscard]] bool set_io_timeouts(std::uint32_t receive_timeout_ms,
                                       std::uint32_t send_timeout_ms,
                                       std::string* error = nullptr) const;
    [[nodiscard]] int send_all(std::span<const std::byte> bytes,
                               std::string* error = nullptr) const;
    [[nodiscard]] int receive(std::span<std::byte> bytes,
                              std::string* error = nullptr) const;
    [[nodiscard]] bool wait_readable(std::uint32_t timeout_ms,
                                     std::string* error = nullptr) const;
    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] std::uintptr_t native_handle() const noexcept;
    [[nodiscard]] std::uint16_t local_port(
        std::string* error = nullptr) const;
    void close() noexcept;

private:
    std::uintptr_t handle_ = kInvalidNativeHandle;
};

class IocpPort {
public:
    struct Completion {
        std::uint32_t bytes_transferred = 0;
        std::uintptr_t completion_key = 0;
        std::uintptr_t overlapped = 0;
        std::uint32_t error_code = 0;
    };

    IocpPort() noexcept;
    ~IocpPort();
    IocpPort(const IocpPort&) = delete;
    IocpPort& operator=(const IocpPort&) = delete;

    [[nodiscard]] bool create(std::uint32_t concurrency = 0,
                              std::string* error = nullptr);
    [[nodiscard]] bool associate(const TcpSocket& socket,
                                 std::uintptr_t completion_key,
                                 std::string* error = nullptr) const;
    [[nodiscard]] bool wait(Completion& completion,
                            std::uint32_t timeout_ms = 0xffffffffu,
                            std::string* error = nullptr) const;
    [[nodiscard]] bool post(const Completion& completion,
                            std::string* error = nullptr) const;
    [[nodiscard]] bool is_open() const noexcept;
    void close() noexcept;

private:
    std::uintptr_t handle_ = 0;
};

enum class VideoDeliveryMode : std::uint8_t {
    multicast,
    unicast,
};

class DeliveryModeSelector {
public:
    explicit DeliveryModeSelector(std::uint32_t failures_before_fallback = 3,
                                  std::uint32_t successes_before_recovery = 5);

    void record_multicast_probe(bool received) noexcept;
    void reset() noexcept;
    [[nodiscard]] VideoDeliveryMode mode() const noexcept;
    [[nodiscard]] std::uint32_t consecutive_failures() const noexcept;
    [[nodiscard]] std::uint32_t consecutive_recovery_successes() const noexcept;

private:
    std::uint32_t threshold_ = 3;
    std::uint32_t recovery_threshold_ = 5;
    std::uint32_t failures_ = 0;
    std::uint32_t recovery_successes_ = 0;
    VideoDeliveryMode mode_ = VideoDeliveryMode::multicast;
};

} // namespace nstu::net
