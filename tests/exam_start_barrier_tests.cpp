#include "nstu/exam_start_barrier.hpp"

#include <algorithm>
#include <cassert>
#include <vector>

namespace {

using Barrier = nstu::server::ExamStartBarrier<int>;

bool contains(const std::vector<int>& values, int value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

} // namespace

int main() {
    // All ready before the grace period -> begin at once, releasing everyone.
    {
        Barrier barrier({1, 2, 3}, /*quorum*/ 2, /*grace*/ 1000, /*start*/ 0);
        assert(barrier.poll(0).empty());
        assert(barrier.mark_ready(1));
        assert(barrier.poll(10).empty());  // not all ready, grace not elapsed
        assert(barrier.mark_ready(2));
        assert(barrier.mark_ready(3));
        const auto begun = barrier.poll(30);
        assert(barrier.begun());
        assert(begun.size() == 3);
        assert(contains(begun, 1) && contains(begun, 2) && contains(begun, 3));
        assert(barrier.poll(40).empty());  // releasing is idempotent
    }

    // Quorum + timeout: a stuck third machine does not hold up the room, and it
    // joins late the moment it finally reports.
    {
        Barrier barrier({1, 2, 3}, 2, 1000, 0);
        assert(barrier.mark_ready(1));
        assert(barrier.mark_ready(2));
        assert(barrier.poll(999).empty());  // quorum met but grace not elapsed
        const auto begun = barrier.poll(1000);
        assert(barrier.begun());
        assert(begun.size() == 2);
        assert(contains(begun, 1) && contains(begun, 2) && !contains(begun, 3));
        assert(barrier.mark_ready(3));
        const auto late = barrier.poll(1500);
        assert(late.size() == 1 && contains(late, 3));
        assert(barrier.poll(1600).empty());
    }

    // Below quorum after the grace period -> keep waiting, never begin.
    {
        Barrier barrier({1, 2, 3, 4}, 3, 500, 0);
        assert(barrier.mark_ready(1));
        assert(barrier.mark_ready(2));
        assert(barrier.poll(10000).empty());  // only 2 ready, quorum is 3
        assert(!barrier.begun());
    }

    // Unknown and duplicate reports change nothing.
    {
        Barrier barrier({1, 2}, 2, 0, 0);
        assert(!barrier.mark_ready(9));  // not in the cohort
        assert(barrier.mark_ready(1));
        assert(!barrier.mark_ready(1));  // duplicate
        assert(barrier.poll(0).empty());  // 1 of 2 ready, quorum 2
        assert(barrier.mark_ready(2));
        assert(barrier.poll(0).size() == 2);
    }

    // Quorum clamps: 0 becomes 1, and a value above the cohort size becomes the
    // size (i.e. wait for everyone).
    {
        Barrier zero({1, 2}, 0, 0, 0);  // quorum clamped up to 1
        assert(zero.mark_ready(1));
        assert(zero.poll(0).size() == 1);  // 1 >= quorum 1, grace 0 elapsed
        Barrier big({1, 2, 3}, 99, 0, 0);  // quorum clamped down to 3
        assert(big.mark_ready(1));
        assert(big.mark_ready(2));
        assert(big.poll(100).empty());  // 2 of 3, must wait for all
        assert(big.mark_ready(3));
        assert(big.poll(100).size() == 3);
    }

    return 0;
}
