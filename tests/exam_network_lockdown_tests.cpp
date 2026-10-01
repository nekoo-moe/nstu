#include "nstu/exam_network_lockdown.hpp"

#include <cassert>
#include <cstdint>
#include <vector>

int main() {
    using namespace nstu::client;

    nstu::net::WfpAllowlistConfig config;
    config.allowed_ipv4 = {0x7f000001u};  // only the teacher endpoint is allowed

    // Normal path: arm -> apply once (idempotent), disarm -> clear once.
    {
        ExamNetworkLockdown guard(/*max_active*/ 100000, /*stale*/ 10000);
        assert(guard.poll(0) == LockdownAction::none);  // not armed yet
        guard.arm(config, 1000);
        assert(guard.poll(1000) == LockdownAction::apply);
        assert(guard.applied());
        assert(guard.config().allowed_ipv4 == config.allowed_ipv4);
        assert(guard.poll(1500) == LockdownAction::none);  // already applied
        guard.heartbeat(2000);
        assert(guard.poll(2000) == LockdownAction::none);
        guard.disarm();
        assert(guard.poll(2001) == LockdownAction::clear);
        assert(!guard.applied());
        assert(guard.poll(2002) == LockdownAction::none);  // already cleared
    }

    // Safety cap: with no stop, the block still clears once max_active elapses.
    {
        ExamNetworkLockdown guard(5000, 100000);
        guard.arm(config, 0);
        assert(guard.poll(0) == LockdownAction::apply);
        guard.heartbeat(4000);  // alive, but the hard cap is absolute
        assert(guard.poll(4000) == LockdownAction::none);
        assert(guard.poll(5001) == LockdownAction::clear);
    }

    // Watchdog: if heartbeats stop, the block clears even within the cap, and a
    // fresh arm (e.g. a new exam after recovery) blocks again.
    {
        ExamNetworkLockdown guard(100000, 3000);
        guard.arm(config, 0);
        assert(guard.poll(0) == LockdownAction::apply);
        assert(guard.poll(3000) == LockdownAction::none);   // within the window
        assert(guard.poll(3001) == LockdownAction::clear);  // heartbeat is stale
        guard.arm(config, 10000);
        assert(guard.poll(10000) == LockdownAction::apply);
    }

    // A clock that runs backwards must never extend the block.
    {
        ExamNetworkLockdown guard(5000, 5000);
        guard.arm(config, 10000);
        assert(guard.poll(10000) == LockdownAction::apply);
        assert(guard.poll(9000) == LockdownAction::none);  // earlier than start
    }

    return 0;
}
