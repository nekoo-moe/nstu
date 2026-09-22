#include "nstu/network.hpp"

#if defined(_WIN32)
#include <winsock2.h>
#include <mswsock.h>
#include <mstcpip.h>
#include <ws2tcpip.h>
#include <windows.h>
#endif

#include <algorithm>
#include <limits>
#include <utility>

namespace nstu::net {
namespace {

void set_error(std::string* error, const char* message) {
    if (error != nullptr) {
        *error = message;
    }
}

} // namespace

TcpSocket::TcpSocket() noexcept = default;

TcpSocket::TcpSocket(std::uintptr_t native_handle) noexcept
    : handle_(native_handle) {}

TcpSocket::~TcpSocket() {
    close();
}

TcpSocket::TcpSocket(TcpSocket&& other) noexcept
    : handle_(std::exchange(other.handle_, kInvalidNativeHandle)) {}

TcpSocket& TcpSocket::operator=(TcpSocket&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = std::exchange(other.handle_, kInvalidNativeHandle);
    }
    return *this;
}

bool TcpSocket::connect(const std::string& address, std::uint16_t port,
                        std::string* error) {
    return connect_with_timeout(address, port, 5000, error);
}

bool TcpSocket::connect_with_timeout(const std::string& address,
                                     std::uint16_t port,
                                     std::uint32_t timeout_ms,
                                     std::string* error) {
#if defined(_WIN32)
    close();
    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_port = htons(port);
    if (InetPtonA(AF_INET, address.c_str(), &endpoint.sin_addr) != 1) {
        set_error(error, "invalid IPv4 address");
        return false;
    }
    const SOCKET socket_handle = WSASocketW(
        AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
    if (socket_handle == INVALID_SOCKET) {
        set_error(error, "TCP socket creation failed");
        return false;
    }
    u_long nonblocking = 1;
    if (ioctlsocket(socket_handle, FIONBIO, &nonblocking) == SOCKET_ERROR) {
        closesocket(socket_handle);
        set_error(error, "TCP nonblocking setup failed");
        return false;
    }
    const int connect_result = ::connect(
        socket_handle, reinterpret_cast<const sockaddr*>(&endpoint),
        sizeof(endpoint));
    if (connect_result == SOCKET_ERROR) {
        const int connect_error = WSAGetLastError();
        if (connect_error != WSAEWOULDBLOCK &&
            connect_error != WSAEINPROGRESS &&
            connect_error != WSAEINVAL) {
            closesocket(socket_handle);
            set_error(error, "TCP connect failed");
            return false;
        }
        fd_set writable;
        fd_set failed;
        FD_ZERO(&writable);
        FD_ZERO(&failed);
        FD_SET(socket_handle, &writable);
        FD_SET(socket_handle, &failed);
        const auto bounded_timeout = std::max<std::uint32_t>(timeout_ms, 1);
        timeval timeout{
            static_cast<long>(bounded_timeout / 1000u),
            static_cast<long>((bounded_timeout % 1000u) * 1000u),
        };
        const int ready = select(0, nullptr, &writable, &failed, &timeout);
        if (ready <= 0) {
            closesocket(socket_handle);
            set_error(error, ready == 0 ? "TCP connect timed out"
                                        : "TCP connect wait failed");
            return false;
        }
        int socket_error = 0;
        int socket_error_bytes = sizeof(socket_error);
        if (getsockopt(socket_handle, SOL_SOCKET, SO_ERROR,
                       reinterpret_cast<char*>(&socket_error),
                       &socket_error_bytes) == SOCKET_ERROR ||
            socket_error != 0 || FD_ISSET(socket_handle, &failed)) {
            closesocket(socket_handle);
            set_error(error, "TCP connect failed");
            return false;
        }
    }
    nonblocking = 0;
    if (ioctlsocket(socket_handle, FIONBIO, &nonblocking) == SOCKET_ERROR) {
        closesocket(socket_handle);
        set_error(error, "TCP blocking mode restore failed");
        return false;
    }
    handle_ = static_cast<std::uintptr_t>(socket_handle);
    return true;
#else
    (void)address;
    (void)port;
    (void)timeout_ms;
    set_error(error, "TCP transport requires Windows");
    return false;
#endif
}

bool TcpSocket::listen(std::uint16_t port, int backlog, std::string* error) {
#if defined(_WIN32)
    close();
    const SOCKET socket_handle = WSASocketW(
        AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
    if (socket_handle == INVALID_SOCKET) {
        set_error(error, "TCP listener creation failed");
        return false;
    }
    const BOOL reuse = TRUE;
    setsockopt(socket_handle, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_port = htons(port);
    endpoint.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(socket_handle, reinterpret_cast<const sockaddr*>(&endpoint),
             sizeof(endpoint)) == SOCKET_ERROR ||
        ::listen(socket_handle, backlog) == SOCKET_ERROR) {
        closesocket(socket_handle);
        set_error(error, "TCP bind/listen failed");
        return false;
    }
    handle_ = static_cast<std::uintptr_t>(socket_handle);
    return true;
#else
    (void)port;
    (void)backlog;
    set_error(error, "TCP transport requires Windows");
    return false;
#endif
}

TcpSocket TcpSocket::accept(std::string* error) const {
#if defined(_WIN32)
    if (!is_open()) {
        set_error(error, "listener is not open");
        return {};
    }
    const SOCKET accepted = ::accept(static_cast<SOCKET>(handle_), nullptr,
                                     nullptr);
    if (accepted == INVALID_SOCKET) {
        set_error(error, "TCP accept failed");
        return {};
    }
    return TcpSocket(static_cast<std::uintptr_t>(accepted));
#else
    set_error(error, "TCP transport requires Windows");
    return {};
#endif
}

bool TcpSocket::enable_keepalive(std::uint32_t idle_ms,
                                 std::uint32_t interval_ms,
                                 std::string* error) const {
#if defined(_WIN32)
    if (!is_open()) {
        set_error(error, "socket is not open");
        return false;
    }
    tcp_keepalive keepalive{TRUE, idle_ms, interval_ms};
    DWORD returned = 0;
    if (WSAIoctl(static_cast<SOCKET>(handle_), SIO_KEEPALIVE_VALS, &keepalive,
                 sizeof(keepalive), nullptr, 0, &returned, nullptr, nullptr) ==
        SOCKET_ERROR) {
        set_error(error, "TCP keepalive configuration failed");
        return false;
    }
    return true;
#else
    (void)idle_ms;
    (void)interval_ms;
    set_error(error, "TCP transport requires Windows");
    return false;
#endif
}

bool TcpSocket::set_io_timeouts(std::uint32_t receive_timeout_ms,
                                std::uint32_t send_timeout_ms,
                                std::string* error) const {
#if defined(_WIN32)
    if (!is_open()) {
        set_error(error, "socket is not open");
        return false;
    }
    const DWORD receive_timeout = receive_timeout_ms;
    const DWORD send_timeout = send_timeout_ms;
    if (setsockopt(static_cast<SOCKET>(handle_), SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&receive_timeout),
                   sizeof(receive_timeout)) == SOCKET_ERROR ||
        setsockopt(static_cast<SOCKET>(handle_), SOL_SOCKET, SO_SNDTIMEO,
                   reinterpret_cast<const char*>(&send_timeout),
                   sizeof(send_timeout)) == SOCKET_ERROR) {
        set_error(error, "socket timeout configuration failed");
        return false;
    }
    return true;
#else
    (void)receive_timeout_ms;
    (void)send_timeout_ms;
    set_error(error, "TCP transport requires Windows");
    return false;
#endif
}

int TcpSocket::send_all(std::span<const std::byte> bytes,
                        std::string* error) const {
#if defined(_WIN32)
    if (!is_open() || bytes.size() >
        static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        set_error(error, "invalid TCP send");
        return -1;
    }
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        const auto remaining = bytes.size() - sent;
        const int result = ::send(
            static_cast<SOCKET>(handle_),
            reinterpret_cast<const char*>(bytes.data() + sent),
            static_cast<int>(remaining), 0);
        if (result <= 0) {
            set_error(error, "TCP send failed");
            return -1;
        }
        sent += static_cast<std::size_t>(result);
    }
    return static_cast<int>(sent);
#else
    (void)bytes;
    set_error(error, "TCP transport requires Windows");
    return -1;
#endif
}

