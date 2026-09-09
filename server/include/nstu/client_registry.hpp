#pragma once

#include "nstu/network.hpp"
#include "nstu/control_messages.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace nstu::server {

enum class ClientStatus : std::uint8_t {
    connecting,
    online,
    degraded,
    locked,
    offline,
};

struct ClientRecord {
    std::uint64_t id = 0;
    std::string hostname;
    std::string address;
    ClientStatus status = ClientStatus::connecting;
    net::VideoDeliveryMode delivery = net::VideoDeliveryMode::multicast;
    std::uint32_t latency_ms = 0;
    std::uint32_t packet_loss_per_mille = 0;
    std::uint64_t packet_loss_sample_size = 0;
    bool streaming = false;
    bool snapshotting = false;
    bool viewing_broadcast = false;
    std::uint8_t frames_per_second = 0;
    std::uint16_t snapshot_interval_seconds = 0;
    std::uint16_t snapshot_width = 0;
    std::uint16_t snapshot_height = 0;
    std::uint64_t snapshot_captured_at_unix_milliseconds = 0;
    std::uint64_t snapshot_generation = 0;
    // Immutable after publication so registry snapshots can share the JPEG
    // storage instead of copying it once per UI frame.
    std::shared_ptr<const std::vector<std::byte>> snapshot_jpeg;
    std::uint8_t bad_loss_windows = 0;
    std::uint8_t good_loss_windows = 0;
    std::chrono::steady_clock::time_point last_seen{};
};

class ClientRegistry {
public:
    void upsert(ClientRecord record);
    [[nodiscard]] bool set_status(std::uint64_t id, ClientStatus status);
    [[nodiscard]] bool touch(std::uint64_t id);
    [[nodiscard]] bool update_snapshot(
        std::uint64_t id, const control::SnapshotFrame& frame);
    [[nodiscard]] bool update_health(std::uint64_t id, std::uint32_t latency_ms,
                                     std::uint32_t loss_per_mille,
                                     std::uint64_t finalized_sample_size,
                                     net::VideoDeliveryMode delivery);
    std::size_t expire(std::chrono::steady_clock::time_point now,
                       std::chrono::milliseconds timeout);
    [[nodiscard]] std::vector<ClientRecord> snapshot() const;
    [[nodiscard]] std::size_t size() const;

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::uint64_t, ClientRecord> clients_;
};

[[nodiscard]] const char* to_string(ClientStatus status) noexcept;

} // namespace nstu::server
