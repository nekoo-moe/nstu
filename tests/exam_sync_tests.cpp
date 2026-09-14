#include "nstu/exam_sync.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

template <typename Array>
void fill_bytes(Array& bytes, std::uint8_t first) {
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = static_cast<std::byte>(first + index);
    }
}

nstu::exam::AnswerEvent make_event(std::uint64_t sequence) {
    nstu::exam::AnswerEvent event;
    event.package_id = "sample-package";
    fill_bytes(event.package_digest, 0x20);
    fill_bytes(event.client_id, 0x40);
    fill_bytes(event.session_id, 0x60);
    event.candidate_id = "candidate-01";
    event.question_id = "reading-01";
    event.question_revision = 3;
    event.sequence = sequence;
    event.client_time_unix_milliseconds = 1'700'000'000'000ULL;
    event.kind = nstu::exam::AnswerKind::upsert;
    event.answer = {std::byte{'B'}};
    if (sequence != 1) {
        event.previous_event_hash[0] = std::byte{0x7f};
    }
    return event;
}

std::filesystem::path test_path(const char* suffix) {
    const auto tick = std::chrono::high_resolution_clock::now()
                          .time_since_epoch()
                          .count();
    return std::filesystem::temp_directory_path() /
           (std::string("nstu-exam-sync-") + std::to_string(tick) + suffix);
}

void test_journal_persistence() {
    using namespace nstu::exam;

    const auto path = test_path("-journal.bin");
    std::error_code cleanup_error;
    std::filesystem::remove(path, cleanup_error);

    AnswerJournal journal;
    std::string error;
    assert(journal.open(path, &error));

    auto first = make_event(1);
    const auto first_hash = hash_answer_event(first);
    assert(first_hash.has_value());
    auto first_outcome = journal.append(first, &error);
    if (first_outcome.status != AppendStatus::accepted) {
        std::cerr << "first journal append failed: " << error << " status "
                  << static_cast<int>(first_outcome.status) << "\\n";
    }
    assert(first_outcome.status == AppendStatus::accepted);
    assert(first_outcome.ack.status == AnswerAckStatus::accepted);

    const auto duplicate = journal.append(first, &error);
    assert(duplicate.status == AppendStatus::duplicate);

    auto second = make_event(2);
    second.previous_event_hash = *first_hash;
    const auto second_hash = hash_answer_event(second);
    assert(second_hash.has_value());
    const auto second_outcome = journal.append(second, &error);
    assert(second_outcome.status == AppendStatus::accepted);

    auto gap = make_event(4);
    gap.previous_event_hash = *second_hash;
    const auto gap_outcome = journal.append(gap, &error);
    assert(gap_outcome.status == AppendStatus::gap);

    auto conflict = second;
    conflict.answer = {std::byte{'C'}};
    const auto conflict_outcome = journal.append(conflict, &error);
    assert(conflict_outcome.status == AppendStatus::conflict);

    auto finalize = make_event(3);
    finalize.question_id.clear();
    finalize.answer.clear();
    finalize.kind = AnswerKind::finalize;
    finalize.previous_event_hash = *second_hash;
    auto malformed_finalize = finalize;
    malformed_finalize.question_id = "unexpected-question";
    assert(!validate_answer_event(malformed_finalize));
    malformed_finalize = finalize;
    malformed_finalize.answer = {std::byte{'x'}};
    assert(!validate_answer_event(malformed_finalize));
    const auto finalize_outcome = journal.append(finalize, &error);
    assert(finalize_outcome.status == AppendStatus::accepted);
    assert(journal.event_count() == 3);

    auto post_finalize = make_event(4);
    post_finalize.previous_event_hash = *hash_answer_event(finalize);
    const auto post_finalize_outcome = journal.append(post_finalize, &error);
    assert(post_finalize_outcome.status == AppendStatus::rejected);

    StateRequest request;
    request.package_id = first.package_id;
    request.package_digest = first.package_digest;
    request.client_id = first.client_id;
    request.session_id = first.session_id;
    request.candidate_id = first.candidate_id;
    const auto state = journal.state(request, &error);
    assert(state.has_value());
    assert(state->finalized);
    assert(state->highest_contiguous_sequence == 3);
    assert(state->answers.size() == 1);
    assert(state->answers.front().question_id == first.question_id);
    assert(!journal.export_state(request, path, &error));
    journal.close();

    AnswerJournal reopened;
    assert(reopened.open(path, &error));
    assert(reopened.event_count() == 3);
    const auto reopened_state = reopened.state(request, &error);
    assert(reopened_state.has_value());
    assert(reopened_state->finalized);
    assert(reopened_state->answers.size() == 1);

    {
        std::ofstream tail(path, std::ios::binary | std::ios::app);
        assert(tail.good());
        tail.put('\x7f');
        tail.put('\x01');
    }
    reopened.close();
    AnswerJournal recovered;
    assert(recovered.open(path, &error));
    assert(recovered.recovered_truncated_tail());
    assert(recovered.event_count() == 3);
    recovered.close();
    std::filesystem::remove(path, cleanup_error);
}

