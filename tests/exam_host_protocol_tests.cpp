#include "nstu/agent_protocol.hpp"
#include "nstu/exam_bridge.hpp"
#include "nstu/exam_host.hpp"
#include "nstu/exam_profile.hpp"
#include "nstu/exam_sync.hpp"

#include <windows.h>

#include <array>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

template <typename Array>
void fill_bytes(Array& bytes, std::uint8_t first) {
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = static_cast<std::byte>(first + index);
    }
}

nstu::exam::AnswerEvent make_event() {
    nstu::exam::AnswerEvent event;
    event.package_id = "sample-package";
    fill_bytes(event.package_digest, 0x10);
    fill_bytes(event.client_id, 0x30);
    fill_bytes(event.session_id, 0x50);
    event.candidate_id = "candidate-01";
    event.question_id = "reading-01";
    event.question_revision = 2;
    event.sequence = 1;
    event.client_time_unix_milliseconds = 1'700'000'000'000ULL;
    event.kind = nstu::exam::AnswerKind::upsert;
    event.answer = {std::byte{'B'}};
    return event;
}

void assert_agent_round_trip(nstu::client::AgentMessageType type,
                             const std::vector<std::byte>& payload) {
    const auto wire = nstu::client::encode_agent_message({type, payload});
    assert(!wire.empty());
    const auto decoded = nstu::client::decode_agent_message(wire);
    assert(decoded.has_value());
    assert(decoded->type == type);
    assert(decoded->payload == payload);
}

void assert_path_policy_rejects(
    const std::filesystem::path& data_root,
    const std::filesystem::path& package_root,
    const std::filesystem::path& web_root,
    const std::filesystem::path& user_data_root) {
    std::string error;
    assert(!nstu::client::validate_exam_path_policy(
        data_root, package_root, web_root, user_data_root, &error));
    assert(!error.empty());
}

void exercise_exam_path_policy() {
    std::error_code error;
    const auto temporary = std::filesystem::temp_directory_path(error);
    assert(!error);
    const auto suffix = std::to_string(GetCurrentProcessId()) + "-" +
                        std::to_string(std::chrono::steady_clock::now()
                                           .time_since_epoch()
                                           .count());
    const auto root = temporary / ("nstu-exam-policy-" + suffix);
    std::filesystem::remove_all(root, error);
    assert(!error);

    const auto data_root = root / "data";
    const auto package_area = data_root / "exams";
    const auto package_root = package_area / "sample";
    const auto web_root = package_root / "exam" / "web";
    const auto user_area = data_root / "exam-user-data";
    const auto user_leaf = user_area / "session-01";
    const auto profile_root = root / "local-app-data" / "NSTU" /
                              "exam-webview";
    const auto profile_leaf = profile_root / "session-01";
    assert(std::filesystem::create_directories(web_root, error));
    assert(!error);
    assert(std::filesystem::create_directories(user_area, error));
    assert(!error);
    assert(std::filesystem::create_directories(profile_root, error));
    assert(!error);

    std::string policy_error;
    assert(nstu::client::validate_exam_path_policy(
        data_root, package_root, web_root, user_leaf, &policy_error));
    policy_error.clear();
    // An omitted web root resolves to package_root/exam/web.
    assert(nstu::client::validate_exam_path_policy(
        data_root, package_root, {}, user_leaf, &policy_error));
    policy_error.clear();
    // A not-yet-created session leaf is valid when its parent is controlled.
    assert(nstu::client::validate_exam_path_policy(
        data_root, package_root, web_root, user_area / "new-session",
        &policy_error));
    policy_error.clear();
    assert(nstu::client::validate_exam_user_data_path(
        profile_root, profile_leaf, &policy_error));
    policy_error.clear();
    assert(nstu::client::validate_exam_user_data_path(
        profile_root, profile_root / "new-session", &policy_error));
    std::filesystem::remove_all(profile_root, error);
    assert(!error);
    policy_error.clear();
    // The host creates the profile after validation; a missing policy root is
    // accepted when its canonical parent is still resolvable.
    assert(nstu::client::validate_exam_user_data_path(
        profile_root, profile_root / "first-session", &policy_error));

    const auto sibling_area = data_root / "exams-evil" / "sample";
    assert(std::filesystem::create_directories(sibling_area, error));
    assert(!error);
    assert_path_policy_rejects(data_root, sibling_area, {}, user_leaf);

    const auto outside_package = root / "outside-package";
    assert(std::filesystem::create_directories(outside_package, error));
    assert(!error);
    assert_path_policy_rejects(data_root, outside_package, {}, user_leaf);
    assert_path_policy_rejects(data_root, package_area, {}, user_leaf);
    assert_path_policy_rejects(data_root,
                               package_area / ".." / "outside-package", {},
                               user_leaf);

    const auto outside_web = package_root / ".." / "web-outside";
    assert(std::filesystem::create_directories(outside_web, error));
    assert(!error);
    assert_path_policy_rejects(data_root, package_root, outside_web, user_leaf);

    const auto sibling_user = data_root / "exam-user-data-evil" / "session";
    assert(std::filesystem::create_directories(sibling_user, error));
    assert(!error);
    assert_path_policy_rejects(data_root, package_root, web_root, sibling_user);
    assert_path_policy_rejects(data_root, package_root, web_root, user_area);

    assert_path_policy_rejects(data_root, package_root, web_root,
                               profile_leaf);
    policy_error.clear();
    assert(!nstu::client::validate_exam_user_data_path(
        profile_root, profile_root, &policy_error));
    assert(!policy_error.empty());
    policy_error.clear();
    assert(!nstu::client::validate_exam_user_data_path(
        profile_root, profile_root / ".." / "outside-profile", &policy_error));
    assert(!policy_error.empty());

    assert_path_policy_rejects(data_root, "relative-package", {}, user_leaf);

    std::filesystem::remove_all(root, error);
    assert(!error);
}

