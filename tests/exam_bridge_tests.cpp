#include "nstu/exam_bridge.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

nstu::client::AgentMessage make_message(std::uint8_t marker) {
    return {nstu::client::AgentMessageType::exam_answer_ack,
            {static_cast<std::byte>(marker)}};
}

} // namespace

int main() {
    using namespace nstu::client;

    ExamBridge bridge;
    assert(bridge.size() == 0);
    assert(!bridge.publish({AgentMessageType::chat, {std::byte{1}}}));
    assert(bridge.publish(make_message(1)));
    assert(bridge.size() == 1);
    const auto first = bridge.try_pop();
    assert(first.has_value());
    assert(first->type == AgentMessageType::exam_answer_ack);
    assert(first->payload.size() == 1 &&
           first->payload.front() == std::byte{1});
    assert(!bridge.try_pop().has_value());

    for (std::size_t index = 0; index < kMaximumExamBridgeMessages + 5;
         ++index) {
        assert(bridge.publish(make_message(static_cast<std::uint8_t>(index))));
    }
    assert(bridge.size() == kMaximumExamBridgeMessages);
    const auto oldest_retained = bridge.try_pop();
    assert(oldest_retained.has_value());
    assert(oldest_retained->payload.front() ==
           static_cast<std::byte>(5));
    bridge.clear();
    assert(bridge.size() == 0);
    return 0;
}
