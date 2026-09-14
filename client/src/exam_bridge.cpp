#include "nstu/exam_bridge.hpp"

#include <utility>

namespace nstu::client {

bool ExamBridge::publish(AgentMessage message) noexcept {
    if ((message.type != AgentMessageType::exam_answer_ack &&
         message.type != AgentMessageType::exam_state_response) ||
        message.payload.size() > kMaximumAgentPayloadBytes) {
        return false;
    }
    try {
        std::scoped_lock lock(mutex_);
        if (messages_.size() >= kMaximumExamBridgeMessages) {
            messages_.pop_front();
        }
        messages_.push_back(std::move(message));
        return true;
    } catch (...) {
        // The agent must not terminate because a diagnostic/UI bridge ran out
        // of memory. The durable service outbox remains authoritative.
        return false;
    }
}

std::optional<AgentMessage> ExamBridge::try_pop() {
    std::scoped_lock lock(mutex_);
    if (messages_.empty()) {
        return std::nullopt;
    }
    AgentMessage message = std::move(messages_.front());
    messages_.pop_front();
    return message;
}

std::size_t ExamBridge::size() const {
    std::scoped_lock lock(mutex_);
    return messages_.size();
}

void ExamBridge::clear() {
    std::scoped_lock lock(mutex_);
    messages_.clear();
}

ExamBridge& exam_bridge() {
    static ExamBridge bridge;
    return bridge;
}

} // namespace nstu::client
