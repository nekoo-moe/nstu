#include "nstu/setup/startup_options.hpp"

#include <cassert>
#include <initializer_list>
#include <iostream>
#include <string>
#include <string_view>

namespace {

void test_endpoint_syntax() {
    using nstu::setup::parse_diagnostic_server_port;
    using nstu::setup::valid_diagnostic_server_address;
    assert(parse_diagnostic_server_port(L"47001") == 47001);
    assert(parse_diagnostic_server_port(L"1") == 1);
    assert(parse_diagnostic_server_port(L"65535") == 65535);
    for (const auto* value : {L"", L"0", L"65536", L"4294967297", L"-1",
                              L"+1", L" 80", L"80 ", L"47oops", L"1.5"}) {
        assert(!parse_diagnostic_server_port(value));
    }
    for (const auto* value : {L"192.168.10.10", L"teacher-pc.school.local",
                              L"::1", L"fe80::123%12"}) {
        assert(valid_diagnostic_server_address(value));
    }
    for (const auto* value : {L"", L" teacher", L"teacher\n", L"https://teacher",
                              L"teacher/path", L"teacher\\path", L"\"teacher\""}) {
        assert(!valid_diagnostic_server_address(value));
    }
    assert(!valid_diagnostic_server_address(std::wstring(254, L'a')));
    assert(!valid_diagnostic_server_address(std::wstring_view(L"teacher\0ignored", 15)));
}

void test_saved_boot_endpoint() {
    nstu::setup::DiagnosticOptions options;
    options.boot_check = true;
    bool called = false;
    assert(nstu::setup::load_diagnostic_boot_endpoint(
        options, false, false, [&](bool address, bool port) {
            called = true;
            assert(address && port);
            return nstu::setup::SavedDiagnosticEndpoint{L"teacher.lab", 48001};
        }));
    assert(called);
    assert(options.server_address == L"teacher.lab");
    assert(options.server_port == 48001);
}

void test_explicit_overrides() {
    nstu::setup::DiagnosticOptions options;
    options.boot_check = true;
    options.server_address = L"manual.lab";
    options.server_port = 49001;
    assert(nstu::setup::load_diagnostic_boot_endpoint(
        options, true, true, [](bool, bool) {
            assert(false && "Explicit endpoint must not read the registry");
            return nstu::setup::SavedDiagnosticEndpoint{};
        }));
    assert(options.server_address == L"manual.lab");
    assert(options.server_port == 49001);

    assert(nstu::setup::load_diagnostic_boot_endpoint(
        options, true, false, [](bool address, bool port) {
            assert(!address && port);
            return nstu::setup::SavedDiagnosticEndpoint{std::nullopt, 48001};
        }));
    assert(options.server_address == L"manual.lab");
    assert(options.server_port == 48001);

    options.server_port = 49001;
    assert(nstu::setup::load_diagnostic_boot_endpoint(
        options, false, true, [](bool address, bool port) {
            assert(address && !port);
            return nstu::setup::SavedDiagnosticEndpoint{L"saved.lab", std::nullopt};
        }));
    assert(options.server_address == L"saved.lab");
    assert(options.server_port == 49001);
}

void test_bad_saved_values_fail_closed() {
    for (const auto& saved : {
             nstu::setup::SavedDiagnosticEndpoint{},
             nstu::setup::SavedDiagnosticEndpoint{L"teacher.lab", 0},
             nstu::setup::SavedDiagnosticEndpoint{L"teacher.lab", 65536},
             nstu::setup::SavedDiagnosticEndpoint{L"teacher.lab", 0xffffffff},
             nstu::setup::SavedDiagnosticEndpoint{L"teacher\n", 47001}}) {
        nstu::setup::DiagnosticOptions options;
        options.boot_check = true;
        assert(!nstu::setup::load_diagnostic_boot_endpoint(
            options, false, false, [&](bool, bool) { return saved; }));
        if (!saved.port || *saved.port == 0 || *saved.port > 65535) {
            assert(options.server_port == 0);
        }
        if (!saved.address || !nstu::setup::valid_diagnostic_server_address(*saved.address)) {
            assert(options.server_address.empty());
        }
    }
}

void test_other_runs_do_not_read_saved_client_config() {
    for (const bool server : {false, true}) {
        nstu::setup::DiagnosticOptions options;
        options.role = server ? nstu::setup::DiagnosticRole::server
                              : nstu::setup::DiagnosticRole::client;
        options.boot_check = server;
        assert(nstu::setup::load_diagnostic_boot_endpoint(
            options, false, false, [](bool, bool) {
                assert(false && "Manual/server runs must not read client config");
                return nstu::setup::SavedDiagnosticEndpoint{};
            }));
        assert(options.server_address.empty());
        assert(options.server_port == 47001);
    }
}

} // namespace

int main() {
    test_endpoint_syntax();
    test_saved_boot_endpoint();
    test_explicit_overrides();
    test_bad_saved_values_fail_closed();
    test_other_runs_do_not_read_saved_client_config();
    std::cout << "Diagnostics startup endpoint tests passed\n";
}
