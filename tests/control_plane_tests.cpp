#include "nstu/control_channel.hpp"
#include "nstu/control_messages.hpp"
#include "nstu/control_plane.hpp"
#include "nstu/exam_control.hpp"
#include "nstu/multicast.hpp"

#include <cassert>
#include <chrono>
#include <cstddef>
#include <string>
#include <thread>
#include <vector>

namespace {

nstu::security::ClientId client_id() {
    nstu::security::ClientId id{};
    for (std::size_t index = 0; index < id.size(); ++index) {
        id[index] = static_cast<std::byte>(index + 7);
    }
    return id;
}

std::vector<std::byte> client_key() {
    std::vector<std::byte> key(nstu::security::kMinimumProtocolKeyBytes);
    for (std::size_t index = 0; index < key.size(); ++index) {
        key[index] = static_cast<std::byte>(0x50 + index);
    }
    return key;
}

template <typename Predicate>
bool wait_until(Predicate predicate) {
    for (int attempt = 0; attempt < 200; ++attempt) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

} // namespace

int main() {
    nstu::net::WinsockRuntime winsock;
    assert(winsock.ready());
    const auto id = client_id();
    auto key = client_key();
    constexpr std::uint32_t key_id = 4;
    nstu::security::KeyStore key_store;
    std::string error;
    assert(key_store.enroll(id, key_id, key, &error));
    nstu::server::ClientRegistry registry;
    nstu::server::ServerControlPlane control_plane(registry, key_store);
    nstu::server::ServerControlPlaneConfig config;
    config.port = 0;
    config.maximum_clients = 64;
    assert(control_plane.start(std::move(config), &error));

    nstu::net::TcpSocket socket;
    assert(socket.connect("127.0.0.1", control_plane.local_port(), &error));
    assert(socket.set_io_timeouts(5000, 5000, &error));
    const auto now = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    auto session = nstu::control::client_handshake(
        socket, id, key_id, key, now, std::chrono::seconds(120), &error);
    assert(session.has_value());
    nstu::control::AuthenticatedControlChannel channel(
        std::move(socket), std::move(*session));
    assert(nstu::control::decode_uwf_configure_request(
               nstu::control::encode_uwf_configure_request(true)) == true);
    assert(nstu::control::decode_uwf_configure_request(
               nstu::control::encode_uwf_configure_request(false)) == false);
    nstu::control::UwfConfigureReport uwf_report{
        .outcome = nstu::control::UwfConfigureOutcome::armed,
        .reboot_required = true,
        .data_exclusion_ready = true,
        .registry_exclusion_ready = true,
        .detail = "UWF is armed",
    };
    const auto uwf_report_payload =
        nstu::control::encode_uwf_configure_report(uwf_report);
    const auto decoded_uwf_report =
        nstu::control::decode_uwf_configure_report(uwf_report_payload);
    assert(decoded_uwf_report.has_value());
    assert(decoded_uwf_report->outcome == uwf_report.outcome);
    assert(decoded_uwf_report->reboot_required);
    assert(decoded_uwf_report->data_exclusion_ready);
    assert(decoded_uwf_report->registry_exclusion_ready);
    assert(decoded_uwf_report->detail == uwf_report.detail);
    auto malformed_uwf_report = uwf_report_payload;
    malformed_uwf_report[1] |= std::byte{0x80};
    assert(!nstu::control::decode_uwf_configure_report(
                malformed_uwf_report).has_value());

    nstu::control::ClientStatusReport status;
    status.hostname = "LAB-PC-01";
    status.session_id = 3;
    status.latency_ms = 8;
    status.packet_loss_per_mille = 5;
    status.packet_loss_sample_size = 500;
    const auto status_payload = nstu::control::encode_status_report(status);
    assert(channel.send(nstu::protocol::CommandType::hello, 1,
                        status_payload, &error));
    assert(wait_until([&] {
        const auto snapshot = registry.snapshot();
        return snapshot.size() == 1 && snapshot[0].hostname == "LAB-PC-01";
    }));
    const auto clients = registry.snapshot();
    assert(clients.size() == 1);
    assert(clients[0].hostname == "LAB-PC-01");
    const auto registry_id = clients[0].id;

    assert(control_plane.set_locked(registry_id, true, &error));
    const auto lock = channel.receive(&error);
    assert(lock.has_value());
    assert(lock->envelope.type == nstu::protocol::CommandType::lock);

    assert(control_plane.set_frozen(registry_id, true, &error));
    const auto freeze = channel.receive(&error);
    assert(freeze.has_value());
    assert(freeze->envelope.type ==
           nstu::protocol::CommandType::freeze_set);
    assert(nstu::control::decode_freeze_state(freeze->payload) == true);
    assert(channel.send(
        nstu::protocol::CommandType::freeze_report, 2,
        nstu::control::encode_freeze_state(true), &error));
    assert(wait_until([&] {
        const auto snapshot = registry.snapshot();
        return !snapshot.empty() && snapshot[0].frozen;
    }));

    assert(control_plane.set_frozen(registry_id, false, &error));
    const auto thaw = channel.receive(&error);
    assert(thaw.has_value());
    assert(thaw->envelope.type ==
           nstu::protocol::CommandType::freeze_set);
    assert(nstu::control::decode_freeze_state(thaw->payload) == false);
    assert(channel.send(
        nstu::protocol::CommandType::freeze_report, 3,
        nstu::control::encode_freeze_state(false), &error));
    assert(wait_until([&] {
        const auto snapshot = registry.snapshot();
        return !snapshot.empty() && !snapshot[0].frozen;
    }));

    assert(control_plane.configure_uwf(registry_id, true, &error));
    const auto uwf_command = channel.receive(&error);
    assert(uwf_command.has_value());
    assert(uwf_command->envelope.type ==
           nstu::protocol::CommandType::uwf_configure);
    assert(nstu::control::decode_uwf_configure_request(
               uwf_command->payload) == true);
    assert(channel.send(nstu::protocol::CommandType::uwf_report, 4,
                        uwf_report_payload, &error));
    assert(wait_until([&] {
        const auto snapshot = registry.snapshot();
        return !snapshot.empty() && snapshot[0].uwf.reported &&
               snapshot[0].uwf.phase ==
                   nstu::control::UwfFleetPhase::awaiting_restart;
    }));
    // The single-client report describes an attempt, never a proof, so it must
    // not open the exam gate.
    assert(!registry.snapshot()[0].uwf.proves_current_protection());

    assert(control_plane.set_snapshots(registry_id, true, 7, &error));
    const auto snapshots = channel.receive(&error);
    assert(snapshots.has_value());
    assert(snapshots->envelope.type ==
           nstu::protocol::CommandType::start_snapshots);
    assert(nstu::control::decode_snapshot_schedule(snapshots->payload) == 7);

    const nstu::control::OverlayStroke stroke{
        .x0 = 10,
        .y0 = 20,
        .x1 = 30,
        .y1 = 40,
        .thickness = 3,
        .rgba = 0xe5484dffu,
    };
    assert(control_plane.send_overlay_stroke(registry_id, stroke, &error));
    const auto overlay = channel.receive(&error);
    assert(overlay.has_value());
    assert(overlay->envelope.type ==
           nstu::protocol::CommandType::overlay_stroke);
    assert(nstu::control::decode_overlay_stroke(overlay->payload).has_value());

    assert(control_plane.start_remote_control(registry_id, &error));
    const auto remote_start = channel.receive(&error);
    assert(remote_start.has_value());
    assert(remote_start->envelope.type ==
           nstu::protocol::CommandType::remote_start);
    const nstu::wire::RemoteInputPacket remote_input{
        .input_type = static_cast<std::uint8_t>(nstu::wire::RemoteInputType::keyboard),
        .flags = 0,
        .x = 0,
        .y = 0,
        .virtual_key = 0x74,
        .reserved = 0,
        .mouse_data = 0,
    };
    assert(control_plane.send_remote_input(registry_id, remote_input, &error));
    const auto remote_event = channel.receive(&error);
    assert(remote_event.has_value());
    assert(remote_event->envelope.type ==
           nstu::protocol::CommandType::remote_input);
    assert(remote_event->payload.size() == sizeof(remote_input));
    assert(control_plane.stop_remote_control(registry_id, &error));
    const auto remote_end = channel.receive(&error);
    assert(remote_end.has_value());
    assert(remote_end->envelope.type ==
           nstu::protocol::CommandType::remote_end);

    nstu::exam::ExamStartRequest exam_start;
    exam_start.package_root = "C:/ProgramData/NSTU/exams/sample";
    exam_start.web_root = "C:/ProgramData/NSTU/exams/sample/exam/web";
    exam_start.user_data_root = "C:/ProgramData/NSTU/exam-user-data";
    exam_start.package_id = "sample-package";
    exam_start.candidate_id = "candidate-01";
    exam_start.client_id = id;
    for (std::size_t index = 0; index < exam_start.package_digest.size(); ++index) {
        exam_start.package_digest[index] = static_cast<std::byte>(index + 1);
    }
    for (std::size_t index = 0; index < exam_start.session_id.size(); ++index) {
        exam_start.session_id[index] = static_cast<std::byte>(index + 0x10);
    }
    auto wrong_identity = exam_start;
    wrong_identity.client_id[0] = std::byte{0xff};
    // Identity is checked in every channel, so a mismatched request is refused
    // even by the DEV build.
    assert(!control_plane.start_exam(registry_id, wrong_identity, &error));
#if NSTU_DEV_UNPROTECTED_EXAM
    // The DEV (UNPROTECTED) build bypasses only the readiness gate: a correctly
    // formed request starts even with no proven current-session protection.
    assert(control_plane.start_exam(registry_id, exam_start, &error));
#else
    // Release fails closed: without proven current-session UWF protection a
    // correctly formed request is refused, then permitted once a verified
    // report with a completed probe on a known boot proves protection.
    assert(!control_plane.start_exam(registry_id, exam_start, &error));
    nstu::control::UwfFleetStatusReport verified;
    verified.phase = nstu::control::UwfFleetPhase::verified_protected;
    verified.probe.probe_succeeded = true;
    verified.probe.filter_current_enabled = true;
    verified.probe.system_volume_current_protected = true;
    for (std::size_t index = 0; index < verified.boot_id.size(); ++index) {
        verified.boot_id[index] = static_cast<std::byte>(0xa0 + index);
    }
    verified.detail = "UWF protects this session";
    assert(registry.set_uwf_fleet_status(registry_id, verified));
    assert(registry.snapshot()[0].uwf.proves_current_protection());
    assert(control_plane.start_exam(registry_id, exam_start, &error));
#endif
    const auto exam_start_command = channel.receive(&error);
    assert(exam_start_command.has_value());
    assert(exam_start_command->envelope.type ==
           nstu::protocol::CommandType::exam_start);
    const auto decoded_exam_start =
        nstu::exam::decode_exam_start_request(exam_start_command->payload);
    assert(decoded_exam_start.has_value());
    assert(decoded_exam_start->client_id == id);
    assert(decoded_exam_start->package_id == exam_start.package_id);

    assert(control_plane.stop_exam(registry_id, &error));
    const auto exam_stop_command = channel.receive(&error);
    assert(exam_stop_command.has_value());
    assert(exam_stop_command->envelope.type ==
           nstu::protocol::CommandType::exam_stop);
    assert(exam_stop_command->payload.empty());

    nstu::control::SnapshotFrame host_frame;
    host_frame.width = 320;
    host_frame.height = 180;
    host_frame.captured_at_unix_milliseconds = 100;
    host_frame.jpeg = {std::byte{0xff}, std::byte{0xd8}, std::byte{0xff},
                       std::byte{0xd9}};
    assert(control_plane.broadcast_host_snapshot(host_frame, &error));
    const auto host_snapshot = channel.receive(&error);
    assert(host_snapshot.has_value());
    assert(host_snapshot->envelope.type ==
           nstu::protocol::CommandType::host_snapshot);
    assert(nstu::control::decode_snapshot_frame(host_snapshot->payload)
               .has_value());

    status.locked = true;
    const auto locked_payload = nstu::control::encode_status_report(status);
    assert(channel.send(nstu::protocol::CommandType::status_report, 5,
                        locked_payload, &error));
    assert(wait_until([&] {
        const auto snapshot = registry.snapshot();
        return !snapshot.empty() &&
               snapshot[0].status == nstu::server::ClientStatus::locked;
    }));

    channel.close();
    assert(wait_until([&] {
        const auto snapshot = registry.snapshot();
        return !snapshot.empty() &&
               snapshot[0].status == nstu::server::ClientStatus::offline;
    }));
    control_plane.stop();

    // Server-name plumbing (the room label carried on the pairing beacon). It
    // is a display/routing hint, so it is sanitized to the discovery name bound
    // but never trimmed, and it clears when the plane stops. No sockets are
    // exercised here: server_name() reflects exactly what start() and
    // set_server_name() stored.
    {
        nstu::server::ClientRegistry naming_registry;
        nstu::security::KeyStore naming_key_store;
        nstu::server::ServerControlPlane naming_plane(naming_registry,
                                                      naming_key_store);
        nstu::server::ServerControlPlaneConfig naming_config;
        naming_config.port = 0;
        // A control character in the configured name is replaced; printable
        // ASCII, interior spaces included, is preserved.
        naming_config.server_name = "Lab 7\tRoom";
        assert(naming_plane.start(std::move(naming_config), &error));
        assert(naming_plane.server_name() == "Lab 7?Room");

        // set_server_name re-sanitizes but does not trim: surrounding spaces
        // are kept, because a room label is not a credential to normalize.
        naming_plane.set_server_name("  Room 12  ");
        assert(naming_plane.server_name() == "  Room 12  ");

        // Over-long names are clamped to the discovery name bound (64 bytes).
        naming_plane.set_server_name(std::string(200, 'R'));
        assert(naming_plane.server_name().size() == 64);

        // Stopping clears the advertised name so a restart re-derives it.
        naming_plane.stop();
        assert(naming_plane.server_name().empty());
    }

    // An empty configured name leaves server_name() empty; the beacon then
    // falls back to the computer name at the call site.
    {
        nstu::server::ClientRegistry unnamed_registry;
        nstu::security::KeyStore unnamed_key_store;
        nstu::server::ServerControlPlane unnamed_plane(unnamed_registry,
                                                       unnamed_key_store);
        nstu::server::ServerControlPlaneConfig unnamed_config;
        unnamed_config.port = 0;
        assert(unnamed_plane.start(std::move(unnamed_config), &error));
        assert(unnamed_plane.server_name().empty());
        unnamed_plane.stop();
    }

    nstu::security::secure_zero(key);
    return 0;
}