void exercise_exam_profile_isolation() {
    const auto root = std::filesystem::path(L"C:\\Users\\student\\AppData\\Local\\NSTU\\exam-webview");
    nstu::security::Sha256Digest package_digest{};
    nstu::security::ClientId client_id{};
    nstu::exam::SessionId session_id{};
    fill_bytes(package_digest, 0x10);
    fill_bytes(client_id, 0x40);
    fill_bytes(session_id, 0x70);

    const auto first = nstu::client::derive_exam_profile_path(
        root, package_digest, client_id, session_id);
    assert(first.has_value());
    assert(first->parent_path() == root);
    const auto leaf = first->filename().string();
    assert(leaf.starts_with("v2-"));
    assert(leaf.size() == 3 + 64 + 1 + 32 + 1 + 32);
    assert(leaf.find_first_not_of("v0123456789abcdef-") == std::string::npos);

    // The same authenticated context intentionally reuses its profile so a
    // reconnect can recover browser-local durable state.
    const auto same = nstu::client::derive_exam_profile_path(
        root, package_digest, client_id, session_id);
    assert(same == first);

    auto other_package = package_digest;
    other_package[0] ^= std::byte{0x01};
    assert(nstu::client::derive_exam_profile_path(
               root, other_package, client_id, session_id) != first);
    auto other_client = client_id;
    other_client[0] ^= std::byte{0x01};
    assert(nstu::client::derive_exam_profile_path(
               root, package_digest, other_client, session_id) != first);
    auto other_session = session_id;
    other_session[0] ^= std::byte{0x01};
    assert(nstu::client::derive_exam_profile_path(
               root, package_digest, client_id, other_session) != first);

    assert(!nstu::client::derive_exam_profile_path(
        std::filesystem::path(L"relative"), package_digest, client_id,
        session_id));
    auto zero_digest = package_digest;
    zero_digest.fill(std::byte{0});
    assert(!nstu::client::derive_exam_profile_path(
        root, zero_digest, client_id, session_id));
}