int TcpSocket::receive(std::span<std::byte> bytes, std::string* error) const {
#if defined(_WIN32)
    if (!is_open() || bytes.size() >
        static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        set_error(error, "invalid TCP receive");
        return -1;
    }
    const int result = ::recv(static_cast<SOCKET>(handle_),
                              reinterpret_cast<char*>(bytes.data()),
                              static_cast<int>(bytes.size()), 0);
    if (result == SOCKET_ERROR) {
        set_error(error, "TCP receive failed");
        return -1;
    }
    return result;
#else
    (void)bytes;
    set_error(error, "TCP transport requires Windows");
    return -1;
#endif
}

bool TcpSocket::wait_readable(std::uint32_t timeout_ms,
                              std::string* error) const {
#if defined(_WIN32)
    if (!is_open()) {
        set_error(error, "socket is not open");
        return false;
    }
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(static_cast<SOCKET>(handle_), &read_set);
    timeval timeout{
        static_cast<long>(timeout_ms / 1000u),
        static_cast<long>((timeout_ms % 1000u) * 1000u),
    };
    const int result = select(0, &read_set, nullptr, nullptr, &timeout);
    if (result == SOCKET_ERROR) {
        set_error(error, "socket readiness wait failed");
        return false;
    }
    return result > 0;
#else
    (void)timeout_ms;
    set_error(error, "TCP transport requires Windows");
    return false;
#endif
}

