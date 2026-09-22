#include "nstu/audit.hpp"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using nstu::audit::Category;
using nstu::audit::Event;
using nstu::audit::Record;
using nstu::audit::Severity;

Record make_record(std::uint64_t sequence) {
    Record record;
    record.sequence = sequence;
    record.timestamp_unix_milliseconds = 1700000000000ull + sequence;
    record.event.category = Category::exam;
    record.event.severity = Severity::warning;
    record.event.component = "exam_gate";
    record.event.action = "start_blocked";
    record.event.result = "unprotected";
    record.event.operation_id = 42;
    record.event.client_id = 7;
    record.event.detail = "client has not proven current-session protection";
    return record;
}

void codec_round_trip() {
    std::vector<Record> records;
    for (std::uint64_t index = 1; index <= 5; ++index) {
        records.push_back(make_record(index));
    }
    const auto payload = nstu::audit::encode_audit_chunk(records);
    assert(!payload.empty());
    const auto decoded = nstu::audit::decode_audit_chunk(payload);
    assert(decoded.has_value());
    assert(decoded->size() == records.size());
    for (std::size_t index = 0; index < records.size(); ++index) {
        assert((*decoded)[index].sequence == records[index].sequence);
        assert((*decoded)[index].event.category == Category::exam);
        assert((*decoded)[index].event.severity == Severity::warning);
        assert((*decoded)[index].event.operation_id == 42);
        assert((*decoded)[index].event.client_id == 7);
        assert((*decoded)[index].event.action == "start_blocked");
    }

    const auto ack = nstu::audit::encode_audit_ack(9);
    const auto ack_decoded = nstu::audit::decode_audit_ack(ack);
    assert(ack_decoded.has_value());
    assert(*ack_decoded == 9);
}

void codec_rejections() {
    const auto payload = nstu::audit::encode_audit_chunk(
        std::vector<Record>{make_record(1)});
    assert(!payload.empty());

    // Wrong version.
    auto bad_version = payload;
    bad_version[0] = std::byte{2};
    assert(!nstu::audit::decode_audit_chunk(bad_version).has_value());

    // Count larger than the hard maximum.
    auto huge_count = payload;
    huge_count[1] = std::byte{0xff};
    huge_count[2] = std::byte{0xff};
    assert(!nstu::audit::decode_audit_chunk(huge_count).has_value());

    // Trailing byte.
    auto trailing = payload;
    trailing.push_back(std::byte{0});
    assert(!nstu::audit::decode_audit_chunk(trailing).has_value());

    // Truncated.
    auto truncated = payload;
    truncated.pop_back();
    assert(!nstu::audit::decode_audit_chunk(truncated).has_value());

    assert(!nstu::audit::decode_audit_chunk({}).has_value());

    // Ack must be exactly version + u64.
    auto ack = nstu::audit::encode_audit_ack(3);
    ack.pop_back();
    assert(!nstu::audit::decode_audit_ack(ack).has_value());
    auto ack_trailing = nstu::audit::encode_audit_ack(3);
    ack_trailing.push_back(std::byte{0});
    assert(!nstu::audit::decode_audit_ack(ack_trailing).has_value());
}

void redaction_holds() {
    // A caller mistakenly puts secrets and raw identifiers in a field. None of
    // it may survive into the encoded chunk or the formatted sink line.
    Record record = make_record(1);
    record.event.detail =
        "password=hunter2 key at C:/Users/teacher/secret.pem host 10.0.0.5";
    record.event.result = "token=abcdef0123456789abcdef0123456789";

    const std::string line = nstu::audit::format_record_line(record);
    assert(line.find("hunter2") == std::string::npos);
    assert(line.find("10.0.0.5") == std::string::npos);
    assert(line.find("C:/Users") == std::string::npos);
    assert(line.find("abcdef0123456789") == std::string::npos);
    // The record must remain exactly one line.
    assert(line.find('\n') == line.size() - 1);
    assert(line.find('\n') != std::string::npos);

    const auto payload = nstu::audit::encode_audit_chunk(
        std::vector<Record>{record});
    const auto decoded = nstu::audit::decode_audit_chunk(payload);
    assert(decoded.has_value());
    assert((*decoded)[0].event.detail.find("hunter2") == std::string::npos);
    assert((*decoded)[0].event.detail.find("10.0.0.5") == std::string::npos);

    // Control characters cannot break the one-line invariant either.
    Record control = make_record(2);
    control.event.detail = "line1\nline2\ttabbed";
    const std::string control_line = nstu::audit::format_record_line(control);
    assert(control_line.find("line1\nline2") == std::string::npos);
}

