#include "nstu/exam_sync.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <set>
#include <string_view>
#include <type_traits>

namespace nstu::exam {
namespace {

inline constexpr std::uint32_t kEventMagic = 0x3156454eu; // "NEV1"
inline constexpr std::uint32_t kAckMagic = 0x314b414eu; // "NAK1"
inline constexpr std::uint32_t kRequestMagic = 0x3151524eu; // "NRQ1"
inline constexpr std::uint32_t kResponseMagic = 0x3152534eu; // "NSR1"
inline constexpr std::uint16_t kWireVersion = 1;
inline constexpr std::uint16_t kStateResponseWireVersion = 3;
inline constexpr std::uint16_t kStateResponsePreviousWireVersion = 2;
inline constexpr std::size_t kMaximumWireBytes = kMaximumExamPayloadBytes;
inline constexpr std::size_t kAnswerEventFixedBytes =
    sizeof(std::uint32_t) + sizeof(std::uint16_t) + sizeof(std::uint8_t) * 2 +
    sizeof(std::uint64_t) * 2 + sizeof(std::uint32_t) +
    sizeof(std::uint16_t) * 3 + sizeof(std::uint32_t) +
    security::kSha256Bytes + security::kClientIdBytes +
    sizeof(SessionId) + security::kSha256Bytes;
inline constexpr std::size_t kAnswerAckBytes =
    sizeof(std::uint32_t) + sizeof(std::uint16_t) + sizeof(std::uint8_t) * 2 +
    sizeof(SessionId) + sizeof(std::uint64_t) * 3 +
    security::kSha256Bytes * 2;
inline constexpr std::size_t kStateRequestFixedBytes =
    sizeof(std::uint32_t) + sizeof(std::uint16_t) + sizeof(std::uint16_t) * 2 +
    security::kSha256Bytes + security::kClientIdBytes + sizeof(SessionId);
inline constexpr std::size_t kStateResponseV1FixedBytes =
    sizeof(std::uint32_t) + sizeof(std::uint16_t) + sizeof(std::uint8_t) * 2 +
    sizeof(std::uint64_t) + sizeof(std::uint16_t) * 3 +
    security::kSha256Bytes + security::kClientIdBytes + sizeof(SessionId) +
    security::kSha256Bytes;
inline constexpr std::size_t kStateResponseFixedBytes =
    kStateResponseV1FixedBytes + sizeof(std::uint16_t) * 2;
inline constexpr std::size_t kStateResponseV3FixedBytes =
    kStateResponseFixedBytes + security::kSha256Bytes;

void set_error(std::string* error, const char* message) {
    if (error != nullptr) {
        *error = message;
    }
}

template <typename T>
void append_le(std::vector<std::byte>& output, T value) {
    static_assert(std::is_unsigned_v<T>);
    const auto offset = output.size();
    output.resize(offset + sizeof(T));
    for (std::size_t index = 0; index < sizeof(T); ++index) {
        output[offset + index] = static_cast<std::byte>(value & 0xffu);
        value >>= 8u;
    }
}

template <typename T>
bool read_le(std::span<const std::byte> input, std::size_t& offset,
             T& value) {
    static_assert(std::is_unsigned_v<T>);
    if (offset > input.size() || sizeof(T) > input.size() - offset) {
        return false;
    }
    value = 0;
    for (std::size_t index = 0; index < sizeof(T); ++index) {
        value |= static_cast<T>(std::to_integer<unsigned int>(input[offset++]))
                 << (index * 8u);
    }
    return true;
}

void append_bytes(std::vector<std::byte>& output,
                  std::span<const std::byte> bytes) {
    output.insert(output.end(), bytes.begin(), bytes.end());
}

bool read_bytes(std::span<const std::byte> input, std::size_t& offset,
                std::span<std::byte> output) {
    if (offset > input.size() || output.size() > input.size() - offset) {
        return false;
    }
    std::copy_n(input.begin() + static_cast<std::ptrdiff_t>(offset),
                output.size(), output.begin());
    offset += output.size();
    return true;
}

bool all_zero(std::span<const std::byte> bytes) noexcept {
    return std::all_of(bytes.begin(), bytes.end(),
                       [](std::byte value) { return value == std::byte{0}; });
}

bool valid_text(std::string_view value, std::size_t maximum,
                bool allow_empty = false) noexcept {
    if ((!allow_empty && value.empty()) || value.size() > maximum) {
        return false;
    }
    return std::none_of(value.begin(), value.end(),
                        [](char character) { return character == '\0'; });
}

bool valid_kind(AnswerKind kind) noexcept {
    return kind == AnswerKind::upsert || kind == AnswerKind::clear ||
           kind == AnswerKind::finalize;
}

bool valid_ack_status(AnswerAckStatus status) noexcept {
    const auto value = static_cast<std::uint8_t>(status);
    return value >= static_cast<std::uint8_t>(AnswerAckStatus::accepted) &&
           value <= static_cast<std::uint8_t>(AnswerAckStatus::unavailable);
}

bool valid_ack_shape(const AnswerAck& ack) noexcept {
    if (!valid_ack_status(ack.status) || all_zero(ack.session_id) ||
        ack.server_time_unix_milliseconds == 0) {
        return false;
    }
    switch (ack.status) {
    case AnswerAckStatus::accepted:
    case AnswerAckStatus::duplicate:
        return ack.sequence != 0 &&
               ack.highest_contiguous_sequence >= ack.sequence &&
               !all_zero(ack.event_hash) && !all_zero(ack.state_hash);
    case AnswerAckStatus::gap:
        return ack.sequence != 0 &&
               ack.highest_contiguous_sequence < ack.sequence;
    case AnswerAckStatus::conflict:
        return ack.sequence != 0;
    case AnswerAckStatus::rejected:
    case AnswerAckStatus::unavailable:
        return true;
    }
    return false;
}

bool valid_answer_state(const AnswerState& answer) noexcept {
    return valid_text(answer.question_id, kMaximumQuestionIdBytes) &&
           (answer.kind == AnswerKind::upsert ||
            answer.kind == AnswerKind::clear) &&
           answer.sequence != 0 && answer.answer.size() <= kMaximumAnswerBytes &&
           answer.question_revision != 0 && !all_zero(answer.event_hash) &&
           (answer.kind != AnswerKind::clear || answer.answer.empty());
}

template <typename Array>
void append_array(std::vector<std::byte>& output, const Array& value) {
    append_bytes(output, value);
}

template <typename Array>
bool read_array(std::span<const std::byte> input, std::size_t& offset,
                Array& value) {
    return read_bytes(input, offset, value);
}

bool read_string(std::span<const std::byte> input, std::size_t& offset,
                 std::uint16_t length, std::size_t maximum,
                 std::string& output, bool allow_empty = false) {
    if (length > maximum || offset > input.size() ||
        static_cast<std::size_t>(length) > input.size() - offset) {
        return false;
    }
    output.assign(reinterpret_cast<const char*>(input.data() + offset), length);
    offset += length;
    return valid_text(output, maximum, allow_empty);
}

bool valid_identity(const security::ClientId& client_id,
                    const SessionId& session_id) noexcept {
    return !all_zero(client_id) && !all_zero(session_id);
}

} // namespace

bool validate_answer_event(const AnswerEvent& event, std::string* error) {
    if (!valid_text(event.package_id, kMaximumPackageIdBytes) ||
        all_zero(event.package_digest) || !valid_identity(event.client_id,
                                                          event.session_id) ||
        !valid_text(event.candidate_id, kMaximumCandidateIdBytes) ||
        !valid_kind(event.kind) || event.question_revision == 0 ||
        event.sequence == 0 || event.client_time_unix_milliseconds == 0 ||
        event.answer.size() > kMaximumAnswerBytes ||
        (event.kind != AnswerKind::finalize &&
         !valid_text(event.question_id, kMaximumQuestionIdBytes)) ||
        (event.kind == AnswerKind::finalize &&
         (!event.question_id.empty() || !event.answer.empty())) ||
        (event.kind == AnswerKind::clear && !event.answer.empty()) ||
        (event.sequence == 1 && !all_zero(event.previous_event_hash))) {
        set_error(error, "invalid exam answer event");
        return false;
    }
    return true;
}

std::vector<std::byte> encode_answer_event(const AnswerEvent& event) {
    if (!validate_answer_event(event, nullptr) ||
        event.answer.size() > std::numeric_limits<std::uint32_t>::max()) {
        return {};
    }
    std::vector<std::byte> wire;
    wire.reserve(kAnswerEventFixedBytes + event.package_id.size() +
                 event.candidate_id.size() +
                 event.question_id.size() + event.answer.size());
    append_le(wire, kEventMagic);
    append_le(wire, kWireVersion);
    append_le(wire, static_cast<std::uint8_t>(event.kind));
    append_le(wire, static_cast<std::uint8_t>(0));
    append_le(wire, event.sequence);
    append_le(wire, event.client_time_unix_milliseconds);
    append_le(wire, event.question_revision);
    append_le(wire, static_cast<std::uint16_t>(event.package_id.size()));
    append_le(wire, static_cast<std::uint16_t>(event.candidate_id.size()));
    append_le(wire, static_cast<std::uint16_t>(event.question_id.size()));
    append_le(wire, static_cast<std::uint32_t>(event.answer.size()));
    append_array(wire, event.package_digest);
    append_array(wire, event.client_id);
    append_array(wire, event.session_id);
    append_array(wire, event.previous_event_hash);
    wire.insert(wire.end(),
                reinterpret_cast<const std::byte*>(event.package_id.data()),
                reinterpret_cast<const std::byte*>(event.package_id.data()) +
                    event.package_id.size());
    wire.insert(wire.end(),
                reinterpret_cast<const std::byte*>(event.candidate_id.data()),
                reinterpret_cast<const std::byte*>(event.candidate_id.data()) +
                    event.candidate_id.size());
    wire.insert(wire.end(),
                reinterpret_cast<const std::byte*>(event.question_id.data()),
                reinterpret_cast<const std::byte*>(event.question_id.data()) +
                    event.question_id.size());
    append_bytes(wire, event.answer);
    return wire.size() <= kMaximumWireBytes ? wire : std::vector<std::byte>{};
}

std::optional<AnswerEvent> decode_answer_event(
    std::span<const std::byte> payload, std::string* error) {
    if (payload.size() < kAnswerEventFixedBytes ||
        payload.size() > kMaximumWireBytes) {
        set_error(error, "exam answer event is truncated or oversized");
        return std::nullopt;
    }
    std::size_t offset = 0;
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    std::uint8_t raw_kind = 0;
    std::uint8_t reserved = 0;
    std::uint16_t package_bytes = 0;
    std::uint16_t candidate_bytes = 0;
    std::uint16_t question_bytes = 0;
    std::uint32_t answer_bytes = 0;
    AnswerEvent event;
    if (!read_le(payload, offset, magic) || magic != kEventMagic ||
        !read_le(payload, offset, version) || version != kWireVersion ||
        !read_le(payload, offset, raw_kind) ||
        !read_le(payload, offset, reserved) || reserved != 0 ||
        !read_le(payload, offset, event.sequence) ||
        !read_le(payload, offset, event.client_time_unix_milliseconds) ||
        !read_le(payload, offset, event.question_revision) ||
        !read_le(payload, offset, package_bytes) ||
        !read_le(payload, offset, candidate_bytes) ||
        !read_le(payload, offset, question_bytes) ||
        !read_le(payload, offset, answer_bytes) ||
        !read_array(payload, offset, event.package_digest) ||
        !read_array(payload, offset, event.client_id) ||
        !read_array(payload, offset, event.session_id) ||
        !read_array(payload, offset, event.previous_event_hash)) {
        set_error(error, "invalid exam answer event header");
        return std::nullopt;
    }
    event.kind = static_cast<AnswerKind>(raw_kind);
    if (package_bytes > kMaximumPackageIdBytes ||
        candidate_bytes > kMaximumCandidateIdBytes ||
        question_bytes > kMaximumQuestionIdBytes ||
        answer_bytes > kMaximumAnswerBytes ||
        payload.size() - offset < static_cast<std::size_t>(package_bytes) +
                                      candidate_bytes + question_bytes +
                                      answer_bytes ||
        payload.size() - offset != static_cast<std::size_t>(package_bytes) +
                                        candidate_bytes + question_bytes +
                                        answer_bytes ||
        !read_string(payload, offset, package_bytes, kMaximumPackageIdBytes,
                     event.package_id) ||
        !read_string(payload, offset, candidate_bytes,
                     kMaximumCandidateIdBytes, event.candidate_id) ||
        !read_string(payload, offset, question_bytes, kMaximumQuestionIdBytes,
                     event.question_id, true)) {
        set_error(error, "invalid exam answer event fields");
        return std::nullopt;
    }
    event.answer.assign(payload.begin() + static_cast<std::ptrdiff_t>(offset),
                        payload.end());
    if (!validate_answer_event(event, error)) {
        return std::nullopt;
    }
    return event;
}

std::optional<security::Sha256Digest> hash_answer_event(
    const AnswerEvent& event) {
    const auto wire = encode_answer_event(event);
    if (wire.empty()) {
        return std::nullopt;
    }
    return security::sha256(wire);
}

std::vector<std::byte> encode_answer_ack(const AnswerAck& ack) {
    if (!valid_ack_shape(ack)) {
        return {};
    }
    std::vector<std::byte> wire;
    wire.reserve(kAnswerAckBytes);
    append_le(wire, kAckMagic);
    append_le(wire, kWireVersion);
    append_le(wire, static_cast<std::uint8_t>(ack.status));
    append_le(wire, static_cast<std::uint8_t>(0));
    append_array(wire, ack.session_id);
    append_le(wire, ack.sequence);
    append_le(wire, ack.highest_contiguous_sequence);
    append_le(wire, ack.server_time_unix_milliseconds);
    append_array(wire, ack.event_hash);
    append_array(wire, ack.state_hash);
    return wire;
}

std::optional<AnswerAck> decode_answer_ack(
    std::span<const std::byte> payload) {
    if (payload.size() != kAnswerAckBytes) {
        return std::nullopt;
    }
    AnswerAck ack;
    std::size_t offset = 0;
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    std::uint8_t raw_status = 0;
    std::uint8_t reserved = 0;
    if (!read_le(payload, offset, magic) || magic != kAckMagic ||
        !read_le(payload, offset, version) || version != kWireVersion ||
        !read_le(payload, offset, raw_status) ||
        !read_le(payload, offset, reserved) || reserved != 0 ||
        !read_array(payload, offset, ack.session_id) ||
        !read_le(payload, offset, ack.sequence) ||
        !read_le(payload, offset, ack.highest_contiguous_sequence) ||
        !read_le(payload, offset, ack.server_time_unix_milliseconds) ||
        !read_array(payload, offset, ack.event_hash) ||
        !read_array(payload, offset, ack.state_hash)) {
        return std::nullopt;
    }
    ack.status = static_cast<AnswerAckStatus>(raw_status);
    if (!valid_ack_shape(ack)) {
        return std::nullopt;
    }
    return ack;
}

std::vector<std::byte> encode_state_request(const StateRequest& request) {
    if (!valid_text(request.package_id, kMaximumPackageIdBytes) ||
        all_zero(request.package_digest) || !valid_identity(request.client_id,
                                                            request.session_id) ||
        !valid_text(request.candidate_id, kMaximumCandidateIdBytes)) {
        return {};
    }
    std::vector<std::byte> wire;
    wire.reserve(kStateRequestFixedBytes + request.package_id.size() +
                 request.candidate_id.size());
    append_le(wire, kRequestMagic);
    append_le(wire, kWireVersion);
    append_le(wire, static_cast<std::uint16_t>(request.package_id.size()));
    append_le(wire, static_cast<std::uint16_t>(request.candidate_id.size()));
    append_array(wire, request.package_digest);
    append_array(wire, request.client_id);
    append_array(wire, request.session_id);
    wire.insert(wire.end(),
                reinterpret_cast<const std::byte*>(request.package_id.data()),
                reinterpret_cast<const std::byte*>(request.package_id.data()) +
                    request.package_id.size());
    wire.insert(wire.end(),
                reinterpret_cast<const std::byte*>(request.candidate_id.data()),
                reinterpret_cast<const std::byte*>(request.candidate_id.data()) +
                    request.candidate_id.size());
    return wire;
}

std::optional<StateRequest> decode_state_request(
    std::span<const std::byte> payload) {
    if (payload.size() < kStateRequestFixedBytes ||
        payload.size() > kStateRequestFixedBytes +
                             kMaximumPackageIdBytes +
                             kMaximumCandidateIdBytes) {
        return std::nullopt;
    }
    StateRequest request;
    std::size_t offset = 0;
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    std::uint16_t package_bytes = 0;
    std::uint16_t candidate_bytes = 0;
    if (!read_le(payload, offset, magic) || magic != kRequestMagic ||
        !read_le(payload, offset, version) || version != kWireVersion ||
        !read_le(payload, offset, package_bytes) ||
        !read_le(payload, offset, candidate_bytes) ||
        !read_array(payload, offset, request.package_digest) ||
        !read_array(payload, offset, request.client_id) ||
        !read_array(payload, offset, request.session_id) ||
        !read_string(payload, offset, package_bytes, kMaximumPackageIdBytes,
                     request.package_id) ||
        !read_string(payload, offset, candidate_bytes,
                     kMaximumCandidateIdBytes, request.candidate_id) ||
        offset != payload.size() ||
        !valid_identity(request.client_id, request.session_id) ||
        all_zero(request.package_digest)) {
        return std::nullopt;
    }
    return request;
}

bool valid_state_response_identity(const StateResponse& response) noexcept {
    return valid_text(response.package_id, kMaximumPackageIdBytes) &&
           all_zero(response.package_digest) == false &&
           valid_identity(response.client_id, response.session_id) &&
           valid_text(response.candidate_id, kMaximumCandidateIdBytes) &&
           response.chunk_count != 0 &&
           response.chunk_count <= kMaximumStateChunks &&
           response.chunk_index < response.chunk_count &&
           response.answers.size() <= kMaximumStateEntries;
}

bool valid_state_response_metadata(const StateResponse& response) noexcept {
    if (!valid_state_response_identity(response)) {
        return false;
    }
    // A non-empty state needs both the state digest and the chain cursor. An
    // empty state has no cursor; state_hash may be omitted or zero there.
    return response.highest_contiguous_sequence == 0
               ? all_zero(response.last_event_hash)
               : !all_zero(response.state_hash) &&
                     !all_zero(response.last_event_hash);
}

std::size_t answer_wire_bytes(const AnswerState& answer) noexcept {
    // question length (u16), revision (u32), sequence (u64), kind/reserved
    // (u8/u8), answer length (u32), event hash, question bytes, answer bytes.
    return sizeof(std::uint16_t) + sizeof(std::uint32_t) +
           sizeof(std::uint64_t) + sizeof(std::uint8_t) * 2 +
           sizeof(std::uint32_t) + security::kSha256Bytes +
           answer.question_id.size() + answer.answer.size();
}

std::vector<std::byte> encode_state_response_chunk(
    const StateResponse& response) {
    if (!valid_state_response_metadata(response) ||
        response.answers.size() > kMaximumStateEntries) {
        return {};
    }
    const auto base_bytes = kStateResponseV3FixedBytes + response.package_id.size() +
                            response.candidate_id.size();
    std::size_t answer_bytes = 0;
    std::set<std::string> question_ids;
    std::set<std::uint64_t> answer_sequences;
    for (const auto& answer : response.answers) {
        if (!valid_answer_state(answer) ||
            answer.sequence > response.highest_contiguous_sequence ||
            answer.question_id.size() >
                std::numeric_limits<std::uint16_t>::max() ||
            !question_ids.insert(answer.question_id).second ||
            !answer_sequences.insert(answer.sequence).second) {
            return {};
        }
        const auto bytes = answer_wire_bytes(answer);
        if (answer_bytes > kMaximumWireBytes - bytes) {
            return {};
        }
        answer_bytes += bytes;
    }
    if (base_bytes > kMaximumWireBytes ||
        answer_bytes > kMaximumWireBytes - base_bytes) {
        return {};
    }

    std::vector<std::byte> wire;
    wire.reserve(base_bytes + answer_bytes);
    append_le(wire, kResponseMagic);
    append_le(wire, kStateResponseWireVersion);
    append_le(wire, static_cast<std::uint8_t>(response.finalized ? 1 : 0));
    append_le(wire, static_cast<std::uint8_t>(0));
    append_le(wire, response.highest_contiguous_sequence);
    append_le(wire, static_cast<std::uint16_t>(response.answers.size()));
    append_le(wire, response.chunk_index);
    append_le(wire, response.chunk_count);
    append_le(wire, static_cast<std::uint16_t>(response.package_id.size()));
    append_le(wire, static_cast<std::uint16_t>(response.candidate_id.size()));
    append_array(wire, response.package_digest);
    append_array(wire, response.client_id);
    append_array(wire, response.session_id);
    append_array(wire, response.state_hash);
    append_array(wire, response.last_event_hash);
    wire.insert(wire.end(),
                reinterpret_cast<const std::byte*>(response.package_id.data()),
                reinterpret_cast<const std::byte*>(response.package_id.data()) +
                    response.package_id.size());
    wire.insert(wire.end(),
                reinterpret_cast<const std::byte*>(response.candidate_id.data()),
                reinterpret_cast<const std::byte*>(response.candidate_id.data()) +
                    response.candidate_id.size());
    for (const auto& answer : response.answers) {
        append_le(wire, static_cast<std::uint16_t>(answer.question_id.size()));
        append_le(wire, answer.question_revision);
        append_le(wire, answer.sequence);
        append_le(wire, static_cast<std::uint8_t>(answer.kind));
        append_le(wire, static_cast<std::uint8_t>(0));
        append_le(wire, static_cast<std::uint32_t>(answer.answer.size()));
        append_array(wire, answer.event_hash);
        wire.insert(wire.end(),
                    reinterpret_cast<const std::byte*>(answer.question_id.data()),
                    reinterpret_cast<const std::byte*>(answer.question_id.data()) +
                        answer.question_id.size());
        append_bytes(wire, answer.answer);
    }
    return wire.size() <= kMaximumWireBytes ? wire : std::vector<std::byte>{};
}

std::vector<std::byte> encode_state_response(const StateResponse& response) {
    return encode_state_response_chunk(response);
}

std::vector<std::vector<std::byte>> encode_state_response_chunks(
    const StateResponse& response) {
    if (!valid_state_response_metadata(response) ||
        response.answers.size() > kMaximumStateEntries) {
        return {};
    }
    const auto base_bytes = kStateResponseV3FixedBytes + response.package_id.size() +
                            response.candidate_id.size();
    if (base_bytes > kMaximumWireBytes) {
        return {};
    }
    for (const auto& answer : response.answers) {
        if (!valid_answer_state(answer) ||
            answer.sequence > response.highest_contiguous_sequence ||
            answer.question_id.size() >
                std::numeric_limits<std::uint16_t>::max() ||
            answer_wire_bytes(answer) > kMaximumWireBytes - base_bytes) {
            return {};
        }
    }

    std::set<std::string> question_ids;
    std::set<std::uint64_t> answer_sequences;
    for (const auto& answer : response.answers) {
        if (!question_ids.insert(answer.question_id).second) {
            return {};
        }
        if (!answer_sequences.insert(answer.sequence).second) {
            return {};
        }
    }

    // Build chunks greedily in the stable question-id order returned by the
    // journal.  The shared chunk ceiling keeps reassembly bounded on every
    // client implementation.
    std::vector<std::vector<AnswerState>> chunk_answers;
    chunk_answers.emplace_back();
    std::size_t current_bytes = base_bytes;
    for (const auto& answer : response.answers) {
        const auto bytes = answer_wire_bytes(answer);
        if (!chunk_answers.back().empty() &&
            current_bytes > kMaximumWireBytes - bytes) {
            chunk_answers.emplace_back();
            current_bytes = base_bytes;
        }
        chunk_answers.back().push_back(answer);
        current_bytes += bytes;
    }
    if (chunk_answers.size() > kMaximumStateChunks ||
        chunk_answers.size() > std::numeric_limits<std::uint16_t>::max()) {
        return {};
    }

    std::vector<std::vector<std::byte>> result;
    result.reserve(chunk_answers.size());
    const auto count = static_cast<std::uint16_t>(chunk_answers.size());
    for (std::size_t index = 0; index < chunk_answers.size(); ++index) {
        StateResponse chunk = response;
        chunk.chunk_index = static_cast<std::uint16_t>(index);
        chunk.chunk_count = count;
        chunk.answers = std::move(chunk_answers[index]);
        auto wire = encode_state_response_chunk(chunk);
        if (wire.empty()) {
            return {};
        }
        result.push_back(std::move(wire));
    }
    return result;
}

std::optional<StateResponse> decode_state_response(
    std::span<const std::byte> payload) {
    if (payload.size() < kStateResponseV1FixedBytes ||
        payload.size() > kMaximumWireBytes) {
        return std::nullopt;
    }
    StateResponse response;
    std::size_t offset = 0;
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    std::uint8_t flags = 0;
    std::uint8_t reserved = 0;
    std::uint16_t answer_count = 0;
    std::uint16_t chunk_index = 0;
    std::uint16_t chunk_count = 1;
    std::uint16_t package_bytes = 0;
    std::uint16_t candidate_bytes = 0;
    if (!read_le(payload, offset, magic) || magic != kResponseMagic ||
        !read_le(payload, offset, version) ||
        (version != kWireVersion &&
         version != kStateResponsePreviousWireVersion &&
         version != kStateResponseWireVersion) ||
        !read_le(payload, offset, flags) || flags > 1 ||
        !read_le(payload, offset, reserved) || reserved != 0 ||
        !read_le(payload, offset, response.highest_contiguous_sequence) ||
        !read_le(payload, offset, answer_count) ||
        ((version == kStateResponsePreviousWireVersion ||
          version == kStateResponseWireVersion) &&
         (!read_le(payload, offset, chunk_index) ||
          !read_le(payload, offset, chunk_count))) ||
        !read_le(payload, offset, package_bytes) ||
        !read_le(payload, offset, candidate_bytes) ||
        answer_count > kMaximumStateEntries ||
        package_bytes > kMaximumPackageIdBytes ||
        candidate_bytes > kMaximumCandidateIdBytes ||
        !read_array(payload, offset, response.package_digest) ||
        !read_array(payload, offset, response.client_id) ||
        !read_array(payload, offset, response.session_id) ||
        !read_array(payload, offset, response.state_hash) ||
        (version == kStateResponseWireVersion &&
         !read_array(payload, offset, response.last_event_hash)) ||
        !read_string(payload, offset, package_bytes, kMaximumPackageIdBytes,
                     response.package_id) ||
        !read_string(payload, offset, candidate_bytes,
                     kMaximumCandidateIdBytes, response.candidate_id) ||
        all_zero(response.package_digest) ||
        !valid_identity(response.client_id, response.session_id) ||
        chunk_count == 0 || chunk_count > kMaximumStateChunks ||
        chunk_index >= chunk_count) {
        return std::nullopt;
    }
    response.finalized = flags != 0;
    response.chunk_index = chunk_index;
    response.chunk_count = chunk_count;
    if ((version == kStateResponseWireVersion &&
         !valid_state_response_metadata(response)) ||
        (version != kStateResponseWireVersion &&
         !valid_state_response_identity(response))) {
        return std::nullopt;
    }
    response.answers.reserve(answer_count);
    std::set<std::string> question_ids;
    std::set<std::uint64_t> answer_sequences;
    for (std::uint16_t index = 0; index < answer_count; ++index) {
        AnswerState answer;
        std::uint16_t question_bytes = 0;
        std::uint8_t raw_kind = 0;
        std::uint8_t answer_reserved = 0;
        std::uint32_t answer_bytes = 0;
        if (!read_le(payload, offset, question_bytes) ||
            !read_le(payload, offset, answer.question_revision) ||
            !read_le(payload, offset, answer.sequence) ||
            !read_le(payload, offset, raw_kind) ||
            !read_le(payload, offset, answer_reserved) ||
            !read_le(payload, offset, answer_bytes) ||
            !read_array(payload, offset, answer.event_hash) ||
            question_bytes == 0 || question_bytes > kMaximumQuestionIdBytes ||
            answer_bytes > kMaximumAnswerBytes ||
            !read_string(payload, offset, question_bytes,
                         kMaximumQuestionIdBytes, answer.question_id) ||
            offset > payload.size() ||
            static_cast<std::size_t>(answer_bytes) > payload.size() - offset) {
            return std::nullopt;
        }
        answer.kind = static_cast<AnswerKind>(raw_kind);
        if (answer_reserved != 0 || !valid_answer_state(answer) ||
            !question_ids.insert(answer.question_id).second ||
            !answer_sequences.insert(answer.sequence).second ||
            answer.sequence > response.highest_contiguous_sequence) {
            return std::nullopt;
        }
        answer.answer.assign(
            payload.begin() + static_cast<std::ptrdiff_t>(offset),
            payload.begin() + static_cast<std::ptrdiff_t>(offset + answer_bytes));
        offset += answer_bytes;
        // Validate the decoded bytes as well as the header. In particular, a
        // malformed clear entry must not smuggle a non-empty answer through the
        // pre-read validation (the answer vector is empty at that point).
        if (!valid_answer_state(answer)) {
            return std::nullopt;
        }
        response.answers.push_back(std::move(answer));
    }
    return offset == payload.size() ? std::optional{std::move(response)}
                                    : std::nullopt;
}

} // namespace nstu::exam
