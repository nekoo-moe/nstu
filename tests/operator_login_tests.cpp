#include "nstu/operator_login.hpp"

#include <cassert>
#include <cstddef>
#include <initializer_list>
#include <vector>

namespace {

std::vector<std::byte> bytes(std::initializer_list<int> values) {
    std::vector<std::byte> out;
    for (const int value : values) {
        out.push_back(static_cast<std::byte>(value));
    }
    return out;
}

} // namespace

int main() {
    using nstu::server::OperatorLoginGate;
    const auto stored = bytes({1, 2, 3, 4});

    // A correct hash grants and leaves the full attempt budget.
    {
        OperatorLoginGate gate(stored, 3, 60000);
        const auto decision = gate.verify(bytes({1, 2, 3, 4}), 0);
        assert(decision.granted);
        assert(!decision.locked_out);
        assert(decision.attempts_remaining == 3);
    }

    // Wrong hashes deny and burn down the budget; a correct hash resets it.
    {
        OperatorLoginGate gate(stored, 3, 60000);
        auto decision = gate.verify(bytes({9, 9, 9, 9}), 0);
        assert(!decision.granted && !decision.locked_out);
        assert(decision.attempts_remaining == 2);
        decision = gate.verify(bytes({9, 9, 9, 9}), 10);
        assert(decision.attempts_remaining == 1);
        decision = gate.verify(bytes({1, 2, 3, 4}), 20);  // correct: streak reset
        assert(decision.granted && decision.attempts_remaining == 3);
        decision = gate.verify(bytes({9, 9, 9, 9}), 30);
        assert(decision.attempts_remaining == 2);  // counting restarts from max
    }

    // Max consecutive failures lock out; a correct hash is refused while
    // locked; once the window elapses it works again.
    {
        OperatorLoginGate gate(stored, 2, 60000);
        assert(!gate.verify(bytes({9, 9, 9, 9}), 0).locked_out);
        const auto locked = gate.verify(bytes({8, 8, 8, 8}), 100);
        assert(locked.locked_out && !locked.granted);
        assert(gate.locked_out(100) && gate.locked_out(60099));
        const auto during = gate.verify(bytes({1, 2, 3, 4}), 60099);
        assert(!during.granted && during.locked_out);  // correct, but locked
        assert(!gate.locked_out(60100));                // window elapsed
        assert(gate.verify(bytes({1, 2, 3, 4}), 60100).granted);
    }

    // A length mismatch never matches and still counts as a failure.
    {
        OperatorLoginGate gate(stored, 5, 1000);
        const auto decision = gate.verify(bytes({1, 2, 3}), 0);
        assert(!decision.granted && decision.attempts_remaining == 4);
    }

    // An empty stored hash (no password provisioned) never grants.
    {
        OperatorLoginGate gate(bytes({}), 5, 1000);
        assert(!gate.verify(bytes({}), 0).granted);
        assert(!gate.verify(bytes({1, 2, 3, 4}), 0).granted);
    }

    return 0;
}
