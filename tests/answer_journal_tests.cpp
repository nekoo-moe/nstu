#include "nstu/exam_sync.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <type_traits>
#include <vector>

namespace {

std::filesystem::path test_root() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
           ("nstu-exam-journal-" + std::to_string(stamp));
}

template <typename Array>
void fill_bytes(Array& bytes, std::uint8_t first) {
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = static_cast<std::byte>(first + index);
    }
}

template <typename T>
void append_le(std::vector<std::byte>& bytes, T value) {
    static_assert(std::is_unsigned_v<T>);
    for (std::size_t index = 0; index < sizeof(T); ++index) {
        bytes.push_back(static_cast<std::byte>(value & 0xffu));
        value >>= 8u;
    }
}

nstu::exam::AnswerEvent make_event(std::uint64_t sequence) {
    nstu::exam::AnswerEvent event;
    event.package_id = "journal-test";
    fill_bytes(event.package_digest, 0x10);
    fill_bytes(event.client_id, 0x30);
    fill_bytes(event.session_id, 0x50);
    event.candidate_id = "candidate-01";
    event.question_id = "q-01";
    event.question_revision = 1;
    event.sequence = sequence;
    event.client_time_unix_milliseconds = 1'700'000'000'000ULL + sequence;
    event.kind = nstu::exam::AnswerKind::upsert;
    event.answer = {static_cast<std::byte>(
        static_cast<unsigned char>('A' + static_cast<int>(sequence)))};
    return event;
}

void chain_from(const nstu::exam::AnswerEvent& previous,
                nstu::exam::AnswerEvent& next) {
    const auto hash = nstu::exam::hash_answer_event(previous);
    assert(hash.has_value());
    next.previous_event_hash = *hash;
}

void test_large_export_round_trip() {
    using namespace nstu::exam;
    const auto root = test_root();
    std::error_code cleanup_error;
    std::filesystem::create_directories(root);
    const auto journal_path = root / "large-answers.bin";
    const auto export_path = root / "large-answer-state.sex1";

    AnswerJournal journal;
    std::string error;
    assert(journal.open(journal_path, &error));
    AnswerEvent previous;
    constexpr std::size_t kAnswerBytes = 2048;
    constexpr std::uint64_t kEventCount = 40;
    for (std::uint64_t sequence = 1; sequence <= kEventCount; ++sequence) {
        auto event = make_event(sequence);
        event.package_id = "large-export";
        event.candidate_id = "large-candidate";
        event.question_id = "question-" + std::to_string(sequence);
        event.answer.assign(kAnswerBytes, std::byte{'x'});
        if (sequence > 1) {
            chain_from(previous, event);
        }
        assert(journal.append(event, &error).status == AppendStatus::accepted);
        previous = std::move(event);
    }

    StateRequest request;
    request.package_id = previous.package_id;
    request.package_digest = previous.package_digest;
    request.client_id = previous.client_id;
    request.session_id = previous.session_id;
    request.candidate_id = previous.candidate_id;
    const auto state = journal.state(request, &error);
    assert(state.has_value());
    const auto chunks = encode_state_response_chunks(*state);
    assert(chunks.size() > 1);
    assert(journal.export_state(request, export_path, &error));
    const auto export_size = std::filesystem::file_size(export_path);
    std::vector<std::byte> bytes(static_cast<std::size_t>(export_size));
    {
        std::ifstream input(export_path, std::ios::binary);
        assert(input.good());
        assert(input.read(reinterpret_cast<char*>(bytes.data()),
                          static_cast<std::streamsize>(bytes.size())));
    }
    const auto restored = decode_state_export(bytes, &error);
    assert(restored.has_value());
    assert(restored->highest_contiguous_sequence ==
           state->highest_contiguous_sequence);
    assert(restored->state_hash == state->state_hash);
    assert(restored->last_event_hash == state->last_event_hash);
    assert(restored->answers.size() == state->answers.size());
    auto tampered = bytes;
    const auto state_hash_position = std::search(
        tampered.begin(), tampered.end(), state->state_hash.begin(),
        state->state_hash.end());
    assert(state_hash_position != tampered.end());
    *state_hash_position ^= std::byte{0x01};
    assert(!decode_state_export(tampered, &error).has_value());
    journal.close();
    std::filesystem::remove_all(root, cleanup_error);
}

