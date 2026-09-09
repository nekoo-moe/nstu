#include "nstu/agent_protocol.hpp"
#include "nstu/control_messages.hpp"
#include "nstu/screen_snapshot.hpp"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iterator>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr wchar_t kWindowClass[] = L"NstuAgentOverlay";
constexpr wchar_t kChatWindowClass[] = L"NstuAgentChat";
constexpr wchar_t kAnnotationWindowClass[] = L"NstuAgentAnnotation";
constexpr wchar_t kBroadcastWindowClass[] = L"NstuAgentBroadcast";
constexpr wchar_t kInstanceMutex[] = L"Local\\NSTU.Agent.Singleton";
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kAgentCommandMessage = WM_APP + 2;
constexpr UINT kAnnotationUpdatedMessage = WM_APP + 3;
constexpr UINT kBroadcastUpdatedMessage = WM_APP + 4;
constexpr UINT kTrayId = 1;
constexpr int kChatMessages = 1001;
constexpr int kChatInput = 1002;
constexpr int kChatSend = 1003;

HWND g_chat_window = nullptr;
HWND g_chat_messages = nullptr;
HWND g_chat_input = nullptr;
WNDPROC g_chat_input_original_proc = nullptr;
HWND g_lock_window = nullptr;
HWND g_annotation_window = nullptr;
HWND g_broadcast_window = nullptr;
std::atomic_bool g_agent_stopping = false;
std::atomic_bool g_locked = false;
std::atomic_bool g_streaming = false;
std::atomic<std::uint8_t> g_stream_fps = 0;
std::atomic_bool g_snapshotting = false;
std::atomic<std::uint16_t> g_snapshot_interval_seconds = 0;
std::atomic_bool g_viewing_broadcast = false;
std::atomic_bool g_remote_control_active = false;
std::mutex g_annotation_mutex;
std::vector<nstu::control::OverlayStroke> g_annotation_strokes;
std::mutex g_broadcast_mutex;
nstu::screen::BgraImage g_broadcast_image;

