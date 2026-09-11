#include "nstu/setup/diagnostics.hpp"

#include <windows.h>
#include <shellapi.h>

#include <atomic>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr wchar_t kWindowClass[] = L"NstuDiagnosticsWindow";
constexpr UINT kStartedMessage = WM_APP + 1;
constexpr UINT kResultMessage = WM_APP + 2;
constexpr UINT kFinishedMessage = WM_APP + 3;
constexpr UINT_PTR kCloseTimer = 2;
constexpr int kListId = 1001;
constexpr int kStatusId = 1002;
constexpr int kCloseId = 1003;

enum class Language { english, vietnamese };

struct Options {
    nstu::setup::DiagnosticOptions diagnostics;
    Language language = Language::english;
    bool auto_close = false;
    bool stay_open = false;
    std::filesystem::path report_path;
};

Options g_options;
HWND g_window = nullptr;
HWND g_list = nullptr;
HWND g_status = nullptr;
std::vector<nstu::setup::DiagnosticResult> g_results;
std::thread g_worker;
bool g_complete = false;
bool g_failed = false;
bool g_warning = false;

std::optional<std::wstring> argument_value(int argc, wchar_t** argv,
                                           std::wstring_view prefix) {
    for (int index = 1; index < argc; ++index) {
        const std::wstring_view argument(argv[index]);
        if (argument.starts_with(prefix)) {
            return std::wstring(argument.substr(prefix.size()));
        }
    }
    return std::nullopt;
}

bool has_argument(int argc, wchar_t** argv, std::wstring_view value) {
    const std::wstring expected(value);
    for (int index = 1; index < argc; ++index) {
        if (_wcsicmp(argv[index], expected.c_str()) == 0) {
            return true;
        }
    }
    return false;
}

Options parse_options() {
    Options options;
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv == nullptr) {
        return options;
    }
    if (const auto target = argument_value(argc, argv, L"--target="); target &&
        _wcsicmp(target->c_str(), L"server") == 0) {
        options.diagnostics.role = nstu::setup::DiagnosticRole::server;
    }
    if (const auto address = argument_value(argc, argv, L"--server-ip=")) {
        options.diagnostics.server_address = *address;
    }
    if (const auto port = argument_value(argc, argv, L"--server-port=")) {
        wchar_t* end = nullptr;
        const unsigned long parsed = wcstoul(port->c_str(), &end, 10);
        if (end != port->c_str() && *end == L'\0' && parsed > 0 &&
            parsed <= 65535) {
            options.diagnostics.server_port =
                static_cast<std::uint16_t>(parsed);
        }
    }
    if (const auto report = argument_value(argc, argv, L"--report=")) {
        options.report_path = *report;
    } else if (const auto log = argument_value(argc, argv, L"--log=")) {
        options.report_path = *log;
    }
    options.diagnostics.installer = has_argument(argc, argv, L"--installer");
    options.diagnostics.boot_check = has_argument(argc, argv, L"--boot-check");
    options.auto_close = has_argument(argc, argv, L"--auto-close");
    options.stay_open = has_argument(argc, argv, L"--diagnostics-stay-open");
    if (has_argument(argc, argv, L"--language=vi")) {
        options.language = Language::vietnamese;
    }
    LocalFree(argv);
    return options;
}

const wchar_t* severity_prefix(nstu::setup::DiagnosticSeverity severity) {
    switch (severity) {
    case nstu::setup::DiagnosticSeverity::pass: return L"[PASS]";
    case nstu::setup::DiagnosticSeverity::warning: return L"[WARN]";
    case nstu::setup::DiagnosticSeverity::failure: return L"[FAIL]";
    case nstu::setup::DiagnosticSeverity::not_applicable: return L"[N/A]";
    }
    return L"[?]";
}

std::wstring result_line(const nstu::setup::DiagnosticResult& item) {
    const bool vi = g_options.language == Language::vietnamese;
    std::wstring line = std::wstring(severity_prefix(item.severity)) + L" " +
                        (vi ? item.title_vi : item.title_en) + L" - " +
                        (vi ? item.detail_vi : item.detail_en);
    const auto& remediation = vi ? item.remediation_vi : item.remediation_en;
    if (!remediation.empty()) {
        line += L" | " + remediation;
    }
    return line;
}

void append_started(const nstu::setup::DiagnosticCheck& check) {
    const bool vi = g_options.language == Language::vietnamese;
    const std::wstring line = L"[....] " + (vi ? check.title_vi : check.title_en) +
                              (vi ? L" - Đang kiểm tra..." : L" - Checking...");
    SendMessageW(g_list, LB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(line.c_str()));
    const LRESULT count = SendMessageW(g_list, LB_GETCOUNT, 0, 0);
    if (count > 0) {
        SendMessageW(g_list, LB_SETTOPINDEX, count - 1, 0);
    }
}

void replace_started(const nstu::setup::DiagnosticResult& item) {
    const LRESULT count = SendMessageW(g_list, LB_GETCOUNT, 0, 0);
    if (count > 0) {
        SendMessageW(g_list, LB_DELETESTRING, count - 1, 0);
        const auto line = result_line(item);
        SendMessageW(g_list, LB_INSERTSTRING, count - 1,
                     reinterpret_cast<LPARAM>(line.c_str()));
        SendMessageW(g_list, LB_SETTOPINDEX, count - 1, 0);
    }
    g_results.push_back(item);
    g_failed = g_failed || item.severity == nstu::setup::DiagnosticSeverity::failure;
    g_warning = g_warning || item.severity == nstu::setup::DiagnosticSeverity::warning;
}

