#include "nstu/named_pipe.hpp"

#include "nstu/protocol.hpp"

#include <windows.h>
#include <sddl.h>
#include <wtsapi32.h>

#include <limits>
#include <utility>
#include <vector>

namespace nstu::client {
namespace {

void set_error(std::string* error, const char* message) {
    if (error != nullptr) {
        *error = message;
    }
}

} // namespace

NamedPipe::NamedPipe() noexcept = default;

NamedPipe::~NamedPipe() {
    close();
}

NamedPipe::NamedPipe(NamedPipe&& other) noexcept
    : handle_(std::exchange(other.handle_, 0)) {}

NamedPipe& NamedPipe::operator=(NamedPipe&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = std::exchange(other.handle_, 0);
    }
    return *this;
}

bool NamedPipe::create_server(std::wstring_view name, std::string* error) {
    close();
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    constexpr wchar_t sddl[] =
        L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;IU)";
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl, SDDL_REVISION_1, &descriptor, nullptr)) {
        set_error(error, "pipe SDDL conversion failed");
        return false;
    }
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
    const std::wstring pipe_name(name);
    const HANDLE pipe = CreateNamedPipeW(
        pipe_name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
            PIPE_REJECT_REMOTE_CLIENTS,
        1, static_cast<DWORD>(protocol::kMaxCommandPayload + 12),
        static_cast<DWORD>(protocol::kMaxCommandPayload + 12), 0, &attributes);
    LocalFree(descriptor);
    if (pipe == INVALID_HANDLE_VALUE) {
        set_error(error, "CreateNamedPipeW failed");
        return false;
    }
    handle_ = reinterpret_cast<std::uintptr_t>(pipe);
    return true;
}

bool NamedPipe::wait_for_client(std::string* error) const {
    if (!is_open()) {
        set_error(error, "pipe is not open");
        return false;
    }
    if (ConnectNamedPipe(reinterpret_cast<HANDLE>(handle_), nullptr) ||
        GetLastError() == ERROR_PIPE_CONNECTED) {
        return true;
    }
    set_error(error, "ConnectNamedPipe failed");
    return false;
}

bool NamedPipe::validate_client_process(
    std::wstring_view expected_image_path, std::uint32_t* session_id,
    std::string* error) const {
    if (session_id != nullptr) {
        *session_id = 0;
    }
    if (!is_open()) {
        set_error(error, "pipe is not open");
        return false;
    }
    if (expected_image_path.empty()) {
        set_error(error, "expected client image path is empty");
        return false;
    }

    DWORD client_pid = 0;
    if (!GetNamedPipeClientProcessId(reinterpret_cast<HANDLE>(handle_),
                                     &client_pid) ||
        client_pid == 0) {
        set_error(error, "GetNamedPipeClientProcessId failed");
        return false;
    }

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                 client_pid);
    if (process == nullptr) {
        set_error(error, "OpenProcess for pipe client failed");
        return false;
    }

    bool valid = false;
    do {
        DWORD client_session = 0;
        if (!ProcessIdToSessionId(client_pid, &client_session) ||
            client_session == 0) {
            set_error(error, "pipe client is not in an interactive session");
            break;
        }

        LPWSTR state_buffer = nullptr;
        DWORD state_bytes = 0;
        const bool state_queried = WTSQuerySessionInformationW(
            WTS_CURRENT_SERVER_HANDLE, client_session, WTSConnectState,
            &state_buffer, &state_bytes) != FALSE;
        WTS_CONNECTSTATE_CLASS state = WTSDown;
        if (state_queried && state_buffer != nullptr &&
            state_bytes >= sizeof(state)) {
            state = *reinterpret_cast<WTS_CONNECTSTATE_CLASS*>(state_buffer);
        }
        if (state_buffer != nullptr) {
            WTSFreeMemory(state_buffer);
        }
        if (!state_queried || (state != WTSActive && state != WTSConnected)) {
            set_error(error, "pipe client session is not interactive");
            break;
        }

        DWORD capacity = MAX_PATH;
        std::vector<wchar_t> image_buffer(capacity);
        std::wstring image_path;
        for (;;) {
            DWORD image_length = capacity;
            if (QueryFullProcessImageNameW(process, 0, image_buffer.data(),
                                           &image_length)) {
                image_path.assign(image_buffer.data(), image_length);
                break;
            }
            const DWORD query_error = GetLastError();
            if (query_error != ERROR_INSUFFICIENT_BUFFER || capacity >= 32768) {
                set_error(error, "QueryFullProcessImageNameW failed");
                break;
            }
            capacity = std::min<DWORD>(capacity * 2, 32768);
            image_buffer.resize(capacity);
        }
        if (image_path.empty()) {
            break;
        }

        auto get_full_path = [](const std::wstring& input) {
            DWORD path_capacity = MAX_PATH;
            std::vector<wchar_t> path_buffer(path_capacity);
            for (;;) {
                const DWORD path_length = GetFullPathNameW(
                    input.c_str(), path_capacity, path_buffer.data(), nullptr);
                if (path_length == 0) {
                    return std::wstring{};
                }
                if (path_length < path_capacity) {
                    return std::wstring(path_buffer.data(), path_length);
                }
                if (path_length >= 32768) {
                    return std::wstring{};
                }
                path_capacity = path_length + 1;
                path_buffer.resize(path_capacity);
            }
        };
        const std::wstring actual_full_path = get_full_path(image_path);
        const std::wstring expected_full_path =
            get_full_path(std::wstring(expected_image_path));
        if (actual_full_path.empty() || expected_full_path.empty() ||
            CompareStringOrdinal(actual_full_path.c_str(), -1,
                                 expected_full_path.c_str(), -1, TRUE) !=
                CSTR_EQUAL) {
            set_error(error, "pipe client image path does not match NSTU agent");
            break;
        }

        if (session_id != nullptr) {
            *session_id = client_session;
        }
        valid = true;
    } while (false);

    CloseHandle(process);
    return valid;
}