void spool_bounds_and_acks() {
    nstu::audit::Spool spool(3);
    Event event;
    event.category = Category::system;
    event.severity = Severity::info;
    event.component = "svc";
    event.action = "tick";
    const auto s1 = spool.emit(event);
    const auto s2 = spool.emit(event);
    const auto s3 = spool.emit(event);
    assert(s1 == 1 && s2 == 2 && s3 == 3);
    assert(spool.size() == 3);
    assert(spool.dropped_count() == 0);

    // Fourth emit drops the oldest, records the drop, and keeps capacity.
    const auto s4 = spool.emit(event);
    assert(s4 == 4);
    assert(spool.size() == 3);
    assert(spool.dropped_count() == 1);

    const auto chunk = spool.peek_chunk();
    assert(chunk.size() == 3);
    assert(chunk.front().sequence == 2);

    // Acknowledging through sequence 3 leaves only sequence 4.
    spool.acknowledge(3);
    assert(spool.size() == 1);
    const auto remaining = spool.peek_chunk();
    assert(remaining.size() == 1);
    assert(remaining.front().sequence == 4);
}

void sink_writes_and_rotates() {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("nstu-audit-test-" +
                      std::to_string(
                          std::chrono::steady_clock::now()
                              .time_since_epoch()
                              .count()));
    std::error_code ignored;
    std::filesystem::remove_all(dir, ignored);

    nstu::audit::Sink sink;
    assert(sink.open(dir));
    assert(sink.is_open());
    assert(sink.write(make_record(1)));
    assert(sink.write(make_record(2)));
    const auto active = dir / "audit.log";
    assert(std::filesystem::exists(active));

    // Force a rotation by inflating the active file past the size cap, then
    // writing one more record. The oversized file must move to audit.1.log and
    // the new record must land in a fresh, small audit.log.
    {
        std::ofstream filler(active, std::ios::binary | std::ios::app);
        const std::string block(1024, 'x');
        for (std::uint64_t written = 0;
             written < nstu::audit::kMaximumSinkFileBytes + 1024;
             written += block.size()) {
            filler.write(block.data(),
                         static_cast<std::streamsize>(block.size()));
        }
    }
    assert(sink.write(make_record(3)));
    assert(std::filesystem::exists(dir / "audit.1.log"));
    assert(std::filesystem::file_size(active) <
           nstu::audit::kMaximumSinkFileBytes);
    sink.close();
    assert(!sink.is_open());
    std::filesystem::remove_all(dir, ignored);
}

void rate_limiter_throttles() {
    nstu::audit::UploadRateLimiter limiter;
    const std::uint64_t base = 1000;
    // Burst of three from a cold bucket.
    assert(limiter.allow(1, base));
    assert(limiter.allow(1, base));
    assert(limiter.allow(1, base));
    // Fourth within the same instant is refused.
    assert(!limiter.allow(1, base));
    // Five seconds later one token has refilled.
    assert(limiter.allow(1, base + 5000));
    assert(!limiter.allow(1, base + 5000));
    // A different client has its own independent bucket.
    assert(limiter.allow(2, base));
}

} // namespace

int main() {
    codec_round_trip();
    codec_rejections();
    redaction_holds();
    spool_bounds_and_acks();
    sink_writes_and_rotates();
    rate_limiter_throttles();
    return 0;
}