void finish() {
    g_complete = true;
    const bool vi = g_options.language == Language::vietnamese;
    const wchar_t* message = g_failed || g_warning
        ? (vi ? L"Đã phát hiện cảnh báo hoặc lỗi. Cửa sổ sẽ được giữ lại."
              : L"Warnings or errors were found. This window will remain open.")
        : (vi ? L"Tất cả kiểm tra bắt buộc đã đạt."
              : L"All required checks passed.");
    SetWindowTextW(g_status, message);
    EnableWindow(GetDlgItem(g_window, kCloseId), TRUE);
    // NSIS invokes diagnostics synchronously. In installer mode it must not
    // wait indefinitely for an operator to close a warning/failure window;
    // the report is persisted before this timed close and the exit code is
    // returned to the installer for its decision.
    const bool installer_close = g_options.diagnostics.installer &&
                                 g_options.auto_close;
    const bool clean_close = !g_failed && !g_warning && g_options.auto_close;
    if ((installer_close || clean_close) && !g_options.stay_open) {
        SetTimer(g_window, kCloseTimer, 1400, nullptr);
    }
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam,
                             LPARAM lparam) {
    switch (message) {
    case WM_CREATE: {
        HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        const bool vi = g_options.language == Language::vietnamese;
        CreateWindowExW(0, L"STATIC",
                        vi ? L"Chẩn đoán hệ thống NSTU"
                           : L"NSTU system diagnostics",
                        WS_CHILD | WS_VISIBLE, 16, 14, 900, 24, window,
                        nullptr, nullptr, nullptr);
        g_list = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"LISTBOX", nullptr,
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOINTEGRALHEIGHT,
            16, 46, 920, 420, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kListId)), nullptr,
            nullptr);
        g_status = CreateWindowExW(
            0, L"STATIC", vi ? L"Đang bắt đầu kiểm tra..." : L"Starting checks...",
            WS_CHILD | WS_VISIBLE, 16, 478, 760, 40, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kStatusId)), nullptr,
            nullptr);
        HWND close = CreateWindowExW(
            0, L"BUTTON", vi ? L"Đóng" : L"Close",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP, 846, 478, 90, 30, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCloseId)), nullptr,
            nullptr);
        for (HWND child : {g_list, g_status, close}) {
            SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        }
        EnableWindow(close, FALSE);
        g_worker = std::thread([] {
            nstu::setup::run_startup_diagnostics(
                g_options.diagnostics,
                [](const nstu::setup::DiagnosticCheck& check) {
                    auto* copy = new nstu::setup::DiagnosticCheck(check);
                    if (!PostMessageW(g_window, kStartedMessage, 0,
                                      reinterpret_cast<LPARAM>(copy))) {
                        delete copy;
                    }
                },
                [](nstu::setup::DiagnosticResult item) {
                    auto* copy = new nstu::setup::DiagnosticResult(std::move(item));
                    if (!PostMessageW(g_window, kResultMessage, 0,
                                      reinterpret_cast<LPARAM>(copy))) {
                        delete copy;
                    }
                });
            PostMessageW(g_window, kFinishedMessage, 0, 0);
        });
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* minimum = reinterpret_cast<MINMAXINFO*>(lparam);
        minimum->ptMinTrackSize.x = 760;
        minimum->ptMinTrackSize.y = 580;
        return 0;
    }
    case kStartedMessage: {
        auto* check = reinterpret_cast<nstu::setup::DiagnosticCheck*>(lparam);
        append_started(*check);
        delete check;
        return 0;
    }
    case kResultMessage: {
        auto* item = reinterpret_cast<nstu::setup::DiagnosticResult*>(lparam);
        replace_started(*item);
        delete item;
        const auto completed = g_results.size();
        const auto total = nstu::setup::diagnostic_checks(g_options.diagnostics).size();
        const std::wstring status =
            (g_options.language == Language::vietnamese ? L"Đã hoàn tất kiểm tra "
                                                         : L"Completed check ") +
            std::to_wstring(completed) + L"/" + std::to_wstring(total) + L"...";
        SetWindowTextW(g_status, status.c_str());
        return 0;
    }
    case kFinishedMessage:
        finish();
        return 0;
    case WM_TIMER:
        if (wparam == kCloseTimer) {
            KillTimer(window, kCloseTimer);
            DestroyWindow(window);
            return 0;
        }
        break;
    case WM_COMMAND:
        if (LOWORD(wparam) == kCloseId && HIWORD(wparam) == BN_CLICKED &&
            g_complete) {
            DestroyWindow(window);
            return 0;
        }
        break;
    case WM_CLOSE:
        if (g_complete) {
            DestroyWindow(window);
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(g_failed ? 1 : 0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, wchar_t*, int show) {
    g_options = parse_options();
    WNDCLASSW window_class{};
    window_class.hInstance = instance;
    window_class.lpfnWndProc = window_proc;
    window_class.lpszClassName = kWindowClass;
    window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    window_class.hbrBackground =
        static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
    if (!RegisterClassW(&window_class)) {
        return 1;
    }
    g_window = CreateWindowExW(
        WS_EX_APPWINDOW, kWindowClass, L"NSTU Diagnostics",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 970, 560, nullptr, nullptr, instance,
        nullptr);
    if (g_window == nullptr) {
        return 1;
    }
    ShowWindow(g_window, show == 0 ? SW_SHOWNORMAL : show);
    UpdateWindow(g_window);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (g_worker.joinable()) {
        g_worker.join();
    }
    if (!g_options.report_path.empty()) {
        std::string error;
        const bool written = nstu::setup::write_diagnostic_report_json(
            g_options.report_path, g_options.diagnostics, g_results, &error);
        if (!written) {
            OutputDebugStringA(("NSTU diagnostics report: " + error + "\n").c_str());
        }
    }
    return static_cast<int>(message.wParam);
}