void test_outbox_persistence() {
    using namespace nstu::exam;

    const auto path = test_path("-outbox.bin");
    std::error_code cleanup_error;
    std::filesystem::remove(path, cleanup_error);
    std::string error;
    AnswerOutbox outbox;
    assert(outbox.open(path, &error));

    auto first = make_event(1);
    const auto first_hash = hash_answer_event(first);
    assert(first_hash.has_value());
    assert(outbox.enqueue(first, &error));
    assert(outbox.enqueue(first, &error));

    auto second = make_event(2);
    second.previous_event_hash = *first_hash;
    const auto second_hash = hash_answer_event(second);
    assert(second_hash.has_value());
    assert(outbox.enqueue(second, &error));
    assert(outbox.size() == 2);
    outbox.close();

    AnswerOutbox reopened;
    assert(reopened.open(path, &error));
    assert(reopened.pending().size() == 2);
    assert(reopened.acknowledge(first.session_id, 1, *first_hash, &error));
    assert(reopened.size() == 1);
    assert(reopened.acknowledge(second.session_id, 2, *second_hash, &error));
    assert(reopened.size() == 0);
    reopened.close();

    AnswerOutbox empty;
    assert(empty.open(path, &error));
    assert(empty.pending().empty());
    empty.close();
    std::filesystem::remove(path, cleanup_error);
}

} // namespace

