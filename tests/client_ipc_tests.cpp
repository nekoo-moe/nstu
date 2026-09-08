#include "nstu/agent_protocol.hpp"
#include "nstu/client_config.hpp"

#include <windows.h>
#include <wtsapi32.h>

#include <array>
#include <cassert>
#include <cstring>
#include <string>
#include <thread>
#include <filesystem>
#include <vector>

int main() {
    const std::wstring pipe_name = L"\\\\.\\pipe\\nstu-test-" +
                                   std::to_wstring(GetCurrentProcessId());
    nstu::client::NamedPipe server;
    std::string error;
    assert(server.create_server(pipe_name, &error));

    std::thread server_thread([&server] {
        std::string thread_error;
        assert(server.wait_for_client(&thread_error));
        wchar_t executable[MAX_PATH]{};
        assert(GetModuleFileNameW(nullptr, executable, MAX_PATH) != 0);
        std::uint32_t session_id = 0;
        DWORD current_session = 0;
        assert(ProcessIdToSessionId(GetCurrentProcessId(), &current_session));
        LPWSTR state_buffer = nullptr;
        DWORD state_bytes = 0;
        const bool state_queried =
            current_session != 0 &&
            WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE,
                                        current_session, WTSConnectState,
                                        &state_buffer, &state_bytes) != FALSE;
        WTS_CONNECTSTATE_CLASS state = WTSDown;
        if (state_queried && state_buffer != nullptr &&
            state_bytes >= sizeof(state)) {
            state = *reinterpret_cast<WTS_CONNECTSTATE_CLASS*>(state_buffer);
        }
        if (state_buffer != nullptr) {
            WTSFreeMemory(state_buffer);
        }
        const bool interactive_session =
            state_queried && (state == WTSActive || state == WTSConnected);
        const bool valid_client = server.validate_client_process(
            executable, &session_id, &thread_error);
        assert(valid_client == interactive_session);
        if (interactive_session) {
            assert(session_id == current_session);
        } else {
            assert(session_id == 0);
        }
        assert(!server.validate_client_process(
            L"C:\\NSTU\\missing\\nstu-agent.exe", &session_id,
            &thread_error));
        std::array<std::byte, 1> command{};
        assert(server.read(command, &thread_error) == 1);
        assert(command[0] == std::byte{0x01});
        const std::array<std::byte, 1> response{std::byte{0x02}};
        assert(server.write(response, &thread_error) == 1);
    });

    nstu::client::NamedPipe client;
    assert(client.connect_client(pipe_name, 2000, &error));
    const std::array<std::byte, 1> command{std::byte{0x01}};
    assert(client.write(command, &error) == 1);
    std::array<std::byte, 1> response{};
    assert(client.read(response, &error) == 1);
    assert(response[0] == std::byte{0x02});
    server_thread.join();

    const nstu::client::AgentStatus status{
        .locked = true,
        .streaming = true,
        .snapshotting = true,
        .viewing_broadcast = true,
        .frames_per_second = 10,
        .snapshot_interval_seconds = 7,
        .session_id = 7,
    };
    const nstu::client::AgentMessage status_message{
        nstu::client::AgentMessageType::status_report,
        nstu::client::encode_agent_status(status)};
    const auto status_wire =
        nstu::client::encode_agent_message(status_message);
    const auto decoded_message =
        nstu::client::decode_agent_message(status_wire);
    assert(decoded_message.has_value());
    const auto decoded_status =
        nstu::client::decode_agent_status(decoded_message->payload);
    assert(decoded_status.has_value());
    assert(decoded_status->locked);
    assert(decoded_status->streaming);
    assert(decoded_status->snapshotting);
    assert(decoded_status->viewing_broadcast);
    assert(decoded_status->frames_per_second == 10);
    assert(decoded_status->snapshot_interval_seconds == 7);
    assert(decoded_status->session_id == 7);
    auto corrupt = status_wire;
    corrupt[0] ^= std::byte{1};
    assert(!nstu::client::decode_agent_message(corrupt).has_value());

    const nstu::wire::RemoteInputPacket remote_input{
        .input_type = static_cast<std::uint8_t>(nstu::wire::RemoteInputType::mouse),
        .flags = static_cast<std::uint8_t>(nstu::wire::RemoteInputFlags::mouse_absolute),
        .x = 640,
        .y = 360,
        .virtual_key = 0,
        .reserved = 0,
        .mouse_data = 0,
    };
    const auto remote_payload = nstu::client::encode_remote_input(remote_input);
    assert(remote_payload.size() == sizeof(remote_input));
    const auto decoded_remote =
        nstu::client::decode_remote_input(remote_payload);
    assert(decoded_remote.has_value());
    assert(std::memcmp(&*decoded_remote, &remote_input,
                       sizeof(remote_input)) == 0);
    const auto remote_message = nstu::client::encode_agent_message(
        {nstu::client::AgentMessageType::remote_input, remote_payload});
    assert(nstu::client::decode_agent_message(remote_message).has_value());

    nstu::client::ClientRuntimeConfig runtime;
    runtime.server_address = "127.0.0.1";
    runtime.server_port = 47001;
    runtime.client_id.fill(std::byte{0x22});
    runtime.key_id = 9;
    runtime.pre_shared_key.resize(nstu::security::kMinimumProtocolKeyBytes,
                                  std::byte{0x44});
    wchar_t temporary_directory[MAX_PATH]{};
    assert(GetTempPathW(MAX_PATH, temporary_directory) != 0);
    const auto config_path = std::filesystem::path(temporary_directory) /
        (L"nstu-client-config-test-" +
         std::to_wstring(GetCurrentProcessId()) + L".bin");
    const std::array<std::byte, 4> entropy{
        std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    assert(nstu::client::save_client_runtime_config(
        runtime, config_path.wstring(), entropy, &error));
    nstu::client::ClientRuntimeConfig loaded;
    assert(nstu::client::load_client_runtime_config(
        loaded, config_path.wstring(), entropy, &error));
    assert(loaded.server_address == runtime.server_address);
    assert(loaded.client_id == runtime.client_id);
    assert(loaded.pre_shared_key == runtime.pre_shared_key);
    nstu::client::clear_client_runtime_config(runtime);
    nstu::client::clear_client_runtime_config(loaded);
    assert(DeleteFileW(config_path.c_str()));
    return 0;
}
