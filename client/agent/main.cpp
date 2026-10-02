#include "nstu/agent_protocol.hpp"
#include "nstu/exam_profile.hpp"
#include "nstu/control_messages.hpp"
#include "nstu/deployment.hpp"
#include "nstu/exam_bridge.hpp"
#include "nstu/exam_control.hpp"
#include "nstu/exam_host.hpp"
#include "nstu/screen_snapshot.hpp"

#include <windows.h>
#include <shellapi.h>
#include <objbase.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <filesystem>
#include <iterator>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

// WebView2 is an apartment-threaded COM client.  Keep the initialization
// scoped to the agent UI thread so every exit path (including window-creation
// failures) balances a successful CoInitializeEx call.
class UiComApartment final {
public:
    UiComApartment() noexcept
        : result_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}

    ~UiComApartment() {
        if (SUCCEEDED(result_)) {
            CoUninitialize();
        }
    }

    UiComApartment(const UiComApartment&) = delete;
    UiComApartment& operator=(const UiComApartment&) = delete;

    [[nodiscard]] bool ready() const noexcept {
        return SUCCEEDED(result_);
    }

    [[nodiscard]] HRESULT result() const noexcept { return result_; }

private:
    HRESULT result_;
};

constexpr wchar_t kWindowClass[] = L"NstuAgentOverlay";
constexpr wchar_t kChatWindowClass[] = L"NstuAgentChat";
constexpr wchar_t kAnnotationWindowClass[] = L"NstuAgentAnnotation";
constexpr wchar_t kBroadcastWindowClass[] = L"NstuAgentBroadcast";
constexpr wchar_t kPairingWindowClass[] = L"NstuAgentPairing";
constexpr wchar_t kInstanceMutex[] = L"Local\\NSTU.Agent.Singleton";
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kAgentCommandMessage = WM_APP + 2;
constexpr UINT kAnnotationUpdatedMessage = WM_APP + 3;
constexpr UINT kBroadcastUpdatedMessage = WM_APP + 4;
// The WebView2 host drains exam_bridge() after this notification. Keeping the
// bridge notification in the agent avoids doing browser work on the pipe
// thread.
constexpr UINT kExamBridgeMessage = WM_APP + 5;
// A service pipe loss is terminal for the current exam session. The pipe
// thread posts this message and the UI thread performs ExamHost::stop(), which
// is required because WebView2 and the kiosk window are UI/STA-owned.
constexpr UINT kExamServiceDisconnectedMessage = WM_APP + 6;
constexpr UINT kExamHostStatusMessage = WM_APP + 7;
// Pairing state is written by the pipe thread and drawn by the UI thread,
// so the transport never touches a window directly.
constexpr UINT kPairingUpdatedMessage = WM_APP + 8;
constexpr UINT kTrayId = 1;
constexpr int kChatMessages = 1001;
constexpr int kChatInput = 1002;
constexpr int kChatSend = 1003;
constexpr int kPairingList = 1004;
constexpr int kPairingConnect = 1005;
constexpr UINT_PTR kPairingStatusTimer = 1;

HWND g_chat_window = nullptr;
HWND g_chat_messages = nullptr;
HWND g_chat_input = nullptr;
WNDPROC g_chat_input_original_proc = nullptr;
// Shared Segoe UI font for the agent's Win32 form controls (chat + pairing).
// The default GUI stock font is the dated MS Shell Dlg face; a real Segoe UI
// pass is what stops the chat looking like a 90s dialog. Created lazily and
// kept for the process lifetime (reclaimed by the OS at exit).
HFONT g_ui_font = nullptr;
UINT g_ui_font_dpi = 0;
HWND g_lock_window = nullptr;
HWND g_annotation_window = nullptr;
HWND g_broadcast_window = nullptr;
HWND g_pairing_window = nullptr;
HWND g_pairing_list = nullptr;
nstu::client::ExamHost g_exam_host;
std::atomic_bool g_agent_stopping = false;
std::atomic_bool g_locked = false;
std::atomic_bool g_streaming = false;
std::atomic<std::uint8_t> g_stream_fps = 0;
std::atomic_bool g_snapshotting = false;
std::atomic<std::uint16_t> g_snapshot_interval_seconds = 0;
std::atomic_bool g_viewing_broadcast = false;
std::atomic_bool g_remote_control_active = false;
std::atomic_bool g_managed = false;
// Whether the service currently reports a live authenticated session to the
// teacher server. The chat window is gated on this: it only opens when a
// teacher is actually reachable, otherwise the agent shows "Something went
// wrong." Driven by AgentMessageType::server_online from the service.
std::atomic_bool g_server_connected = false;
std::mutex g_annotation_mutex;
std::vector<nstu::control::OverlayStroke> g_annotation_strokes;
std::mutex g_broadcast_mutex;
nstu::screen::BgraImage g_broadcast_image;
std::mutex g_service_queue_mutex;
std::deque<nstu::client::AgentMessage> g_service_queue;
constexpr std::size_t kMaximumQueuedServiceMessages = 128;
std::mutex g_exam_command_mutex;
std::deque<nstu::client::AgentMessage> g_exam_commands;
constexpr std::size_t kMaximumQueuedExamCommands = 4;
bool g_exam_controls_suppressed = false;
bool g_chat_was_visible_for_exam = false;
bool g_lock_was_visible_for_exam = false;
bool g_annotation_was_visible_for_exam = false;
bool g_broadcast_was_visible_for_exam = false;

// Pairing is the one thing this agent puts on screen before the machine
// belongs to anybody, so it has its own window rather than a tray balloon
// nobody reads. It has four faces: a menu when the LAN answers with more
// than one server, an acknowledgement that the request went out, the six
// digits the teacher has to match, and a sentence saying how it ended.
enum class PairingView {
    hidden,
    choices,
    waiting,
    code,
    status,
};

std::mutex g_pairing_mutex;
PairingView g_pairing_view = PairingView::hidden;
std::vector<nstu::client::AgentPairingChoice> g_pairing_choices;
std::wstring g_pairing_caption;
std::wstring g_pairing_code;

void queue_service_message(nstu::client::AgentMessage message) noexcept {
    if (message.payload.size() > nstu::client::kMaximumAgentPayloadBytes) {
        OutputDebugStringA("NSTU exam message exceeded the agent payload limit\n");
        return;
    }
    try {
        std::scoped_lock lock(g_service_queue_mutex);
        if (g_service_queue.size() >= kMaximumQueuedServiceMessages) {
            // Answer/state messages are durable in the service outbox. Keep
            // the queue bounded and discard the oldest volatile transport
            // copy if the service is unavailable for an extended period.
            g_service_queue.pop_front();
        }
        g_service_queue.push_back(std::move(message));
    } catch (...) {
        OutputDebugStringA("NSTU could not queue an exam service message\n");
    }
}

// Pairing text is printable ASCII by the time the codec has accepted it,
// so widening it needs no code page and cannot fail.
std::wstring widen_ascii(std::string_view text) {
    std::wstring wide;
    wide.reserve(text.size());
    for (const char character : text) {
        wide.push_back(
            static_cast<wchar_t>(static_cast<unsigned char>(character)));
    }
    return wide;
}

void notify_pairing_view() noexcept {
    if (g_pairing_window != nullptr) {
        PostMessageW(g_pairing_window, kPairingUpdatedMessage, 0, 0);
    }
}

void show_pairing_choices(
    std::vector<nstu::client::AgentPairingChoice> choices) {
    {
        std::scoped_lock lock(g_pairing_mutex);
        g_pairing_choices = std::move(choices);
        g_pairing_caption =
            L"More than one NSTU server answered on this network. Ask your "
            L"teacher which one this computer belongs to.";
        g_pairing_code.clear();
        g_pairing_view = PairingView::choices;
    }
    notify_pairing_view();
}

void show_pairing_code(const nstu::client::AgentPairingCode& code) {
    {
        std::scoped_lock lock(g_pairing_mutex);
        g_pairing_code = widen_ascii(code.code);
        // The digits are worth nothing on their own: what makes them a
        // check is that the same six appear on the server, so the name of
        // the server claiming them is part of the question.
        g_pairing_caption =
            L"Tell your teacher these digits. Approve on \"" +
            widen_ascii(code.server_name) +
            L"\" only if the same six digits are on that screen.";
        g_pairing_view = PairingView::code;
    }
    notify_pairing_view();
}

void show_pairing_status(const nstu::client::AgentPairingStatus& status) {
    {
        std::scoped_lock lock(g_pairing_mutex);
        g_pairing_caption = widen_ascii(status.detail);
        g_pairing_code.clear();
        g_pairing_view = PairingView::status;
    }
    notify_pairing_view();
}

// Nothing on this window can finish without the service, so a lost pipe
// takes it off the screen rather than leaving a stale code up.
void hide_pairing_view() {
    {
        std::scoped_lock lock(g_pairing_mutex);
        g_pairing_view = PairingView::hidden;
        g_pairing_choices.clear();
        g_pairing_caption.clear();
        g_pairing_code.clear();
    }
    notify_pairing_view();
}

void queue_exam_command_for_ui(nstu::client::AgentMessage message) noexcept {
    try {
        std::scoped_lock lock(g_exam_command_mutex);
        if (message.type == nstu::client::AgentMessageType::exam_stop) {
            // A stop supersedes any not-yet-started request. This prevents a
            // delayed start from resurrecting a session after disconnect.
            g_exam_commands.clear();
        } else {
            std::erase_if(g_exam_commands, [](const auto& queued) {
                return queued.type ==
                       nstu::client::AgentMessageType::exam_start;
            });
        }
        if (g_exam_commands.size() >= kMaximumQueuedExamCommands) {
            g_exam_commands.pop_front();
        }
        g_exam_commands.push_back(std::move(message));
    } catch (...) {
        OutputDebugStringA("NSTU could not queue an exam UI command\n");
    }
}

std::optional<nstu::client::AgentMessage> pop_exam_command_for_ui() noexcept {
    try {
        std::scoped_lock lock(g_exam_command_mutex);
        if (g_exam_commands.empty()) return std::nullopt;
        auto result = std::move(g_exam_commands.front());
        g_exam_commands.pop_front();
        return result;
    } catch (...) {
        return std::nullopt;
    }
}

