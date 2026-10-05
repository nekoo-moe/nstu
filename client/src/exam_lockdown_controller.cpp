#include "nstu/exam_lockdown_controller.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstring>

namespace nstu::client {

namespace {

void set_controller_error(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
}

bool is_zero_session(const exam::SessionId& session_id) noexcept {
    return std::all_of(session_id.begin(), session_id.end(),
                       [](std::byte b) { return b == std::byte{0}; });
}

bool starts_with_case_insensitive(std::string_view text,
                                  std::string_view prefix) noexcept {
    if (text.size() < prefix.size()) return false;
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(text[i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i]))) {
            return false;
        }
    }
    return true;
}

} // namespace

bool parse_origin_hostname(std::string_view origin, std::string& hostname) {
    hostname.clear();
    if (origin.empty() || origin.size() > 253) {
        return false;
    }
    // Must be printable ASCII without whitespace
    for (const char c : origin) {
        const auto uc = static_cast<unsigned char>(c);
        if (uc <= 0x20 || uc >= 0x7f) {
            return false;
        }
    }
    // No userinfo permitted
    if (origin.find('@') != std::string_view::npos) {
        return false;
    }

    std::string_view host_part = origin;
    if (starts_with_case_insensitive(host_part, "https://")) {
        host_part = host_part.substr(8);
    } else if (starts_with_case_insensitive(host_part, "http://")) {
        host_part = host_part.substr(7);
    } else if (host_part.size() >= 2 && host_part[0] == '/' && host_part[1] == '/') {
        host_part = host_part.substr(2);
    } else if (host_part.find("://") != std::string_view::npos) {
        return false;
    }

    // Strip path, query, fragment
    const auto sep = host_part.find_first_of("/?#");
    if (sep != std::string_view::npos) {
        host_part = host_part.substr(0, sep);
    }

    // Strip port if present
    const auto colon = host_part.rfind(':');
    if (colon != std::string_view::npos) {
        const auto port_str = host_part.substr(colon + 1);
        if (port_str.empty() || port_str.size() > 5) {
            return false;
        }
        for (const char d : port_str) {
            if (d < '0' || d > '9') {
                return false;
            }
        }
        unsigned long port = 0;
        const auto [ptr, ec] = std::from_chars(
            port_str.data(), port_str.data() + port_str.size(), port);
        if (ec != std::errc{} || ptr != port_str.data() + port_str.size() ||
            port < 1 || port > 65535) {
            return false;
        }
        host_part = host_part.substr(0, colon);
    }

    if (host_part.empty() || host_part.size() > 253) {
        return false;
    }
    if (host_part.front() == '.' || host_part.back() == '.') {
        return false;
    }

    // Validate DNS labels
    std::size_t start = 0;
    while (start < host_part.size()) {
        const auto dot = host_part.find('.', start);
        const auto label = (dot == std::string_view::npos)
                               ? host_part.substr(start)
                               : host_part.substr(start, dot - start);
        if (label.empty() || label.size() > 63) {
            return false;
        }
        if (label.front() == '-' || label.back() == '-') {
            return false;
        }
        for (const char c : label) {
            const bool valid = (c >= 'a' && c <= 'z') ||
                               (c >= 'A' && c <= 'Z') ||
                               (c >= '0' && c <= '9') ||
                               (c == '-');
            if (!valid) {
                return false;
            }
        }
        if (dot == std::string_view::npos) {
            break;
        }
        start = dot + 1;
    }

    hostname = std::string(host_part);
    return true;
}

bool is_disallowed_ipv4(std::uint32_t ip) noexcept {
    if (ip == 0) return true; // 0.0.0.0
    const auto first_octet = (ip >> 24) & 0xffu;
    if (first_octet == 127) return true; // loopback 127.0.0.0/8
    if ((ip & 0xffff0000u) == 0xa9fe0000u) return true; // link-local 169.254.0.0/16
    if (first_octet >= 224) return true; // multicast, reserved, broadcast
    return false;
}

std::vector<std::uint32_t> resolve_allowed_origins(
    std::span<const std::string> origins,
    OriginResolveFn resolve_fn) {
    if (origins.size() > 32) {
        // Over-count rejected
        return {};
    }

    std::vector<std::uint32_t> resolved;
    for (const auto& origin : origins) {
        std::string hostname;
        if (!parse_origin_hostname(origin, hostname)) {
            // Malformed entry rejected; keep partial results
            continue;
        }

        if (resolve_fn) {
            const auto ips = resolve_fn(hostname);
            for (const auto ip : ips) {
                if (!is_disallowed_ipv4(ip)) {
                    resolved.push_back(ip);
                }
            }
        } else {
            ADDRINFOA hints{};
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_STREAM;
            PADDRINFOA result = nullptr;
            if (getaddrinfo(hostname.c_str(), nullptr, &hints, &result) == 0 &&
                result != nullptr) {
                for (auto* ptr = result; ptr != nullptr; ptr = ptr->ai_next) {
                    if (ptr->ai_family == AF_INET && ptr->ai_addr != nullptr) {
                        const auto* addr =
                            reinterpret_cast<const sockaddr_in*>(ptr->ai_addr);
                        const std::uint32_t ip = ntohl(addr->sin_addr.s_addr);
                        if (!is_disallowed_ipv4(ip)) {
                            resolved.push_back(ip);
                        }
                    }
                }
                freeaddrinfo(result);
            }
        }
    }

    if (resolved.empty()) {
        return {};
    }

    std::sort(resolved.begin(), resolved.end());
    resolved.erase(std::unique(resolved.begin(), resolved.end()), resolved.end());
    return resolved;
}

