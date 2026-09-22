#include "nstu/client_registry.hpp"

#include <algorithm>
#include <new>
#include <utility>

namespace nstu::server {
namespace {

constexpr bool boot_id_is_set(const control::BootId& id) noexcept {
    return id != control::BootId{};
}

constexpr control::UwfFleetPhase phase_for_configure_outcome(
    control::UwfConfigureOutcome outcome) noexcept {
    switch (outcome) {
    case control::UwfConfigureOutcome::armed:
    case control::UwfConfigureOutcome::reboot_pending:
        return control::UwfFleetPhase::awaiting_restart;
    case control::UwfConfigureOutcome::already_enabled:
        // This report has no current-session probe. A fresh probe must settle
        // whether the already-enabled filter protects this boot.
        return control::UwfFleetPhase::verifying;
    case control::UwfConfigureOutcome::unsupported_edition:
        return control::UwfFleetPhase::unsupported;
    case control::UwfConfigureOutcome::feature_missing:
    case control::UwfConfigureOutcome::probe_unavailable:
    case control::UwfConfigureOutcome::provider_unavailable:
    case control::UwfConfigureOutcome::invalid_data_root:
    case control::UwfConfigureOutcome::readiness_failed:
    case control::UwfConfigureOutcome::checkpoint_required:
    case control::UwfConfigureOutcome::access_denied:
    case control::UwfConfigureOutcome::failed:
    case control::UwfConfigureOutcome::busy:
        return control::UwfFleetPhase::failed;
    }
    return control::UwfFleetPhase::failed;
}

void retire_uwf_proof(UwfFleetState& uwf, std::string detail) {
    uwf.phase = control::UwfFleetPhase::verifying;
    uwf.verified_boot_id = {};
    uwf.probe = {};
    uwf.detail = std::move(detail);
    uwf.updated_at = std::chrono::steady_clock::now();
}

} // namespace

void ClientRegistry::upsert(ClientRecord record) {
    std::scoped_lock lock(mutex_);
    const auto existing = clients_.find(record.id);
    if (existing != clients_.end()) {
        // Managed state and UWF state arrive in separate authenticated reports.
        // Periodic status refreshes must not erase either one. Stale UWF proof
        // is retired at connection boundaries by demote_uwf_verification(), not
        // here, because upsert() also runs for every periodic status report.
        record.frozen = existing->second.frozen;
        record.uwf = existing->second.uwf;
    }
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

bool ClientRegistry::set_frozen(std::uint64_t id, bool frozen) {
    std::scoped_lock lock(mutex_);
    const auto found = clients_.find(id);
    if (found == clients_.end()) {
        return false;
    }
    found->second.frozen = frozen;
    found->second.last_seen = std::chrono::steady_clock::now();
    return true;
}

bool ClientRegistry::set_uwf_report(
    std::uint64_t id, const control::UwfConfigureReport& report) {
    std::scoped_lock lock(mutex_);
    const auto found = clients_.find(id);
    if (found == clients_.end()) {
        return false;
    }

    auto& uwf = found->second.uwf;
    const auto phase = phase_for_configure_outcome(report.outcome);
    const auto now = std::chrono::steady_clock::now();
    uwf.reported = true;
    uwf.phase = phase;
    if (phase == control::UwfFleetPhase::awaiting_restart) {
        uwf.restart_expected = true;
        if (!boot_id_is_set(uwf.configure_boot_id)) {
            uwf.configure_boot_id = uwf.observed_boot_id;
        }
    }
    // Legacy reports carry no probe, so they cannot retain old evidence or
    // produce verified_protected.
    uwf.probe = {};
    uwf.verified_boot_id = {};
    uwf.detail = report.detail;
    uwf.updated_at = now;
    found->second.last_seen = now;
    return true;
}

bool ClientRegistry::begin_uwf_fleet_operation(std::uint64_t id,
                                               std::uint64_t operation_id,
                                               bool restart_requested) {
    if (operation_id == 0) {
        return false;
    }
    std::scoped_lock lock(mutex_);
    const auto found = clients_.find(id);
    if (found == clients_.end()) {
        return false;
    }

    auto& uwf = found->second.uwf;
    const auto now = std::chrono::steady_clock::now();
    uwf.operation_id = operation_id;
    uwf.restart_requested = restart_requested;
    uwf.restart_expected = false;
    uwf.reported = false;
    uwf.phase = control::UwfFleetPhase::requested;
    uwf.configure_boot_id = uwf.observed_boot_id;
    uwf.verified_boot_id = {};
    uwf.probe = {};
    uwf.detail = "reboot-to-restore operation requested";
    uwf.requested_at = now;
    uwf.updated_at = now;
    return true;
}

bool ClientRegistry::set_uwf_fleet_status(
    std::uint64_t id, const control::UwfFleetStatusReport& report) {
    std::scoped_lock lock(mutex_);
    const auto found = clients_.find(id);
    if (found == clients_.end()) {
        return false;
    }

    auto& uwf = found->second.uwf;
    const auto now = std::chrono::steady_clock::now();
    found->second.last_seen = now;
    if (report.operation_id != 0 && uwf.operation_id != 0 &&
        report.operation_id != uwf.operation_id) {
        // Late report from a superseded operation. It is valid protocol input,
        // but it must not overwrite current operation state.
        return true;
    }
    if (report.operation_id != 0) {
        uwf.operation_id = report.operation_id;
    }

    uwf.reported = true;
    uwf.observed_boot_id = report.boot_id;
    uwf.probe = report.probe;
    uwf.detail = report.detail;
    uwf.updated_at = now;

    auto phase = report.phase;
    if (phase == control::UwfFleetPhase::awaiting_restart ||
        phase == control::UwfFleetPhase::restarting) {
        uwf.restart_expected = true;
        if (!boot_id_is_set(uwf.configure_boot_id)) {
            uwf.configure_boot_id = report.boot_id;
        }
    }
    if (phase == control::UwfFleetPhase::verified_protected) {
        if (!control::probe_proves_protection(report.probe)) {
            // Decoder already rejects this combination. Check again where exam
            // authorization reads state rather than relying on that layer.
            phase = control::UwfFleetPhase::failed;
            uwf.detail = "UWF protection was claimed without evidence";
        } else if (uwf.restart_expected &&
                   boot_id_is_set(uwf.configure_boot_id) &&
                   report.boot_id == uwf.configure_boot_id) {
            // An operation which armed UWF for next boot cannot become protected
            // without changing boot identity. Already-protected machines never
            // set restart_expected and may verify immediately on the same boot.
            phase = control::UwfFleetPhase::verifying;
            uwf.detail =
                "UWF protection was claimed without the expected restart";
        }
    }

    uwf.phase = phase;
    uwf.verified_boot_id = phase == control::UwfFleetPhase::verified_protected
                               ? report.boot_id
                               : control::BootId{};
    return true;
}

bool ClientRegistry::demote_uwf_verification(std::uint64_t id) {
    std::scoped_lock lock(mutex_);
    const auto found = clients_.find(id);
    if (found == clients_.end() ||
        found->second.uwf.phase !=
            control::UwfFleetPhase::verified_protected) {
        return false;
    }
    retire_uwf_proof(found->second.uwf,
                     "re-checking UWF protection after reconnect");
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
    // Publish one immutable allocation. ClientRegistry::snapshot() is called
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
            if (client.uwf.phase ==
                control::UwfFleetPhase::verified_protected) {
                retire_uwf_proof(
                    client.uwf,
                    "re-checking UWF protection after the client went offline");
            }
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

const char* to_string(control::UwfFleetPhase phase) noexcept {
    switch (phase) {
    case control::UwfFleetPhase::idle:
        return "Not configured";
    case control::UwfFleetPhase::requested:
        return "Requested";
    case control::UwfFleetPhase::configuring:
        return "Configuring";
    case control::UwfFleetPhase::awaiting_restart:
        return "Awaiting restart";
    case control::UwfFleetPhase::restarting:
        return "Restarting";
    case control::UwfFleetPhase::verifying:
        return "Verifying";
    case control::UwfFleetPhase::verified_protected:
        return "Protected";
    case control::UwfFleetPhase::failed:
        return "Failed";
    case control::UwfFleetPhase::unsupported:
        return "Unsupported";
    }
    return "Unknown";
}

} // namespace nstu::server
