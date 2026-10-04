#include "nstu/remote_session_process.hpp"
#include "nstu/client_registry.hpp"

#include <windows.h>

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

bool wait_until(const std::function<bool()>& predicate) {
    for (int attempt = 0; attempt < 1500; ++attempt) {
        if (predicate()) return true;
        std::this_thread::sleep_for(10ms);
    }
    return false;
}

std::filesystem::path fake_child_path() {
    wchar_t module[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, module, MAX_PATH);
    assert(length > 0 && length < MAX_PATH);
    return std::filesystem::path(module).parent_path() /
           L"nstu_remote_session_fake_child.exe";
}

nstu::server::RemoteSessionProcessConfig config(
    std::vector<std::wstring> stream_arguments = {}) {
    nstu::server::RemoteSessionProcessConfig output;
    output.executable = fake_child_path();
    output.required_file_version = L"1.2.3.4";
    output.application = L"Desktop";
    output.host_port = 0;
    output.preflight_timeout = 200ms;
    output.stop_timeout = 200ms;
    output.maximum_diagnostic_bytes = 64;
    output.stream_arguments = std::move(stream_arguments);
    return output;
}

bool allow(std::uint64_t, std::string*) { return true; }

} // namespace

int main() {
    using nstu::server::RemoteSessionProcess;
    using nstu::server::RemoteSessionState;
    using nstu::server::RemoteSessionStopReason;

    assert(nstu::server::quote_windows_process_argument(L"plain") == L"plain");
    assert(nstu::server::quote_windows_process_argument(L"") == L"\"\"");
    assert(nstu::server::quote_windows_process_argument(L"two words") ==
           L"\"two words\"");
    const std::wstring quoted_trailing_slash =
        std::wstring{L"\"ends with "} + L"\\\\" + L"\"";
    assert(nstu::server::quote_windows_process_argument(L"ends with \\") ==
           quoted_trailing_slash);
    assert(nstu::server::quote_windows_process_argument(L"say \"yes\"") ==
           L"\"say \\\"yes\\\"\"");

    {
        auto missing = config();
        missing.executable = L"Z:\\missing\\remote-viewer.exe";
        RemoteSessionProcess session(std::move(missing));
        std::string error;
        assert(!session.start({1, L"127.0.0.1"}, allow, &error));
        assert(!error.empty());
    }

    {
        auto wrong_version = config();
        wrong_version.required_file_version = L"9.9.9.9";
        RemoteSessionProcess session(std::move(wrong_version));
        std::string error;
        assert(!session.start({2, L"127.0.0.1"}, allow, &error));
        assert(error == "remote-session file version does not match the pin");
    }

    {
        RemoteSessionProcess session(config());
        std::string error;
        assert(!session.start({2, L"not-an-address"}, allow, &error));
        assert(!error.empty());
    }

    {
        RemoteSessionProcess session(config());
        std::string error;
        assert(session.start({3, L"127.0.0.1"},
                             [](std::uint64_t, std::string* denial) {
                                 if (denial != nullptr) *denial = "exam active";
                                 return false;
                             },
                             &error));
        assert(wait_until([&] {
            return session.snapshot().state == RemoteSessionState::failed;
        }));
        assert(session.snapshot().bounded_diagnostic == "exam active");
    }

    {
        const auto path = std::filesystem::temp_directory_path() /
                          L"nstu-remote-session-argv.txt";
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        const std::wstring tricky = L"space quote \" tail\\";
        RemoteSessionProcess session(config({L"--record-args", path.wstring(),
                                             tricky, L"--exit-code", L"0"}));
        std::string error;
        assert(session.start({4, L"127.0.0.1"}, allow, &error));
        assert(wait_until([&] {
            return session.snapshot().state == RemoteSessionState::exited;
        }));
        std::wifstream input(path);
        const std::wstring contents((std::istreambuf_iterator<wchar_t>(input)),
                                    std::istreambuf_iterator<wchar_t>());
        assert(contents.find(tricky) != std::wstring::npos);
        std::filesystem::remove(path, ignored);
    }

    {
        RemoteSessionProcess session(
            config({L"--diagnostic-bytes", L"256", L"--exit-code", L"7"}));
        std::string error;
        assert(session.start({5, L"127.0.0.1"}, allow, &error));
        assert(!session.start({6, L"127.0.0.1"}, allow, &error));
        assert(wait_until([&] {
            return session.snapshot().state == RemoteSessionState::exited;
        }));
        const auto snapshot = session.snapshot();
        assert(snapshot.exit_code == 7u);
        assert(snapshot.bounded_diagnostic.size() == 64);
    }

    {
        // 1 MiB is far beyond the anonymous pipe buffer; without continuous
        // draining the child blocks in write() and never exits.
        RemoteSessionProcess session(
            config({L"--diagnostic-bytes", L"1048576", L"--exit-code", L"9"}));
        std::string error;
        assert(session.start({8, L"127.0.0.1"}, allow, &error));
        assert(wait_until([&] {
            return session.snapshot().state == RemoteSessionState::exited;
        }));
        const auto flooded = session.snapshot();
        assert(flooded.exit_code == 9u);
        assert(flooded.bounded_diagnostic.size() == 64);
    }

    {
        const auto marker = std::filesystem::temp_directory_path() /
                            L"nstu-remote-session-descendant.txt";
        std::error_code ignored;
        std::filesystem::remove(marker, ignored);
        RemoteSessionProcess session(config({L"--spawn-descendant",
                                             marker.wstring(), L"--sleep-ms",
                                             L"30000"}));
        std::string error;
        assert(session.start({7, L"127.0.0.1"}, allow, &error));
        assert(wait_until([&] {
            return session.snapshot().state == RemoteSessionState::running &&
                   std::filesystem::is_regular_file(marker);
        }));
        DWORD descendant_id = 0;
        std::ifstream(marker) >> descendant_id;
        assert(descendant_id != 0);
        session.stop(RemoteSessionStopReason::explicit_stop);
        session.stop(RemoteSessionStopReason::explicit_stop);
        assert(wait_until([&] {
            const auto state = session.snapshot().state;
            return state == RemoteSessionState::exited ||
                   state == RemoteSessionState::failed;
        }));
        const auto events = session.drain_events();
        assert(!events.empty());
        assert(wait_until([&] {
            const HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, descendant_id);
            if (process == nullptr) return true;
            const DWORD wait = WaitForSingleObject(process, 0);
            CloseHandle(process);
            return wait == WAIT_OBJECT_0;
        }));
        std::filesystem::remove(marker, ignored);
    }

    {
        using nstu::server::ClientRecord;
        using nstu::server::ClientStatus;
        using nstu::server::RemoteSessionTarget;
        using nstu::server::resolve_remote_session_target;

        std::vector<ClientRecord> clients;
        RemoteSessionTarget target;
        std::string error;

        // Empty list
        assert(!resolve_remote_session_target(clients, 1, &target, &error));
        assert(!error.empty());

        // Client not found
        ClientRecord c1;
        c1.id = 1;
        c1.status = ClientStatus::online;
        c1.address = "192.168.1.100";
        clients.push_back(c1);

        assert(!resolve_remote_session_target(clients, 999, &target, &error));
        assert(error == "client was not found in registry");

        // Offline client
        ClientRecord c_offline;
        c_offline.id = 2;
        c_offline.status = ClientStatus::offline;
        c_offline.address = "192.168.1.101";
        clients.push_back(c_offline);
        assert(!resolve_remote_session_target(clients, 2, &target, &error));
        assert(error == "client is offline");

        // Connecting client
        ClientRecord c_connecting;
        c_connecting.id = 3;
        c_connecting.status = ClientStatus::connecting;
        c_connecting.address = "192.168.1.102";
        clients.push_back(c_connecting);
        assert(!resolve_remote_session_target(clients, 3, &target, &error));
        assert(error == "client connection is not authenticated");

        // Plain IPv4
        assert(resolve_remote_session_target(clients, 1, &target, &error));
        assert(target.client_id == 1);
        assert(target.numeric_host == L"192.168.1.100");

        // IPv4 with port (e.g. "192.168.1.105:47001")
        ClientRecord c_port;
        c_port.id = 4;
        c_port.status = ClientStatus::online;
        c_port.address = "192.168.1.105:47001";
        clients.push_back(c_port);
        assert(resolve_remote_session_target(clients, 4, &target, &error));
        assert(target.client_id == 4);
        assert(target.numeric_host == L"192.168.1.105");

        // IPv6 bracketed with port (e.g. "[2001:db8::1]:47001")
        ClientRecord c_ipv6_port;
        c_ipv6_port.id = 5;
        c_ipv6_port.status = ClientStatus::online;
        c_ipv6_port.address = "[2001:db8::1]:47001";
        clients.push_back(c_ipv6_port);
        assert(resolve_remote_session_target(clients, 5, &target, &error));
        assert(target.client_id == 5);
        assert(target.numeric_host == L"2001:db8::1");

        // IPv6 plain (e.g. "2001:db8::1")
        ClientRecord c_ipv6_plain;
        c_ipv6_plain.id = 6;
        c_ipv6_plain.status = ClientStatus::online;
        c_ipv6_plain.address = "2001:db8::1";
        clients.push_back(c_ipv6_plain);
        assert(resolve_remote_session_target(clients, 6, &target, &error));
        assert(target.client_id == 6);
        assert(target.numeric_host == L"2001:db8::1");

        // Malformed brackets
        ClientRecord c_bad_bracket;
        c_bad_bracket.id = 7;
        c_bad_bracket.status = ClientStatus::online;
        c_bad_bracket.address = "[2001:db8::1";
        clients.push_back(c_bad_bracket);
        assert(!resolve_remote_session_target(clients, 7, &target, &error));
        assert(error == "client address has malformed IPv6 brackets");

        // Hostname instead of numeric IP
        ClientRecord c_hostname;
        c_hostname.id = 8;
        c_hostname.status = ClientStatus::online;
        c_hostname.address = "student-workstation.lan";
        clients.push_back(c_hostname);
        assert(!resolve_remote_session_target(clients, 8, &target, &error));
        assert(error == "client address is not a valid numeric IPv4 or IPv6 host");

        // Unspecified IP 0.0.0.0
        ClientRecord c_zero;
        c_zero.id = 9;
        c_zero.status = ClientStatus::online;
        c_zero.address = "0.0.0.0";
        clients.push_back(c_zero);
        assert(!resolve_remote_session_target(clients, 9, &target, &error));
        assert(error == "client address is not a valid numeric IPv4 or IPv6 host");
    }

    return 0;
}
