#include "nstu/telemetry.hpp"

#include <cassert>
#include <string>

int main() {
    using nstu::telemetry::ConsentPolicy;
    using nstu::telemetry::Event;
    using nstu::telemetry::PublicReport;
    using nstu::telemetry::Severity;

    constexpr ConsentPolicy disabled{};
    static_assert(!nstu::telemetry::collection_enabled(disabled));
    static_assert(!nstu::telemetry::should_prompt_for_error(disabled));
    constexpr ConsentPolicy local_collection{true, false};
    static_assert(nstu::telemetry::collection_enabled(local_collection));
    static_assert(!nstu::telemetry::should_prompt_for_error(local_collection));
    constexpr ConsentPolicy prompt{true, true};
    static_assert(nstu::telemetry::should_prompt_for_error(prompt));

    const auto sanitized = nstu::telemetry::sanitize_public_text(
        "Client 123456 at 192.168.10.10 opened C:\\Users\\nstu\\report.txt "
        "for test@example.com token=0123456789abcdef0123456789abcdef");
    assert(sanitized.find("123456") == std::string::npos);
    assert(sanitized.find("192.168.10.10") == std::string::npos);
    assert(sanitized.find("C:\\Users") == std::string::npos);
    assert(sanitized.find("test@example.com") == std::string::npos);
    assert(sanitized.find("0123456789abcdef") == std::string::npos);

    const auto additional_secrets = nstu::telemetry::sanitize_public_text(
        "MAC 00-11-22-33-44-55 username: teacher password super-secret "
        "IPv6 fe80::4d2a:11ff:fe22:3344");
    assert(additional_secrets.find("00-11-22-33-44-55") == std::string::npos);
    assert(additional_secrets.find("teacher") == std::string::npos);
    assert(additional_secrets.find("super-secret") == std::string::npos);
    assert(additional_secrets.find("fe80::") == std::string::npos);
    const auto school_name = nstu::telemetry::sanitize_public_text(
        "failure school: Le Quy Don High School adapter reset");
    assert(school_name.find("Le Quy Don") == std::string::npos);

    const auto normalized = nstu::telemetry::sanitize_public_text(
        "first\r\n\tsecond", 64);
    assert(normalized == "first second");
    const auto truncated = nstu::telemetry::sanitize_public_text(
        "abcdefghijklmnopqrstuvwxyz", 10);
    assert(truncated == "abcdefg...");

    std::deque<Event> events;
    nstu::telemetry::append_bounded_event(
        events, {Severity::warning, "Network", "peer=10.0.0.1"}, 2);
    nstu::telemetry::append_bounded_event(
        events, {Severity::information, "DXGI", "hardware adapter ready"}, 2);
    nstu::telemetry::append_bounded_event(
        events, {Severity::error, "Snapshot", "Client 98765 failed"}, 2);
    assert(events.size() == 2);
    assert(events.front().component == "DXGI");
    assert(events.back().detail.find("98765") == std::string::npos);
    nstu::telemetry::append_bounded_event(
        events, {Severity::error, "Ignored", "capacity is zero"}, 0);
    assert(events.size() == 2);
    nstu::telemetry::append_bounded_event(
        events, {Severity::error, "Snapshot", "Client 98765 failed"}, 2);
    assert(events.size() == 2);

    PublicReport report;
    report.application = "NSTU Server";
    report.version = "0.1.0";
    report.build_channel = "Release";
    report.fields.push_back({"Adapter", "Intel HD Graphics 530"});
    report.fields.push_back({"Endpoint", "10.20.30.40:47001"});
    report.events = events;
    const auto markdown = nstu::telemetry::build_public_markdown(report);
    assert(markdown.find("NSTU Server") != std::string::npos);
    assert(markdown.find("Intel HD Graphics 530") != std::string::npos);
    assert(markdown.find("98765") == std::string::npos);
    assert(markdown.find("10.20.30.40") == std::string::npos);
    assert(markdown.find("IP/MAC addresses") != std::string::npos);
    return 0;
}
