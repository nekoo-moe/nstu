#pragma once

#include "nstu/client_config.hpp"
#include "nstu/discovery.hpp"
#include "nstu/pairing.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>

// The client half of verified pairing. A machine that has just been installed
// holds no key, so it cannot authenticate anything and nothing it hears on the
// LAN is trustworthy yet. What it can do is run one ephemeral exchange with a
// candidate server and put six digits on its own screen; an operator standing
// at the server approves only when those six digits match theirs. That
// comparison is the entire root of trust, and it is what replaces copying an
// enrollment secret onto every machine by hand.
namespace nstu::client {

// Called once the code is known and before the wait begins, so the agent can
// put it on screen while the teacher walks over.
using PairingCodeObserver =
    std::function<void(const std::string& code,
                       const std::string& server_name)>;

struct PairingAttemptOptions {
    // The server closes its own side at three minutes. This only bounds a
    // server that stops answering altogether.
    std::chrono::seconds approval_timeout{200};
    std::uint32_t connect_timeout_ms = 2000;
    // The dispatcher drops idle connections after fifteen seconds, so a
    // machine waiting on a human has to keep saying it is still there.
    std::uint32_t heartbeat_interval_ms = 4000;
};

enum class PairingOutcome {
    // Approved. `config` carries the derived key; persist it, then clear it.
    enrolled,
    // The operator looked at the request and said no.
    declined,
    // Nobody answered in time, at either end.
    timed_out,
    // The server is not accepting new machines right now.
    unavailable,
    // Anything else: unreachable, malformed, or a tag that did not verify.
    failed,
};

[[nodiscard]] const char* pairing_outcome_text(PairingOutcome outcome) noexcept;

struct PairingAttemptResult {
    PairingOutcome outcome = PairingOutcome::failed;
    ClientRuntimeConfig config;
};

// How a configured room preference resolved against the servers currently
// answering the pre-enrollment sweep. The room is a routing hint, never a
// credential: it only chooses which candidate to try, and the six-digit SAS
// still gates the pairing itself.
enum class RoomSelection {
    // No room was configured; fall back to today's sole-candidate/menu flow.
    no_preference,
    // Exactly one answering server carries the wanted room; auto-select it.
    matched,
    // Zero servers carry it, or several do (ambiguous); do not guess.
    unmatched,
};

struct RoomSelectionResult {
    RoomSelection kind = RoomSelection::no_preference;
    // Valid only when `kind == matched`: index into the candidate span.
    std::size_t index = 0;
};

// Chooses the candidate whose advertised room matches `preferred_room`. The
// comparison is trimmed and case-insensitive, and `preferred_room` is put
// through discovery::sanitize_server_name first so it is normalised the same
// way the beacon normalises the name it advertises (a diacritic room name
// therefore still matches). An empty/whitespace-only preference yields
// `no_preference`; exactly one match yields `matched`; zero or multiple
// matches yield `unmatched` so the caller keeps sweeping rather than pairing
// to the wrong room. Pure and side-effect free, so it is unit-testable without
// a live sweep.
[[nodiscard]] RoomSelectionResult select_preferred_room_candidate(
    std::span<const discovery::PairingCandidate> candidates,
    std::string_view preferred_room);

// Runs one whole exchange against one candidate and returns what the operator
// decided. Writes nothing to disk: where a machine identity is allowed to land
// is the caller's decision, not this function's.
[[nodiscard]] PairingAttemptResult pair_with_server(
    const discovery::PairingCandidate& candidate,
    std::string_view client_uuid, std::string_view hostname,
    const PairingCodeObserver& code_observer, std::stop_token stop_token,
    const PairingAttemptOptions& options = {}, std::string* error = nullptr);

// This machine's stable identity. Pairing needs one before there is any NSTU
// state to read it from, so it comes from the machine itself.
[[nodiscard]] std::string machine_uuid(std::string* error = nullptr);
[[nodiscard]] std::string machine_hostname();

// The classroom label this machine was told to prefer at install time, read
// from HKLM\Software\NSTU\PreferredRoom. The installer writes it in plaintext
// beside InstallRole because it must be readable before this machine holds any
// NSTU key; it is a routing hint, never a credential. Empty when unset or
// unreadable. Returned as UTF-8 (not stripped to ASCII) so a diacritic room
// name reduces to the same bytes the server's advertised name does when both
// pass through sanitize_server_name.
[[nodiscard]] std::string read_preferred_room_seed();

} // namespace nstu::client