void clear_exam_commands_for_ui() noexcept {
    try {
        std::scoped_lock lock(g_exam_command_mutex);
        g_exam_commands.clear();
    } catch (...) {
        OutputDebugStringA("NSTU could not clear queued exam commands\n");
    }
}

std::string bytes_to_hex(std::span<const std::byte> bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (const auto byte : bytes) {
        const auto value = std::to_integer<unsigned int>(byte);
        result.push_back(digits[(value >> 4u) & 0x0fu]);
        result.push_back(digits[value & 0x0fu]);
    }
    return result;
}

std::string json_escape(std::string_view value) {
    std::string result;
    result.reserve(value.size() + 16);
    static constexpr char digits[] = "0123456789abcdef";
    for (const unsigned char character : value) {
        switch (character) {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\b': result += "\\b"; break;
        case '\f': result += "\\f"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (character < 0x20u) {
                result += "\\u00";
                result.push_back(digits[(character >> 4u) & 0x0fu]);
                result.push_back(digits[character & 0x0fu]);
            } else {
                result.push_back(static_cast<char>(character));
            }
            break;
        }
    }
    return result;
}

bool flush_service_messages(nstu::client::NamedPipe& pipe) noexcept {
    std::deque<nstu::client::AgentMessage> pending;
    {
        std::scoped_lock lock(g_service_queue_mutex);
        pending.swap(g_service_queue);
    }
    while (!pending.empty()) {
        auto message = std::move(pending.front());
        pending.pop_front();
        if (nstu::client::send_agent_message(pipe, message, nullptr)) {
            continue;
        }
        // Preserve unsent messages for the next authenticated pipe instance.
        // The already-sent prefix must not be replayed here; the service's
        // durable outbox handles duplicate answer events separately.
        std::scoped_lock lock(g_service_queue_mutex);
        if (g_service_queue.size() >= kMaximumQueuedServiceMessages) {
            g_service_queue.pop_back();
        }
        g_service_queue.push_front(std::move(message));
        while (!pending.empty()) {
            if (g_service_queue.size() >= kMaximumQueuedServiceMessages) {
                g_service_queue.pop_back();
            }
            g_service_queue.push_front(std::move(pending.back()));
            pending.pop_back();
        }
        return false;
    }
    return true;
}

bool exam_host_engaged() noexcept {
    const auto state = g_exam_host.state();
    return g_exam_host.window() != nullptr &&
           state != nstu::client::ExamHostState::idle &&
           state != nstu::client::ExamHostState::failed;
}

std::wstring utf8_to_wide(std::span<const std::byte> bytes);

void restore_exam_suppressed_windows() noexcept {
    if (!g_exam_controls_suppressed) {
        return;
    }
    if (g_chat_window != nullptr) {
        ShowWindow(g_chat_window,
                   g_chat_was_visible_for_exam ? SW_SHOWNA : SW_HIDE);
    }
    if (g_lock_window != nullptr) {
        ShowWindow(g_lock_window,
                   g_lock_was_visible_for_exam ? SW_SHOWNA : SW_HIDE);
    }
    if (g_annotation_window != nullptr) {
        ShowWindow(g_annotation_window,
                   g_annotation_was_visible_for_exam ? SW_SHOWNOACTIVATE
                                                      : SW_HIDE);
    }
    if (g_broadcast_window != nullptr) {
        ShowWindow(g_broadcast_window,
                   g_broadcast_was_visible_for_exam ? SW_SHOWNOACTIVATE
                                                     : SW_HIDE);
    }
    g_exam_controls_suppressed = false;
}

void restore_control_window_order() {
    if (exam_host_engaged()) {
        if (!g_exam_controls_suppressed) {
            g_exam_controls_suppressed = true;
            g_chat_was_visible_for_exam =
                g_chat_window != nullptr && IsWindowVisible(g_chat_window);
            g_lock_was_visible_for_exam =
                g_lock_window != nullptr && IsWindowVisible(g_lock_window);
            g_annotation_was_visible_for_exam =
                g_annotation_window != nullptr &&
                IsWindowVisible(g_annotation_window);
            g_broadcast_was_visible_for_exam =
                g_broadcast_window != nullptr &&
                IsWindowVisible(g_broadcast_window);
        }
        if (g_chat_window != nullptr) ShowWindow(g_chat_window, SW_HIDE);
        if (g_lock_window != nullptr) ShowWindow(g_lock_window, SW_HIDE);
        if (g_annotation_window != nullptr)
            ShowWindow(g_annotation_window, SW_HIDE);
        if (g_broadcast_window != nullptr)
            ShowWindow(g_broadcast_window, SW_HIDE);
        g_exam_host.enforce_foreground();
        return;
    }
    restore_exam_suppressed_windows();
    constexpr UINT flags =
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER;
    if (g_broadcast_window != nullptr &&
        IsWindowVisible(g_broadcast_window)) {
        SetWindowPos(g_broadcast_window, HWND_TOPMOST, 0, 0, 0, 0, flags);
    }
    if (g_annotation_window != nullptr &&
        IsWindowVisible(g_annotation_window)) {
        SetWindowPos(g_annotation_window, HWND_TOPMOST, 0, 0, 0, 0, flags);
    }
    if (g_lock_window != nullptr && g_locked.load() &&
        IsWindowVisible(g_lock_window)) {
        SetWindowPos(g_lock_window, HWND_TOPMOST, 0, 0, 0, 0, flags);
    }
}

// Entry point for the authenticated exam-start path. The service/session
// layer supplies the already validated package options; this function only
// wires the UI-owned host to the bounded, asynchronous pipe queue.
[[maybe_unused]] bool start_exam_host_ui(
    HWND owner, const nstu::client::ExamHostOptions& options) {
    if (g_exam_host.window() != nullptr) {
        if (g_exam_host.active()) {
            return true;
        }
        g_exam_host.stop();
    }
    nstu::client::ExamHostCallbacks callbacks;
    callbacks.send_to_service = [](nstu::client::AgentMessage message) {
        queue_service_message(std::move(message));
    };
    callbacks.status = [owner](std::string status) {
        const std::string line = "NSTU exam host: " + status + "\n";
        OutputDebugStringA(line.c_str());
        if (owner != nullptr) {
            // Failure can be reported by an asynchronous WebView2 callback.
            // Marshal cleanup back to the agent UI thread instead of stopping
            // the host re-entrantly from inside the browser callback.
            PostMessageW(owner, kExamHostStatusMessage, 0, 0);
        }
    };
    std::string error;
    const bool started = g_exam_host.start(owner, options, std::move(callbacks),
                                           &error);
    if (!started || g_exam_host.state() == nstu::client::ExamHostState::failed) {
        if (!error.empty()) {
            const std::string line = "NSTU exam host start failed: " + error +
                                     "\n";
            OutputDebugStringA(line.c_str());
        }
        g_exam_host.stop();
        restore_control_window_order();
        return false;
    }
    restore_control_window_order();
    return true;
}

bool start_exam_from_command_ui(
    HWND owner, const nstu::client::AgentMessage& message) {
    const auto request = nstu::exam::decode_exam_start_request(message.payload);
    if (!request) {
        OutputDebugStringA("NSTU rejected malformed exam start command\n");
        return false;
    }
    const auto package_root = utf8_to_wide(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(request->package_root.data()),
        request->package_root.size()));
    const auto web_root = utf8_to_wide(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(request->web_root.data()),
        request->web_root.size()));
    const auto user_data_root = utf8_to_wide(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(request->user_data_root.data()),
        request->user_data_root.size()));
    if (package_root.empty() ||
        (!request->web_root.empty() && web_root.empty()) ||
        (!request->user_data_root.empty() && user_data_root.empty())) {
        OutputDebugStringA("NSTU exam start path is not valid UTF-8\n");
        return false;
    }
    std::string data_root_error;
    const auto data_root = nstu::deployment::data_root(&data_root_error);
    if (data_root.empty()) {
        OutputDebugStringA(("NSTU exam data root is unavailable: " +
                            (data_root_error.empty()
                                 ? "unknown error"
                                 : data_root_error) +
                            "\n")
                               .c_str());
        return false;
    }
    wchar_t local_app_data_buffer[32768]{};
    const DWORD local_app_data_length = GetEnvironmentVariableW(
        L"LOCALAPPDATA", local_app_data_buffer,
        static_cast<DWORD>(std::size(local_app_data_buffer)));
    if (local_app_data_length == 0 ||
        local_app_data_length >= std::size(local_app_data_buffer)) {
        OutputDebugStringA("NSTU exam local profile root is unavailable\n");
        return false;
    }
    const auto profile_root = std::filesystem::path(local_app_data_buffer) /
                              L"NSTU" / L"exam-webview";
    const auto derived_user_data_root =
        nstu::client::derive_exam_profile_path(
            profile_root, request->package_digest, request->client_id,
            request->session_id);
    if (!derived_user_data_root) {
        OutputDebugStringA("NSTU exam profile identity is invalid\n");
        return false;
    }
    const auto& effective_user_data_root = *derived_user_data_root;
    if (!user_data_root.empty()) {
        // Keep the wire field for protocol compatibility, but never permit a
        // server command to select an arbitrary persistent browser profile.
        // An explicit path is accepted only when it resolves to the same
        // digest/client/session-bound directory selected locally.
        std::string supplied_path_error;
        if (!nstu::client::validate_exam_user_data_path(
                profile_root, user_data_root, &supplied_path_error)) {
            OutputDebugStringA("NSTU exam supplied profile path is invalid\n");
            return false;
        }
        std::error_code supplied_compare_error;
        std::error_code derived_compare_error;
        const auto supplied_canonical = std::filesystem::weakly_canonical(
            user_data_root, supplied_compare_error);
        const auto derived_canonical = std::filesystem::weakly_canonical(
            effective_user_data_root, derived_compare_error);
        if (supplied_compare_error || derived_compare_error ||
            supplied_canonical != derived_canonical) {
            OutputDebugStringA(
                "NSTU exam supplied profile path does not match the authenticated context\n");
            return false;
        }
    }
    std::string path_error;
    if (!nstu::client::validate_exam_path_policy(
            data_root, package_root, web_root, {},
            &path_error)) {
        OutputDebugStringA(("NSTU exam path policy rejected start: " +
                            (path_error.empty() ? "unknown error" : path_error) +
                            "\n")
                               .c_str());
        return false;
    }
    if (!nstu::client::validate_exam_user_data_path(
            profile_root, effective_user_data_root, &path_error)) {
        OutputDebugStringA(("NSTU exam profile path rejected: " +
                            (path_error.empty() ? "unknown error" : path_error) +
                            "\n")
                               .c_str());
        return false;
    }
    const std::string context =
        "{\"packageId\":\"" + json_escape(request->package_id) +
        "\",\"packageDigestHex\":\"" +
        bytes_to_hex(request->package_digest) +
        "\",\"clientIdHex\":\"" + bytes_to_hex(request->client_id) +
        "\",\"sessionIdHex\":\"" + bytes_to_hex(request->session_id) +
        "\",\"candidateId\":\"" + json_escape(request->candidate_id) +
        "\",\"synchronizedStart\":true}";
    nstu::client::ExamHostOptions options;
    options.package_root = package_root;
    options.web_root = web_root;
    options.user_data_root = effective_user_data_root;
    options.allowed_data_root = data_root;
    options.allowed_user_data_root = profile_root;
    options.context_json = context;
    options.expected_digest_hex = bytes_to_hex(request->package_digest);
    options.require_digest = true;
    return start_exam_host_ui(owner, options);
}

