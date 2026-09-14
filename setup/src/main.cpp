#include "nstu/setup/diagnostics.hpp"
#include "nstu/setup/startup_options.hpp"

#include <windows.h>
#include <shellapi.h>

#include <array>
#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#ifndef RRF_SUBKEY_WOW6464KEY
// Older MinGW SDK headers omit this documented RegGetValue flag. Keep the
// explicit 64-bit registry-view lookup used by the installer contract.
#define RRF_SUBKEY_WOW6464KEY 0x00010000u
#endif

namespace {

#if NSTU_INTERNAL_TEST_BUILD
constexpr wchar_t kWindowTitle[] = L"NSTU Diagnostics - INTERNAL VM TEST";
constexpr wchar_t kHeadingEnglish[] =
    L"NSTU system diagnostics - INTERNAL VM TEST";
constexpr wchar_t kHeadingVietnamese[] =
    L"Chẩn đoán hệ thống NSTU - INTERNAL VM TEST";
#else
constexpr wchar_t kWindowTitle[] = L"NSTU Diagnostics";
constexpr wchar_t kHeadingEnglish[] = L"NSTU system diagnostics";
constexpr wchar_t kHeadingVietnamese[] = L"Chẩn đoán hệ thống NSTU";
#endif

constexpr wchar_t kWindowClass[] = L"NstuDiagnosticsWindow";
constexpr UINT kProgressMessage = WM_APP + 1;
constexpr UINT_PTR kProgressTimer = 1;
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
    bool invalid_configuration = false;
};

Options g_options;
HWND g_window = nullptr;
HWND g_list = nullptr;
HWND g_status = nullptr;
std::vector<nstu::setup::DiagnosticResult> g_results;
using DiagnosticEvent = std::variant<nstu::setup::DiagnosticCheck,
                                     nstu::setup::DiagnosticResult>;
std::mutex g_event_mutex;
std::vector<DiagnosticEvent> g_pending_events;
std::atomic<bool> g_worker_complete = false;
std::atomic<bool> g_worker_failed = false;
std::jthread g_worker;
std::size_t g_expected_checks = 0;
bool g_complete = false;
bool g_failed = false;
bool g_warning = false;
bool g_report_written = false;

struct LocalFreeDeleter {
    void operator()(wchar_t** value) const noexcept { LocalFree(value); }
};

