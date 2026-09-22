// The client half of verified pairing, driven end to end against the real
// control plane. A machine that holds no key runs the whole exchange itself,
// puts six digits on its own screen, and only an operator approval on the
// other side turns that into a key it can authenticate with.
#include "nstu/client_pairing.hpp"
#include "nstu/control_channel.hpp"
#include "nstu/control_plane.hpp"
#include "nstu/keyring.hpp"
#include "nstu/multicast.hpp"

#include <windows.h>

#include <array>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr const char* kClientUuid = "2D74B9C1-5A08-4F3E-8B62-19E7C4A05D3F";
constexpr const char* kClientHostname = "LAB-PC-11";
constexpr const char* kServerName = "Lab A";

// What one attempt produced, collected where the worker thread can fill it in
// and the main thread can read it once the thread has been joined.
struct PairingRun {
    nstu::client::PairingAttemptResult result;
    std::string code;
    std::string server_name;
    std::string error;
};

nstu::client::PairingAttemptOptions test_options() {
    nstu::client::PairingAttemptOptions options;
    options.approval_timeout = std::chrono::seconds(20);
    options.connect_timeout_ms = 2000;
    // Far shorter than production so a test spends a second, not a minute,
    // proving the keep-alive path is exercised at all.
    options.heartbeat_interval_ms = 200;
    return options;
}

void run_pairing(const nstu::discovery::PairingCandidate& candidate,
                 PairingRun& run, std::stop_token token) {
    run.result = nstu::client::pair_with_server(
        candidate, kClientUuid, kClientHostname,
        [&run](const std::string& code, const std::string& server_name) {
            run.code = code;
            run.server_name = server_name;
        },
        std::move(token), test_options(), &run.error);
}