std::uint64_t unix_milliseconds_now() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<
        std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

std::wstring utf8_to_wide(std::span<const std::byte> bytes) {
    if (bytes.empty()) {
        return {};
    }
    const auto* text = reinterpret_cast<const char*>(bytes.data());
    const int length = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text, static_cast<int>(bytes.size()),
        nullptr, 0);
    if (length <= 0) {
        return {};
    }
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text,
                            static_cast<int>(bytes.size()), wide.data(),
                            length) != length) {
        return {};
    }
    return wide;
}

bool send_agent_status(nstu::client::NamedPipe& pipe) {
    const nstu::client::AgentStatus status{
        .locked = g_locked.load(),
        .streaming = g_streaming.load(),
        .snapshotting = g_snapshotting.load(),
        .viewing_broadcast = g_viewing_broadcast.load(),
        .frames_per_second = g_stream_fps.load(),
        .snapshot_interval_seconds =
            g_snapshot_interval_seconds.load(),
        .session_id = WTSGetActiveConsoleSessionId(),
    };
    return nstu::client::send_agent_message(
        pipe,
        {nstu::client::AgentMessageType::status_report,
         nstu::client::encode_agent_status(status)},
         nullptr);
}

void stop_remote_control() noexcept {
    if (g_remote_control_active.exchange(false)) {
        (void)BlockInput(FALSE);
    }
}

bool apply_remote_input(const nstu::wire::RemoteInputPacket& packet) {
    if (packet.input_type == static_cast<std::uint8_t>(
                                nstu::wire::RemoteInputType::keyboard)) {
        INPUT input{};
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = packet.virtual_key;
        input.ki.dwFlags =
            (packet.flags & static_cast<std::uint8_t>(
                                nstu::wire::RemoteInputFlags::key_up))
                ? KEYEVENTF_KEYUP
                : 0;
        return SendInput(1, &input, sizeof(input)) == 1;
    }
    if (packet.input_type != static_cast<std::uint8_t>(
                                nstu::wire::RemoteInputType::mouse)) {
        return false;
    }
    INPUT input{};
    input.type = INPUT_MOUSE;
    const auto normalized = static_cast<std::uint8_t>(
        nstu::wire::RemoteInputFlags::mouse_normalized);
    if ((packet.flags & normalized) != 0) {
        if (packet.x < 0 || packet.x > 65535 || packet.y < 0 ||
            packet.y > 65535) {
            return false;
        }
        input.mi.dx = packet.x;
        input.mi.dy = packet.y;
    } else {
        const int width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        const int height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        const int origin_x = GetSystemMetrics(SM_XVIRTUALSCREEN);
        const int origin_y = GetSystemMetrics(SM_YVIRTUALSCREEN);
        if (width <= 1 || height <= 1 || packet.x < origin_x ||
            packet.y < origin_y || packet.x >= origin_x + width ||
            packet.y >= origin_y + height) {
            return false;
        }
        input.mi.dx = static_cast<LONG>(
            (static_cast<std::int64_t>(packet.x - origin_x) * 65535) /
            (width - 1));
        input.mi.dy = static_cast<LONG>(
            (static_cast<std::int64_t>(packet.y - origin_y) * 65535) /
            (height - 1));
    }
    input.mi.mouseData = packet.mouse_data;
    // MOUSEEVENTF_MOVE is required alongside ABSOLUTE for the dx/dy to
    // reposition the cursor; without it SendInput succeeds but the pointer
    // never moves and button events fire at a stale location.
    input.mi.dwFlags =
        MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    const auto flags = packet.flags;
    if ((flags & static_cast<std::uint8_t>(
                     nstu::wire::RemoteInputFlags::mouse_left_down)) != 0) {
        input.mi.dwFlags |= MOUSEEVENTF_LEFTDOWN;
    }
    if ((flags & static_cast<std::uint8_t>(
                     nstu::wire::RemoteInputFlags::mouse_left_up)) != 0) {
        input.mi.dwFlags |= MOUSEEVENTF_LEFTUP;
    }
    if ((flags & static_cast<std::uint8_t>(
                     nstu::wire::RemoteInputFlags::mouse_right_down)) != 0) {
        input.mi.dwFlags |= MOUSEEVENTF_RIGHTDOWN;
    }
    if ((flags & static_cast<std::uint8_t>(
                     nstu::wire::RemoteInputFlags::mouse_right_up)) != 0) {
        input.mi.dwFlags |= MOUSEEVENTF_RIGHTUP;
    }
    return SendInput(1, &input, sizeof(input)) == 1;
}