bool TcpSocket::is_open() const noexcept {
    return handle_ != kInvalidNativeHandle;
}

std::uintptr_t TcpSocket::native_handle() const noexcept {
    return handle_;
}

std::uint16_t TcpSocket::local_port(std::string* error) const {
#if defined(_WIN32)
    if (!is_open()) {
        set_error(error, "socket is not open");
        return 0;
    }
    sockaddr_in address{};
    int address_bytes = sizeof(address);
    if (getsockname(static_cast<SOCKET>(handle_),
                    reinterpret_cast<sockaddr*>(&address),
                    &address_bytes) == SOCKET_ERROR) {
        set_error(error, "getsockname failed");
        return 0;
    }
    return ntohs(address.sin_port);
#else
    set_error(error, "TCP transport requires Windows");
    return 0;
#endif
}

void TcpSocket::close() noexcept {
#if defined(_WIN32)
    if (is_open()) {
        closesocket(static_cast<SOCKET>(handle_));
    }
#endif
    handle_ = kInvalidNativeHandle;
}

IocpPort::IocpPort() noexcept = default;

IocpPort::~IocpPort() {
    close();
}

bool IocpPort::create(std::uint32_t concurrency, std::string* error) {
#if defined(_WIN32)
    close();
    const HANDLE port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0,
                                                concurrency);
    if (port == nullptr) {
        set_error(error, "CreateIoCompletionPort failed");
        return false;
    }
    handle_ = reinterpret_cast<std::uintptr_t>(port);
    return true;
#else
    (void)concurrency;
    set_error(error, "IOCP requires Windows");
    return false;
#endif
}

bool IocpPort::associate(const TcpSocket& socket,
                         std::uintptr_t completion_key,
                         std::string* error) const {
#if defined(_WIN32)
    if (!is_open() || !socket.is_open() ||
        CreateIoCompletionPort(
            reinterpret_cast<HANDLE>(socket.native_handle()),
            reinterpret_cast<HANDLE>(handle_), completion_key, 0) == nullptr) {
        set_error(error, "IOCP socket association failed");
        return false;
    }
    return true;
#else
    (void)socket;
    (void)completion_key;
    set_error(error, "IOCP requires Windows");
    return false;
#endif
}

