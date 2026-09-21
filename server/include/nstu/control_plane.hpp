#pragma once

#include "nstu/client_registry.hpp"
#include "nstu/exam_control.hpp"
#include "nstu/exam_sync.hpp"
#include "nstu/iocp_dispatcher.hpp"
#include "nstu/key_store.hpp"
#include "nstu/protocol_headers.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nstu::server {

struct ServerControlPlaneConfig {
    std::uint16_t port = 47001;
    std::wstring keyring_path;
    // Persistent server-owned exam journal. This is intentionally separate
    // from any client UWF/Deep Freeze data root.
    std::filesystem::path exam_journal_path;
    std::vector<std::byte> keyring_entropy;
    std::vector<std::byte> enrollment_secret;
    std::size_t maximum_clients = 512;
};

// One client waiting for the teacher to approve it. The six-digit code is the
// whole security story: the operator only approves when it matches what the
// client machine is showing, which is what rules out a man in the middle.
struct PendingPairing {
    std::uint64_t pairing_id = 0;
    std::string client_uuid;
    std::string hostname;
    std::string address;
    std::string short_authentication_string;
    std::uint32_t seconds_remaining = 0;
};

class ServerControlPlane {
public:
    ServerControlPlane(ClientRegistry& registry,
                       security::KeyStore& key_store);
    ~ServerControlPlane();
    ServerControlPlane(const ServerControlPlane&) = delete;
    ServerControlPlane& operator=(const ServerControlPlane&) = delete;

    [[nodiscard]] bool start(ServerControlPlaneConfig config,
                             std::string* error = nullptr);
    void stop() noexcept;

    [[nodiscard]] bool send_command(
        std::uint64_t client_id, protocol::CommandType type,
        std::span<const std::byte> payload = {},
        std::string* error = nullptr);
    [[nodiscard]] bool set_locked(std::uint64_t client_id, bool locked,
                                  std::string* error = nullptr);
    [[nodiscard]] bool set_frozen(std::uint64_t client_id, bool frozen,
                                  std::string* error = nullptr);
    [[nodiscard]] bool configure_uwf(
        std::uint64_t client_id, bool checkpoint_acknowledged,
        std::string* error = nullptr);
    [[nodiscard]] bool configure_uwf_fleet(
        std::uint64_t client_id, bool checkpoint_acknowledged,
        bool restart_requested, std::string* error = nullptr);
    [[nodiscard]] bool set_streaming(std::uint64_t client_id, bool enabled,
                                     std::uint8_t frames_per_second,
                                     std::string* error = nullptr);
    [[nodiscard]] bool set_snapshots(std::uint64_t client_id, bool enabled,
                                     std::uint16_t interval_seconds,
                                     std::string* error = nullptr);
    [[nodiscard]] bool send_overlay_stroke(
        std::uint64_t client_id, const control::OverlayStroke& stroke,
        std::string* error = nullptr);
    [[nodiscard]] bool clear_overlay(std::uint64_t client_id,
                                     std::string* error = nullptr);
    [[nodiscard]] bool broadcast_host_snapshot(
        const control::SnapshotFrame& frame,
        std::string* error = nullptr);
    [[nodiscard]] bool stop_host_broadcast(std::string* error = nullptr);
    [[nodiscard]] bool request_keyframe(std::uint64_t client_id,
                                        std::string* error = nullptr);
    [[nodiscard]] bool send_chat(std::uint64_t client_id,
                                 std::string_view utf8_message,
                                 std::string* error = nullptr);
    [[nodiscard]] bool start_remote_control(std::uint64_t client_id,
                                            std::string* error = nullptr);
    [[nodiscard]] bool send_remote_input(
        std::uint64_t client_id, const wire::RemoteInputPacket& packet,
        std::string* error = nullptr);
    [[nodiscard]] bool stop_remote_control(std::uint64_t client_id,
                                            std::string* error = nullptr);
    // Starts/stops the client-side exam host over the authenticated control
    // channel. The server stores the package and answer journal; it never
    // creates a desktop overlay or applies reboot-to-restore itself.
    [[nodiscard]] bool start_exam(
        std::uint64_t client_id, const exam::ExamStartRequest& request,
        std::string* error = nullptr);
    [[nodiscard]] bool stop_exam(std::uint64_t client_id,
                                 std::string* error = nullptr);

    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] std::uint16_t local_port() const noexcept;
    [[nodiscard]] std::size_t authenticated_client_count() const noexcept;

    // Verified pairing. Nothing here happens without the operator: the beacon
    // that lets an unenrolled machine find this server is off until the window
    // is opened, and a pending request only becomes a key when approve is
    // called with the code matching the client screen.
    void set_pairing_window(bool open, std::string_view server_name = {});
    [[nodiscard]] bool pairing_window_open() const noexcept;
    [[nodiscard]] std::vector<PendingPairing> pending_pairings() const;
    [[nodiscard]] bool approve_pairing(std::uint64_t pairing_id,
                                       std::string* error = nullptr);
    [[nodiscard]] bool reject_pairing(std::uint64_t pairing_id,
                                      std::string* error = nullptr);
    // Drops requests the operator left unanswered. Safe to call every frame.
    std::size_t expire_pending_pairings();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nstu::server