void pipe_control_loop(HWND overlay) {
    auto next_snapshot = std::chrono::steady_clock::now();
    // Live Focus streaming captures on its own fps-paced clock, distinct from
    // the slow class-view snapshot cadence above.
    auto next_stream_frame = std::chrono::steady_clock::now();
    // Streaming diagnostics accumulators (plan Epic B); flushed ~once/second so
    // "laggy" can be attributed to real capture+encode cost, not guessed.
    auto stream_stat_window = std::chrono::steady_clock::now();
    int stream_frame_count = 0;
    double stream_capture_ms_total = 0.0;
    std::size_t stream_bytes_total = 0;
    while (!g_agent_stopping.load()) {
        nstu::client::NamedPipe pipe;
        if (!pipe.connect_client(nstu::client::kControlPipeName, 1000,
                                 nullptr)) {
            Sleep(50);
            continue;
        }
        bool pipe_failed = false;
        while (!g_agent_stopping.load()) {
            // ExamHost callbacks are issued on the UI/STA thread. Only this
            // transport thread drains the queue, so WebView2 never blocks on
            // a named-pipe write.
            if (!flush_service_messages(pipe)) {
                pipe_failed = true;
                break;
            }
            std::uint32_t available = 0;
            if (!pipe.available_bytes(available, nullptr)) {
                pipe_failed = true;
                break;
            }
            if (available == 0) {
                Sleep(10);
            } else {
                auto message =
                    nstu::client::receive_agent_message(pipe, nullptr);
                if (!message) {
                    pipe_failed = true;
                    break;
                }
                if (message->type == nstu::client::AgentMessageType::lock ||
                    message->type == nstu::client::AgentMessageType::unlock ||
                    message->type ==
                        nstu::client::AgentMessageType::stop_stream ||
                    message->type ==
                        nstu::client::AgentMessageType::keyframe_request) {
                    SendMessageW(overlay, kAgentCommandMessage,
                                 static_cast<WPARAM>(message->type), 0);
                } else if (message->type ==
                               nstu::client::AgentMessageType::start_stream &&
                           message->payload.size() == 1) {
                    SendMessageW(
                        overlay, kAgentCommandMessage,
                        static_cast<WPARAM>(message->type),
                        std::to_integer<std::uint8_t>(message->payload[0]));
                } else if (message->type ==
                           nstu::client::AgentMessageType::chat) {
                    const auto chat = utf8_to_wide(message->payload);
                    if (!chat.empty()) {
                        // Label the sender so the student can tell teacher
                        // messages apart from their own "You:" lines.
                        const std::wstring labeled = L"Teacher: " + chat;
                        SendMessageW(overlay, kAgentCommandMessage,
                                     static_cast<WPARAM>(message->type),
                                     reinterpret_cast<LPARAM>(labeled.c_str()));
                    }
                } else if (message->type ==
                           nstu::client::AgentMessageType::remote_start) {
                    // Arm remote control unconditionally. BlockInput(TRUE)
                    // fails for a non-elevated / RDP-session agent, and gating
                    // arming on it silently dropped every remote_input packet
                    // (see apply below). Suppressing local input is a
                    // best-effort nicety, not a precondition for control.
                    g_remote_control_active = true;
                    (void)BlockInput(TRUE);
                } else if (message->type ==
                           nstu::client::AgentMessageType::remote_input) {
                    const auto packet = nstu::client::decode_remote_input(
                        message->payload);
                    if (packet && g_remote_control_active.load()) {
                        (void)apply_remote_input(*packet);
                    }
                } else if (message->type ==
                           nstu::client::AgentMessageType::remote_end) {
                    stop_remote_control();
                } else if (message->type ==
                           nstu::client::AgentMessageType::start_snapshots) {
                    const auto interval = nstu::control::decode_snapshot_schedule(
                        message->payload);
                    if (interval) {
                        g_snapshot_interval_seconds = *interval;
                        g_snapshotting = true;
                        next_snapshot = std::chrono::steady_clock::now();
                    }
                } else if (message->type ==
                           nstu::client::AgentMessageType::stop_snapshots) {
                    g_snapshotting = false;
                    g_snapshot_interval_seconds = 0;
                } else if (message->type ==
                           nstu::client::AgentMessageType::overlay_stroke) {
                    const auto stroke = nstu::control::decode_overlay_stroke(
                        message->payload);
                    if (stroke) {
                        {
                            std::scoped_lock lock(g_annotation_mutex);
                            constexpr std::size_t maximum_strokes = 4096;
                            if (g_annotation_strokes.size() >= maximum_strokes) {
                                g_annotation_strokes.erase(
                                    g_annotation_strokes.begin(),
                                    g_annotation_strokes.begin() + 512);
                            }
                            g_annotation_strokes.push_back(*stroke);
                        }
                        PostMessageW(g_annotation_window,
                                     kAnnotationUpdatedMessage, 1, 0);
                    }
                } else if (message->type ==
                           nstu::client::AgentMessageType::overlay_clear) {
                    {
                        std::scoped_lock lock(g_annotation_mutex);
                        g_annotation_strokes.clear();
                    }
                    PostMessageW(g_annotation_window,
                                 kAnnotationUpdatedMessage, 0, 0);
                } else if (message->type ==
                           nstu::client::AgentMessageType::overlay_erase) {
                    const auto path = nstu::control::decode_overlay_stroke(
                        message->payload);
                    if (path) {
                        bool changed = false;
                        {
                            std::scoped_lock lock(g_annotation_mutex);
                            // Strokes and the erase path share the 0..65535
                            // normalized space. Remove any stroke with an
                            // endpoint inside the erase brush around either end
                            // of this path segment; freehand strokes are short,
                            // so endpoint proximity tracks the cursor well.
                            const double radius =
                                std::max(1200.0,
                                         static_cast<double>(path->thickness) *
                                             400.0);
                            const double r2 = radius * radius;
                            const auto near_end = [&](std::uint16_t sx,
                                                      std::uint16_t sy) {
                                const double d0x =
                                    static_cast<double>(sx) - path->x0;
                                const double d0y =
                                    static_cast<double>(sy) - path->y0;
                                const double d1x =
                                    static_cast<double>(sx) - path->x1;
                                const double d1y =
                                    static_cast<double>(sy) - path->y1;
                                return (d0x * d0x + d0y * d0y) <= r2 ||
                                       (d1x * d1x + d1y * d1y) <= r2;
                            };
                            const auto before = g_annotation_strokes.size();
                            std::erase_if(
                                g_annotation_strokes,
                                [&](const nstu::control::OverlayStroke& s) {
                                    return near_end(s.x0, s.y0) ||
                                           near_end(s.x1, s.y1);
                                });
                            changed = g_annotation_strokes.size() != before;
                        }
                        if (changed) {
                            PostMessageW(g_annotation_window,
                                         kAnnotationUpdatedMessage, 0, 0);
                        }
                    }
                } else if (message->type ==
                           nstu::client::AgentMessageType::host_snapshot) {
                    const auto frame = nstu::control::decode_snapshot_frame(
                        message->payload);
                    if (frame) {
                        nstu::screen::BgraImage decoded;
                        const auto bytes = std::span<const std::byte>(
                            frame->jpeg.data(), frame->jpeg.size());
                        if (nstu::screen::decode_jpeg(
                                bytes, frame->width, frame->height, decoded,
                                nullptr)) {
                            {
                                std::scoped_lock lock(g_broadcast_mutex);
                                g_broadcast_image = std::move(decoded);
                            }
                            g_viewing_broadcast = true;
                            PostMessageW(g_broadcast_window,
                                         kBroadcastUpdatedMessage, 1, 0);
                        }
                    }
                } else if (message->type ==
                           nstu::client::AgentMessageType::host_broadcast_stop) {
                    g_viewing_broadcast = false;
                    PostMessageW(g_broadcast_window,
                                 kBroadcastUpdatedMessage, 0, 0);
                } else if (message->type ==
                               nstu::client::AgentMessageType::exam_answer_ack ||
                           message->type ==
                               nstu::client::AgentMessageType::exam_state_response ||
                           message->type ==
                               nstu::client::AgentMessageType::exam_begin) {
                    // The service has already decoded and authenticated these
                    // payloads. The bounded bridge lets an optional exam host
                    // consume them on the UI thread without blocking this
                    // transport loop. exam_begin is the synchronized-start
                    // release: the host reveals the questions and anchors the
                    // countdown to its authoritative start time.
                    if (nstu::client::exam_bridge().publish(std::move(*message))) {
                        PostMessageW(overlay, kExamBridgeMessage, 0, 0);
                    }
                } else if (message->type ==
                               nstu::client::AgentMessageType::exam_start ||
                           message->type ==
                               nstu::client::AgentMessageType::exam_stop) {
                    // ExamHost/WebView2 is UI/STA-owned. Move the command to
                    // the message loop instead of touching it from the pipe
                    // worker.
                    queue_exam_command_for_ui(std::move(*message));
                    PostMessageW(overlay, kAgentCommandMessage,
                                 static_cast<WPARAM>(
                                     nstu::client::AgentMessageType::exam_start),
                                 0);
                } else if (message->type ==
                           nstu::client::AgentMessageType::managed_state) {
                    if (const auto managed =
                            nstu::control::decode_freeze_state(
                                message->payload)) {
                        SendMessageW(
                            overlay, kAgentCommandMessage,
                            static_cast<WPARAM>(message->type),
                            static_cast<LPARAM>(*managed));
                    }
                } else if (message->type ==
                           nstu::client::AgentMessageType::server_online) {
                    // Teacher-connection state. One payload byte, strictly 0
                    // or 1 per the protocol contract: anything else is a
                    // malformed frame and must not be read as "connected".
                    if (message->payload.size() == 1 &&
                        (message->payload[0] == std::byte{0} ||
                         message->payload[0] == std::byte{1})) {
                        const bool connected =
                            message->payload[0] == std::byte{1};
                        g_server_connected = connected;
                        SendMessageW(overlay, kAgentCommandMessage,
                                     static_cast<WPARAM>(message->type),
                                     static_cast<LPARAM>(connected ? 1 : 0));
                    }
                } else if (message->type ==
                           nstu::client::AgentMessageType::pairing_choices) {
                    if (auto choices =
                            nstu::client::decode_agent_pairing_choices(
                                message->payload)) {
                        show_pairing_choices(std::move(*choices));
                    }
                } else if (message->type ==
                           nstu::client::AgentMessageType::pairing_code) {
                    if (const auto code =
                            nstu::client::decode_agent_pairing_code(
                                message->payload)) {
                        show_pairing_code(*code);
                    }
                } else if (message->type ==
                           nstu::client::AgentMessageType::pairing_status) {
                    if (const auto status =
                            nstu::client::decode_agent_pairing_status(
                                message->payload)) {
                        show_pairing_status(*status);
                    }
                }
                if (!send_agent_status(pipe)) {
                    pipe_failed = true;
                    break;
                }
            }
            const auto now = std::chrono::steady_clock::now();
            // Focus live streaming takes priority over the slow class-view
            // snapshot: while streaming, capture at the requested fps and a
            // higher resolution; otherwise fall back to the periodic snapshot.
            const bool streaming = g_streaming.load();
            const bool want_stream = streaming && now >= next_stream_frame;
            const bool want_snapshot = !streaming &&
                                       g_snapshotting.load() &&
                                       now >= next_snapshot;
            if (want_stream || want_snapshot) {
                const std::uint16_t capture_width =
                    static_cast<std::uint16_t>(want_stream ? 1280 : 480);
                const std::uint16_t capture_height =
                    static_cast<std::uint16_t>(want_stream ? 720 : 270);
                const std::uint8_t capture_quality =
                    static_cast<std::uint8_t>(want_stream ? 60 : 52);
                nstu::screen::JpegImage jpeg;
                const auto capture_begin = std::chrono::steady_clock::now();
                if (nstu::screen::capture_primary_screen_jpeg(
                        jpeg, capture_width, capture_height, capture_quality,
                        nstu::control::kMaximumSnapshotJpegBytes, nullptr)) {
                    const std::size_t jpeg_bytes = jpeg.bytes.size();
                    nstu::control::SnapshotFrame frame;
                    frame.width = jpeg.width;
                    frame.height = jpeg.height;
                    frame.captured_at_unix_milliseconds =
                        unix_milliseconds_now();
                    frame.jpeg = std::move(jpeg.bytes);
                    const auto payload =
                        nstu::control::encode_snapshot_frame(frame);
                    if (!payload.empty()) {
                        if (!nstu::client::send_agent_message(
                            pipe,
                            {nstu::client::AgentMessageType::snapshot_frame,
                             payload},
                            nullptr)) {
                            pipe_failed = true;
                            break;
                        }
                    }
                    if (want_stream) {
                        stream_capture_ms_total +=
                            std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() -
                                capture_begin)
                                .count();
                        stream_bytes_total += jpeg_bytes;
                        ++stream_frame_count;
                    }
                }
                if (want_stream) {
                    const std::uint8_t fps = std::clamp<std::uint8_t>(
                        g_stream_fps.load(), 5, 15);
                    next_stream_frame =
                        now + std::chrono::milliseconds(1000 / fps);
                    const auto window_elapsed =
                        std::chrono::steady_clock::now() - stream_stat_window;
                    if (window_elapsed >= std::chrono::seconds(1) &&
                        stream_frame_count > 0) {
                        const double secs =
                            std::chrono::duration<double>(window_elapsed)
                                .count();
                        const auto fmt1 = [](double v) {
                            const long long t =
                                static_cast<long long>(v * 10.0 + 0.5);
                            return std::to_string(t / 10) + "." +
                                   std::to_string(t % 10);
                        };
                        const std::string line =
                            "NSTU stream: " +
                            fmt1(stream_frame_count / secs) + " fps (target " +
                            std::to_string(fps) + "), capture+encode avg " +
                            fmt1(stream_capture_ms_total / stream_frame_count) +
                            " ms, avg " +
                            std::to_string((stream_bytes_total /
                                            stream_frame_count) /
                                           1024) +
                            " KiB/frame\n";
                        OutputDebugStringA(line.c_str());
                        stream_stat_window = std::chrono::steady_clock::now();
                        stream_frame_count = 0;
                        stream_capture_ms_total = 0.0;
                        stream_bytes_total = 0;
                    }
                } else {
                    next_snapshot = now + std::chrono::seconds(
                        g_snapshot_interval_seconds.load());
                }
            }
        }
        stop_remote_control();
        hide_pairing_view();
        // Losing the service pipe means chat can no longer reach the teacher
        // (chat_submit rides this pipe), so drop the connected flag and take
        // the chat window down until a fresh session re-seeds server_online.
        g_server_connected = false;
        PostMessageW(overlay, kAgentCommandMessage,
                     static_cast<WPARAM>(
                         nstu::client::AgentMessageType::server_online),
                     0);
        pipe.close();
        if (pipe_failed && !g_agent_stopping.load()) {
            // Fail closed for exams. The host owns the WebView2 controller and
            // must be stopped by the UI thread, never from this pipe thread.
            queue_exam_command_for_ui(
                {nstu::client::AgentMessageType::exam_stop, {}});
            PostMessageW(overlay, kAgentCommandMessage,
                         static_cast<WPARAM>(
                             nstu::client::AgentMessageType::exam_start),
                         0);
            PostMessageW(overlay, kExamServiceDisconnectedMessage, 0, 0);
        }
    }
}