void exercise_exam_package_inspection() {
    std::error_code error;
    const auto temporary = std::filesystem::temp_directory_path(error);
    assert(!error);
    const auto suffix = std::to_string(GetCurrentProcessId()) + "-" +
                        std::to_string(std::chrono::steady_clock::now()
                                           .time_since_epoch()
                                           .count());
    const auto root = temporary / ("nstu-exam-package-" + suffix);
    const auto package = root / "sample";
    const auto web = package / "exam" / "web";
    assert(std::filesystem::create_directories(web, error));
    assert(!error);

    const std::string manifest = R"json({
  "id": "inspection-sample",
  "title": "Inspection sample",
  "durationSeconds": 600,
  "questions": [{
    "id": "q1",
    "type": "short_answer",
    "prompt": "Answer"
  }]
})json";
    {
        std::ofstream manifest_file(package / "manifest.json",
                                    std::ios::binary);
        assert(manifest_file);
        manifest_file << manifest;
    }
    {
        std::ofstream page_file(web / "index.html", std::ios::binary);
        assert(page_file);
        page_file << "<!doctype html><title>sample</title>";
    }
    {
        std::ofstream asset_file(web / "app.js", std::ios::binary);
        assert(asset_file);
        asset_file << "document.body.dataset.ready='1';";
    }

    nstu::client::ExamPackageReport report;
    std::string inspection_error;
    assert(nstu::client::inspect_exam_package(
        package, web, {}, false, report, &inspection_error));
    assert(report.package_root == std::filesystem::weakly_canonical(package));
    assert(report.web_root == std::filesystem::weakly_canonical(web));
    assert(report.manifest_json == manifest);
    assert(report.digest_hex.size() == 64);
    const auto pinned_digest = report.digest_hex;

    nstu::client::ExamPackageReport pinned;
    inspection_error.clear();
    assert(nstu::client::inspect_exam_package(
        package, web, pinned_digest, true, pinned, &inspection_error));
    assert(pinned.digest_hex == pinned_digest);

    {
        std::ofstream page_file(web / "index.html", std::ios::binary |
                                             std::ios::trunc);
        assert(page_file);
        page_file << "<!doctype html><title>tampered</title>";
    }
    nstu::client::ExamPackageReport tampered;
    inspection_error.clear();
    assert(!nstu::client::inspect_exam_package(
        package, web, pinned_digest, true, tampered, &inspection_error));
    assert(!inspection_error.empty());

    // The package root itself must never become the WebView web root: that
    // would expose manifest.json and non-web package data through the mapped
    // origin. The host requires a child directory such as exam/web.
    inspection_error.clear();
    assert(!nstu::client::inspect_exam_package(
        package, package, {}, false, tampered, &inspection_error));
    assert(!inspection_error.empty());

    std::filesystem::remove_all(root, error);
    assert(!error);
}

