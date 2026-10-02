#include "nstu/exam_control.hpp"

#include <cassert>
#include <cstddef>
#include <vector>

namespace {

nstu::exam::ExamStartRequest make_request() {
    nstu::exam::ExamStartRequest request;
    request.package_root = "C:/ProgramData/NSTU/exams/sample";
    request.web_root = "C:/ProgramData/NSTU/exams/sample/exam/web";
    request.user_data_root = "C:/ProgramData/NSTU/exam-user-data";
    request.package_id = "sample-package";
    request.candidate_id = "candidate-01";
    for (std::size_t index = 0; index < request.package_digest.size(); ++index) {
        request.package_digest[index] = static_cast<std::byte>(index + 1);
    }
    for (std::size_t index = 0; index < request.client_id.size(); ++index) {
        request.client_id[index] = static_cast<std::byte>(0xa0 + index);
        request.session_id[index] = static_cast<std::byte>(0x40 + index);
    }
    return request;
}

} // namespace

int main() {
    const auto original = make_request();
    assert(nstu::exam::validate_exam_start_request(original));
    const auto encoded = nstu::exam::encode_exam_start_request(original);
    assert(!encoded.empty());
    const auto decoded = nstu::exam::decode_exam_start_request(encoded);
    assert(decoded.has_value());
    assert(decoded->package_root == original.package_root);
    assert(decoded->web_root == original.web_root);
    assert(decoded->user_data_root == original.user_data_root);
    assert(decoded->package_id == original.package_id);
    assert(decoded->candidate_id == original.candidate_id);
    assert(decoded->package_digest == original.package_digest);
    assert(decoded->client_id == original.client_id);
    assert(decoded->session_id == original.session_id);

    auto truncated = encoded;
    truncated.pop_back();
    assert(!nstu::exam::decode_exam_start_request(truncated));

    auto trailing = encoded;
    trailing.push_back(std::byte{0});
    assert(!nstu::exam::decode_exam_start_request(trailing));

    auto invalid = encoded;
    // The EXS1 payload version starts immediately after the four-byte magic.
    invalid[4] = std::byte{0xff};
    assert(!nstu::exam::decode_exam_start_request(invalid));

    auto legacy = encoded;
    legacy[4] = std::byte{1};
    legacy[5] = std::byte{0};
    // Version 1 has no package identity and must never be authorized.
    assert(!nstu::exam::decode_exam_start_request(legacy));

    auto missing_wire_package_id = encoded;
    // Header offsets: magic(4), version(2), flags(2), then four u16 lengths.
    missing_wire_package_id[14] = std::byte{0};
    missing_wire_package_id[15] = std::byte{0};
    assert(!nstu::exam::decode_exam_start_request(missing_wire_package_id));

    auto path = original;
    path.package_root = "https://example.invalid/exam";
    // The codec deliberately validates shape/encoding, while the host owns
    // the absolute/local path policy. This remains a valid UTF-8 transport
    // value and is rejected later by ExamHost package inspection.
    assert(nstu::exam::validate_exam_start_request(path));

    path.candidate_id.assign(129, 'x');
    assert(!nstu::exam::validate_exam_start_request(path));
    assert(nstu::exam::encode_exam_start_request(path).empty());

    auto missing_package_id = original;
    missing_package_id.package_id.clear();
    assert(!nstu::exam::validate_exam_start_request(missing_package_id));
    assert(nstu::exam::encode_exam_start_request(missing_package_id).empty());

    auto oversized_package_id = original;
    oversized_package_id.package_id.assign(
        nstu::exam::kMaximumExamControlPackageIdBytes + 1, 'p');
    assert(!nstu::exam::validate_exam_start_request(oversized_package_id));
    assert(nstu::exam::encode_exam_start_request(oversized_package_id).empty());

    auto invalid_package_id = original;
    invalid_package_id.package_id = "bad\x01id";
    assert(!nstu::exam::validate_exam_start_request(invalid_package_id));
    assert(nstu::exam::encode_exam_start_request(invalid_package_id).empty());
    // Synchronized-start barrier: ERDY ready report round-trip + rejections.
    nstu::exam::ExamReadyReport ready;
    ready.package_id = original.package_id;
    ready.candidate_id = original.candidate_id;
    ready.package_digest = original.package_digest;
    ready.client_id = original.client_id;
    ready.session_id = original.session_id;
    assert(nstu::exam::validate_exam_ready_report(ready));
    const auto ready_wire = nstu::exam::encode_exam_ready_report(ready);
    assert(!ready_wire.empty());
    const auto ready_decoded = nstu::exam::decode_exam_ready_report(ready_wire);
    assert(ready_decoded.has_value());
    assert(ready_decoded->package_id == ready.package_id);
    assert(ready_decoded->candidate_id == ready.candidate_id);
    assert(ready_decoded->package_digest == ready.package_digest);
    assert(ready_decoded->client_id == ready.client_id);
    assert(ready_decoded->session_id == ready.session_id);
    auto ready_truncated = ready_wire;
    ready_truncated.pop_back();
    assert(!nstu::exam::decode_exam_ready_report(ready_truncated));
    auto ready_trailing = ready_wire;
    ready_trailing.push_back(std::byte{0});
    assert(!nstu::exam::decode_exam_ready_report(ready_trailing));
    auto ready_bad_version = ready_wire;
    ready_bad_version[4] = std::byte{0xff};
    assert(!nstu::exam::decode_exam_ready_report(ready_bad_version));
    // EBGN begin command: round-trip, cross-magic rejection, bounds.
    nstu::exam::ExamBeginCommand begin;
    begin.package_id = original.package_id;
    begin.candidate_id = original.candidate_id;
    begin.package_digest = original.package_digest;
    begin.client_id = original.client_id;
    begin.session_id = original.session_id;
    begin.server_start_unix_milliseconds = 1770000000000ull;
    begin.duration_seconds = 3600;
    assert(nstu::exam::validate_exam_begin_command(begin));
    const auto begin_wire = nstu::exam::encode_exam_begin_command(begin);
    assert(!begin_wire.empty());
    const auto begin_decoded = nstu::exam::decode_exam_begin_command(begin_wire);
    assert(begin_decoded.has_value());
    assert(begin_decoded->server_start_unix_milliseconds ==
           begin.server_start_unix_milliseconds);
    assert(begin_decoded->duration_seconds == begin.duration_seconds);
    assert(begin_decoded->package_id == begin.package_id);
    assert(begin_decoded->session_id == begin.session_id);
    // The two barrier messages must never be confused on the wire.
    assert(!nstu::exam::decode_exam_ready_report(begin_wire));
    assert(!nstu::exam::decode_exam_begin_command(ready_wire));
    // duration 0 means "use the manifest duration" and is accepted.
    auto begin_zero_duration = begin;
    begin_zero_duration.duration_seconds = 0;
    assert(nstu::exam::validate_exam_begin_command(begin_zero_duration));
    // Out-of-range duration and a zero start instant are rejected.
    auto begin_short_duration = begin;
    begin_short_duration.duration_seconds = 59;
    assert(!nstu::exam::validate_exam_begin_command(begin_short_duration));
    assert(nstu::exam::encode_exam_begin_command(begin_short_duration).empty());
    auto begin_zero_start = begin;
    begin_zero_start.server_start_unix_milliseconds = 0;
    assert(!nstu::exam::validate_exam_begin_command(begin_zero_start));
    return 0;
}