void append_chat_line(const wchar_t* message) {
    if (g_chat_messages == nullptr) {
        return;
    }
    SendMessageW(g_chat_messages, LB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(message));
    // The agent runs for the whole session, so the transcript is bounded the
    // same way the server's is; an unbounded LISTBOX grows USER/GDI memory for
    // as long as the machine stays on.
    constexpr LRESULT maximum_lines = 200;
    LRESULT count = SendMessageW(g_chat_messages, LB_GETCOUNT, 0, 0);
    while (count > maximum_lines) {
        SendMessageW(g_chat_messages, LB_DELETESTRING, 0, 0);
        --count;
    }
    if (count > 0) {
        // Scroll the newest line into view *without* selecting it. LB_SETCURSEL
        // would also paint the full-width system highlight across that row,
        // which reads as a selected item in what is really a transcript.
        SendMessageW(g_chat_messages, LB_SETTOPINDEX, count - 1, 0);
    }
}

void submit_chat_message() {
    if (g_chat_input == nullptr) {
        return;
    }
    wchar_t message_text[512]{};
    GetWindowTextW(g_chat_input, message_text,
                   static_cast<int>(std::size(message_text)));
    if (message_text[0] == L'\0') {
        return;
    }
    // Nothing can be sent without a live teacher session; saying "You: ..."
    // then would claim a delivery that never happens.
    if (!g_server_connected.load()) {
        append_chat_line(L"Not connected to the teacher. Message not sent.");
        return;
    }
    // Relay the raw line to the teacher through the service. Discovery/UI runs
    // on this thread; the pipe thread drains the bounded service queue, so this
    // never blocks on the socket.
    bool queued = false;
    const int utf8_length = WideCharToMultiByte(CP_UTF8, 0, message_text, -1,
                                                nullptr, 0, nullptr, nullptr);
    if (utf8_length > 1) {
        std::string utf8(static_cast<std::size_t>(utf8_length - 1), '\0');
        if (WideCharToMultiByte(CP_UTF8, 0, message_text, -1, utf8.data(),
                                utf8_length, nullptr, nullptr) > 0) {
            std::vector<std::byte> payload(
                reinterpret_cast<const std::byte*>(utf8.data()),
                reinterpret_cast<const std::byte*>(utf8.data()) + utf8.size());
            queue_service_message(
                {nstu::client::AgentMessageType::chat_submit,
                 std::move(payload)});
            queued = true;
        }
    }
    if (!queued) {
        append_chat_line(L"That message could not be sent.");
        return;
    }
    wchar_t line[540]{};
    swprintf_s(line, L"You: %s", message_text);
    append_chat_line(line);
    SetWindowTextW(g_chat_input, L"");
}

LRESULT CALLBACK chat_input_window_proc(HWND control, UINT message,
                                        WPARAM wparam, LPARAM lparam) {
    if (message == WM_KEYDOWN && wparam == VK_RETURN) {
        if (g_chat_window != nullptr) {
            SendMessageW(g_chat_window, WM_COMMAND,
                         MAKEWPARAM(kChatSend, BN_CLICKED),
                         reinterpret_cast<LPARAM>(control));
        }
        return 0;
    }
    if (g_chat_input_original_proc != nullptr) {
        return CallWindowProcW(g_chat_input_original_proc, control, message,
                               wparam, lparam);
    }
    return DefWindowProcW(control, message, wparam, lparam);
}

// Resolves an optional OS export without a function-pointer cast. Casting a
// FARPROC straight to the typed signature trips -Wcast-function-type on GCC
// and C4191 on MSVC (both warnings are errors in CI), so copy the bits the way
// client/src/exam_host.cpp already does.
template <typename Function>
Function load_proc(HMODULE module, const char* name) noexcept {
    const FARPROC raw = module == nullptr ? nullptr : GetProcAddress(module, name);
    Function function{};
    static_assert(sizeof(function) == sizeof(raw));
    std::memcpy(&function, &raw, sizeof(function));
    return function;
}

// Per-monitor DPI, resolved per window. The agent's form windows are authored
// in 96-DPI pixels; every control rect and font height is mapped through the
// owning window's DPI so Windows never has to bitmap-stretch (blur) them.
UINT window_dpi(HWND window) noexcept {
    using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
    static const auto get_dpi_for_window = load_proc<GetDpiForWindowFn>(
        GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    if (get_dpi_for_window != nullptr && window != nullptr) {
        const UINT dpi = get_dpi_for_window(window);
        if (dpi >= 72u) {
            return dpi;
        }
    }
    return 96u;
}

// Maps a length authored at 96 DPI onto `dpi`. Identity at 96 DPI, so the
// layout is byte-identical on a standard display.
int dpi_scaled(int length_at_96dpi, UINT dpi) noexcept {
    return MulDiv(length_at_96dpi, static_cast<int>(dpi), 96);
}

// DPI the OS last reported for each form window. WM_DPICHANGED carries the
// authoritative new value in wParam, and the relayout and repaint it triggers
// have to use that same number: a path that re-queries the window instead can
// read the pre-change DPI and leave the fonts and the control rects
// disagreeing. Zero means the OS has not told us yet, so ask it.
UINT g_chat_window_dpi = 0;
UINT g_pairing_window_dpi = 0;

UINT cached_or_window_dpi(UINT cached, HWND window) noexcept {
    return cached != 0u ? cached : window_dpi(window);
}

// Windows paints the caption bar light unless the window opts in, which left a
// white title bar sitting above the agent's dark client area. Attribute 20 is
// the documented value from Windows 10 20H1 on; 19 is the pre-20H1 spelling.
// Both are resolved at runtime so the agent still starts where neither exists.
void apply_dark_titlebar(HWND window) noexcept {
    using DwmSetWindowAttributeFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID,
                                                     DWORD);
    // Deliberately never freed: the agent keeps its windows for the whole
    // session, and the import is resolved once.
    static const HMODULE dwmapi = LoadLibraryW(L"dwmapi.dll");
    static const auto set_attribute =
        load_proc<DwmSetWindowAttributeFn>(dwmapi, "DwmSetWindowAttribute");
    if (set_attribute == nullptr || window == nullptr) {
        return;
    }
    constexpr DWORD kImmersiveDarkMode = 20;
    constexpr DWORD kImmersiveDarkModePre20H1 = 19;
    const BOOL enabled = TRUE;
    if (FAILED(set_attribute(window, kImmersiveDarkMode, &enabled,
                             sizeof(enabled)))) {
        (void)set_attribute(window, kImmersiveDarkModePre20H1, &enabled,
                            sizeof(enabled));
    }
}