void test_answer_state_limit() {
    using namespace nstu::exam;
    const auto root = test_root();
    std::error_code cleanup_error;
    std::filesystem::create_directories(root);
    const auto journal_path = root / "bounded-answers.bin";

    AnswerJournal journal;
    std::string error;
    assert(journal.open(journal_path, &error));
    AnswerEvent previous;
    for (std::uint64_t sequence = 1;
         sequence <= kMaximumStateEntries; ++sequence) {
        auto event = make_event(sequence);
        event.package_id = "bounded-export";
        event.candidate_id = "bounded-candidate";
        event.question_id = "question-" + std::to_string(sequence);
        if (sequence > 1) {
            chain_from(previous, event);
        }
        assert(journal.append(event, &error).status == AppendStatus::accepted);
        previous = std::move(event);
    }
    auto overflow = make_event(kMaximumStateEntries + 1);
    overflow.package_id = "bounded-export";
    overflow.candidate_id = "bounded-candidate";
    overflow.question_id = "question-overflow";
    chain_from(previous, overflow);
    const auto rejected = journal.append(overflow, &error);
    assert(rejected.status == AppendStatus::rejected);
    assert(journal.event_count() == kMaximumStateEntries);

    StateRequest request;
    request.package_id = previous.package_id;
    request.package_digest = previous.package_digest;
    request.client_id = previous.client_id;
    request.session_id = previous.session_id;
    request.candidate_id = previous.candidate_id;
    const auto state = journal.state(request, &error);
    assert(state.has_value());
    assert(state->answers.size() == kMaximumStateEntries);
    journal.close();

    AnswerJournal reopened;
    assert(reopened.open(journal_path, &error));
    assert(reopened.event_count() == kMaximumStateEntries);
    reopened.close();
    std::filesystem::remove_all(root, cleanup_error);
}

void test_single_writer_lease() {
    using namespace nstu::exam;
    const auto root = test_root();
    std::error_code cleanup_error;
    std::filesystem::create_directories(root);
    const auto journal_path = root / "single-writer-journal.bin";
    const auto outbox_path = root / "single-writer-outbox.bin";
    std::string error;

    AnswerJournal first_journal;
    AnswerJournal second_journal;
    assert(first_journal.open(journal_path, &error));
    assert(!second_journal.open(journal_path, &error));
    first_journal.close();
    assert(second_journal.open(journal_path, &error));
    second_journal.close();

    AnswerOutbox first_outbox;
    AnswerOutbox second_outbox;
    assert(first_outbox.open(outbox_path, &error));
    assert(!second_outbox.open(outbox_path, &error));
    first_outbox.close();
    assert(second_outbox.open(outbox_path, &error));
    second_outbox.close();
    std::filesystem::remove_all(root, cleanup_error);
}

} // namespace

