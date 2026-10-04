#include "nstu/agent_protocol.hpp"
#include "nstu/exam_host.hpp"
#include "nstu/exam_lockdown_controller.hpp"

#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

namespace {

struct FakeWfpSeam {
    std::vector<nstu::net::WfpAllowlistConfig> apply_calls;
    int clear_calls = 0;
    int fail_apply_times = 0;
    int fail_clear_times = 0;

    bool apply(const nstu::net::WfpAllowlistConfig& config, std::string* error) {
        apply_calls.push_back(config);
        if (fail_apply_times > 0) {
            --fail_apply_times;
            if (error) {
                *error = "simulated apply failure";
            }
            return false;
        }
        return true;
    }

    bool clear(std::string* error) {
        ++clear_calls;
        if (fail_clear_times > 0) {
            --fail_clear_times;
            if (error) {
                *error = "simulated clear failure";
            }
            return false;
        }
        return true;
    }
};

nstu::security::ClientId make_test_client_id(std::byte fill_byte) {
    nstu::security::ClientId id{};
    id.fill(fill_byte);
    return id;
}

nstu::exam::SessionId make_test_session_id(std::byte fill_byte) {
    nstu::exam::SessionId id{};
    id.fill(fill_byte);
    return id;
}

} // namespace

int main() {
    using namespace nstu::client;

    const auto client_id = make_test_client_id(std::byte{0x11});
    const auto wrong_client_id = make_test_client_id(std::byte{0x22});
    const auto session_id = make_test_session_id(std::byte{0x33});
    const auto zero_session_id = make_test_session_id(std::byte{0x00});

    // 1. Refuses to arm on invalid guards:
    //    - stale epoch token
    //    - zero session_id
    //    - wrong client_id
    //    - package_verified = false
    //    - empty IPv4 set
    {
        FakeWfpSeam seam;
        ExamLockdownController controller(
            [&](const nstu::net::WfpAllowlistConfig& cfg, std::string* err) {
                return seam.apply(cfg, err);
            },
            [&](std::string* err) { return seam.clear(err); });

        const auto token = controller.begin_attempt();
        ExamLockdownArmParams valid_params{
            .session_id = session_id,
            .client_id = client_id,
            .package_verified = true,
            .duration_seconds = 3600,
            .allowed_ipv4 = {0x08080808u},
        };

        // Stale epoch token
        std::string err;
        assert(!controller.arm(token - 1, valid_params, client_id, 1000, &err));
        assert(!controller.arm(token + 1, valid_params, client_id, 1000, &err));

        // Zero session_id
        auto zero_session_params = valid_params;
        zero_session_params.session_id = zero_session_id;
        assert(!controller.arm(token, zero_session_params, client_id, 1000, &err));

        // Wrong client_id
        auto wrong_client_params = valid_params;
        wrong_client_params.client_id = wrong_client_id;
        assert(!controller.arm(token, wrong_client_params, client_id, 1000, &err));

        // Package not verified
        auto unverified_params = valid_params;
        unverified_params.package_verified = false;
        assert(!controller.arm(token, unverified_params, client_id, 1000, &err));

        // Empty IPv4 set
        auto empty_ip_params = valid_params;
        empty_ip_params.allowed_ipv4.clear();
        assert(!controller.arm(token, empty_ip_params, client_id, 1000, &err));

        assert(!controller.is_armed());
        assert(!controller.is_actually_blocked());

        // Valid arm succeeds
        assert(controller.arm(token, valid_params, client_id, 1000, &err));
        assert(controller.is_armed());
        assert(!controller.is_actually_blocked()); // not blocked until tick
    }

    // 2. Arm then tick applies exactly once; subsequent ticks are idempotent
    {
        FakeWfpSeam seam;
        ExamLockdownController controller(
            [&](const nstu::net::WfpAllowlistConfig& cfg, std::string* err) {
                return seam.apply(cfg, err);
            },
            [&](std::string* err) { return seam.clear(err); });

        const auto token = controller.begin_attempt();
        ExamLockdownArmParams params{
            .session_id = session_id,
            .client_id = client_id,
            .package_verified = true,
            .duration_seconds = 3600,
            .allowed_ipv4 = {0x08080808u},
        };

        assert(controller.arm(token, params, client_id, 1000));
        assert(seam.apply_calls.empty());

        assert(controller.tick(1000) == LockdownAction::apply);
        assert(seam.apply_calls.size() == 1);
        assert(seam.apply_calls[0].allowed_ipv4 == params.allowed_ipv4);
        assert(controller.is_actually_blocked());

        // Subsequent ticks return none and do not invoke apply_fn again
        assert(controller.tick(1500) == LockdownAction::none);
        assert(controller.tick(2000) == LockdownAction::none);
        assert(seam.apply_calls.size() == 1);
        assert(seam.clear_calls == 0);
    }

    // 3. Disarm clears exactly once, immediately, not only on next tick
    {
        FakeWfpSeam seam;
        ExamLockdownController controller(
            [&](const nstu::net::WfpAllowlistConfig& cfg, std::string* err) {
                return seam.apply(cfg, err);
            },
            [&](std::string* err) { return seam.clear(err); });

        const auto token = controller.begin_attempt();
        ExamLockdownArmParams params{
            .session_id = session_id,
            .client_id = client_id,
            .package_verified = true,
            .duration_seconds = 3600,
            .allowed_ipv4 = {0x08080808u},
        };

        assert(controller.arm(token, params, client_id, 1000));
        assert(controller.tick(1000) == LockdownAction::apply);
        assert(controller.is_actually_blocked());
        assert(seam.clear_calls == 0);

        // Immediate clear on disarm
        controller.disarm();
        assert(!controller.is_armed());
        assert(!controller.is_actually_blocked());
        assert(seam.clear_calls == 1);

        // Subsequent tick does not re-clear
        assert(controller.tick(2000) == LockdownAction::none);
        assert(seam.clear_calls == 1);
    }

    // 4. Hard cap elapses -> tick clears; heartbeats stop -> tick clears
    {
        // 4a: Heartbeats stop -> tick clears after stale window (90s)
        FakeWfpSeam seam;
        ExamLockdownController controller(
            [&](const nstu::net::WfpAllowlistConfig& cfg, std::string* err) {
                return seam.apply(cfg, err);
            },
            [&](std::string* err) { return seam.clear(err); });

        const auto token = controller.begin_attempt();
        ExamLockdownArmParams params{
            .session_id = session_id,
            .client_id = client_id,
            .package_verified = true,
            .duration_seconds = 3600,
            .allowed_ipv4 = {0x08080808u},
        };

        assert(controller.arm(token, params, client_id, 1000));
        assert(controller.tick(1000) == LockdownAction::apply);
        assert(controller.is_actually_blocked());

        // Heartbeats arrive regularly
        controller.heartbeat(10'000);
        assert(controller.tick(10'000) == LockdownAction::none);
        controller.heartbeat(20'000);
        assert(controller.tick(20'000) == LockdownAction::none);

        // Heartbeats stop; within 90s it remains blocked
        assert(controller.tick(109'000) == LockdownAction::none); // 89s since last heartbeat
        assert(controller.is_actually_blocked());

        // Heartbeat timeout elapses (> 90s) -> tick clears
        assert(controller.tick(111'000) == LockdownAction::clear); // 91s
        assert(!controller.is_actually_blocked());
        assert(seam.clear_calls == 1);
    }
    {
        // 4b: Hard cap elapses -> tick clears even if heartbeats continue
        FakeWfpSeam seam;
        ExamLockdownController controller(
            [&](const nstu::net::WfpAllowlistConfig& cfg, std::string* err) {
                return seam.apply(cfg, err);
            },
            [&](std::string* err) { return seam.clear(err); });

        const auto token = controller.begin_attempt();
        // Duration 60s -> max_active = (60 + 30*60)*1000 = 1,860,000 ms
        ExamLockdownArmParams params{
            .session_id = session_id,
            .client_id = client_id,
            .package_verified = true,
            .duration_seconds = 60,
            .allowed_ipv4 = {0x08080808u},
        };

        assert(controller.arm(token, params, client_id, 0));
        assert(controller.tick(0) == LockdownAction::apply);
        assert(controller.is_actually_blocked());

        // Send heartbeat right at 1,859,000 ms
        controller.heartbeat(1'859'000);
        assert(controller.tick(1'859'000) == LockdownAction::none);
        assert(controller.is_actually_blocked());

        // Advance past hard cap (> 1,860,000 ms) with fresh heartbeat: cap is absolute
        controller.heartbeat(1'860'001);
        assert(controller.tick(1'860'001) == LockdownAction::clear);
        assert(!controller.is_actually_blocked());
    }

    // 5. Clock moving backwards never extends the block
    {
        FakeWfpSeam seam;
        ExamLockdownController controller(
            [&](const nstu::net::WfpAllowlistConfig& cfg, std::string* err) {
                return seam.apply(cfg, err);
            },
            [&](std::string* err) { return seam.clear(err); });

        const auto token = controller.begin_attempt();
        ExamLockdownArmParams params{
            .session_id = session_id,
            .client_id = client_id,
            .package_verified = true,
            .duration_seconds = 3600,
            .allowed_ipv4 = {0x08080808u},
        };

        assert(controller.arm(token, params, client_id, 10'000));
        assert(controller.tick(10'000) == LockdownAction::apply);
        assert(controller.tick(9'000) == LockdownAction::none); // clock backwards
        assert(controller.is_actually_blocked());

        controller.disarm();
        assert(seam.clear_calls == 1);
        assert(!controller.is_actually_blocked());
        assert(controller.tick(8'000) == LockdownAction::none);
    }

    // 6. clear_fn fails N times then succeeds -> block is eventually cleared and
    //    retried every tick; never forgotten (finding 1)
    {
        FakeWfpSeam seam;
        seam.fail_clear_times = 3;
        ExamLockdownController controller(
            [&](const nstu::net::WfpAllowlistConfig& cfg, std::string* err) {
                return seam.apply(cfg, err);
            },
            [&](std::string* err) { return seam.clear(err); });

        const auto token = controller.begin_attempt();
        ExamLockdownArmParams params{
            .session_id = session_id,
            .client_id = client_id,
            .package_verified = true,
            .duration_seconds = 3600,
            .allowed_ipv4 = {0x08080808u},
        };

        assert(controller.arm(token, params, client_id, 1000));
        assert(controller.tick(1000) == LockdownAction::apply);
        assert(controller.is_actually_blocked());

        // Disarm triggers clear, which fails (fail 1/3)
        controller.disarm();
        assert(seam.clear_calls == 1);
        assert(controller.is_actually_blocked()); // Still blocked because clear failed!

        // Next tick retries clear (fail 2/3)
        assert(controller.tick(2000) == LockdownAction::none);
        assert(seam.clear_calls == 2);
        assert(controller.is_actually_blocked());

        // Next tick retries clear (fail 3/3)
        assert(controller.tick(3000) == LockdownAction::none);
        assert(seam.clear_calls == 3);
        assert(controller.is_actually_blocked());

        // Next tick retries clear -> succeeds!
        assert(controller.tick(4000) == LockdownAction::clear);
        assert(seam.clear_calls == 4);
        assert(!controller.is_actually_blocked());

        // Later ticks do not retry
        assert(controller.tick(5000) == LockdownAction::none);
        assert(seam.clear_calls == 4);
    }

    // 7. apply_fn fails -> bounded retries (5), exam not aborted, gives up (finding 1)
    {
        FakeWfpSeam seam;
        seam.fail_apply_times = 10; // Fails all 5 attempts
        ExamLockdownController controller(
            [&](const nstu::net::WfpAllowlistConfig& cfg, std::string* err) {
                return seam.apply(cfg, err);
            },
            [&](std::string* err) { return seam.clear(err); });

        const auto token = controller.begin_attempt();
        ExamLockdownArmParams params{
            .session_id = session_id,
            .client_id = client_id,
            .package_verified = true,
            .duration_seconds = 3600,
            .allowed_ipv4 = {0x08080808u},
        };

        assert(controller.arm(token, params, client_id, 1000));
        assert(controller.apply_retries() == 0);
        assert(!controller.last_apply_failed());

        // Attempt 1
        assert(controller.tick(1000) == LockdownAction::none);
        assert(controller.apply_retries() == 1);
        assert(!controller.is_actually_blocked());

        // Attempt 2
        assert(controller.tick(2000) == LockdownAction::none);
        assert(controller.apply_retries() == 2);

        // Attempt 3
        assert(controller.tick(3000) == LockdownAction::none);
        assert(controller.apply_retries() == 3);

        // Attempt 4
        assert(controller.tick(4000) == LockdownAction::none);
        assert(controller.apply_retries() == 4);

        // Attempt 5 -> reaches kMaxApplyRetries (5), marks last_apply_failed
        assert(controller.tick(5000) == LockdownAction::none);
        assert(controller.apply_retries() == 5);
        assert(controller.last_apply_failed());
        assert(!controller.is_actually_blocked());

        // Further ticks do not retry apply
        assert(controller.tick(6000) == LockdownAction::none);
        assert(seam.apply_calls.size() == 5);
    }

    // 8. Stop arrives while arm worker is mid-flight -> late arm(old_token)
    //    is rejected and nothing is applied (finding 4)
    {
        FakeWfpSeam seam;
        ExamLockdownController controller(
            [&](const nstu::net::WfpAllowlistConfig& cfg, std::string* err) {
                return seam.apply(cfg, err);
            },
            [&](std::string* err) { return seam.clear(err); });

        const auto token = controller.begin_attempt();

        // While worker was computing package digest and DNS, stop arrived:
        controller.disarm();

        // Arm worker finishes and calls arm with the old token
        ExamLockdownArmParams params{
            .session_id = session_id,
            .client_id = client_id,
            .package_verified = true,
            .duration_seconds = 3600,
            .allowed_ipv4 = {0x08080808u},
        };
        std::string err;
        assert(!controller.arm(token, params, client_id, 1000, &err));
        assert(!controller.is_armed());
        assert(!controller.is_actually_blocked());

        // Tick does not apply anything
        assert(controller.tick(2000) == LockdownAction::none);
        assert(seam.apply_calls.empty());
    }

    // 9. Startup sweep invokes clear_fn exactly once even with nothing armed
    {
        FakeWfpSeam seam;
        ExamLockdownController controller(
            [&](const nstu::net::WfpAllowlistConfig& cfg, std::string* err) {
                return seam.apply(cfg, err);
            },
            [&](std::string* err) { return seam.clear(err); });

        assert(seam.clear_calls == 0);
        assert(controller.startup_sweep());
        assert(seam.clear_calls == 1);
    }

    // 10. Separately testable origin resolver:
    //     - malformed/oversized/over-count rejected
    //     - partial failure keeps resolved subset
    //     - all-unresolvable -> empty
    //     - loopback/0.0.0.0/link-local/multicast discarded
    {
        // Hostname parser tests
        std::string host;
        assert(parse_origin_hostname("https://example.com/test", host) && host == "example.com");
        assert(parse_origin_hostname("http://sub.domain.org:8080/path?q=1#frag", host) && host == "sub.domain.org");
        assert(parse_origin_hostname("192.168.1.100", host) && host == "192.168.1.100");
        assert(parse_origin_hostname("//api.service.edu:443", host) && host == "api.service.edu");
        // Rejections:
        assert(!parse_origin_hostname("", host));
        assert(!parse_origin_hostname("ftp://example.com", host));
        assert(!parse_origin_hostname("user@example.com", host));
        assert(!parse_origin_hostname("example.com:0", host));
        assert(!parse_origin_hostname("example.com:65536", host));
        assert(!parse_origin_hostname(".example.com", host));
        assert(!parse_origin_hostname("example..com", host));
        assert(!parse_origin_hostname("example.com.", host));
        assert(!parse_origin_hostname(std::string(254, 'a'), host));

        // is_disallowed_ipv4 checks
        assert(is_disallowed_ipv4(0x00000000u));          // 0.0.0.0
        assert(is_disallowed_ipv4(0x7f000001u));          // 127.0.0.1
        assert(is_disallowed_ipv4(0x7fffffffu));          // 127.255.255.255
        assert(is_disallowed_ipv4((169u << 24u) | (254u << 16u) | 1u)); // 169.254.0.1 (link-local)
        assert(is_disallowed_ipv4((224u << 24u) | 1u));   // multicast
        assert(is_disallowed_ipv4(0xffffffffu));          // 255.255.255.255 broadcast
        assert(!is_disallowed_ipv4((192u << 24u) | (168u << 16u) | (1u << 8u) | 10u)); // 192.168.1.10
        assert(!is_disallowed_ipv4((8u << 24u) | (8u << 16u) | (8u << 8u) | 8u));       // 8.8.8.8

        // Resolver with mock resolve_fn
        std::vector<std::string> test_origins = {
            "https://valid1.com",
            "http://valid2.org",
            "https://unresolvable.net",
            "https://loopback.org",
        };

        auto mock_resolver = [](const std::string& h) -> std::vector<std::uint32_t> {
            if (h == "valid1.com") {
                return {0x08080808u}; // 8.8.8.8
            }
            if (h == "valid2.org") {
                return {0x08080404u, 0x08080808u}; // 8.8.4.4, duplicate 8.8.8.8
            }
            if (h == "loopback.org") {
                return {0x7f000001u}; // 127.0.0.1 (must be filtered out)
            }
            return {}; // unresolvable
        };

        auto resolved = resolve_allowed_origins(test_origins, mock_resolver);
        assert(resolved.size() == 2);
        assert(resolved[0] == 0x08080404u);
        assert(resolved[1] == 0x08080808u);

        // All unresolvable -> returns empty
        std::vector<std::string> bad_origins = {"https://unresolvable.net"};
        assert(resolve_allowed_origins(bad_origins, mock_resolver).empty());

        // Over-count (> 32) -> rejected
        std::vector<std::string> too_many(33, "https://valid1.com");
        assert(resolve_allowed_origins(too_many, mock_resolver).empty());
    }

    // 11. Origins absent -> controller never armed (policy table row 1)
    {
        ExamLockdownPolicy policy;
        std::string manifest = R"({
            "packageId": "test-pkg",
            "title": "Exam 1",
            "durationSeconds": 3600
        })";
        assert(extract_exam_lockdown_policy(manifest, policy));
        assert(policy.allowed_origins.empty());
        assert(policy.duration_seconds == 3600);
    }

    // 12. heartbeat() when not armed is a no-op
    {
        FakeWfpSeam seam;
        ExamLockdownController controller(
            [&](const nstu::net::WfpAllowlistConfig& cfg, std::string* err) {
                return seam.apply(cfg, err);
            },
            [&](std::string* err) { return seam.clear(err); });

        assert(!controller.is_armed());
        controller.heartbeat(5000);
        assert(!controller.is_armed());
        assert(controller.tick(5000) == LockdownAction::none);
        assert(seam.apply_calls.empty());
        assert(seam.clear_calls == 0);
    }

    // 13. Extractor:
    //     - absent field ok
    //     - valid entries parsed and deduplicated
    //     - bad entries rejected
    //     - duration bounds (60..86400) enforced
    {
        ExamLockdownPolicy policy;
        std::string err;

        // Absent allowedOrigins is valid
        assert(extract_exam_lockdown_policy(
            R"({"durationSeconds": 1800})", policy, &err));
        assert(policy.allowed_origins.empty());
        assert(policy.duration_seconds == 1800);

        // Valid allowedOrigins
        assert(extract_exam_lockdown_policy(
            R"({
                "durationSeconds": 7200,
                "allowedOrigins": [
                    "https://canvas.instructure.com",
                    "https://math.org"
                ]
            })", policy, &err));
        assert(policy.allowed_origins.size() == 2);
        assert(policy.duration_seconds == 7200);

        // Duplicate allowedOrigins rejected (uniqueItems: true)
        assert(!extract_exam_lockdown_policy(
            R"({
                "durationSeconds": 7200,
                "allowedOrigins": [
                    "https://canvas.instructure.com",
                    "https://canvas.instructure.com"
                ]
            })", policy, &err));

        // Duration too low (< 60)
        assert(!extract_exam_lockdown_policy(
            R"({"durationSeconds": 59})", policy, &err));

        // Duration too high (> 86400)
        assert(!extract_exam_lockdown_policy(
            R"({"durationSeconds": 86401})", policy, &err));

        // Bad allowedOrigins (not an array)
        assert(!extract_exam_lockdown_policy(
            R"({"durationSeconds": 3600, "allowedOrigins": "https://bad.com"})",
            policy, &err));

        // Bad allowedOrigins entry (invalid hostname)
        assert(!extract_exam_lockdown_policy(
            R"({"durationSeconds": 3600, "allowedOrigins": ["ftp://bad.com"]})",
            policy, &err));

        // Bad allowedOrigins entry (non-string element)
        assert(!extract_exam_lockdown_policy(
            R"({"durationSeconds": 3600, "allowedOrigins": [123]})",
            policy, &err));
    }

    // 14. exam_begin duration override updates lockdown controller duration
    {
        FakeWfpSeam seam;
        ExamLockdownController controller(
            [&](const nstu::net::WfpAllowlistConfig& cfg, std::string* err) {
                return seam.apply(cfg, err);
            },
            [&](std::string* err) { return seam.clear(err); });

        const auto token = controller.begin_attempt();
        ExamLockdownArmParams params{
            .session_id = session_id,
            .client_id = client_id,
            .package_verified = true,
            .duration_seconds = 1800, // 30 min initial
            .allowed_ipv4 = {0x08080808u},
        };

        assert(controller.arm(token, params, client_id, 0));
        assert(controller.tick(0) == LockdownAction::apply);

        // Update duration to 7200s (2 hours)
        controller.update_duration(7200);
        // Advance clock to 2,000,000 ms (past initial 1800s + 30m = 3600s = 3,600,000 ms?
        // Wait: 1800s + 1800s slack = 3600s = 3,600,000 ms.
        // With update to 7200s + 1800s slack = 9000s = 9,000,000 ms cap.
        controller.heartbeat(4'000'000);
        assert(controller.tick(4'000'000) == LockdownAction::none); // Not expired!
        assert(controller.is_actually_blocked());
    }

    // 15. AgentMessageType::exam_alive codec verification
    {
        const AgentMessage alive{AgentMessageType::exam_alive, {}};
        const auto encoded = encode_agent_message(alive);
        assert(!encoded.empty());
        const auto decoded = decode_agent_message(encoded);
        assert(decoded.has_value());
        assert(decoded->type == AgentMessageType::exam_alive);
        assert(decoded->payload.empty());
    }

    return 0;
}