// Opt the whole process into per-monitor DPI v2 before any window exists.
// Without this Windows reports virtualized coordinates and stretches every
// agent window, which is what made the chat look soft and dated on a scaled
// display. Falls back through the older APIs on down-level Windows.
void enable_process_dpi_awareness() noexcept {
    using SetProcessDpiAwarenessContextFn =
        BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    using SetProcessDpiAwarenessFn = HRESULT(WINAPI*)(int);
    if (const auto user32 = GetModuleHandleW(L"user32.dll")) {
        const auto set_context = load_proc<SetProcessDpiAwarenessContextFn>(
            user32, "SetProcessDpiAwarenessContext");
        if (set_context != nullptr &&
            set_context(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
            return;
        }
    }
    if (const auto shcore = LoadLibraryW(L"shcore.dll")) {
        const auto set_awareness =
            load_proc<SetProcessDpiAwarenessFn>(shcore, "SetProcessDpiAwareness");
        if (set_awareness != nullptr) {
            // 2 == PROCESS_PER_MONITOR_DPI_AWARE
            (void)set_awareness(2);
        }
        FreeLibrary(shcore);
        return;
    }
    (void)SetProcessDPIAware();
}

// Lazily builds the shared Segoe UI control font for a DPI. CLEARTYPE_QUALITY
// keeps it crisp; ~12pt at the window's DPI matches the pairing screen's text.
HFONT ui_font(UINT dpi) {
    if (g_ui_font == nullptr || g_ui_font_dpi != dpi) {
        const HFONT replacement = CreateFontW(
            -dpi_scaled(16, dpi), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        if (replacement == nullptr) {
            return g_ui_font;
        }
        if (g_ui_font != nullptr) {
            DeleteObject(g_ui_font);
        }
        g_ui_font = replacement;
        g_ui_font_dpi = dpi;
    }
    return g_ui_font;
}

// Applies the current font to every child control of a form window.
BOOL CALLBACK set_child_font(HWND child, LPARAM font) {
    SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(font), TRUE);
    return TRUE;
}

// Dark surface palette for the agent's form windows. These mirror the server
// dashboard's default dark theme so a student machine and the teacher console
// read as one product instead of a dark app next to a white 90s dialog.
constexpr COLORREF kSurfaceColor = RGB(25, 28, 33);
constexpr COLORREF kControlColor = RGB(33, 37, 43);
constexpr COLORREF kTextColor = RGB(224, 228, 234);
constexpr COLORREF kMutedTextColor = RGB(133, 141, 153);
constexpr COLORREF kButtonColor = RGB(42, 47, 55);
constexpr COLORREF kButtonHoverColor = RGB(54, 60, 69);
constexpr COLORREF kAccentColor = RGB(96, 163, 200);

HBRUSH surface_brush() {
    static const HBRUSH brush = CreateSolidBrush(kSurfaceColor);
    return brush;
}

HBRUSH control_brush() {
    static const HBRUSH brush = CreateSolidBrush(kControlColor);
    return brush;
}

// Paints a themed control background and text colour for the standard
// LISTBOX/EDIT/STATIC children, which otherwise render system-white.
LRESULT themed_control_color(WPARAM device_context, COLORREF background,
                             HBRUSH brush) {
    const auto hdc = reinterpret_cast<HDC>(device_context);
    SetTextColor(hdc, kTextColor);
    SetBkColor(hdc, background);
    return reinterpret_cast<LRESULT>(brush);
}

LRESULT CALLBACK chat_window_proc(HWND window, UINT message, WPARAM wparam,
                                  LPARAM lparam) {
    switch (message) {
    case WM_CREATE: {
        const UINT dpi = window_dpi(window);
        g_chat_window_dpi = dpi;
        apply_dark_titlebar(window);
        g_chat_messages = CreateWindowExW(
            0, L"LISTBOX", nullptr,
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP |
                LBS_NOINTEGRALHEIGHT,
            0, 0, 0, 0, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kChatMessages)),
            GetModuleHandleW(nullptr), nullptr);
        g_chat_input = CreateWindowExW(
            0, L"EDIT", nullptr,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            0, 0, 0, 0, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kChatInput)),
            GetModuleHandleW(nullptr), nullptr);
        if (g_chat_input != nullptr) {
            g_chat_input_original_proc = reinterpret_cast<WNDPROC>(
                SetWindowLongPtrW(g_chat_input, GWLP_WNDPROC,
                                  reinterpret_cast<LONG_PTR>(
                                      chat_input_window_proc)));
        }
        // Owner-drawn so the action button follows the dark palette; a themed
        // system button ignores WM_CTLCOLORBTN and would stay light grey.
        CreateWindowExW(
            0, L"BUTTON", L"Send",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0, 0, 0, 0, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kChatSend)),
            GetModuleHandleW(nullptr), nullptr);
        EnumChildWindows(window, set_child_font,
                         reinterpret_cast<LPARAM>(ui_font(dpi)));
        append_chat_line(L"NSTU client chat ready.");
        return 0;
    }
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLOREDIT:
        return themed_control_color(wparam, kControlColor, control_brush());
    case WM_CTLCOLORSTATIC:
        return themed_control_color(wparam, kSurfaceColor, surface_brush());
    case WM_DRAWITEM: {
        const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lparam);
        if (item == nullptr || item->CtlID != kChatSend) {
            break;
        }
        const bool pressed = (item->itemState & ODS_SELECTED) != 0;
        const bool focused = (item->itemState & ODS_FOCUS) != 0;
        const HBRUSH face = CreateSolidBrush(
            pressed ? kButtonHoverColor : kButtonColor);
        FillRect(item->hDC, &item->rcItem, face);
        DeleteObject(face);
        if (focused) {
            // A visible focus ring is what makes Tab navigation usable.
            const HBRUSH ring = CreateSolidBrush(kAccentColor);
            FrameRect(item->hDC, &item->rcItem, ring);
            DeleteObject(ring);
        }
        wchar_t caption[32]{};
        GetWindowTextW(item->hwndItem, caption,
                       static_cast<int>(std::size(caption)));
        SetBkMode(item->hDC, TRANSPARENT);
        SetTextColor(item->hDC, kTextColor);
        RECT text_area = item->rcItem;
        DrawTextW(item->hDC, caption, -1, &text_area,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return TRUE;
    }
    case WM_DPICHANGED: {
        g_chat_window_dpi = HIWORD(wparam);
        const auto* suggested = reinterpret_cast<const RECT*>(lparam);
        if (suggested != nullptr) {
            SetWindowPos(window, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        EnumChildWindows(
            window, set_child_font,
            reinterpret_cast<LPARAM>(ui_font(g_chat_window_dpi)));
        return 0;
    }
    case WM_SIZE: {
        const UINT dpi = cached_or_window_dpi(g_chat_window_dpi, window);
        const int pad = dpi_scaled(8, dpi);
        const int gap = dpi_scaled(8, dpi);
        const int row_height = dpi_scaled(28, dpi);
        const int send_width = dpi_scaled(76, dpi);
        const int width = LOWORD(lparam);
        const int height = HIWORD(lparam);
        // Lay the bottom row out from the right edge so the input and the
        // button always meet with one gap and neither overhangs the frame.
        const int row_y = std::max(pad, height - pad - row_height);
        const int send_x = std::max(pad, width - pad - send_width);
        if (g_chat_messages != nullptr) {
            MoveWindow(g_chat_messages, pad, pad,
                       std::max(pad, width - pad * 2),
                       std::max(pad, row_y - gap - pad), TRUE);
        }
        if (g_chat_input != nullptr) {
            MoveWindow(g_chat_input, pad, row_y,
                       std::max(pad, send_x - gap - pad), row_height, TRUE);
        }
        if (const HWND send_button = GetDlgItem(window, kChatSend)) {
            MoveWindow(send_button, send_x, row_y, send_width, row_height,
                       TRUE);
        }
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wparam) == kChatSend && HIWORD(wparam) == BN_CLICKED &&
            g_chat_input != nullptr) {
            submit_chat_message();
            return 0;
        }
        break;
    case WM_CLOSE:
        ShowWindow(window, SW_HIDE);
        return 0;
    case WM_DESTROY:
        if (g_chat_input != nullptr && g_chat_input_original_proc != nullptr) {
            SetWindowLongPtrW(
                g_chat_input, GWLP_WNDPROC,
                reinterpret_cast<LONG_PTR>(g_chat_input_original_proc));
            g_chat_input_original_proc = nullptr;
        }
        g_chat_window = nullptr;
        g_chat_messages = nullptr;
        g_chat_input = nullptr;
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

void submit_pairing_selection(HWND window) {
    if (g_pairing_list == nullptr) {
        return;
    }
    const auto index = SendMessageW(g_pairing_list, LB_GETCURSEL, 0, 0);
    if (index == LB_ERR || index < 0) {
        return;
    }
    auto payload = nstu::client::encode_agent_pairing_selection(
        static_cast<std::uint16_t>(index));
    if (payload.empty()) {
        return;
    }
    queue_service_message(
        {nstu::client::AgentMessageType::pairing_select,
         std::move(payload)});
    {
        std::scoped_lock lock(g_pairing_mutex);
        g_pairing_view = PairingView::waiting;
        g_pairing_caption = L"Contacting the server...";
        g_pairing_code.clear();
    }
    PostMessageW(window, kPairingUpdatedMessage, 0, 0);
}

void draw_pairing_text(HDC device, const RECT& area, int height,
                       int weight, const wchar_t* face,
                       COLORREF color, const std::wstring& content,
                       UINT format) {
    if (content.empty()) {
        return;
    }
    const HFONT font = CreateFontW(
        height, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH, face);
    const auto previous = SelectObject(device, font);
    SetTextColor(device, color);
    RECT bounds = area;
    DrawTextW(device, content.c_str(), -1, &bounds, format);
    SelectObject(device, previous);
    DeleteObject(font);
}

LRESULT CALLBACK pairing_window_proc(HWND window, UINT message,
                                     WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CREATE: {
        const UINT dpi = window_dpi(window);
        g_pairing_window_dpi = dpi;
        apply_dark_titlebar(window);
        g_pairing_list = CreateWindowExW(
            0, L"LISTBOX", nullptr,
            WS_CHILD | WS_VSCROLL | WS_TABSTOP | LBS_NOINTEGRALHEIGHT |
                LBS_NOTIFY,
            dpi_scaled(16, dpi), dpi_scaled(96, dpi), dpi_scaled(452, dpi),
            dpi_scaled(112, dpi), window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPairingList)),
            GetModuleHandleW(nullptr), nullptr);
        const HWND connect = CreateWindowExW(
            0, L"BUTTON", L"Connect",
            WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
            dpi_scaled(368, dpi), dpi_scaled(218, dpi), dpi_scaled(100, dpi),
            dpi_scaled(28, dpi), window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPairingConnect)),
            GetModuleHandleW(nullptr), nullptr);
        (void)connect;
        EnumChildWindows(window, set_child_font,
                         reinterpret_cast<LPARAM>(ui_font(dpi)));
        return 0;
    }
    case WM_CTLCOLORLISTBOX:
        return themed_control_color(wparam, kControlColor, control_brush());
    case WM_CTLCOLORSTATIC:
        return themed_control_color(wparam, kSurfaceColor, surface_brush());
    case WM_DRAWITEM: {
        const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lparam);
        if (item == nullptr || item->CtlID != kPairingConnect) {
            break;
        }
        const bool pressed = (item->itemState & ODS_SELECTED) != 0;
        const bool focused = (item->itemState & ODS_FOCUS) != 0;
        const HBRUSH face = CreateSolidBrush(
            pressed ? kButtonHoverColor : kButtonColor);
        FillRect(item->hDC, &item->rcItem, face);
        DeleteObject(face);
        if (focused) {
            const HBRUSH ring = CreateSolidBrush(kAccentColor);
            FrameRect(item->hDC, &item->rcItem, ring);
            DeleteObject(ring);
        }
        wchar_t caption[32]{};
        GetWindowTextW(item->hwndItem, caption,
                       static_cast<int>(std::size(caption)));
        SetBkMode(item->hDC, TRANSPARENT);
        SetTextColor(item->hDC, kTextColor);
        RECT text_area = item->rcItem;
        DrawTextW(item->hDC, caption, -1, &text_area,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return TRUE;
    }
    case WM_DPICHANGED: {
        const UINT dpi = HIWORD(wparam);
        g_pairing_window_dpi = dpi;
        const auto* suggested = reinterpret_cast<const RECT*>(lparam);
        if (suggested != nullptr) {
            SetWindowPos(window, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        if (g_pairing_list != nullptr) {
            MoveWindow(g_pairing_list, dpi_scaled(16, dpi),
                       dpi_scaled(96, dpi), dpi_scaled(452, dpi),
                       dpi_scaled(112, dpi), TRUE);
        }
        if (const HWND connect = GetDlgItem(window, kPairingConnect)) {
            MoveWindow(connect, dpi_scaled(368, dpi), dpi_scaled(218, dpi),
                       dpi_scaled(100, dpi), dpi_scaled(28, dpi), TRUE);
        }
        EnumChildWindows(window, set_child_font,
                         reinterpret_cast<LPARAM>(ui_font(dpi)));
        InvalidateRect(window, nullptr, TRUE);
        return 0;
    }
    case kPairingUpdatedMessage: {
        PairingView view = PairingView::hidden;
        std::vector<nstu::client::AgentPairingChoice> choices;
        {
            std::scoped_lock lock(g_pairing_mutex);
            view = g_pairing_view;
            choices = g_pairing_choices;
        }
        const bool choosing = view == PairingView::choices;
        if (g_pairing_list != nullptr) {
            if (choosing) {
                SendMessageW(g_pairing_list, LB_RESETCONTENT, 0, 0);
                for (const auto& choice : choices) {
                    // The address is shown next to the name because a name
                    // is whatever a beacon claimed it is, and two of them
                    // can be identical.
                    const auto label = widen_ascii(choice.server_name) +
                        L"  -  " + widen_ascii(choice.address);
                    SendMessageW(g_pairing_list, LB_ADDSTRING, 0,
                                 reinterpret_cast<LPARAM>(label.c_str()));
                }
                SendMessageW(g_pairing_list, LB_SETCURSEL, 0, 0);
            }
            ShowWindow(g_pairing_list, choosing ? SW_SHOW : SW_HIDE);
        }
        if (const HWND connect = GetDlgItem(window, kPairingConnect);
            connect != nullptr) {
            ShowWindow(connect, choosing ? SW_SHOW : SW_HIDE);
        }
        KillTimer(window, kPairingStatusTimer);
        if (view == PairingView::status) {
            // An outcome is a sentence, not a decision. Leaving it up would
            // park a dead window in front of somebody trying to work.
            SetTimer(window, kPairingStatusTimer, 8000, nullptr);
        }
        if (view == PairingView::hidden) {
            ShowWindow(window, SW_HIDE);
            return 0;
        }
        InvalidateRect(window, nullptr, TRUE);
        ShowWindow(window, SW_SHOW);
        SetForegroundWindow(window);
        return 0;
    }
    case WM_TIMER:
        if (wparam == kPairingStatusTimer) {
            KillTimer(window, kPairingStatusTimer);
            hide_pairing_view();
            return 0;
        }
        break;
    case WM_COMMAND:
        if ((LOWORD(wparam) == kPairingConnect &&
             HIWORD(wparam) == BN_CLICKED) ||
            (LOWORD(wparam) == kPairingList &&
             HIWORD(wparam) == LBN_DBLCLK)) {
            submit_pairing_selection(window);
            return 0;
        }
        break;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        const HDC device = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        FillRect(device, &client, surface_brush());
        const UINT dpi = cached_or_window_dpi(g_pairing_window_dpi, window);
        std::wstring caption;
        std::wstring code;
        {
            std::scoped_lock lock(g_pairing_mutex);
            caption = g_pairing_caption;
            code = g_pairing_code;
        }
        SetBkMode(device, TRANSPARENT);
        const RECT caption_area{dpi_scaled(16, dpi), dpi_scaled(16, dpi),
                                client.right - dpi_scaled(16, dpi),
                                dpi_scaled(92, dpi)};
        draw_pairing_text(device, caption_area, -dpi_scaled(16, dpi),
                          FW_NORMAL, L"Segoe UI", kTextColor, caption,
                          DT_WORDBREAK | DT_NOPREFIX);
        const RECT code_area{dpi_scaled(16, dpi), dpi_scaled(100, dpi),
                             client.right - dpi_scaled(16, dpi),
                             dpi_scaled(200, dpi)};
        draw_pairing_text(device, code_area, -dpi_scaled(56, dpi), FW_BOLD,
                          L"Consolas", kAccentColor, code,
                          DT_CENTER | DT_SINGLELINE | DT_VCENTER |
                              DT_NOPREFIX);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_CLOSE:
        // Closing the window declines nothing; the request is the service's
        // and the server still has to answer it.
        ShowWindow(window, SW_HIDE);
        return 0;
    case WM_DESTROY:
        KillTimer(window, kPairingStatusTimer);
        g_pairing_window = nullptr;
        g_pairing_list = nullptr;
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

COLORREF stroke_color(std::uint32_t rgba) {
    return RGB((rgba >> 24u) & 0xffu, (rgba >> 16u) & 0xffu,
               (rgba >> 8u) & 0xffu);
}

LRESULT CALLBACK annotation_window_proc(HWND window, UINT message,
                                        WPARAM wparam, LPARAM lparam) {
    (void)wparam;
    (void)lparam;
    if (message == kAnnotationUpdatedMessage) {
        const bool visible = wparam != 0;
        ShowWindow(window, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
        InvalidateRect(window, nullptr, TRUE);
        restore_control_window_order();
        return 0;
    }
    if (message == WM_ERASEBKGND) {
        return 1;
    }
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        HDC context = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        constexpr COLORREF transparent_color = RGB(1, 2, 3);
        HBRUSH background = CreateSolidBrush(transparent_color);
        FillRect(context, &client, background);
        DeleteObject(background);
        std::vector<nstu::control::OverlayStroke> strokes;
        {
            std::scoped_lock lock(g_annotation_mutex);
            strokes = g_annotation_strokes;
        }
        const int width = std::max(1L, client.right - client.left);
        const int height = std::max(1L, client.bottom - client.top);
        for (const auto& stroke : strokes) {
            HPEN pen = CreatePen(
                PS_SOLID, stroke.thickness, stroke_color(stroke.rgba));
            const HGDIOBJ previous = SelectObject(context, pen);
            const int x0 = static_cast<int>(
                static_cast<std::uint64_t>(stroke.x0) * width / 65535u);
            const int y0 = static_cast<int>(
                static_cast<std::uint64_t>(stroke.y0) * height / 65535u);
            const int x1 = static_cast<int>(
                static_cast<std::uint64_t>(stroke.x1) * width / 65535u);
            const int y1 = static_cast<int>(
                static_cast<std::uint64_t>(stroke.y1) * height / 65535u);
            MoveToEx(context, x0, y0, nullptr);
            LineTo(context, x1, y1);
            SelectObject(context, previous);
            DeleteObject(pen);
        }
        EndPaint(window, &paint);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

LRESULT CALLBACK broadcast_window_proc(HWND window, UINT message,
                                       WPARAM wparam, LPARAM lparam) {
    (void)lparam;
    if (message == kBroadcastUpdatedMessage) {
        ShowWindow(window, wparam != 0 ? SW_SHOWNOACTIVATE : SW_HIDE);
        InvalidateRect(window, nullptr, FALSE);
        restore_control_window_order();
        return 0;
    }
    if (message == WM_ERASEBKGND) {
        return 1;
    }
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        HDC context = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        FillRect(context, &client,
                 static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        std::scoped_lock lock(g_broadcast_mutex);
        if (!g_broadcast_image.pixels.empty()) {
            BITMAPINFO information{};
            information.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            information.bmiHeader.biWidth =
                static_cast<LONG>(g_broadcast_image.width);
            information.bmiHeader.biHeight =
                -static_cast<LONG>(g_broadcast_image.height);
            information.bmiHeader.biPlanes = 1;
            information.bmiHeader.biBitCount = 32;
            information.bmiHeader.biCompression = BI_RGB;
            SetStretchBltMode(context, HALFTONE);
            StretchDIBits(
                context, 0, 0, client.right - client.left,
                client.bottom - client.top, 0, 0, g_broadcast_image.width,
                g_broadcast_image.height, g_broadcast_image.pixels.data(),
                &information, DIB_RGB_COLORS, SRCCOPY);
        }
        EndPaint(window, &paint);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam,
                             LPARAM lparam) {
    switch (message) {
    case WM_CLOSE:
        ShowWindow(window, SW_HIDE);
        return 0;
    case WM_DESTROY:
        // ExamHost owns a UI/STA WebView2 controller. Stop it before the
        // message loop exits so no asynchronous browser callback can outlive
        // the agent window or pipe thread.
        g_exam_host.stop();
        restore_exam_suppressed_windows();
        stop_remote_control();
        PostQuitMessage(0);
        return 0;
    case kTrayMessage:
        if (lparam == WM_LBUTTONDBLCLK) {
            if (exam_host_engaged()) {
                g_exam_host.enforce_foreground();
                return 0;
            }
            if (!g_server_connected.load()) {
                // No live teacher session: chat has nowhere to go, so tell the
                // student rather than opening an empty window.
                show_pairing_status(
                    {.outcome = 0, .detail = "Something went wrong."});
                return 0;
            }
            if (g_chat_window != nullptr) {
                ShowWindow(g_chat_window,
                           IsWindowVisible(g_chat_window) ? SW_HIDE : SW_SHOW);
                SetForegroundWindow(g_chat_window);
            }
        }
        return 0;
    case kAgentCommandMessage: {
        const auto type = static_cast<nstu::client::AgentMessageType>(wparam);
        if (type == nstu::client::AgentMessageType::lock) {
            if (exam_host_engaged()) {
                g_exam_host.enforce_foreground();
                return 0;
            }
            g_locked = true;
            ShowWindow(window, SW_SHOW);
            restore_control_window_order();
        } else if (type == nstu::client::AgentMessageType::unlock) {
            if (exam_host_engaged()) {
                g_exam_host.enforce_foreground();
                return 0;
            }
            g_locked = false;
            ShowWindow(window, SW_HIDE);
            restore_control_window_order();
        } else if (type ==
                       nstu::client::AgentMessageType::managed_state) {
            g_managed = lparam != 0;
            NOTIFYICONDATAW tray{};
            tray.cbSize = sizeof(tray);
            tray.hWnd = window;
            tray.uID = kTrayId;
            tray.uFlags = NIF_TIP;
            lstrcpyW(tray.szTip, g_managed.load()
                                      ? L"NSTU client - Managed"
                                      : L"NSTU client");
            Shell_NotifyIconW(NIM_MODIFY, &tray);
        } else if (type == nstu::client::AgentMessageType::server_online) {
            g_server_connected = lparam != 0;
            if (lparam == 0 && g_chat_window != nullptr) {
                // The teacher session dropped. Take the chat window down so it
                // cannot sit open against a server that is no longer there, and
                // make sure the exam-restore path does not bring it back while
                // still disconnected.
                ShowWindow(g_chat_window, SW_HIDE);
                g_chat_was_visible_for_exam = false;
            }
        } else if (type == nstu::client::AgentMessageType::chat && lparam != 0) {
            if (exam_host_engaged()) {
                g_exam_host.enforce_foreground();
                return 0;
            }
            append_chat_line(reinterpret_cast<const wchar_t*>(lparam));
            if (g_chat_window != nullptr) {
                ShowWindow(g_chat_window, SW_SHOW);
            }
        } else if (type == nstu::client::AgentMessageType::start_stream &&
                   lparam >= 5 && lparam <= 15) {
            g_stream_fps = static_cast<std::uint8_t>(lparam);
            g_streaming = true;
        } else if (type == nstu::client::AgentMessageType::stop_stream) {
            g_streaming = false;
            g_stream_fps = 0;
        } else if (type == nstu::client::AgentMessageType::exam_start) {
            while (const auto command = pop_exam_command_for_ui()) {
                if (command->type ==
                    nstu::client::AgentMessageType::exam_stop) {
                    g_exam_host.stop();
                    restore_control_window_order();
                    continue;
                }
                if (command->type ==
                    nstu::client::AgentMessageType::exam_start) {
                    (void)start_exam_from_command_ui(window, *command);
                }
                // Preserve command ordering: the queue coalesces starts and
                // makes stop terminal, so one pass is enough in normal use.
            }
            restore_control_window_order();
        }
        return 0;
    }
    case kExamBridgeMessage:
        g_exam_host.drain_bridge();
        if (exam_host_engaged()) {
            g_exam_host.enforce_foreground();
        }
        return 0;
    case kExamServiceDisconnectedMessage:
        // The service is the authenticated control authority. If its pipe
        // disappears, stop the kiosk immediately; browser/service durable
        // queues retain answers for the next authenticated connection.
        clear_exam_commands_for_ui();
        if (g_exam_host.window() != nullptr) {
            g_exam_host.stop();
            restore_control_window_order();
        }
        return 0;
    case kExamHostStatusMessage:
        if (g_exam_host.state() == nstu::client::ExamHostState::failed) {
            g_exam_host.stop();
            restore_control_window_order();
        }
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC context = BeginPaint(window, &paint);
        FillRect(context, &paint.rcPaint,
                 static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        SetTextColor(context, RGB(255, 255, 255));
        SetBkMode(context, TRANSPARENT);
        const wchar_t text[] = L"This computer is locked by the instructor.";
        RECT area{};
        GetClientRect(window, &area);
        DrawTextW(context, text, -1, &area,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        EndPaint(window, &paint);
        return 0;
    }
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, wchar_t*, int) {
    // Must run before any window exists, otherwise Windows virtualizes the
    // agent's coordinates and bitmap-stretches every control it owns.
    enable_process_dpi_awareness();
    const UiComApartment com_apartment;
    if (!com_apartment.ready()) {
        const auto result = com_apartment.result();
        wchar_t detail[128]{};
        (void)FormatMessageW(
            FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr, static_cast<DWORD>(result), 0, detail,
            static_cast<DWORD>(std::size(detail)), nullptr);
        std::wstring message =
            L"NSTU client could not initialize the WebView2 UI apartment.";
        if (detail[0] != L'\0') {
            message += L"\r\n\r\n";
            message += detail;
        }
        MessageBoxW(nullptr, message.c_str(), L"NSTU client startup error",
                    MB_OK | MB_ICONERROR | MB_TASKMODAL);
        return 1;
    }
    HANDLE instance_mutex = CreateMutexW(nullptr, TRUE, kInstanceMutex);
    if (instance_mutex == nullptr || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (instance_mutex != nullptr) {
            CloseHandle(instance_mutex);
        }
        return 0;
    }
    WNDCLASSW window_class{};
    window_class.hInstance = instance;
    window_class.lpfnWndProc = window_proc;
    window_class.lpszClassName = kWindowClass;
    window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    RegisterClassW(&window_class);
    WNDCLASSW chat_class{};
    chat_class.hInstance = instance;
    chat_class.lpfnWndProc = chat_window_proc;
    chat_class.lpszClassName = kChatWindowClass;
    chat_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    chat_class.hbrBackground = surface_brush();
    RegisterClassW(&chat_class);
    WNDCLASSW annotation_class{};
    annotation_class.hInstance = instance;
    annotation_class.lpfnWndProc = annotation_window_proc;
    annotation_class.lpszClassName = kAnnotationWindowClass;
    annotation_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    RegisterClassW(&annotation_class);
    WNDCLASSW broadcast_class{};
    broadcast_class.hInstance = instance;
    broadcast_class.lpfnWndProc = broadcast_window_proc;
    broadcast_class.lpszClassName = kBroadcastWindowClass;
    broadcast_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    RegisterClassW(&broadcast_class);
    WNDCLASSW pairing_class{};
    pairing_class.hInstance = instance;
    pairing_class.lpfnWndProc = pairing_window_proc;
    pairing_class.lpszClassName = kPairingWindowClass;
    pairing_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    pairing_class.hbrBackground = surface_brush();
    RegisterClassW(&pairing_class);
    const int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    HWND window = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kWindowClass, L"NSTU Lock",
        WS_POPUP, x, y, width, height, nullptr, nullptr, instance, nullptr);
    if (window == nullptr) {
        CloseHandle(instance_mutex);
        return 1;
    }
    g_lock_window = window;
    // Size and centre the chat on the primary work area at that monitor's DPI
    // rather than letting the shell cascade a fixed 520x340 that is half-size
    // on a scaled display.
    const UINT primary_dpi = window_dpi(window);
    const int chat_width = dpi_scaled(520, primary_dpi);
    const int chat_height = dpi_scaled(340, primary_dpi);
    RECT work_area{};
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &work_area, 0)) {
        work_area = {0, 0, GetSystemMetrics(SM_CXSCREEN),
                     GetSystemMetrics(SM_CYSCREEN)};
    }
    const int chat_x = work_area.left +
        ((work_area.right - work_area.left) - chat_width) / 2;
    const int chat_y = work_area.top +
        ((work_area.bottom - work_area.top) - chat_height) / 2;
    g_chat_window = CreateWindowExW(
        0, kChatWindowClass, L"NSTU Client Chat", WS_OVERLAPPEDWINDOW,
        chat_x, chat_y, chat_width, chat_height, nullptr, nullptr, instance,
        nullptr);
    if (g_chat_window == nullptr) {
        DestroyWindow(window);
        CloseHandle(instance_mutex);
        return 1;
    }
    g_annotation_window = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED |
            WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
        kAnnotationWindowClass, L"NSTU Annotation", WS_POPUP, x, y, width,
        height, nullptr, nullptr, instance, nullptr);
    g_broadcast_window = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kBroadcastWindowClass, L"NSTU Teacher Broadcast", WS_POPUP, x, y,
        width, height, nullptr, nullptr, instance, nullptr);
    if (g_annotation_window == nullptr || g_broadcast_window == nullptr) {
        if (g_annotation_window != nullptr) {
            DestroyWindow(g_annotation_window);
        }
        if (g_broadcast_window != nullptr) {
            DestroyWindow(g_broadcast_window);
        }
        DestroyWindow(g_chat_window);
        DestroyWindow(window);
        CloseHandle(instance_mutex);
        return 1;
    }
    SetLayeredWindowAttributes(g_annotation_window, RGB(1, 2, 3), 0,
                               LWA_COLORKEY);
    // Fixed size and no maximize: this asks one question at a time and
    // there is nothing in it worth resizing. A machine whose desktop
    // refuses the window can still be managed once it is paired, so a
    // failure here is not fatal - every use of it is guarded.
    const int kPairingWidth = dpi_scaled(500, primary_dpi);
    const int kPairingHeight = dpi_scaled(300, primary_dpi);
    g_pairing_window = CreateWindowExW(
        WS_EX_TOPMOST, kPairingWindowClass, L"NSTU setup",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        work_area.left + ((work_area.right - work_area.left) -
                          kPairingWidth) / 2,
        work_area.top + ((work_area.bottom - work_area.top) -
                         kPairingHeight) / 2,
        kPairingWidth, kPairingHeight, nullptr, nullptr, instance,
        nullptr);
    NOTIFYICONDATAW tray{};
    tray.cbSize = sizeof(tray);
    tray.hWnd = window;
    tray.uID = kTrayId;
    tray.uFlags = NIF_MESSAGE | NIF_TIP | NIF_ICON;
    tray.uCallbackMessage = kTrayMessage;
    tray.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    lstrcpyW(tray.szTip, L"NSTU client");
    Shell_NotifyIconW(NIM_ADD, &tray);
    // The chat window stays hidden at startup. It only opens once the service
    // reports a live teacher session (server_online) and the student opens it,
    // or when the teacher sends a message; opening it with no teacher would
    // just present a dead window, so that path shows "Something went wrong."
    std::thread control_thread(pipe_control_loop, window);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        // Route through the visible form window so Tab/Shift+Tab cycle its
        // controls; without this the chat and pairing dialogs are mouse-only.
        const HWND active = GetActiveWindow();
        if ((active == g_chat_window || active == g_pairing_window) &&
            active != nullptr && IsDialogMessageW(active, &message)) {
            continue;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    Shell_NotifyIconW(NIM_DELETE, &tray);
    g_agent_stopping = true;
    // Ensure the WebView2 controller and low-level keyboard hook are torn
    // down on the UI thread before waiting for the pipe worker.
    g_exam_host.stop();
    restore_exam_suppressed_windows();
    if (control_thread.joinable()) {
        control_thread.join();
    }
    if (g_pairing_window != nullptr) {
        DestroyWindow(g_pairing_window);
    }
    DestroyWindow(g_broadcast_window);
    DestroyWindow(g_annotation_window);
    g_lock_window = nullptr;
    CloseHandle(instance_mutex);
    return static_cast<int>(message.wParam);
}
