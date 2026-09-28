#include "nstu/client_registry.hpp"
#include "nstu/snapshot_generation_gate.hpp"
#include "nstu/stream_rearm_watchdog.hpp"

#include <cassert>
#include <chrono>

namespace {

nstu::control::BootId boot_id(unsigned char seed) {
    nstu::control::BootId id{};
    for (std::size_t index = 0; index < id.size(); ++index) {
        id[index] = static_cast<std::byte>(seed + index);
    }
    return id;
}

nstu::control::UwfProtectionProbe protected_probe() {
    nstu::control::UwfProtectionProbe probe;
    probe.probe_succeeded = true;
    probe.filter_current_enabled = true;
    probe.system_volume_current_protected = true;
    return probe;
}

} // namespace

int main() {
    using namespace std::chrono_literals;
    nstu::server::ClientRegistry registry;
    assert(registry.size() == 0);
    assert(registry.snapshot().empty());
    const auto now = std::chrono::steady_clock::now();
    registry.upsert({
        .id = 1,
        .hostname = "LAB-PC-01",
        .address = "192.168.1.101",
        .status = nstu::server::ClientStatus::online,
        .snapshot_jpeg = {},
        .last_seen = now,
    });
    assert(registry.size() == 1);
    assert(registry.set_frozen(1, true));
    assert(registry.snapshot()[0].frozen);
    // Ordinary status upserts must preserve the independently reported
    // managed state.
    registry.upsert({
        .id = 1,
        .hostname = "LAB-PC-01",
        .address = "192.168.1.101",
        .status = nstu::server::ClientStatus::online,
        .snapshot_jpeg = {},
        .last_seen = now,
    });
    assert(registry.snapshot()[0].frozen);
    assert(registry.set_frozen(1, false));
    assert(!registry.snapshot()[0].frozen);
    assert(!registry.set_frozen(99, true));
    const nstu::control::UwfConfigureReport uwf_report{
        .outcome = nstu::control::UwfConfigureOutcome::armed,
        .reboot_required = true,
        .data_exclusion_ready = true,
        .registry_exclusion_ready = true,
        .detail = "UWF is armed",
    };
    assert(registry.set_uwf_report(1, uwf_report));
    auto uwf_state = registry.snapshot()[0].uwf;
    assert(uwf_state.reported);
    assert(uwf_state.phase ==
           nstu::control::UwfFleetPhase::awaiting_restart);
    assert(!uwf_state.proves_current_protection());
    assert(!registry.set_uwf_report(99, uwf_report));

    // A fleet proof is bound to operation and boot identity. A stale operation
    // is ignored, and a claim from the configure boot is refused once this
    // operation has entered a restart-only phase.
    const auto configure_boot = boot_id(0x10);
    nstu::control::UwfFleetStatusReport initial_state;
    initial_state.boot_id = configure_boot;
    initial_state.phase = nstu::control::UwfFleetPhase::idle;
    initial_state.detail = "UWF is disabled";
    assert(registry.set_uwf_fleet_status(1, initial_state));
    assert(registry.begin_uwf_fleet_operation(1, 41, true));
    assert(!registry.begin_uwf_fleet_operation(1, 0, true));
    assert(!registry.begin_uwf_fleet_operation(99, 41, true));

    nstu::control::UwfFleetStatusReport restarting;
    restarting.operation_id = 41;
    restarting.boot_id = configure_boot;
    restarting.phase = nstu::control::UwfFleetPhase::restarting;
    restarting.detail = "restarting";
    assert(registry.set_uwf_fleet_status(1, restarting));

    auto same_boot_claim = restarting;
    same_boot_claim.phase =
        nstu::control::UwfFleetPhase::verified_protected;
    same_boot_claim.probe = protected_probe();
    same_boot_claim.detail = "protected";
    assert(registry.set_uwf_fleet_status(1, same_boot_claim));
    uwf_state = registry.snapshot()[0].uwf;
    assert(uwf_state.phase == nstu::control::UwfFleetPhase::verifying);
    assert(!uwf_state.proves_current_protection());

    auto stale_claim = same_boot_claim;
    stale_claim.operation_id = 40;
    stale_claim.boot_id = boot_id(0x20);
    assert(registry.set_uwf_fleet_status(1, stale_claim));
    uwf_state = registry.snapshot()[0].uwf;
    assert(uwf_state.operation_id == 41);
    assert(uwf_state.observed_boot_id == configure_boot);

    auto verified = same_boot_claim;
    verified.boot_id = boot_id(0x20);
    assert(registry.set_uwf_fleet_status(1, verified));
    uwf_state = registry.snapshot()[0].uwf;
    assert(uwf_state.operation_id == 41);
    assert(uwf_state.proves_current_protection());
    assert(uwf_state.verified_boot_id == verified.boot_id);

    // Periodic status upserts preserve live state. Connection boundaries retire
    // proof and require a fresh probe from the current session.
    registry.upsert({
        .id = 1,
        .hostname = "LAB-PC-01",
        .address = "192.168.1.101",
        .status = nstu::server::ClientStatus::online,
        .snapshot_jpeg = {},
        .last_seen = now,
    });
    assert(registry.snapshot()[0].uwf.proves_current_protection());
    assert(registry.demote_uwf_verification(1));
    uwf_state = registry.snapshot()[0].uwf;
    assert(uwf_state.phase == nstu::control::UwfFleetPhase::verifying);
    assert(!uwf_state.proves_current_protection());
    assert(!registry.demote_uwf_verification(1));

    nstu::control::SnapshotFrame frame;
    frame.width = 320;
    frame.height = 180;
    frame.captured_at_unix_milliseconds = 123;
    frame.jpeg = {std::byte{0xff}, std::byte{0xd8}, std::byte{0xff},
                  std::byte{0xd9}};
    assert(registry.update_snapshot(1, frame));
    const auto first_snapshot = registry.snapshot();
    const auto second_snapshot = registry.snapshot();
    assert(first_snapshot[0].snapshot_jpeg);
    assert(first_snapshot[0].snapshot_jpeg ==
           second_snapshot[0].snapshot_jpeg);
    assert(*first_snapshot[0].snapshot_jpeg == frame.jpeg);

    nstu::server::SnapshotGenerationGate generation_gate;
    assert(!generation_gate.begin(0));
    assert(generation_gate.begin(1));
    assert(!generation_gate.begin(1));
    generation_gate.mark_failed(1);
    assert(generation_gate.failed(1));
    assert(!generation_gate.begin(1));
    assert(generation_gate.begin(2));
    generation_gate.mark_succeeded(2);
    assert(generation_gate.succeeded(2));
    assert(!generation_gate.begin(2));
    // A graphics-resource reset must permit the same published generation to
    // be uploaded again after its previous shader-resource view is discarded.
    generation_gate.reset();
    assert(generation_gate.begin(2));
    generation_gate.mark_succeeded(2);
    assert(generation_gate.succeeded(2));
    assert(generation_gate.begin(3));
    generation_gate.reset();
    assert(generation_gate.begin(3));

    assert(registry.update_health(
        1, 4, 80, 1000, nstu::net::VideoDeliveryMode::unicast));
    assert(registry.update_health(
        1, 4, 80, 1000, nstu::net::VideoDeliveryMode::unicast));
    assert(registry.update_health(
        1, 4, 80, 1000, nstu::net::VideoDeliveryMode::unicast));
    auto clients = registry.snapshot();
    assert(clients.size() == 1);
    assert(clients[0].status == nstu::server::ClientStatus::degraded);
    assert(clients[0].delivery == nstu::net::VideoDeliveryMode::unicast);

    for (int sample = 0; sample < 4; ++sample) {
        assert(registry.update_health(
            1, 4, 10, 1000, nstu::net::VideoDeliveryMode::multicast));
    }
    clients = registry.snapshot();
    assert(clients[0].status == nstu::server::ClientStatus::degraded);
    assert(registry.update_health(
        1, 4, 10, 1000, nstu::net::VideoDeliveryMode::multicast));
    clients = registry.snapshot();
    assert(clients[0].status == nstu::server::ClientStatus::online);

    assert(registry.update_health(
        1, 4, 100, 20, nstu::net::VideoDeliveryMode::multicast));
    assert(registry.update_health(
        1, 4, 100, 20, nstu::net::VideoDeliveryMode::multicast));
    assert(registry.update_health(
        1, 4, 100, 20, nstu::net::VideoDeliveryMode::multicast));
    clients = registry.snapshot();
    assert(clients[0].status == nstu::server::ClientStatus::online);
    assert(registry.expire(now + 10s, 5s) == 1);
    clients = registry.snapshot();
    assert(clients[0].status == nstu::server::ClientStatus::offline);

    // StreamRearmWatchdog: level-triggered self-heal for a stalled live stream.
    // stalled_since resets to the timestamp of the last delivered frame, so the
    // stall window is measured from the most recent generation change. The first
    // re-arm of a contiguous stall episode returns `first`; later ones `repeat`.
    {
        using nstu::server::StreamRearm;
        nstu::server::StreamRearmWatchdog watchdog;
        const auto base = std::chrono::steady_clock::now();
        constexpr auto stall = 2000ms;

        // Wanting no client never re-arms.
        assert(watchdog.update(0, 0, base, stall) == StreamRearm::none);

        // First arm of client 7 seeds the baseline without re-arming on the
        // same tick the edge-triggered path already issued set_streaming.
        assert(watchdog.update(7, 100, base, stall) == StreamRearm::none);
        assert(watchdog.watched_client_id() == 7);

        // Frames flowing (generation advances) hold the watchdog off and move
        // the stall baseline forward to each frame's timestamp.
        assert(watchdog.update(7, 101, base + 1s, stall) == StreamRearm::none);
        assert(watchdog.update(7, 102, base + 2s, stall) ==
               StreamRearm::none);  // last frame at +2s

        // Frames stop (generation frozen at 102). Measured from +2s:
        assert(watchdog.update(7, 102, base + 3s, stall) ==
               StreamRearm::none);  // 1.0s < 2s
        assert(watchdog.update(7, 102, base + 3900ms, stall) ==
               StreamRearm::none);  // 1.9s < 2s
        assert(watchdog.update(7, 102, base + 4s, stall) ==
               StreamRearm::first);  // 2.0s -> first re-arm of episode

        // Subsequent re-arms in the same episode are `repeat` (re-sent quietly),
        // one per full window.
        assert(watchdog.update(7, 102, base + 5s, stall) ==
               StreamRearm::none);  // 1.0s since +4s
        assert(watchdog.update(7, 102, base + 6s, stall) ==
               StreamRearm::repeat);  // 2.0s since +4s

        // A delivered frame ends the episode; the next stall logs `first` again.
        assert(watchdog.update(7, 103, base + 6500ms, stall) ==
               StreamRearm::none);  // frame -> baseline +6.5s
        assert(watchdog.update(7, 103, base + 8s, stall) ==
               StreamRearm::none);  // 1.5s since +6.5s

        // Switching the wanted client re-seeds; no immediate re-arm even if the
        // new client has delivered nothing yet.
        assert(watchdog.update(9, 500, base + 100s, stall) == StreamRearm::none);
        assert(watchdog.watched_client_id() == 9);
        assert(watchdog.update(9, 500, base + 101s, stall) ==
               StreamRearm::none);  // 1s stalled
        assert(watchdog.update(9, 500, base + 102s, stall) ==
               StreamRearm::first);  // 2s stalled -> first re-arm of new episode

        // Dropping to no wanted client disarms; re-selecting seeds fresh.
        assert(watchdog.update(0, 0, base + 103s, stall) == StreamRearm::none);
        assert(watchdog.watched_client_id() == 0);
        assert(watchdog.update(9, 500, base + 103s, stall) ==
               StreamRearm::none);  // seeds, no re-arm
    }
    return 0;
}