void restore_control_window_order() {
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

void send_agent_status(nstu::client::NamedPipe& pipe) {
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
    (void)nstu::client::send_agent_message(
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
    input.mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
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
    while (!g_agent_stopping.load()) {
        nstu::client::NamedPipe pipe;
        if (!pipe.connect_client(nstu::client::kControlPipeName, 1000,
                                 nullptr)) {
            continue;
        }
        while (!g_agent_stopping.load()) {
            std::uint32_t available = 0;
            if (!pipe.available_bytes(available, nullptr)) {
                break;
            }
            if (available == 0) {
                Sleep(10);
            } else {
                const auto message =
                    nstu::client::receive_agent_message(pipe, nullptr);
                if (!message) {
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
                        SendMessageW(overlay, kAgentCommandMessage,
                                     static_cast<WPARAM>(message->type),
                                     reinterpret_cast<LPARAM>(chat.c_str()));
                    }
                } else if (message->type ==
                           nstu::client::AgentMessageType::remote_start) {
                    if (BlockInput(TRUE) != FALSE) {
                        g_remote_control_active = true;
                    }
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
                }
                send_agent_status(pipe);
            }
            const auto now = std::chrono::steady_clock::now();
            if (g_snapshotting.load() && now >= next_snapshot) {
                nstu::screen::JpegImage jpeg;
                if (nstu::screen::capture_primary_screen_jpeg(
                        jpeg, 480, 270, 52,
                        nstu::control::kMaximumSnapshotJpegBytes, nullptr)) {
                    nstu::control::SnapshotFrame frame;
                    frame.width = jpeg.width;
                    frame.height = jpeg.height;
                    frame.captured_at_unix_milliseconds =
                        unix_milliseconds_now();
                    frame.jpeg = std::move(jpeg.bytes);
                    const auto payload =
                        nstu::control::encode_snapshot_frame(frame);
                    if (!payload.empty()) {
                        (void)nstu::client::send_agent_message(
                            pipe,
                            {nstu::client::AgentMessageType::snapshot_frame,
                             payload},
                            nullptr);
                    }
                }
                next_snapshot = now + std::chrono::seconds(
                    g_snapshot_interval_seconds.load());
            }
        }
        stop_remote_control();
    }
}

void append_chat_line(const wchar_t* message) {
    if (g_chat_messages == nullptr) {
        return;
    }
    SendMessageW(g_chat_messages, LB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(message));
    const auto count = SendMessageW(g_chat_messages, LB_GETCOUNT, 0, 0);
    if (count > 0) {
        SendMessageW(g_chat_messages, LB_SETCURSEL, count - 1, 0);
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

LRESULT CALLBACK chat_window_proc(HWND window, UINT message, WPARAM wparam,
                                  LPARAM lparam) {
    switch (message) {
    case WM_CREATE:
        g_chat_messages = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"LISTBOX", nullptr,
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOINTEGRALHEIGHT,
            8, 8, 460, 220, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kChatMessages)),
            GetModuleHandleW(nullptr), nullptr);
        g_chat_input = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", nullptr,
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            8, 240, 360, 26, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kChatInput)),
            GetModuleHandleW(nullptr), nullptr);
        if (g_chat_input != nullptr) {
            g_chat_input_original_proc = reinterpret_cast<WNDPROC>(
                SetWindowLongPtrW(g_chat_input, GWLP_WNDPROC,
                                  reinterpret_cast<LONG_PTR>(
                                      chat_input_window_proc)));
        }
        CreateWindowExW(
            0, L"BUTTON", L"Send",
            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            380, 240, 80, 26, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kChatSend)),
            GetModuleHandleW(nullptr), nullptr);
        append_chat_line(L"NSTU client chat ready.");
        return 0;
    case WM_SIZE: {
        const int width = LOWORD(lparam);
        const int height = HIWORD(lparam);
        const int input_y = std::max(32, height - 38);
        if (g_chat_messages != nullptr) {
            MoveWindow(g_chat_messages, 8, 8, std::max(80, width - 16),
                       std::max(40, input_y - 16), TRUE);
        }
        if (g_chat_input != nullptr) {
            MoveWindow(g_chat_input, 8, input_y, std::max(40, width - 96), 26,
                       TRUE);
        }
        const auto send_button = GetDlgItem(window, kChatSend);
        if (send_button != nullptr) {
            MoveWindow(send_button, std::max(8, width - 80), input_y, 72, 26,
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
        stop_remote_control();
        PostQuitMessage(0);
        return 0;
    case kTrayMessage:
        if (lparam == WM_LBUTTONDBLCLK) {
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
            g_locked = true;
            ShowWindow(window, SW_SHOW);
            restore_control_window_order();
        } else if (type == nstu::client::AgentMessageType::unlock) {
            g_locked = false;
            ShowWindow(window, SW_HIDE);
            restore_control_window_order();
        } else if (type == nstu::client::AgentMessageType::chat && lparam != 0) {
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
        }
        return 0;
    }
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
    chat_class.hbrBackground = static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
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
    g_chat_window = CreateWindowExW(
        0, kChatWindowClass, L"NSTU Client Chat", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 520, 340, nullptr, nullptr, instance,
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
    NOTIFYICONDATAW tray{};
    tray.cbSize = sizeof(tray);
    tray.hWnd = window;
    tray.uID = kTrayId;
    tray.uFlags = NIF_MESSAGE | NIF_TIP | NIF_ICON;
    tray.uCallbackMessage = kTrayMessage;
    tray.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    lstrcpyW(tray.szTip, L"NSTU client");
    Shell_NotifyIconW(NIM_ADD, &tray);
    ShowWindow(g_chat_window, SW_SHOWDEFAULT);
    std::thread control_thread(pipe_control_loop, window);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    Shell_NotifyIconW(NIM_DELETE, &tray);
    g_agent_stopping = true;
    if (control_thread.joinable()) {
        control_thread.join();
    }
    DestroyWindow(g_broadcast_window);
    DestroyWindow(g_annotation_window);
    g_lock_window = nullptr;
    CloseHandle(instance_mutex);
    return static_cast<int>(message.wParam);
}
