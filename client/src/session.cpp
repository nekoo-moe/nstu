#include "nstu/session.hpp"

#include <windows.h>
#include <sddl.h>
#include <userenv.h>
#include <wtsapi32.h>

#include <algorithm>
#include <vector>

namespace nstu::client {
namespace {

void set_error(std::string* error, const char* message) {
    if (error != nullptr) {
        *error = message;
    }
}

std::vector<DWORD> interactive_session_candidates() {
    std::vector<DWORD> candidates;
    const DWORD console_session = WTSGetActiveConsoleSessionId();
    if (console_session != 0xffffffffu) {
        candidates.push_back(console_session);
    }

    PWTS_SESSION_INFOW sessions = nullptr;
    DWORD count = 0;
    if (!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &sessions,
                               &count)) {
        return candidates;
    }
    const auto append_state = [&](WTS_CONNECTSTATE_CLASS state) {
        for (DWORD index = 0; index < count; ++index) {
            if (sessions[index].State != state ||
                std::ranges::find(candidates, sessions[index].SessionId) !=
                    candidates.end()) {
                continue;
            }
            candidates.push_back(sessions[index].SessionId);
        }
    };
    append_state(WTSActive);
    append_state(WTSConnected);
    WTSFreeMemory(sessions);
    return candidates;
}

} // namespace

bool launch_agent_in_active_session(const std::wstring& agent_path,
                                     std::string* error) {
    HANDLE user_token = nullptr;
    for (const DWORD session_id : interactive_session_candidates()) {
        if (WTSQueryUserToken(session_id, &user_token)) {
            break;
        }
    }
    if (user_token == nullptr) {
        set_error(error, "no active interactive user token");
        return false;
    }
    void* environment = nullptr;
    CreateEnvironmentBlock(&environment, user_token, FALSE);
    std::wstring command = L"\"" + agent_path + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.lpDesktop = const_cast<wchar_t*>(L"winsta0\\default");
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessAsUserW(
        user_token, agent_path.c_str(), command.data(), nullptr, nullptr, FALSE,
        CREATE_UNICODE_ENVIRONMENT, environment, nullptr, &startup, &process);
    if (environment != nullptr) {
        DestroyEnvironmentBlock(environment);
    }
    CloseHandle(user_token);
    if (!created) {
        set_error(error, "CreateProcessAsUserW failed");
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

bool harden_service_dacl(const std::wstring& service_name, std::string* error) {
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (manager == nullptr) {
        set_error(error, "OpenSCManagerW failed");
        return false;
    }
    SC_HANDLE service = OpenServiceW(manager, service_name.c_str(), WRITE_DAC);
    if (service == nullptr) {
        CloseServiceHandle(manager);
        set_error(error, "OpenServiceW failed");
        return false;
    }
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    constexpr wchar_t sddl[] =
        L"D:P(A;;CCDCLCSWRPWPDTLOCRSDRCWDWO;;;SY)"
        L"(A;;CCDCLCSWRPWPDTLOCRSDRCWDWO;;;BA)"
        L"(A;;LCLORC;;;AU)";
    const bool converted = ConvertStringSecurityDescriptorToSecurityDescriptorW(
        sddl, SDDL_REVISION_1, &descriptor, nullptr) != FALSE;
    const bool applied = converted &&
        SetServiceObjectSecurity(service, DACL_SECURITY_INFORMATION,
                                 descriptor) != FALSE;
    if (descriptor != nullptr) {
        LocalFree(descriptor);
    }
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    if (!applied) {
        set_error(error, "service DACL update failed");
    }
    return applied;
}

} // namespace nstu::client