void exercise_exam_manifest_asset_validation() {
    std::error_code error;
    const auto temporary = std::filesystem::temp_directory_path(error);
    assert(!error);
    const auto suffix = std::to_string(GetCurrentProcessId()) + "-" +
                        std::to_string(std::chrono::steady_clock::now()
                                           .time_since_epoch()
                                           .count());
    const auto root = temporary / ("nstu-exam-assets-" + suffix);
    const auto package = root / "sample";
    const auto web = package / "exam" / "web";
    const auto media = package / "media";
    const auto documents = package / "documents";
    assert(std::filesystem::create_directories(web, error));
    assert(!error);
    assert(std::filesystem::create_directories(media, error));
    assert(!error);
    assert(std::filesystem::create_directories(documents, error));
    assert(!error);

    {
        std::ofstream page_file(web / "index.html", std::ios::binary);
        assert(page_file);
        page_file << "<!doctype html><title>assets</title>";
    }
    {
        std::ofstream audio_file(media / "listening.mp3", std::ios::binary);
        assert(audio_file);
        audio_file << "audio";
    }
    {
        std::ofstream document_file(documents / "reference.pdf",
                                    std::ios::binary);
        assert(document_file);
        document_file << "%PDF-1.4\n";
    }

    const auto write_manifest = [&](std::string_view audio,
                                    std::string_view document) {
        std::ofstream manifest_file(package / "manifest.json",
                                    std::ios::binary | std::ios::trunc);
        assert(manifest_file);
        manifest_file << R"json({
  "id": "inspection-assets",
  "title": "Inspection assets",
  "durationSeconds": 600,
  "questions": [{
    "id": "q1",
    "type": "listening",
    "prompt": "Listen",
    "audio": ")json"
                      << audio << R"json(",
    "options": ["A", "B"]
  }],
  "documents": [{"url": ")json"
                      << document << R"json("}]
})json";
    };

    write_manifest("media/listening.mp3", "documents/reference.pdf");
    nstu::client::ExamPackageReport report;
    std::string inspection_error;
    assert(nstu::client::inspect_exam_package(
        package, web, {}, false, report, &inspection_error));

    write_manifest("media/missing.mp3", "documents/reference.pdf");
    inspection_error.clear();
    assert(!nstu::client::inspect_exam_package(
        package, web, {}, false, report, &inspection_error));
    assert(inspection_error.find("missing asset") != std::string::npos);

    // Deployment metadata must never become browser-readable merely because a
    // package manifest names it as an ordinary media/document path.
    write_manifest("MANIFEST.JSON", "documents/reference.pdf");
    inspection_error.clear();
    assert(!nstu::client::inspect_exam_package(
        package, web, {}, false, report, &inspection_error));
    assert(!inspection_error.empty());

    write_manifest("media/listening.mp3", "MANIFEST.P7S");
    inspection_error.clear();
    assert(!nstu::client::inspect_exam_package(
        package, web, {}, false, report, &inspection_error));
    assert(!inspection_error.empty());

    std::filesystem::remove_all(root, error);
    assert(!error);
}

} // namespace