nstu::setup::SavedDiagnosticEndpoint read_saved_endpoint(bool read_address,
                                                        bool read_port) {
    nstu::setup::SavedDiagnosticEndpoint saved;
    // NSIS uses SetRegView 64. Query the same machine-wide view explicitly,
    // including if a technician launches a 32-bit diagnostics helper.
    if (read_address) {
        std::array<wchar_t, 254> address{};
        DWORD bytes = static_cast<DWORD>(sizeof(address));
        const auto status = RegGetValueW(
            HKEY_LOCAL_MACHINE, L"Software\\NSTU", L"ServerAddress",
            RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY | RRF_ZEROONFAILURE,
            nullptr, address.data(), &bytes);
        if (status == ERROR_SUCCESS && bytes >= sizeof(wchar_t) &&
            bytes <= sizeof(address) && bytes % sizeof(wchar_t) == 0) {
            const auto characters = bytes / sizeof(wchar_t);
            if (address[characters - 1] == L'\0') {
                // Include every stored character except the terminator;
                // address validation rejects embedded NULs or trailing data.
                saved.address = std::wstring(address.data(), characters - 1);
            }
        }
    }
    if (read_port) {
        DWORD port = 0;
        DWORD bytes = sizeof(port);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, L"Software\\NSTU", L"ServerPort",
                         RRF_RT_REG_DWORD | RRF_SUBKEY_WOW6464KEY |
                             RRF_ZEROONFAILURE,
                         nullptr, &port, &bytes) == ERROR_SUCCESS &&
            bytes == sizeof(port)) {
            saved.port = port;
        }
    }
    return saved;
}

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
    const std::unique_ptr<wchar_t*, LocalFreeDeleter> arguments(
        CommandLineToArgvW(GetCommandLineW(), &argc));
    wchar_t** const argv = arguments.get();
    if (argv == nullptr) {
        options.invalid_configuration = true;
        return options;
    }
    if (const auto target = argument_value(argc, argv, L"--target="); target &&
        _wcsicmp(target->c_str(), L"server") == 0) {
        options.diagnostics.role = nstu::setup::DiagnosticRole::server;
    }
    const auto address = argument_value(argc, argv, L"--server-ip=");
    const auto port = argument_value(argc, argv, L"--server-port=");
    if (address) {
        options.diagnostics.server_address = *address;
        options.invalid_configuration =
            !nstu::setup::valid_diagnostic_server_address(*address);
    }
    if (port) {
        if (const auto parsed = nstu::setup::parse_diagnostic_server_port(*port)) {
            options.diagnostics.server_port = *parsed;
        } else {
            options.diagnostics.server_port = 0;
            options.invalid_configuration = true;
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
    if (!nstu::setup::load_diagnostic_boot_endpoint(
            options.diagnostics, address.has_value(), port.has_value(),
            read_saved_endpoint)) {
        options.invalid_configuration = true;
    }
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
    const auto line = result_line(item);
    if (count > 0) {
        SendMessageW(g_list, LB_DELETESTRING, count - 1, 0);
        SendMessageW(g_list, LB_INSERTSTRING, count - 1,
                     reinterpret_cast<LPARAM>(line.c_str()));
        SendMessageW(g_list, LB_SETTOPINDEX, count - 1, 0);
    } else {
        SendMessageW(g_list, LB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(line.c_str()));
    }
    g_results.push_back(item);
    g_failed = g_failed || item.severity == nstu::setup::DiagnosticSeverity::failure;
    g_warning = g_warning || item.severity == nstu::setup::DiagnosticSeverity::warning;
}

void append_failure(std::string id, std::wstring detail_en,
                    std::wstring detail_vi, std::uint32_t error) {
    g_failed = true;
    nstu::setup::DiagnosticResult failure;
    failure.id = std::move(id);
    failure.severity = nstu::setup::DiagnosticSeverity::failure;
    failure.title_en = L"Diagnostics";
    failure.title_vi = L"Chẩn đoán";
    failure.detail_en = std::move(detail_en);
    failure.detail_vi = std::move(detail_vi);
    failure.error_code = error;
    append_started({failure.id, failure.title_en, failure.title_vi});
    replace_started(failure);
}

void finish(HWND window) {
    if (g_complete) return;
    g_complete = true;
    KillTimer(window, kProgressTimer);
    if (g_worker_failed.load(std::memory_order_acquire) ||
        g_results.size() != g_expected_checks) {
        append_failure("diagnostics_runtime",
                       L"Diagnostics did not complete all required checks. Retry and review the report.",
                       L"Chưa hoàn tất tất cả kiểm tra bắt buộc. Hãy thử lại và xem báo cáo.",
                       ERROR_GEN_FAILURE);
    }
    if (g_options.invalid_configuration) {
        append_failure("diagnostics_configuration",
                       L"The server address or port is invalid or missing. Client boot checks read saved HKLM\\Software\\NSTU values unless explicitly overridden.",
                       L"Địa chỉ hoặc cổng server không hợp lệ hoặc bị thiếu. Kiểm tra khi client khởi động dùng cấu hình HKLM\\Software\\NSTU nếu không có tham số thay thế.",
                       ERROR_INVALID_DATA);
    }
    if (!g_options.report_path.empty()) {
        std::string error;
        try {
            g_report_written = nstu::setup::write_diagnostic_report_json(
                g_options.report_path, g_options.diagnostics, g_results, &error);
        } catch (...) {
            g_report_written = false;
        }
        if (!g_report_written) {
            OutputDebugStringA(("NSTU diagnostics report: " + error + "\n").c_str());
            append_failure("diagnostics_report",
                           L"The diagnostic report could not be saved. Check the report path, disk space and write permissions.",
                           L"Không thể lưu báo cáo chẩn đoán. Kiểm tra đường dẫn, dung lượng đĩa và quyền ghi.",
                           ERROR_WRITE_FAULT);
        }
    }
    const bool vi = g_options.language == Language::vietnamese;
    std::wstring message = g_failed || g_warning
        ? (vi ? L"Đã phát hiện cảnh báo hoặc lỗi."
              : L"Warnings or errors were found.")
        : (vi ? L"Tất cả kiểm tra bắt buộc đã đạt."
              : L"All required checks passed.");
    if (g_report_written) {
        message += vi ? L" Báo cáo: " : L" Report: ";
        message += g_options.report_path.wstring();
    } else if (!g_options.report_path.empty()) {
        message += vi ? L" Không thể lưu báo cáo." : L" Report could not be saved.";
    }
    SetWindowTextW(g_status, message.c_str());
    EnableWindow(GetDlgItem(window, kCloseId), TRUE);
    // Manual issue runs remain open for review. NSIS invokes this helper
    // synchronously, so installer issue runs use a bounded timer instead of
    // blocking the install indefinitely. Save the report before choosing the
    // summary and exit status so report failures cannot look successful.
    const auto close_delay = nstu::setup::diagnostic_close_delay_ms(
        g_options.diagnostics.installer, g_options.auto_close,
        g_options.stay_open, g_failed || g_warning);
    if (close_delay != 0) {
        if (SetTimer(window, kCloseTimer, static_cast<UINT>(close_delay),
                     nullptr) == 0) {
            // A timer allocation failure must not turn a synchronous
            // installer preflight into an unbounded wait. The report has
            // already been written (or explicitly reported as failed).
            DestroyWindow(window);
        }
    }
}

void queue_event(HWND window, DiagnosticEvent event) {
    {
        const std::lock_guard lock(g_event_mutex);
        g_pending_events.push_back(std::move(event));
    }
    // The timer is the fallback if the Windows message queue is full. Event
    // data always remains owned by the queue, including during window teardown.
    PostMessageW(window, kProgressMessage, 0, 0);
}

void process_progress(HWND window) {
    if (g_complete) return;
    // Acquire completion before draining: when true, every worker event has
    // already been queued. Reading it after the drain could miss a final event.
    const bool complete = g_worker_complete.load(std::memory_order_acquire);
    std::vector<DiagnosticEvent> events;
    {
        const std::lock_guard lock(g_event_mutex);
        events.swap(g_pending_events);
    }
    for (const auto& event : events) {
        if (const auto* check = std::get_if<nstu::setup::DiagnosticCheck>(&event)) {
            append_started(*check);
        } else {
            replace_started(std::get<nstu::setup::DiagnosticResult>(event));
        }
    }
    if (!events.empty()) {
        const std::wstring status =
            (g_options.language == Language::vietnamese ? L"Đã hoàn tất kiểm tra "
                                                         : L"Completed check ") +
            std::to_wstring(g_results.size()) + L"/" +
            std::to_wstring(g_expected_checks) + L"...";
        SetWindowTextW(g_status, status.c_str());
    }
    if (complete) finish(window);
}

LRESULT window_proc_impl(HWND window, UINT message, WPARAM wparam,
                         LPARAM lparam) {
    switch (message) {
    case WM_CREATE: {
        HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        const bool vi = g_options.language == Language::vietnamese;
        CreateWindowExW(0, L"STATIC",
                        vi ? kHeadingVietnamese : kHeadingEnglish,
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
        if (g_list == nullptr || g_status == nullptr || close == nullptr ||
            SetTimer(window, kProgressTimer, 100, nullptr) == 0) {
            g_failed = true;
            return -1;
        }
        for (HWND child : {g_list, g_status, close}) {
            SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        }
        EnableWindow(close, FALSE);
        g_expected_checks =
            nstu::setup::diagnostic_checks(g_options.diagnostics).size();
        g_results.reserve(g_expected_checks + 3);
        // WM_CREATE runs before CreateWindowExW assigns g_window. Capture the
        // actual callback HWND instead of racing that assignment in the worker.
        g_worker = std::jthread([window] {
            try {
                nstu::setup::run_startup_diagnostics(
                    g_options.diagnostics,
                    [window](const nstu::setup::DiagnosticCheck& check) {
                        queue_event(window, check);
                    },
                    [window](nstu::setup::DiagnosticResult item) {
                        queue_event(window, std::move(item));
                    });
            } catch (...) {
                g_worker_failed.store(true, std::memory_order_release);
            }
            g_worker_complete.store(true, std::memory_order_release);
            PostMessageW(window, kProgressMessage, 0, 0);
        });
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* minimum = reinterpret_cast<MINMAXINFO*>(lparam);
        minimum->ptMinTrackSize.x = 760;
        minimum->ptMinTrackSize.y = 580;
        return 0;
    }
    case kProgressMessage:
        process_progress(window);
        return 0;
    case WM_TIMER:
        if (wparam == kProgressTimer) {
            process_progress(window);
            return 0;
        }
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
        KillTimer(window, kProgressTimer);
        KillTimer(window, kCloseTimer);
        PostQuitMessage(g_failed || !g_complete ? 1 : 0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam,
                             LPARAM lparam) noexcept {
    try {
        return window_proc_impl(window, message, wparam, lparam);
    } catch (...) {
        // C++ exceptions must not escape a Win32 callback or leave NSIS waiting
        // forever. The worker owns its data and can finish safely after teardown.
        g_failed = true;
        OutputDebugStringW(L"NSTU diagnostics UI failed unexpectedly.\n");
        if (message == WM_CREATE) return -1;
        DestroyWindow(window);
        PostQuitMessage(1);
        return 0;
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, wchar_t*, int show) try {
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
        WS_EX_APPWINDOW, kWindowClass, kWindowTitle,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 970, 560, nullptr, nullptr, instance,
        nullptr);
    if (g_window == nullptr) {
        return 1;
    }
    ShowWindow(g_window, show == 0 ? SW_SHOWNORMAL : show);
    UpdateWindow(g_window);
    SetForegroundWindow(g_window);
    BringWindowToTop(g_window);
    MSG message{};
    BOOL retrieved = 0;
    while ((retrieved = GetMessageW(&message, nullptr, 0, 0)) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (g_worker.joinable()) {
        g_worker.join();
    }
    if (retrieved == -1 || g_failed || !g_complete) return 1;
    return static_cast<int>(message.wParam);
} catch (...) {
    OutputDebugStringW(L"NSTU diagnostics could not start or complete.\n");
    if (g_worker.joinable()) g_worker.join();
    return 1;
}