bool IocpPort::wait(Completion& completion, std::uint32_t timeout_ms,
                    std::string* error) const {
#if defined(_WIN32)
    if (!is_open()) {
        set_error(error, "IOCP is not open");
        return false;
    }
    DWORD bytes_transferred = 0;
    ULONG_PTR completion_key = 0;
    OVERLAPPED* overlapped = nullptr;
    const BOOL result = GetQueuedCompletionStatus(
        reinterpret_cast<HANDLE>(handle_), &bytes_transferred,
        &completion_key, &overlapped, timeout_ms);
    completion.bytes_transferred = bytes_transferred;
    completion.completion_key = static_cast<std::uintptr_t>(completion_key);
    completion.overlapped = reinterpret_cast<std::uintptr_t>(overlapped);
    completion.error_code = result != FALSE
                                ? ERROR_SUCCESS
                                : GetLastError();
    if (result == FALSE && overlapped == nullptr) {
        if (completion.error_code == WAIT_TIMEOUT) {
            set_error(error, "IOCP wait timed out");
        } else {
            set_error(error, "GetQueuedCompletionStatus failed");
        }
        return false;
    }
    return true;
#else
    (void)completion;
    (void)timeout_ms;
    set_error(error, "IOCP requires Windows");
    return false;
#endif
}

bool IocpPort::post(const Completion& completion, std::string* error) const {
#if defined(_WIN32)
    if (!is_open() ||
        PostQueuedCompletionStatus(
            reinterpret_cast<HANDLE>(handle_), completion.bytes_transferred,
            static_cast<ULONG_PTR>(completion.completion_key),
            reinterpret_cast<OVERLAPPED*>(completion.overlapped)) == FALSE) {
        set_error(error, "PostQueuedCompletionStatus failed");
        return false;
    }
    return true;
#else
    (void)completion;
    set_error(error, "IOCP requires Windows");
    return false;
#endif
}

bool IocpPort::is_open() const noexcept {
    return handle_ != 0;
}

void IocpPort::close() noexcept {
#if defined(_WIN32)
    if (is_open()) {
        CloseHandle(reinterpret_cast<HANDLE>(handle_));
    }
#endif
    handle_ = 0;
}

DeliveryModeSelector::DeliveryModeSelector(
    std::uint32_t failures_before_fallback,
    std::uint32_t successes_before_recovery)
    : threshold_(std::max(1u, failures_before_fallback)),
      recovery_threshold_(std::max(1u, successes_before_recovery)) {}

void DeliveryModeSelector::record_multicast_probe(bool received) noexcept {
    if (received) {
        failures_ = 0;
        if (mode_ == VideoDeliveryMode::unicast) {
            recovery_successes_ =
                std::min(recovery_successes_ + 1, recovery_threshold_);
            if (recovery_successes_ >= recovery_threshold_) {
                mode_ = VideoDeliveryMode::multicast;
                recovery_successes_ = 0;
            }
        }
        return;
    }
    recovery_successes_ = 0;
    if (failures_ < threshold_) {
        ++failures_;
    }
    if (failures_ >= threshold_) {
        mode_ = VideoDeliveryMode::unicast;
    }
}

void DeliveryModeSelector::reset() noexcept {
    failures_ = 0;
    recovery_successes_ = 0;
    mode_ = VideoDeliveryMode::multicast;
}

VideoDeliveryMode DeliveryModeSelector::mode() const noexcept {
    return mode_;
}

std::uint32_t DeliveryModeSelector::consecutive_failures() const noexcept {
    return failures_;
}

std::uint32_t DeliveryModeSelector::consecutive_recovery_successes() const
    noexcept {
    return recovery_successes_;
}

} // namespace nstu::net
