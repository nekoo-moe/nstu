#include "nstu/client_registry.hpp"

#include <algorithm>
#include <new>
#include <utility>

namespace nstu::server {

void ClientRegistry::upsert(ClientRecord record) {
    std::scoped_lock lock(mutex_);
    const auto existing = clients_.find(record.id);
    if (existing != clients_.end() &&
        (!record.snapshot_jpeg || record.snapshot_jpeg->empty())) {
        record.snapshot_width = existing->second.snapshot_width;
        record.snapshot_height = existing->second.snapshot_height;
        record.snapshot_captured_at_unix_milliseconds =
            existing->second.snapshot_captured_at_unix_milliseconds;
        record.snapshot_generation = existing->second.snapshot_generation;
        record.snapshot_jpeg = existing->second.snapshot_jpeg;
    }
    clients_.insert_or_assign(record.id, std::move(record));
}

bool ClientRegistry::set_status(std::uint64_t id, ClientStatus status) {
    std::scoped_lock lock(mutex_);
    const auto found = clients_.find(id);
    if (found == clients_.end()) {
        return false;
    }
    found->second.status = status;
    return true;
}

bool ClientRegistry::touch(std::uint64_t id) {
    std::scoped_lock lock(mutex_);
    const auto found = clients_.find(id);
    if (found == clients_.end()) {
        return false;
    }
    found->second.last_seen = std::chrono::steady_clock::now();
    if (found->second.status == ClientStatus::connecting ||
        found->second.status == ClientStatus::offline) {
        found->second.status = ClientStatus::online;
    }
    return true;
}

bool ClientRegistry::update_snapshot(
    std::uint64_t id, const control::SnapshotFrame& frame) {
    if (frame.width == 0 || frame.height == 0 ||
        frame.width > control::kMaximumSnapshotWidth ||
        frame.height > control::kMaximumSnapshotHeight ||
        frame.captured_at_unix_milliseconds == 0 || frame.jpeg.empty() ||
        frame.jpeg.size() > control::kMaximumSnapshotJpegBytes) {
        return false;
    }
    std::shared_ptr<const std::vector<std::byte>> jpeg;
    try {
        // Allocate before taking the lock so allocation failure leaves the
        // previously published frame and metadata untouched.
        jpeg = std::make_shared<const std::vector<std::byte>>(frame.jpeg);
    } catch (const std::bad_alloc&) {
        return false;
    }
    std::scoped_lock lock(mutex_);
    const auto found = clients_.find(id);
    if (found == clients_.end()) {
        return false;
    }
    found->second.snapshot_width = frame.width;
    found->second.snapshot_height = frame.height;
    found->second.snapshot_captured_at_unix_milliseconds =
        frame.captured_at_unix_milliseconds;
    // Publish one immutable allocation.  ClientRegistry::snapshot() is called
    // from the render loop, so shared ownership avoids copying up to 60 KiB
    // for every client on every frame.
    found->second.snapshot_jpeg = std::move(jpeg);
    ++found->second.snapshot_generation;
    found->second.last_seen = std::chrono::steady_clock::now();
    return true;
}

bool ClientRegistry::update_health(std::uint64_t id, std::uint32_t latency_ms,
                                   std::uint32_t loss_per_mille,
                                   std::uint64_t finalized_sample_size,
                                   net::VideoDeliveryMode delivery) {
    std::scoped_lock lock(mutex_);
    const auto found = clients_.find(id);
    if (found == clients_.end()) {
        return false;
    }
    found->second.latency_ms = latency_ms;
    found->second.packet_loss_per_mille = loss_per_mille;
    found->second.packet_loss_sample_size = finalized_sample_size;
    found->second.delivery = delivery;
    found->second.last_seen = std::chrono::steady_clock::now();
    constexpr std::uint64_t minimum_sample_size = 200;
    constexpr std::uint32_t degrade_threshold_per_mille = 50;
    constexpr std::uint32_t recover_threshold_per_mille = 20;
    constexpr std::uint8_t bad_windows_required = 3;
    constexpr std::uint8_t good_windows_required = 5;
    if (finalized_sample_size >= minimum_sample_size) {
        if (loss_per_mille >= degrade_threshold_per_mille) {
            found->second.bad_loss_windows = static_cast<std::uint8_t>(
                std::min<unsigned int>(found->second.bad_loss_windows + 1,
                                       bad_windows_required));
            found->second.good_loss_windows = 0;
        } else if (loss_per_mille <= recover_threshold_per_mille) {
            found->second.good_loss_windows = static_cast<std::uint8_t>(
                std::min<unsigned int>(found->second.good_loss_windows + 1,
                                       good_windows_required));
            found->second.bad_loss_windows = 0;
        } else {
            found->second.bad_loss_windows = 0;
            found->second.good_loss_windows = 0;
        }
    }
    if (found->second.status != ClientStatus::locked &&
        found->second.status != ClientStatus::offline) {
        if (found->second.bad_loss_windows >= bad_windows_required) {
            found->second.status = ClientStatus::degraded;
        } else if (found->second.good_loss_windows >= good_windows_required) {
            found->second.status = ClientStatus::online;
        }
    }
    return true;
}

std::size_t ClientRegistry::expire(
    std::chrono::steady_clock::time_point now,
    std::chrono::milliseconds timeout) {
    std::scoped_lock lock(mutex_);
    std::size_t expired = 0;
    for (auto& [id, client] : clients_) {
        (void)id;
        if (client.status != ClientStatus::offline &&
            now - client.last_seen > timeout) {
            client.status = ClientStatus::offline;
            ++expired;
        }
    }
    return expired;
}

std::vector<ClientRecord> ClientRegistry::snapshot() const {
    std::scoped_lock lock(mutex_);
    std::vector<ClientRecord> result;
    result.reserve(clients_.size());
    for (const auto& [id, client] : clients_) {
        (void)id;
        result.push_back(client);
    }
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return left.hostname < right.hostname;
    });
    return result;
}

std::size_t ClientRegistry::size() const {
    std::scoped_lock lock(mutex_);
    return clients_.size();
}

const char* to_string(ClientStatus status) noexcept {
    switch (status) {
    case ClientStatus::connecting:
        return "Connecting";
    case ClientStatus::online:
        return "Online";
    case ClientStatus::degraded:
        return "Degraded";
    case ClientStatus::locked:
        return "Locked";
    case ClientStatus::offline:
        return "Offline";
    }
    return "Unknown";
}

} // namespace nstu::server
