#include "nstu/client_registry.hpp"
#include "nstu/snapshot_generation_gate.hpp"

#include <cassert>
#include <chrono>

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
    return 0;
}