int main() {
    using namespace nstu;

    exercise_exam_path_policy();
    exercise_exam_profile_isolation();
    exercise_exam_package_inspection();
    exercise_exam_manifest_asset_validation();

    const auto event = make_event();
    const auto event_hash = exam::hash_answer_event(event);
    assert(event_hash.has_value());
    const auto event_payload = exam::encode_answer_event(event);
    assert(!event_payload.empty());
    assert(exam::decode_answer_event(event_payload).has_value());
    assert_agent_round_trip(client::AgentMessageType::exam_answer_event,
                            event_payload);

    exam::StateRequest request;
    request.package_id = event.package_id;
    request.package_digest = event.package_digest;
    request.client_id = event.client_id;
    request.session_id = event.session_id;
    request.candidate_id = event.candidate_id;
    const auto request_payload = exam::encode_state_request(request);
    assert(!request_payload.empty());
    const auto decoded_request = exam::decode_state_request(request_payload);
    assert(decoded_request.has_value());
    assert(decoded_request->package_id == request.package_id);
    assert(decoded_request->candidate_id == request.candidate_id);
    assert_agent_round_trip(client::AgentMessageType::exam_state_request,
                            request_payload);

    exam::AnswerAck ack;
    ack.status = exam::AnswerAckStatus::accepted;
    ack.session_id = event.session_id;
    ack.sequence = 1;
    ack.highest_contiguous_sequence = 1;
    ack.server_time_unix_milliseconds = 1'700'000'000'100ULL;
    ack.event_hash = *event_hash;
    ack.state_hash[0] = std::byte{0xa1};
    const auto ack_payload = exam::encode_answer_ack(ack);
    assert(!ack_payload.empty());
    const auto decoded_ack = exam::decode_answer_ack(ack_payload);
    assert(decoded_ack.has_value());
    assert(decoded_ack->status == exam::AnswerAckStatus::accepted);
    assert(decoded_ack->event_hash == ack.event_hash);
    assert_agent_round_trip(client::AgentMessageType::exam_answer_ack,
                            ack_payload);

    exam::StateResponse response;
    response.package_id = event.package_id;
    response.package_digest = event.package_digest;
    response.client_id = event.client_id;
    response.session_id = event.session_id;
    response.candidate_id = event.candidate_id;
    response.highest_contiguous_sequence = 1;
    response.state_hash = ack.state_hash;
    response.last_event_hash = *event_hash;
    response.answers.push_back({
        .question_id = event.question_id,
        .question_revision = event.question_revision,
        .sequence = event.sequence,
        .kind = exam::AnswerKind::upsert,
        .answer = event.answer,
        .event_hash = *event_hash,
    });
    const auto state_payload = exam::encode_state_response(response);
    assert(!state_payload.empty());
    const auto decoded_state = exam::decode_state_response(state_payload);
    assert(decoded_state.has_value());
    assert(decoded_state->answers.size() == 1);
    assert(decoded_state->answers.front().answer == event.answer);
    assert_agent_round_trip(client::AgentMessageType::exam_state_response,
                            state_payload);

    // Only service responses are allowed into the agent-side host bridge.
    // Browser-originated events and state requests must never be presented as
    // host commands, even when their wire framing is valid.
    client::ExamBridge bridge;
    assert(!bridge.publish({client::AgentMessageType::exam_answer_event,
                            event_payload}));
    assert(!bridge.publish({client::AgentMessageType::exam_state_request,
                            request_payload}));
    assert(bridge.publish({client::AgentMessageType::exam_answer_ack,
                           ack_payload}));
    assert(bridge.publish({client::AgentMessageType::exam_state_response,
                           state_payload}));
    const auto bridge_ack = bridge.try_pop();
    assert(bridge_ack.has_value());
    assert(bridge_ack->type == client::AgentMessageType::exam_answer_ack);
    const auto bridge_state = bridge.try_pop();
    assert(bridge_state.has_value());
    assert(bridge_state->type == client::AgentMessageType::exam_state_response);
    assert(!bridge.try_pop().has_value());

    // Framing failures are rejected before a host can inspect the payload.
    assert(!client::decode_agent_message({}).has_value());
    auto truncated = client::encode_agent_message(
        {client::AgentMessageType::exam_answer_ack, ack_payload});
    assert(!truncated.empty());
    truncated.pop_back();
    assert(!client::decode_agent_message(truncated).has_value());

    auto wrong_magic = client::encode_agent_message(
        {client::AgentMessageType::exam_answer_ack, ack_payload});
    wrong_magic[0] ^= std::byte{1};
    assert(!client::decode_agent_message(wrong_magic).has_value());

    auto wrong_version = client::encode_agent_message(
        {client::AgentMessageType::exam_answer_ack, ack_payload});
    wrong_version[4] ^= std::byte{1};
    assert(!client::decode_agent_message(wrong_version).has_value());

    auto unknown_type = client::encode_agent_message(
        {client::AgentMessageType::exam_answer_ack, ack_payload});
    unknown_type[6] = std::byte{0xff};
    unknown_type[7] = std::byte{0xff};
    assert(!client::decode_agent_message(unknown_type).has_value());

    auto trailing = client::encode_agent_message(
        {client::AgentMessageType::exam_answer_ack, ack_payload});
    trailing.push_back(std::byte{0});
    assert(!client::decode_agent_message(trailing).has_value());

    client::AgentMessage oversized{
        client::AgentMessageType::exam_answer_ack,
        std::vector<std::byte>(client::kMaximumAgentPayloadBytes + 1)};
    assert(client::encode_agent_message(oversized).empty());

    // A valid IPC frame does not make an invalid exam payload trustworthy;
    // the host must still decode the typed payload before using it.
    auto malformed_event = event_payload;
    malformed_event.pop_back();
    const auto malformed_frame = client::encode_agent_message(
        {client::AgentMessageType::exam_answer_event, malformed_event});
    assert(!malformed_frame.empty());
    const auto framed_event = client::decode_agent_message(malformed_frame);
    assert(framed_event.has_value());
    assert(!exam::decode_answer_event(framed_event->payload).has_value());

    return 0;
}
