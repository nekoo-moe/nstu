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

// Reboot-to-restore state for one client.
//
// This replaces the flat uwf_* fields that used to sit directly on
// ClientRecord.  Those recorded what a configuration *attempt* reported, and
// that report is produced before the restart, so at best it says UWF was
// armed.  The exam gate asks a stronger question - is this machine protected
// in the session it is running right now - and only a fresh read-only probe,
// carried on a known boot identity, can answer it.
struct UwfFleetState {
    control::UwfFleetPhase phase = control::UwfFleetPhase::idle;
    // Server-issued and non-zero once an operation has been sent.  Binds every
    // later status report to one operator action, so a late report from a
    // superseded operation cannot be mistaken for the current one.
    std::uint64_t operation_id = 0;
    bool restart_requested = false;
    // Set once this operation has been seen in a phase that only a restart can
    // leave.  The consistency check that the boot identity must have changed
    // applies then and only then: a machine that was already protected when
    // the operation arrived verifies immediately, on the same boot, and that
    // is correct rather than suspicious.
    bool restart_expected = false;
    // True once the client has reported anything at all about UWF.
    bool reported = false;
    // The boot the client was running when the request was issued.
    control::BootId configure_boot_id{};
    // The boot the client says it is running now.
    control::BootId observed_boot_id{};
    // The boot whose probe proved current-session protection.  The proof is
    // bound to that boot and stops counting the moment the client reports
    // running another one.
    control::BootId verified_boot_id{};
    control::UwfProtectionProbe probe;
    std::string detail;
    std::chrono::steady_clock::time_point requested_at{};
    std::chrono::steady_clock::time_point updated_at{};

    // The only predicate an exam may be started on.  Every clause earns its
    // place: the phase is what the client claimed, the probe is the evidence
    // offered for it, and the boot comparison is what stops that evidence
    // outliving the session that produced it.
    [[nodiscard]] bool proves_current_protection() const noexcept {
        return phase == control::UwfFleetPhase::verified_protected &&
               control::probe_proves_protection(probe) &&
               verified_boot_id != control::BootId{} &&
               verified_boot_id == observed_boot_id;
    }
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
    // Last state reported by the client after it applied (or failed to
    // apply) the authenticated managed-mode command.
    bool frozen = false;
    // Reboot-to-restore state.  Distinct from managed mode above: this is disk
    // protection, that is the service and uninstall guard.
    UwfFleetState uwf;
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
    [[nodiscard]] bool set_frozen(std::uint64_t id, bool frozen);
    // Single-client reboot-to-restore report.  Describes an attempt, never a
    // proof: this call has no path to the verified phase.
    [[nodiscard]] bool set_uwf_report(
        std::uint64_t id, const control::UwfConfigureReport& report);
    // Records that a fleet operation has been issued, and pins the boot
    // identity it was issued against.
    [[nodiscard]] bool begin_uwf_fleet_operation(std::uint64_t id,
                                                 std::uint64_t operation_id,
                                                 bool restart_requested);
    // Applies a client fleet status report.  Returns false only when the
    // client is unknown; a report belonging to a superseded operation is
    // dropped without disturbing the current one.
    [[nodiscard]] bool set_uwf_fleet_status(
        std::uint64_t id, const control::UwfFleetStatusReport& report);
    // Retires any proof of current-session protection, returning whether one
    // was retired.  Called whenever the connection that carried the proof is
    // established, replaced or lost, so a proof can never outlive its session.
    bool demote_uwf_verification(std::uint64_t id);
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
[[nodiscard]] const char* to_string(control::UwfFleetPhase phase) noexcept;

} // namespace nstu::server