ExamLockdownController::ExamLockdownController(ApplyFn apply_fn,
                                               ClearFn clear_fn)
    : apply_fn_(std::move(apply_fn)), clear_fn_(std::move(clear_fn)) {}

std::uint64_t ExamLockdownController::begin_attempt() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return ++current_epoch_;
}

bool ExamLockdownController::arm(
    std::uint64_t token,
    const ExamLockdownArmParams& params,
    const security::ClientId& configured_client_id,
    std::uint64_t now_ms,
    std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (token != current_epoch_) {
        set_controller_error(error, "stale or invalid arm token");
        return false;
    }
    if (is_zero_session(params.session_id)) {
        set_controller_error(error, "session_id is zero");
        return false;
    }
    if (params.client_id != configured_client_id) {
        set_controller_error(
            error, "client_id does not match configured service identity");
        return false;
    }
    if (!params.package_verified) {
        set_controller_error(error, "package has not been verified");
        return false;
    }
    if (params.allowed_ipv4.empty()) {
        set_controller_error(error, "allowed IPv4 set is empty");
        return false;
    }

    duration_seconds_ = params.duration_seconds;
    std::uint64_t max_active_ms =
        (static_cast<std::uint64_t>(duration_seconds_) + 30ULL * 60ULL) * 1000ULL;
    if (max_active_ms > kMaxActiveCapMs) {
        max_active_ms = kMaxActiveCapMs;
    }

    net::WfpAllowlistConfig config;
    config.allowed_ipv4 = params.allowed_ipv4;
    config.block_http = true;
    config.block_https = true;

    lockdown_ = ExamNetworkLockdown(max_active_ms, kDefaultStaleAfterMs);
    lockdown_.arm(config, now_ms);

    armed_ = true;
    start_ms_ = now_ms;
    last_heartbeat_ms_ = now_ms;
    apply_retries_ = 0;
    last_apply_failed_ = false;

    return true;
}

void ExamLockdownController::update_duration(
    std::uint32_t duration_seconds) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!armed_ || duration_seconds == 0) {
        return;
    }
    duration_seconds_ = duration_seconds;
    std::uint64_t max_active_ms =
        (static_cast<std::uint64_t>(duration_seconds_) + 30ULL * 60ULL) * 1000ULL;
    if (max_active_ms > kMaxActiveCapMs) {
        max_active_ms = kMaxActiveCapMs;
    }
    const auto config = lockdown_.config();
    lockdown_ = ExamNetworkLockdown(max_active_ms, kDefaultStaleAfterMs);
    lockdown_.arm(config, start_ms_);
    if (last_heartbeat_ms_ > start_ms_) {
        lockdown_.heartbeat(last_heartbeat_ms_);
    }
}

void ExamLockdownController::heartbeat(std::uint64_t now_ms) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!armed_) {
        return;
    }
    last_heartbeat_ms_ = now_ms;
    lockdown_.heartbeat(now_ms);
}

void ExamLockdownController::disarm() noexcept {
    bool call_clear = false;
    ClearFn clear_fn;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++current_epoch_;
        armed_ = false;
        lockdown_.disarm();
        if (actually_blocked_) {
            call_clear = true;
            clear_fn = clear_fn_;
        }
    }
    if (call_clear && clear_fn) {
        std::string err;
        if (clear_fn(&err)) {
            std::lock_guard<std::mutex> lock(mutex_);
            actually_blocked_ = false;
            apply_retries_ = 0;
        }
    }
}

LockdownAction ExamLockdownController::tick(std::uint64_t now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    (void)lockdown_.poll(now_ms);

    if (lockdown_.applied() && !actually_blocked_) {
        if (!last_apply_failed_ && apply_retries_ < kMaxApplyRetries) {
            ++apply_retries_;
            std::string err;
            if (apply_fn_ && apply_fn_(lockdown_.config(), &err)) {
                actually_blocked_ = true;
                return LockdownAction::apply;
            } else {
                last_error_ = std::move(err);
                if (apply_retries_ >= kMaxApplyRetries) {
                    last_apply_failed_ = true;
                }
            }
        }
        return LockdownAction::none;
    }

    if (!lockdown_.applied() && actually_blocked_) {
        std::string err;
        if (clear_fn_ && clear_fn_(&err)) {
            actually_blocked_ = false;
            apply_retries_ = 0;
            return LockdownAction::clear;
        } else {
            last_error_ = std::move(err);
            return LockdownAction::none;
        }
    }

    return LockdownAction::none;
}

bool ExamLockdownController::startup_sweep(std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (clear_fn_) {
        return clear_fn_(error);
    }
    return true;
}

bool ExamLockdownController::is_armed() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return armed_;
}

bool ExamLockdownController::is_actually_blocked() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return actually_blocked_;
}

std::uint64_t ExamLockdownController::current_epoch() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_epoch_;
}

unsigned int ExamLockdownController::apply_retries() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return apply_retries_;
}

bool ExamLockdownController::last_apply_failed() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_apply_failed_;
}

} // namespace nstu::client