int main() {
    using namespace nstu::exam;
    test_large_export_round_trip();
    test_answer_state_limit();
    test_single_writer_lease();
    const auto root = test_root();
    std::error_code cleanup_error;
    std::filesystem::create_directories(root);

    const auto journal_path = root / "answers.bin";
    AnswerEvent first = make_event(1);
    AnswerEvent second = make_event(2);
    chain_from(first, second);

    AnswerJournal journal;
    std::string error;
    assert(journal.open(journal_path, &error));
    const auto accepted = journal.append(first, &error);
    assert(accepted.status == AppendStatus::accepted);
    assert(accepted.ack.highest_contiguous_sequence == 1);

    const auto duplicate = journal.append(first, &error);
    assert(duplicate.status == AppendStatus::duplicate);

    AnswerEvent gap = make_event(4);
    chain_from(second, gap);
    const auto gap_result = journal.append(gap, &error);
    assert(gap_result.status == AppendStatus::gap);

    // Invalid first events must not retain empty in-memory sessions and consume
    // the global session quota. Fill the quota with unique gap attempts, then
    // prove that a valid first event can still create its session.
    for (std::size_t index = 0; index < kMaximumJournalSessions; ++index) {
        auto invalid_first = make_event(2);
        invalid_first.package_id = "invalid-first-" + std::to_string(index);
        invalid_first.session_id[0] = static_cast<std::byte>(index & 0xffu);
        invalid_first.session_id[1] =
            static_cast<std::byte>((index >> 8u) & 0xffu);
        invalid_first.session_id[2] =
            static_cast<std::byte>((index >> 16u) & 0xffu);
        invalid_first.session_id[3] =
            static_cast<std::byte>((index >> 24u) & 0xffu);
        invalid_first.previous_event_hash.fill(std::byte{0});
        const auto invalid_outcome = journal.append(invalid_first, &error);
        assert(invalid_outcome.status == AppendStatus::gap);
    }
    auto post_quota_probe = make_event(1);
    post_quota_probe.package_id = "post-invalid-quota";
    assert(journal.append(post_quota_probe, &error).status ==
           AppendStatus::accepted);

    const auto accepted_second = journal.append(second, &error);
    assert(accepted_second.status == AppendStatus::accepted);

    AnswerEvent conflict = second;
    conflict.answer = {std::byte{'Z'}};
    const auto conflict_result = journal.append(conflict, &error);
    assert(conflict_result.status == AppendStatus::conflict);

    AnswerEvent finalize = make_event(3);
    finalize.kind = AnswerKind::finalize;
    finalize.question_id.clear();
    finalize.answer.clear();
    chain_from(second, finalize);
    const auto finalized = journal.append(finalize, &error);
    assert(finalized.status == AppendStatus::accepted);

    AnswerEvent after_finalize = make_event(4);
    chain_from(finalize, after_finalize);
    const auto rejected = journal.append(after_finalize, &error);
    assert(rejected.status == AppendStatus::rejected);

    StateRequest request;
    request.package_id = first.package_id;
    request.package_digest = first.package_digest;
    request.client_id = first.client_id;
    request.session_id = first.session_id;
    request.candidate_id = first.candidate_id;
    const auto state = journal.state(request, &error);
    assert(state.has_value());
    assert(state->highest_contiguous_sequence == 3);
    assert(state->finalized);
    assert(state->answers.size() == 1);

    // Offline exports are framed so large snapshots can be copied and
    // independently validated without treating the file as a raw wire frame.
    const auto export_path = root / "answer-state.sex1";
    assert(journal.export_state(request, export_path, &error));
    const auto export_size = std::filesystem::file_size(export_path);
    std::vector<std::byte> export_bytes(static_cast<std::size_t>(export_size));
    {
        std::ifstream input(export_path, std::ios::binary);
        assert(input.good());
        assert(export_bytes.empty() ||
               input.read(reinterpret_cast<char*>(export_bytes.data()),
                          static_cast<std::streamsize>(export_bytes.size())));
    }
    const auto decoded_export = decode_state_export(export_bytes, &error);
    assert(decoded_export.has_value());
    assert(decoded_export->package_id == state->package_id);
    assert(decoded_export->highest_contiguous_sequence ==
           state->highest_contiguous_sequence);
    assert(decoded_export->finalized == state->finalized);
    assert(decoded_export->answers.size() == state->answers.size());
    auto truncated_export = export_bytes;
    truncated_export.pop_back();
    assert(!decode_state_export(truncated_export, &error).has_value());
    journal.close();

    AnswerJournal reopened;
    assert(reopened.open(journal_path, &error));
    const auto restored = reopened.state(request, &error);
    assert(restored.has_value());
    assert(restored->highest_contiguous_sequence == 3);
    assert(restored->finalized);
    reopened.close();

    // Only an incomplete final record may be discarded during recovery.
    {
        std::ofstream tail(journal_path, std::ios::binary | std::ios::app);
        tail.put(static_cast<char>(0x7f));
    }
    AnswerJournal tail_recovered;
    assert(tail_recovered.open(journal_path, &error));
    assert(tail_recovered.recovered_truncated_tail());
    tail_recovered.close();

    const auto outbox_path = root / "outbox.bin";
    AnswerOutbox outbox;
    assert(outbox.open(outbox_path, &error));
    assert(outbox.enqueue(first, &error));
    const auto first_hash = hash_answer_event(first);
    assert(first_hash.has_value());
    assert(outbox.acknowledge(first.session_id, first.sequence, *first_hash,
                              &error));
    assert(outbox.size() == 0);

    // The persisted watermark allows sequence two after sequence one was
    // acknowledged and removed from the pending queue.
    assert(outbox.enqueue(second, &error));
    outbox.close();

    AnswerOutbox reopened_outbox;
    assert(reopened_outbox.open(outbox_path, &error));
    const auto pending = reopened_outbox.pending();
    assert(pending.size() == 1);
    assert(pending.front().sequence == 2);
    const auto second_hash = hash_answer_event(second);
    assert(second_hash.has_value());
    assert(reopened_outbox.acknowledge(second.session_id, second.sequence,
                                       *second_hash, &error));
    assert(reopened_outbox.size() == 0);

    // A stale cumulative ACK cannot be verified after its event has been
    // removed, while replaying the exact durable boundary remains idempotent.
    assert(!reopened_outbox.acknowledge(first.session_id, first.sequence,
                                        *first_hash, &error));
    assert(reopened_outbox.acknowledge(second.session_id, second.sequence,
                                       *second_hash, &error));

    AnswerEvent invalid_gap = make_event(4);
    chain_from(second, invalid_gap);
    assert(!reopened_outbox.enqueue(invalid_gap, &error));
    reopened_outbox.close();

    // A v1 plaintext outbox had no watermark section. Opening a file with
    // multiple pending events must reconstruct the complete chain and then
    // migrate it to the encrypted v4 representation without dropping the
    // later event.
    const auto legacy_outbox_path = root / "legacy-v1-outbox.bin";
    std::vector<std::byte> legacy_wire;
    append_le(legacy_wire, std::uint32_t{0x3158424fu}); // "OBX1"
    append_le(legacy_wire, std::uint16_t{1});
    append_le(legacy_wire, std::uint32_t{2});
    for (const auto& event : {first, second}) {
        const auto payload = encode_answer_event(event);
        assert(!payload.empty());
        append_le(legacy_wire, static_cast<std::uint32_t>(payload.size()));
        legacy_wire.insert(legacy_wire.end(), payload.begin(), payload.end());
    }
    {
        std::ofstream legacy_file(legacy_outbox_path,
                                  std::ios::binary | std::ios::trunc);
        assert(legacy_file.good());
        legacy_file.write(
            reinterpret_cast<const char*>(legacy_wire.data()),
            static_cast<std::streamsize>(legacy_wire.size()));
        assert(legacy_file.good());
    }
    AnswerOutbox migrated_legacy;
    assert(migrated_legacy.open(legacy_outbox_path, &error));
    const auto migrated_pending = migrated_legacy.pending();
    assert(migrated_pending.size() == 2);
    assert(migrated_pending[0].sequence == 1);
    assert(migrated_pending[1].sequence == 2);
    migrated_legacy.close();
    {
        std::ifstream migrated_file(legacy_outbox_path, std::ios::binary);
        std::array<char, 4> prefix{};
        assert(migrated_file.read(
            prefix.data(), static_cast<std::streamsize>(prefix.size())));
        constexpr std::array<char, 4> expected_prefix{{'E', 'O', 'B', '1'}};
        assert(prefix == expected_prefix);
    }
    AnswerOutbox reopened_legacy;
    assert(reopened_legacy.open(legacy_outbox_path, &error));
    const auto reopened_legacy_pending = reopened_legacy.pending();
    assert(reopened_legacy_pending.size() == 2);
    assert(reopened_legacy_pending[0].sequence == 1);
    assert(reopened_legacy_pending[1].sequence == 2);
    reopened_legacy.close();

    // Finalization is durable even after the final event is acknowledged and
    // removed from the pending queue.
    const auto finalized_outbox_path = root / "finalized-outbox.bin";
    AnswerOutbox finalized_outbox;
    assert(finalized_outbox.open(finalized_outbox_path, &error));
    auto final_first = make_event(1);
    auto final_event = make_event(2);
    final_event.kind = AnswerKind::finalize;
    final_event.question_id.clear();
    final_event.answer.clear();
    chain_from(final_first, final_event);
    assert(finalized_outbox.enqueue(final_first, &error));
    assert(finalized_outbox.enqueue(final_event, &error));
    auto after_final = make_event(3);
    chain_from(final_event, after_final);
    assert(!finalized_outbox.enqueue(after_final, &error));
    const auto final_hash = hash_answer_event(final_event);
    assert(final_hash.has_value());
    assert(finalized_outbox.acknowledge(final_event.session_id,
                                        final_event.sequence, *final_hash,
                                        &error));
    assert(finalized_outbox.size() == 0);
    finalized_outbox.close();

    // The on-disk representation must be the DPAPI wrapper, not plaintext
    // answer records.
    {
        std::ifstream stored(finalized_outbox_path, std::ios::binary);
        std::array<char, 4> prefix{};
        assert(stored.read(prefix.data(),
                           static_cast<std::streamsize>(prefix.size())));
        constexpr std::array<char, 4> expected_prefix{{'E', 'O', 'B', '1'}};
        assert(prefix == expected_prefix);
    }

    AnswerOutbox reopened_finalized;
    assert(reopened_finalized.open(finalized_outbox_path, &error));
    assert(reopened_finalized.pending().empty());
    // Retrying the exact final event is idempotent, but a new sequence is not.
    assert(reopened_finalized.enqueue(final_event, &error));
    assert(!reopened_finalized.enqueue(after_final, &error));
    reopened_finalized.close();

    // A colliding SessionId across two independent contexts must resolve by
    // the event hash, not reject a valid ACK or choose an arbitrary context.
    const auto ambiguous_outbox_path = root / "ambiguous-outbox.bin";
    AnswerOutbox ambiguous_outbox;
    assert(ambiguous_outbox.open(ambiguous_outbox_path, &error));
    auto collision_a = make_event(1);
    auto collision_b = make_event(1);
    collision_b.package_id = "journal-test-other";
    fill_bytes(collision_b.client_id, 0x70);
    const auto collision_a_hash = hash_answer_event(collision_a);
    assert(collision_a_hash.has_value());
    assert(ambiguous_outbox.enqueue(collision_a, &error));
    assert(ambiguous_outbox.enqueue(collision_b, &error));
    assert(ambiguous_outbox.acknowledge(collision_a.session_id, 1,
                                        *collision_a_hash, &error));
    const auto collision_pending = ambiguous_outbox.pending();
    assert(collision_pending.size() == 1);
    assert(collision_pending.front().package_id == collision_b.package_id);
    const auto collision_b_hash = hash_answer_event(collision_b);
    assert(collision_b_hash.has_value());
    assert(ambiguous_outbox.acknowledge(collision_b.session_id, 1,
                                        *collision_b_hash, &error));
    assert(ambiguous_outbox.pending().empty());
    ambiguous_outbox.close();

    std::filesystem::remove_all(root, cleanup_error);
    return 0;
}