std::vector<nstu::server::PendingPairing> wait_for_pending(
    const nstu::server::ServerControlPlane& server, std::size_t expected) {
    for (int attempt = 0; attempt < 200; ++attempt) {
        auto pending = server.pending_pairings();
        if (pending.size() == expected) {
            return pending;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return server.pending_pairings();
}

// The room preference is a routing hint applied before any key exists, so its
// matching is pure and worth pinning without a live sweep: trimmed,
// case-insensitive, empty means "no preference", and anything ambiguous refuses
// to guess. discovery::PairingCandidate::server_name carries the room the
// beacon advertised.
void test_room_candidate_selection() {
    using nstu::client::RoomSelection;
    using nstu::client::select_preferred_room_candidate;
    const std::array<nstu::discovery::PairingCandidate, 2> rooms{{
        {.address = "127.0.0.1", .port = 1, .server_name = "Room A"},
        {.address = "127.0.0.1", .port = 2, .server_name = "Room B"},
    }};

    // Case-insensitive: the wanted room need not match the advertised casing.
    const auto lower = select_preferred_room_candidate(rooms, "room a");
    assert(lower.kind == RoomSelection::matched);
    assert(lower.index == 0);

    // Surrounding whitespace on the preference is trimmed before comparison.
    const auto padded = select_preferred_room_candidate(rooms, "  Room B  ");
    assert(padded.kind == RoomSelection::matched);
    assert(padded.index == 1);

    // A room no server advertises is not a match, so the caller keeps sweeping
    // rather than pairing to the wrong machine.
    assert(select_preferred_room_candidate(rooms, "Room C").kind ==
           RoomSelection::unmatched);

    // No preference (empty or whitespace only) falls back to today's
    // sole-candidate/menu flow.
    assert(select_preferred_room_candidate(rooms, "").kind ==
           RoomSelection::no_preference);
    assert(select_preferred_room_candidate(rooms, "    ").kind ==
           RoomSelection::no_preference);

    // Two servers advertising the same room is ambiguous; refuse to guess so
    // the operator is never silently paired to an arbitrary one.
    const std::array<nstu::discovery::PairingCandidate, 2> duplicates{{
        {.address = "127.0.0.1", .port = 1, .server_name = "Room A"},
        {.address = "127.0.0.1", .port = 2, .server_name = "room a"},
    }};
    assert(select_preferred_room_candidate(duplicates, "Room A").kind ==
           RoomSelection::unmatched);
}

} // namespace

int main() {
    // Pure, network-free check first: the room filter that runs before any
    // pairing decides which candidate to try.
    test_room_candidate_selection();

    nstu::net::WinsockRuntime winsock;
    assert(winsock.ready());

    const std::array<std::byte, 4> entropy{
        std::byte{3}, std::byte{1}, std::byte{4}, std::byte{7}};
    wchar_t temporary_directory[MAX_PATH]{};
    assert(GetTempPathW(MAX_PATH, temporary_directory) != 0);
    const auto keyring_path = std::filesystem::path(temporary_directory) /
        (L"nstu-client-pairing-" + std::to_wstring(GetCurrentProcessId()) +
         L".bin");

    nstu::security::KeyStore key_store;
    nstu::server::ClientRegistry registry;
    nstu::server::ServerControlPlane server(registry, key_store);
    nstu::server::ServerControlPlaneConfig config;
    config.port = 0;
    config.keyring_path = keyring_path.wstring();
    config.keyring_entropy.assign(entropy.begin(), entropy.end());
    // No enrollment secret anywhere: not on the server, not on the client.
    // Removing the hand-copied secret is the whole point of this path.
    std::string error;
    assert(server.start(std::move(config), &error));

    const nstu::discovery::PairingCandidate candidate{
        .address = "127.0.0.1",
        .port = server.local_port(),
        .server_name = kServerName,
    };

    // With the window closed the client is told to go away, and it reports
    // that as something an operator can act on rather than a generic failure.
    {
        PairingRun closed;
        run_pairing(candidate, closed, std::stop_token{});
        assert(closed.result.outcome ==
               nstu::client::PairingOutcome::unavailable);
        assert(closed.result.config.key_id == 0);
        assert(closed.code.empty());
    }

    server.set_pairing_window(true, kServerName);

    // An operator who says no leaves the client unenrolled and the key store
    // untouched.
    {
        PairingRun declined;
        std::jthread worker([&](std::stop_token token) {
            run_pairing(candidate, declined, std::move(token));
        });
        const auto pending = wait_for_pending(server, 1);
        assert(pending.size() == 1);
        assert(server.reject_pairing(pending[0].pairing_id, &error));
        worker.join();
        assert(declined.result.outcome ==
               nstu::client::PairingOutcome::declined);
        assert(declined.result.config.pre_shared_key.empty());
        assert(declined.result.config.key_id == 0);
    }
    assert(key_store.active_key_count() == 0);
    assert(server.pending_pairings().empty());

    PairingRun approved;
    std::string observed_code;
    std::uint32_t key_id = 0;
    {
        std::jthread worker([&](std::stop_token token) {
            run_pairing(candidate, approved, std::move(token));
        });
        const auto pending = wait_for_pending(server, 1);
        assert(pending.size() == 1);
        assert(pending[0].client_uuid == kClientUuid);
        assert(pending[0].hostname == kClientHostname);
        assert(pending[0].seconds_remaining > 0);
        observed_code = pending[0].short_authentication_string;

        // A teacher walking across a room takes longer than the dispatcher's
        // patience, so the waiting client has to keep the connection alive on
        // its own. Several keep-alives pass through here; if the server
        // treated one as a protocol error the request would vanish.
        std::this_thread::sleep_for(std::chrono::milliseconds(900));
        const auto still_waiting = server.pending_pairings();
        assert(still_waiting.size() == 1);
        assert(still_waiting[0].pairing_id == pending[0].pairing_id);

        assert(server.approve_pairing(pending[0].pairing_id, &error));
        worker.join();
    }

    assert(approved.result.outcome == nstu::client::PairingOutcome::enrolled);
    // The comparison the whole scheme rests on: the digits the client showed
    // are the digits the operator was asked to approve.
    assert(approved.code == observed_code);
    assert(approved.code.size() == nstu::pairing::kSasDigits);
    assert(approved.server_name == kServerName);

    const auto client_id = nstu::pairing::client_id_from_uuid(kClientUuid);
    assert(client_id.has_value());
    assert(approved.result.config.client_id == *client_id);
    assert(approved.result.config.server_address == candidate.address);
    assert(approved.result.config.server_port == candidate.port);
    assert(approved.result.config.key_id != 0);
    assert(approved.result.config.pre_shared_key.size() ==
           sizeof(nstu::pairing::PairingSecrets::enrolled_key));
    key_id = approved.result.config.key_id;

    // The config the client came away with is a working identity: the
    // ordinary authenticated handshake succeeds with it and nothing was
    // copied onto the machine to get there.
    const auto now = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    nstu::net::TcpSocket control_socket;
    assert(control_socket.connect(candidate.address, candidate.port, &error));
    assert(control_socket.set_io_timeouts(5000, 5000, &error));
    const auto session = nstu::control::client_handshake(
        control_socket, approved.result.config.client_id, key_id,
        approved.result.config.pre_shared_key, now, std::chrono::seconds(120),
        &error);
    assert(session.has_value());
    control_socket.close();

    server.set_pairing_window(false);
    server.stop();

    nstu::security::KeyStore restored;
    assert(nstu::security::load_keyring(restored, keyring_path.wstring(),
                                        entropy, &error));
    assert(restored.resolve(*client_id, key_id) ==
           approved.result.config.pre_shared_key);
    assert(DeleteFileW(keyring_path.c_str()));

    // Pairing needs an identity before there is any NSTU state to read one
    // from. A machine that cannot produce one has to say so rather than
    // invent a fresh identity on every attempt.
    std::string identity_error;
    const auto identity = nstu::client::machine_uuid(&identity_error);
    assert(!identity.empty() || !identity_error.empty());
    if (!identity.empty()) {
        assert(nstu::pairing::client_id_from_uuid(identity).has_value());
    }
    assert(!nstu::client::machine_hostname().empty());
    return 0;
}