bool NamedPipe::connect_client(std::wstring_view name, std::uint32_t timeout_ms,
                               std::string* error) {
    close();
    const std::wstring pipe_name(name);
    if (!WaitNamedPipeW(pipe_name.c_str(), timeout_ms)) {
        set_error(error, "named pipe is unavailable");
        return false;
    }
    const HANDLE pipe = CreateFileW(pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE,
                                    0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        set_error(error, "named pipe client connection failed");
        return false;
    }
    handle_ = reinterpret_cast<std::uintptr_t>(pipe);
    return true;
}

int NamedPipe::write(std::span<const std::byte> bytes, std::string* error) const {
    if (!is_open() || bytes.size() >
        static_cast<std::size_t>(std::numeric_limits<DWORD>::max())) {
        set_error(error, "invalid pipe write");
        return -1;
    }
    DWORD written = 0;
    if (!WriteFile(reinterpret_cast<HANDLE>(handle_), bytes.data(),
                   static_cast<DWORD>(bytes.size()), &written, nullptr)) {
        set_error(error, "pipe write failed");
        return -1;
    }
    return static_cast<int>(written);
}

int NamedPipe::read(std::span<std::byte> bytes, std::string* error) const {
    if (!is_open() || bytes.size() >
        static_cast<std::size_t>(std::numeric_limits<DWORD>::max())) {
        set_error(error, "invalid pipe read");
        return -1;
    }
    DWORD read_bytes = 0;
    if (!ReadFile(reinterpret_cast<HANDLE>(handle_), bytes.data(),
                  static_cast<DWORD>(bytes.size()), &read_bytes, nullptr)) {
        set_error(error, "pipe read failed");
        return -1;
    }
    return static_cast<int>(read_bytes);
}

bool NamedPipe::available_bytes(std::uint32_t& bytes, std::string* error) const {
    bytes = 0;
    if (!is_open()) {
        set_error(error, "pipe is not open");
        return false;
    }
    DWORD available = 0;
    if (!PeekNamedPipe(reinterpret_cast<HANDLE>(handle_), nullptr, 0, nullptr,
                       &available, nullptr)) {
        set_error(error, "PeekNamedPipe failed");
        return false;
    }
    bytes = available;
    return true;
}

bool NamedPipe::is_open() const noexcept {
    return handle_ != 0 && handle_ !=
           reinterpret_cast<std::uintptr_t>(INVALID_HANDLE_VALUE);
}

void NamedPipe::close() noexcept {
    if (is_open()) {
        CloseHandle(reinterpret_cast<HANDLE>(handle_));
    }
    handle_ = 0;
}

} // namespace nstu::client
