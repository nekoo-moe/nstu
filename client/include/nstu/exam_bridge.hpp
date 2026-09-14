#pragma once

#include "nstu/agent_protocol.hpp"
#include "nstu/exam_sync.hpp"

#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>

namespace nstu::client {

// The bridge is deliberately small: exam responses are acknowledgements or
// bounded state chunks, never an unbounded second copy of the answer journal.
inline constexpr std::size_t kMaximumExamBridgeMessages =
    nstu::exam::kMaximumStateChunks;

class ExamBridge {
public:
    ExamBridge() = default;
    ExamBridge(const ExamBridge&) = delete;
    ExamBridge& operator=(const ExamBridge&) = delete;

    // Publishes only service-to-agent exam responses. Older responses are
    // discarded when the host is temporarily busy; durable state remains in
    // the service outbox/server journal and can be requested again.
    [[nodiscard]] bool publish(AgentMessage message) noexcept;
    [[nodiscard]] std::optional<AgentMessage> try_pop();
    [[nodiscard]] std::size_t size() const;
    void clear();

private:
    mutable std::mutex mutex_;
    std::deque<AgentMessage> messages_;
};

// Function-local construction avoids static initialization ordering issues in
// the agent executable and keeps the optional native WebView2 host independent
// from the transport thread's lifetime.
[[nodiscard]] ExamBridge& exam_bridge();

} // namespace nstu::client