int main() {
    using namespace nstu::exam;

    const auto event = make_event(1);
    const auto event_wire = encode_answer_event(event);
    assert(!event_wire.empty());
    const auto decoded_event = decode_answer_event(event_wire);
    assert(decoded_event.has_value());
    assert(decoded_event->package_id == event.package_id);
    assert(decoded_event->package_digest == event.package_digest);
    assert(decoded_event->client_id == event.client_id);
    assert(decoded_event->session_id == event.session_id);
    assert(decoded_event->candidate_id == event.candidate_id);
    assert(decoded_event->question_id == event.question_id);
    assert(decoded_event->question_revision == event.question_revision);
    assert(decoded_event->sequence == event.sequence);
    assert(decoded_event->answer == event.answer);

    auto truncated_event = event_wire;
    truncated_event.pop_back();
    assert(!decode_answer_event(truncated_event).has_value());
    auto trailing_event = event_wire;
    trailing_event.push_back(std::byte{0});
    assert(!decode_answer_event(trailing_event).has_value());

    StateRequest request;
    request.package_id.assign(kMaximumPackageIdBytes, 'p');
    request.candidate_id.assign(kMaximumCandidateIdBytes, 'c');
    fill_bytes(request.package_digest, 0x10);
    fill_bytes(request.client_id, 0x30);
    fill_bytes(request.session_id, 0x50);
    const auto request_wire = encode_state_request(request);
    assert(request_wire.size() == 74 + kMaximumPackageIdBytes +
                                      kMaximumCandidateIdBytes);
    const auto decoded_request = decode_state_request(request_wire);
    assert(decoded_request.has_value());
    assert(decoded_request->package_id == request.package_id);
    assert(decoded_request->candidate_id == request.candidate_id);
    assert(decoded_request->client_id == request.client_id);

    AnswerAck gap_ack;
    gap_ack.status = AnswerAckStatus::gap;
    fill_bytes(gap_ack.session_id, 0x70);
    gap_ack.sequence = 5;
    gap_ack.highest_contiguous_sequence = 2;
    gap_ack.server_time_unix_milliseconds = 1'700'000'000'001ULL;
    const auto gap_wire = encode_answer_ack(gap_ack);
    assert(!gap_wire.empty());
    const auto decoded_gap = decode_answer_ack(gap_wire);
    assert(decoded_gap.has_value());
    assert(decoded_gap->status == AnswerAckStatus::gap);
    assert(decoded_gap->sequence == 5);
    assert(decoded_gap->highest_contiguous_sequence == 2);

    AnswerAck invalid_ack = gap_ack;
    invalid_ack.status = AnswerAckStatus::accepted;
    assert(encode_answer_ack(invalid_ack).empty());

    StateResponse response;
    response.package_id = "sample-package";
    response.candidate_id = "candidate-01";
    response.package_digest = event.package_digest;
    response.client_id = event.client_id;
    response.session_id = event.session_id;
    response.highest_contiguous_sequence = 1;
    response.state_hash[0] = std::byte{0x01};
    response.last_event_hash = *hash_answer_event(event);
    response.answers.push_back(AnswerState{
        .question_id = event.question_id,
        .question_revision = event.question_revision,
        .sequence = 1,
        .kind = AnswerKind::upsert,
        .answer = event.answer,
        .event_hash = *hash_answer_event(event),
    });
    const auto response_wire = encode_state_response(response);
    assert(!response_wire.empty());
    const auto decoded_response = decode_state_response(response_wire);
    assert(decoded_response.has_value());
    assert(decoded_response->package_id == response.package_id);
    assert(decoded_response->candidate_id == response.candidate_id);
    assert(decoded_response->answers.size() == 1);
    assert(decoded_response->answers.front().answer == event.answer);
    assert(decoded_response->last_event_hash == response.last_event_hash);

    // An empty recovery state has no chain cursor. Its state hash may be
    // omitted/zero and must remain decodable.
    StateResponse empty_response = response;
    empty_response.highest_contiguous_sequence = 0;
    empty_response.state_hash.fill(std::byte{0});
    empty_response.last_event_hash.fill(std::byte{0});
    empty_response.answers.clear();
    const auto empty_wire = encode_state_response(empty_response);
    assert(!empty_wire.empty());
    assert(decode_state_response(empty_wire).has_value());

    // v3 responses with a non-empty watermark must carry both hashes. These
    // malformed payloads are rejected at decode time even if they bypass the
    // native encoder.
    constexpr std::size_t v3_state_hash_offset = 90;
    constexpr std::size_t v3_last_event_hash_offset = 122;
    const auto v3_answer_start = v3_last_event_hash_offset + 32 +
                                 response.package_id.size() +
                                 response.candidate_id.size();
    auto missing_state_hash = response_wire;
    std::fill(missing_state_hash.begin() +
                  static_cast<std::ptrdiff_t>(v3_state_hash_offset),
              missing_state_hash.begin() +
                  static_cast<std::ptrdiff_t>(v3_state_hash_offset + 32),
              std::byte{0});
    assert(!decode_state_response(missing_state_hash).has_value());
    auto missing_last_event_hash = response_wire;
    std::fill(missing_last_event_hash.begin() +
                  static_cast<std::ptrdiff_t>(v3_last_event_hash_offset),
              missing_last_event_hash.begin() +
                  static_cast<std::ptrdiff_t>(v3_last_event_hash_offset + 32),
              std::byte{0});
    assert(!decode_state_response(missing_last_event_hash).has_value());

    StateResponse invalid_response = response;
    invalid_response.answers.front().sequence = 2;
    assert(encode_state_response(invalid_response).empty());

    AnswerState invalid_clear = response.answers.front();
    invalid_clear.kind = AnswerKind::clear;
    invalid_clear.answer = {std::byte{0x01}};
    invalid_response = response;
    invalid_response.answers = {invalid_clear};
    assert(encode_state_response(invalid_response).empty());

    // The decoder must apply the clear invariant after consuming the answer
    // bytes too; mutating a valid upsert payload must not bypass that check.
    auto malformed_clear_wire = response_wire;
    malformed_clear_wire[v3_answer_start + 14] =
        static_cast<std::byte>(AnswerKind::clear);
    assert(!decode_state_response(malformed_clear_wire).has_value());

    invalid_response = response;
    invalid_response.chunk_count =
        static_cast<std::uint16_t>(kMaximumStateChunks + 1);
    assert(encode_state_response(invalid_response).empty());

    invalid_response = response;
    invalid_response.answers.push_back(response.answers.front());
    assert(encode_state_response(invalid_response).empty());

    // Sequence numbers are unique across a snapshot, even when question IDs
    // differ. This prevents a malformed recovery payload from presenting two
    // competing answers at the same journal watermark.
    invalid_response = response;
    auto duplicate_sequence = response.answers.front();
    duplicate_sequence.question_id = "reading-02";
    invalid_response.answers = {response.answers.front(), duplicate_sequence};
    assert(encode_state_response(invalid_response).empty());

    invalid_response = response;
    invalid_response.answers.front().event_hash.fill(std::byte{0});
    assert(encode_state_response(invalid_response).empty());

    auto too_many_chunks_wire = response_wire;
    // v3 chunk_count is the little-endian u16 at byte offset 20.
    too_many_chunks_wire[20] = static_cast<std::byte>(kMaximumStateChunks + 1);
    too_many_chunks_wire[21] = std::byte{0};
    assert(!decode_state_response(too_many_chunks_wire).has_value());

    // A decoder must reject duplicate question IDs within one chunk, even
    // when the payload was not produced by our encoder.
    constexpr std::size_t v3_answer_count_offset = 16;
    auto duplicate_answers_wire = response_wire;
    duplicate_answers_wire[v3_answer_count_offset] = std::byte{2};
    duplicate_answers_wire[v3_answer_count_offset + 1] = std::byte{0};
    duplicate_answers_wire.insert(
        duplicate_answers_wire.end(),
        response_wire.begin() + static_cast<std::ptrdiff_t>(v3_answer_start),
        response_wire.end());
    assert(!decode_state_response(duplicate_answers_wire).has_value());

    auto zero_event_hash_wire = response_wire;
    const auto v3_event_hash_offset = v3_answer_start + 20;
    std::fill(zero_event_hash_wire.begin() +
                  static_cast<std::ptrdiff_t>(v3_event_hash_offset),
              zero_event_hash_wire.begin() +
                  static_cast<std::ptrdiff_t>(v3_event_hash_offset + 32),
              std::byte{0});
    assert(!decode_state_response(zero_event_hash_wire).has_value());

    // A complete exam state can exceed the 64 KiB authenticated command
    // limit. The wire layer must split it into bounded, independently
    // decodable chunks rather than making recovery fail.
    StateResponse large_response = response;
    large_response.answers.clear();
    for (std::uint64_t sequence = 1; sequence <= 32; ++sequence) {
        AnswerState answer;
        answer.question_id = "question-" + std::to_string(sequence);
        answer.question_revision = 1;
        answer.sequence = sequence;
        answer.kind = AnswerKind::upsert;
        answer.answer.assign(3000, std::byte{'x'});
        answer.event_hash[0] = static_cast<std::byte>((sequence % 255) + 1);
        large_response.answers.push_back(std::move(answer));
    }
    large_response.highest_contiguous_sequence = 32;
    large_response.last_event_hash[0] = std::byte{0x7a};
    const auto chunks = encode_state_response_chunks(large_response);
    assert(chunks.size() > 1);
    for (std::size_t index = 0; index < chunks.size(); ++index) {
        assert(chunks[index].size() <= kMaximumExamPayloadBytes);
        const auto decoded = decode_state_response(chunks[index]);
        assert(decoded.has_value());
        assert(decoded->chunk_index == index);
        assert(decoded->chunk_count == chunks.size());
        assert(decoded->highest_contiguous_sequence == 32);
    }

    // The native splitter must fail closed when a snapshot would require more
    // chunks than the bridge/browser can retain. This is intentionally a
    // bounded failure until paginated recovery is introduced.
    StateResponse over_chunk_limit = response;
    over_chunk_limit.answers.clear();
    over_chunk_limit.highest_contiguous_sequence = kMaximumStateEntries;
    over_chunk_limit.last_event_hash[0] = std::byte{0x7b};
    for (std::uint64_t sequence = 1; sequence <= kMaximumStateEntries;
         ++sequence) {
        AnswerState answer;
        answer.question_id = "large-question-" + std::to_string(sequence);
        answer.question_revision = 1;
        answer.sequence = sequence;
        answer.kind = AnswerKind::upsert;
        answer.answer.assign(9000, std::byte{0x5a});
        answer.event_hash[0] = static_cast<std::byte>((sequence % 255) + 1);
        over_chunk_limit.answers.push_back(std::move(answer));
    }
    assert(encode_state_response_chunks(over_chunk_limit).empty());

    // Compatibility fixtures for the pre-v3 response formats. Version 2 has
    // chunk metadata but no last-event hash; version 1 has neither.
    const auto v3_wire = encode_state_response(response);
    assert(!v3_wire.empty());
    constexpr std::size_t v2_last_hash_offset = 122;
    constexpr std::size_t v2_last_hash_bytes = 32;
    assert(v3_wire.size() > v2_last_hash_offset + v2_last_hash_bytes);
    std::vector<std::byte> v2_wire;
    v2_wire.reserve(v3_wire.size() - v2_last_hash_bytes);
    v2_wire.insert(v2_wire.end(), v3_wire.begin(),
                   v3_wire.begin() + static_cast<std::ptrdiff_t>(v2_last_hash_offset));
    v2_wire.insert(v2_wire.end(),
                   v3_wire.begin() + static_cast<std::ptrdiff_t>(v2_last_hash_offset + v2_last_hash_bytes),
                   v3_wire.end());
    v2_wire[4] = std::byte{2};
    v2_wire[5] = std::byte{0};
    const auto decoded_v2 = decode_state_response(v2_wire);
    assert(decoded_v2.has_value());
    assert(decoded_v2->chunk_index == 0);
    assert(decoded_v2->chunk_count == 1);
    assert(decoded_v2->answers.size() == response.answers.size());
    assert(decoded_v2->last_event_hash == nstu::security::Sha256Digest{});
    std::vector<std::byte> v1_wire;
    v1_wire.reserve(v2_wire.size() - 4);
    v1_wire.insert(v1_wire.end(), v2_wire.begin(), v2_wire.begin() + 4);
    v1_wire.push_back(std::byte{1});
    v1_wire.push_back(std::byte{0});
    v1_wire.insert(v1_wire.end(), v2_wire.begin() + 6,
                   v2_wire.begin() + 18);
    v1_wire.insert(v1_wire.end(), v2_wire.begin() + 22, v2_wire.end());
    const auto decoded_v1 = decode_state_response(v1_wire);
    assert(decoded_v1.has_value());
    assert(decoded_v1->chunk_index == 0);
    assert(decoded_v1->chunk_count == 1);
    assert(decoded_v1->answers.size() == response.answers.size());
    assert(decoded_v1->last_event_hash == nstu::security::Sha256Digest{});

    test_journal_persistence();
    test_outbox_persistence();

    return 0;
}
