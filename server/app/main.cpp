#include "nstu/client_registry.hpp"
#include "nstu/control_plane.hpp"
#include "nstu/deployment.hpp"
#include "nstu/discovery.hpp"
#include "nstu/key_store.hpp"
#include "nstu/screen_snapshot.hpp"
#include "nstu/secret_store.hpp"
#include "nstu/snapshot_generation_gate.hpp"
#include "nstu/stream_rearm_watchdog.hpp"
#include "nstu/telemetry.hpp"
#include "nstu/protocol_headers.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <windowsx.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <mftransform.h>
#include <shellapi.h>
#include <windows.h>
#include <wrl/client.h>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <deque>
#include <iomanip>
#include <sstream>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#ifndef NSTU_PROJECT_VERSION
#define NSTU_PROJECT_VERSION "development"
#endif

#ifndef NSTU_BUILD_CHANNEL
#define NSTU_BUILD_CHANNEL "Local"
#endif

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND window, UINT message, WPARAM wparam, LPARAM lparam);

namespace {

Microsoft::WRL::ComPtr<ID3D11Device> g_device;
Microsoft::WRL::ComPtr<ID3D11DeviceContext> g_context;
Microsoft::WRL::ComPtr<IDXGISwapChain> g_swap_chain;
Microsoft::WRL::ComPtr<ID3D11RenderTargetView> g_render_target;
ImFont* g_heading_font = nullptr;
bool g_dark_mode = false;
bool g_graphics_debug = false;
bool g_graphics_device_lost = false;

struct DiagnosticEvent {
    std::string timestamp;
    std::string severity;
    std::string source;
    std::string message;
};

struct GraphicsReport {
    std::vector<std::string> adapters;
    std::string selected_adapter;
    std::string selected_vendor;
    std::string feature_level;
    std::string device_mode;
    std::string desktop_duplication;
    std::string h264_encoders;
};

std::deque<DiagnosticEvent> g_diagnostics;
GraphicsReport g_graphics_report;
nstu::telemetry::ConsentPolicy g_telemetry_policy;
std::deque<nstu::telemetry::Event> g_telemetry_events;
std::atomic_bool g_telemetry_error_prompt_requested = false;
std::atomic_bool g_telemetry_error_prompt_armed = false;

constexpr wchar_t kServerSettingsKey[] = L"Software\\NSTU\\Server";
constexpr wchar_t kTelemetryEnabledValue[] = L"TelemetryEnabled";
constexpr wchar_t kTelemetryPromptValue[] = L"TelemetryPromptOnError";
constexpr wchar_t kDiagnosticIssueUrl[] =
    L"https://github.com/nekoo-moe/nstu/issues/new?template=sanitized-diagnostic.yml";

class RegistryKey final {
public:
    RegistryKey() = default;
    ~RegistryKey() {
        if (key_ != nullptr) {
            RegCloseKey(key_);
        }
    }
    RegistryKey(const RegistryKey&) = delete;
    RegistryKey& operator=(const RegistryKey&) = delete;

    HKEY* put() noexcept { return &key_; }
    HKEY get() const noexcept { return key_; }

private:
    HKEY key_ = nullptr;
};

bool read_server_setting(const wchar_t* name, bool fallback) noexcept {
    DWORD value = 0;
    DWORD bytes = sizeof(value);
    const auto status = RegGetValueW(
        HKEY_CURRENT_USER, kServerSettingsKey, name, RRF_RT_REG_DWORD,
        nullptr, &value, &bytes);
    return status == ERROR_SUCCESS ? value != 0 : fallback;
}

nstu::telemetry::ConsentPolicy load_telemetry_policy() noexcept {
    nstu::telemetry::ConsentPolicy policy;
    policy.collect_in_background =
        read_server_setting(kTelemetryEnabledValue, false);
    policy.prompt_on_error = policy.collect_in_background &&
        read_server_setting(kTelemetryPromptValue, false);
    return policy;
}

bool save_telemetry_policy(
    const nstu::telemetry::ConsentPolicy& policy) noexcept {
    const DWORD previous_prompt =
        read_server_setting(kTelemetryPromptValue, false) ? 1u : 0u;
    RegistryKey key;
    DWORD disposition = 0;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kServerSettingsKey, 0, nullptr,
                        REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr,
                        key.put(), &disposition) != ERROR_SUCCESS) {
        return false;
    }
    (void)disposition;
    const DWORD enabled = policy.collect_in_background ? 1u : 0u;
    const DWORD prompt = policy.collect_in_background && policy.prompt_on_error
        ? 1u : 0u;
    if (RegSetValueExW(key.get(), kTelemetryPromptValue, 0, REG_DWORD,
                       reinterpret_cast<const BYTE*>(&prompt),
                       sizeof(prompt)) != ERROR_SUCCESS) {
        return false;
    }
    if (RegSetValueExW(key.get(), kTelemetryEnabledValue, 0, REG_DWORD,
                       reinterpret_cast<const BYTE*>(&enabled),
                       sizeof(enabled)) == ERROR_SUCCESS) {
        return true;
    }
    (void)RegSetValueExW(key.get(), kTelemetryPromptValue, 0, REG_DWORD,
                         reinterpret_cast<const BYTE*>(&previous_prompt),
                         sizeof(previous_prompt));
    return false;
}

nstu::telemetry::Severity telemetry_severity(
    std::string_view severity) noexcept {
    if (severity == "error") {
        return nstu::telemetry::Severity::error;
    }
    if (severity == "warning") {
        return nstu::telemetry::Severity::warning;
    }
    return nstu::telemetry::Severity::information;
}

std::string hresult_text(HRESULT result) {
    std::ostringstream stream;
    stream << "0x" << std::uppercase << std::hex
           << static_cast<unsigned long>(result);
    return stream.str();
}

bool is_device_loss_hresult(HRESULT result) noexcept {
    return result == DXGI_ERROR_DEVICE_REMOVED ||
           result == DXGI_ERROR_DEVICE_RESET ||
           result == DXGI_ERROR_DEVICE_HUNG ||
           result == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
}

void record_diagnostic(const char* severity, const char* source,
                       const std::string& message) {
    SYSTEMTIME time{};
    GetLocalTime(&time);
    std::ostringstream timestamp;
    timestamp << std::setfill('0') << std::setw(2) << time.wHour << ':'
              << std::setw(2) << time.wMinute << ':' << std::setw(2)
              << time.wSecond;
    g_diagnostics.push_back({timestamp.str(), severity, source, message});
    while (g_diagnostics.size() > 128) {
        g_diagnostics.pop_front();
    }
    if (nstu::telemetry::collection_enabled(g_telemetry_policy)) {
        nstu::telemetry::append_bounded_event(
            g_telemetry_events,
            {telemetry_severity(severity), source, message});
        if (std::string_view(severity) == "error" &&
            nstu::telemetry::should_prompt_for_error(g_telemetry_policy) &&
            g_telemetry_error_prompt_armed.exchange(false)) {
            g_telemetry_error_prompt_requested.store(true);
        }
    }
}

void record_operation_failure(const char* source, std::string_view operation,
                              const std::string& error) {
    std::string message(operation);
    if (!error.empty()) {
        message += ": ";
        message += error;
    }
    record_diagnostic("warning", source, message);
}

std::wstring diagnostics_text() {
    std::wstring text = L"NSTU graphics initialization failed.\n\n";
    for (const auto& event : g_diagnostics) {
        const int length = MultiByteToWideChar(
            CP_UTF8, 0, event.message.c_str(), -1, nullptr, 0);
        std::wstring message;
        if (length > 1) {
            message.resize(static_cast<std::size_t>(length));
            MultiByteToWideChar(CP_UTF8, 0, event.message.c_str(), -1,
                                message.data(), length);
            message.resize(static_cast<std::size_t>(length - 1));
        }
        text += L"[" + std::wstring(event.severity.begin(),
                                     event.severity.end()) + L"] " +
                std::wstring(event.source.begin(), event.source.end()) +
                L": " + message + L"\n";
    }
    return text;
}

const char* vendor_name(UINT vendor_id) {
    switch (vendor_id) {
    case 0x10DE: return "NVIDIA";
    case 0x1002: return "AMD";
    case 0x8086: return "Intel";
    case 0x1414: return "Microsoft";
    default: return "Unknown";
    }
}

std::string wide_to_utf8(const wchar_t* text) {
    if (text == nullptr || *text == L'\0') return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0,
                                           nullptr, nullptr);
    if (length <= 1) return {};
    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), length,
                        nullptr, nullptr);
    result.resize(static_cast<std::size_t>(length - 1));
    return result;
}

void refresh_graphics_report() {
    g_graphics_report = {};
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    HRESULT result = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(result)) {
        record_diagnostic("error", "DXGI", "CreateDXGIFactory1 failed " +
            hresult_text(result));
        return;
    }
    for (UINT index = 0;; ++index) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        result = factory->EnumAdapters1(index, &adapter);
        if (result == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(result)) {
            record_diagnostic("warning", "DXGI", "EnumAdapters1 failed " +
                hresult_text(result));
            break;
        }
        DXGI_ADAPTER_DESC1 description{};
        if (FAILED(adapter->GetDesc1(&description))) continue;
        std::ostringstream line;
        line << wide_to_utf8(description.Description) << " | "
             << vendor_name(description.VendorId) << " | "
             << (description.DedicatedVideoMemory / (1024u * 1024u))
             << " MB VRAM";
        if ((description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
            line << " | software";
        }
        g_graphics_report.adapters.push_back(line.str());
    }
    if (!g_graphics_report.adapters.empty()) {
        record_diagnostic("info", "DXGI", "Enumerated " +
            std::to_string(g_graphics_report.adapters.size()) + " adapter(s)");
    }

    if (g_device) {
        D3D_FEATURE_LEVEL level = g_device->GetFeatureLevel();
        std::ostringstream feature;
        switch (level) {
        case D3D_FEATURE_LEVEL_11_1: feature << "11.1"; break;
        case D3D_FEATURE_LEVEL_11_0: feature << "11.0"; break;
        case D3D_FEATURE_LEVEL_10_1: feature << "10.1"; break;
        case D3D_FEATURE_LEVEL_10_0: feature << "10.0"; break;
        default: feature << "unknown"; break;
        }
        g_graphics_report.feature_level = feature.str();
        Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
        if (SUCCEEDED(g_device.As(&dxgi_device))) {
            Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
            if (SUCCEEDED(dxgi_device->GetAdapter(&adapter))) {
                DXGI_ADAPTER_DESC description{};
                if (SUCCEEDED(adapter->GetDesc(&description))) {
                    g_graphics_report.selected_adapter =
                        wide_to_utf8(description.Description);
                    g_graphics_report.selected_vendor =
                        vendor_name(description.VendorId);
                    g_graphics_report.device_mode =
                        g_graphics_report.device_mode.empty()
                            ? "Hardware" : g_graphics_report.device_mode;
                }
                Microsoft::WRL::ComPtr<IDXGIOutput> output;
                if (SUCCEEDED(adapter->EnumOutputs(0, &output))) {
                    Microsoft::WRL::ComPtr<IDXGIOutput1> output1;
                    if (SUCCEEDED(output.As(&output1))) {
                        Microsoft::WRL::ComPtr<IDXGIOutputDuplication> duplication;
                        const HRESULT duplicate_result =
                            output1->DuplicateOutput(g_device.Get(), &duplication);
                        g_graphics_report.desktop_duplication =
                            SUCCEEDED(duplicate_result) ? "Available"
                                                        : "Unavailable (" +
                            hresult_text(duplicate_result) + ")";
                    }
                }
            }
        }
    }
    HRESULT mf_result = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    UINT32 encoder_count = 0;
    if (SUCCEEDED(mf_result)) {
        IMFActivate** activates = nullptr;
        MFT_REGISTER_TYPE_INFO output_type{MFMediaType_Video,
                                           MFVideoFormat_H264};
        const HRESULT enum_result = MFTEnumEx(
            MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE,
            nullptr, &output_type, &activates, &encoder_count);
        if (FAILED(enum_result)) encoder_count = 0;
        if (activates != nullptr) {
            for (UINT32 i = 0; i < encoder_count; ++i) activates[i]->Release();
            CoTaskMemFree(activates);
        }
        MFShutdown();
    }
    g_graphics_report.h264_encoders = std::to_string(encoder_count);
}

constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kTrayToggleWindow = 2001;
constexpr UINT kTrayExit = 2002;
UINT g_taskbar_created_message = 0;
NOTIFYICONDATAW g_tray_icon{};
// The manager's main window, used for taskbar attention (FlashWindowEx) when a
// student chat arrives while the window is not in the foreground.
HWND g_main_window = nullptr;

constexpr int kMinimumSnapshotInterval = 5;
constexpr int kMaximumSnapshotInterval = 10;

enum class DashboardView : int {
    room_screens,
    selected_client,
};

enum class RoomFilter : int {
    all,
    online,
    attention,
    locked,
    offline,
};

enum class IconKind : int {
    grid,
    monitor,
    camera,
    stop,
    lock,
    unlock,
    pen,
    erase,
    broadcast,
    chat,
};

enum class Language : int {
    english,
    vietnamese,
};

enum class AnnotationTool : int {
    pen,
    ruler,
    arrow,
    rectangle,
    ellipse,
    eraser,
};

Language g_language = Language::english;

struct DashboardState {
    DashboardView view = DashboardView::room_screens;
    RoomFilter room_filter = RoomFilter::all;
    std::uint64_t selected_client_id = 0;
    int snapshot_interval_seconds = 7;
    // When set, every online computer is snapshotted continuously without the
    // operator clicking "Start snapshots" per machine; a periodic sweep re-arms
    // any client that reconnects. Turn it off to manage snapshots per computer.
    bool auto_monitor = true;
    std::chrono::steady_clock::time_point next_auto_monitor_sweep{};
    Language language = Language::english;
    bool dark_mode = false;
    bool show_offline = true;
    bool broadcast_enabled = false;
    // The client currently receiving a live Focus stream (~15fps). Distinct
    // from the slow class-view snapshots; zero when no client is focused.
    std::uint64_t streaming_client_id = 0;
    // Measured delivered-frame rate for the focused stream (from
    // ClientRecord::snapshot_generation deltas) — Epic B diagnostic so the
    // Focus label shows real fps instead of a fixed "~15fps".
    std::uint64_t fps_sample_client_id = 0;
    std::uint64_t fps_sample_generation = 0;
    std::chrono::steady_clock::time_point fps_sample_time{};
    double measured_stream_fps = 0.0;
    // Self-heals a stalled Focus/remote stream. Arming above is edge-triggered
    // on the streamed client id, so a start_stream lost across a client/agent
    // reconnect leaves the server believing a client streams while its agent
    // sends nothing. This re-asserts set_streaming when delivered frames stop.
    nstu::server::StreamRearmWatchdog stream_watchdog;
    bool annotation_enabled = false;
    bool uwf_confirmation_open = false;
    std::uint64_t uwf_confirmation_client_id = 0;
    bool annotation_dragging = false;
    ImVec2 previous_annotation_point{};
    AnnotationTool annotation_tool = AnnotationTool::pen;
    std::uint32_t annotation_rgba = 0xe5484dffu;
    int annotation_thickness = 4;
    std::chrono::steady_clock::time_point next_host_snapshot{};
    std::string control_status;
    std::string pairing_status;
    bool pairing_panel_open = false;
    std::string startup_error;
    std::string telemetry_report_preview;
    bool telemetry_report_requested = false;
    std::array<char, 96> client_filter{};
    std::array<char, 512> chat_input{};
    // Chat notification state. `chat_counts` is the current per-client inbound
    // (student) message count refreshed each frame; `chat_seen_counts` is the
    // count when the teacher last had that client's chat on screen (badge
    // baseline); `chat_observed_counts` is the count at the previous poll (edge
    // detection for the one-shot taskbar flash). `chat_counts_initialized`
    // seeds all three from the first poll so pre-existing history neither badges
    // nor flashes when the manager launches.
    std::unordered_map<std::uint64_t, std::size_t> chat_counts;
    std::unordered_map<std::uint64_t, std::size_t> chat_seen_counts;
    std::unordered_map<std::uint64_t, std::size_t> chat_observed_counts;
    bool chat_counts_initialized = false;
    // Operator-editable room label shown to computers choosing this server. The
    // buffer backs the "Add computers" text field (sized to the discovery name
    // bound plus a NUL); room_name_path is where the plaintext hint persists and
    // is empty when the data root is unavailable. A display hint, never a
    // credential.
    std::array<char, nstu::discovery::kMaximumServerNameBytes + 1>
        room_name_input{};
    std::filesystem::path room_name_path;
};

const char* tr(const DashboardState& state, const char* english,
               const char* vietnamese) {
    return state.language == Language::vietnamese ? vietnamese : english;
}

// A client has unread student chat when its current inbound count exceeds the
// count the teacher last had on screen for it.
bool client_has_unread_chat(const DashboardState& state, std::uint64_t id) {
    const auto current = state.chat_counts.find(id);
    if (current == state.chat_counts.end()) {
        return false;
    }
    const auto seen = state.chat_seen_counts.find(id);
    const std::size_t seen_count =
        seen == state.chat_seen_counts.end() ? 0 : seen->second;
    return current->second > seen_count;
}

bool update_telemetry_policy(
    DashboardState& state,
    const nstu::telemetry::ConsentPolicy& requested_policy) {
    auto policy = requested_policy;
    if (!policy.collect_in_background) {
        policy.prompt_on_error = false;
    }
    if (!save_telemetry_policy(policy)) {
        state.control_status = tr(
            state, "Could not save diagnostic-sharing settings.",
            "Không thể lưu cài đặt chia sẻ chẩn đoán.");
        return false;
    }

    const bool collection_started =
        !g_telemetry_policy.collect_in_background &&
        policy.collect_in_background;
    const bool prompt_started =
        !nstu::telemetry::should_prompt_for_error(g_telemetry_policy) &&
        nstu::telemetry::should_prompt_for_error(policy);
    g_telemetry_policy = policy;
    if (!policy.collect_in_background) {
        g_telemetry_events.clear();
        g_telemetry_error_prompt_requested.store(false);
        g_telemetry_error_prompt_armed.store(false);
        state.telemetry_report_preview.clear();
        state.control_status = tr(
            state, "Optional diagnostics disabled and collected data cleared.",
            "Đã tắt chẩn đoán tùy chọn và xóa dữ liệu đã thu thập.");
    } else if (collection_started) {
        g_telemetry_error_prompt_armed.store(policy.prompt_on_error);
        record_diagnostic("info", "Diagnostics",
                          "Optional local diagnostic collection enabled");
        state.control_status = tr(
            state, "Optional local diagnostics enabled.",
            "Đã bật chẩn đoán cục bộ tùy chọn.");
    } else {
        if (!policy.prompt_on_error) {
            g_telemetry_error_prompt_requested.store(false);
            g_telemetry_error_prompt_armed.store(false);
        } else if (prompt_started) {
            g_telemetry_error_prompt_armed.store(true);
        }
        state.control_status = tr(
            state, "Diagnostic-sharing settings saved.",
            "Đã lưu cài đặt chia sẻ chẩn đoán.");
    }
    return true;
}

std::string client_count_bucket(std::size_t count) {
    if (count == 0) return "0";
    if (count <= 10) return "1-10";
    if (count <= 25) return "11-25";
    if (count <= 50) return "26-50";
    return "51+";
}

std::string report_value(const std::string& value) {
    return value.empty() ? "Unknown" : value;
}

nstu::telemetry::PublicReport make_telemetry_report(
    const DashboardState& state, std::size_t client_count) {
    nstu::telemetry::PublicReport report;
    report.application = "NSTU Server";
    report.version = NSTU_PROJECT_VERSION;
    report.build_channel = NSTU_BUILD_CHANNEL;
    report.fields = {
        {"Graphics device mode", report_value(g_graphics_report.device_mode)},
        {"Graphics adapter", report_value(g_graphics_report.selected_adapter)},
        {"Graphics vendor", report_value(g_graphics_report.selected_vendor)},
        {"D3D feature level", report_value(g_graphics_report.feature_level)},
        {"Desktop Duplication",
         report_value(g_graphics_report.desktop_duplication)},
        {"Hardware H.264 encoders",
         report_value(g_graphics_report.h264_encoders)},
        {"Snapshot interval",
         std::to_string(state.snapshot_interval_seconds) + " seconds"},
        {"Known client count", client_count_bucket(client_count)},
    };
    report.events = g_telemetry_events;
    return report;
}

void show_main_window(HWND window) {
    ShowWindow(window, SW_RESTORE);
    SetForegroundWindow(window);
}

bool add_tray_icon(HWND window) {
    g_tray_icon = {};
    g_tray_icon.cbSize = sizeof(g_tray_icon);
    g_tray_icon.hWnd = window;
    g_tray_icon.uID = 1;
    g_tray_icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_tray_icon.uCallbackMessage = kTrayMessage;
    g_tray_icon.hIcon =
        LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    lstrcpyW(g_tray_icon.szTip, L"NSTU Server");
    if (!Shell_NotifyIconW(NIM_ADD, &g_tray_icon)) {
        return false;
    }
    g_tray_icon.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &g_tray_icon);
    return true;
}

void show_tray_menu(HWND window) {
    POINT point{};
    GetCursorPos(&point);
    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) {
        return;
    }
    const bool visible = IsWindowVisible(window) != FALSE;
    const bool vietnamese = g_language == Language::vietnamese;
    AppendMenuW(menu, MF_STRING, kTrayToggleWindow,
                visible ? (vietnamese ? L"Ẩn NSTU Server"
                                      : L"Hide NSTU Server")
                        : (vietnamese ? L"Mở NSTU Server"
                                      : L"Open NSTU Server"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kTrayExit,
                vietnamese ? L"Thoát" : L"Exit");
    SetForegroundWindow(window);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN,
                   point.x, point.y, 0, window, nullptr);
    DestroyMenu(menu);
    PostMessageW(window, WM_NULL, 0, 0);
}

struct SnapshotTexture {
    nstu::server::SnapshotGenerationGate generation_gate;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
};

std::unordered_map<std::uint64_t, SnapshotTexture> g_snapshot_textures;

void invalidate_snapshot_textures() noexcept {
    for (auto& [client_id, texture] : g_snapshot_textures) {
        (void)client_id;
        texture.generation_gate.reset();
        texture.width = 0;
        texture.height = 0;
        texture.view.Reset();
    }
}

struct RoomCounts {
    std::size_t online = 0;
    std::size_t attention = 0;
    std::size_t locked = 0;
    std::size_t offline = 0;
};

void create_render_target() {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> back_buffer;
    if (SUCCEEDED(g_swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer)))) {
        const HRESULT result = g_device->CreateRenderTargetView(
            back_buffer.Get(), nullptr, &g_render_target);
        if (FAILED(result)) {
            record_diagnostic("error", "D3D11",
                              "CreateRenderTargetView failed " +
                                  hresult_text(result));
        }
    } else {
        record_diagnostic("error", "DXGI", "Swap-chain back-buffer unavailable");
    }
}

bool create_device(HWND window) {
    g_graphics_device_lost = true;
    invalidate_snapshot_textures();
    DXGI_SWAP_CHAIN_DESC description{};
    description.BufferCount = 2;
    description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.OutputWindow = window;
    description.SampleDesc.Count = 1;
    description.Windowed = TRUE;
    description.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    constexpr D3D_FEATURE_LEVEL requested[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    UINT flags = g_graphics_debug
        ? static_cast<UINT>(D3D11_CREATE_DEVICE_DEBUG) : 0u;
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    HRESULT result = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(result)) {
        record_diagnostic("error", "DXGI", "CreateDXGIFactory1 failed " +
            hresult_text(result));
        return false;
    }
    bool created = false;
    for (UINT index = 0; !created; ++index) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        result = factory->EnumAdapters1(index, &adapter);
        if (result == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(result)) break;
        DXGI_ADAPTER_DESC1 adapter_desc{};
        if (FAILED(adapter->GetDesc1(&adapter_desc)) ||
            (adapter_desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
            continue;
        }
        result = D3D11CreateDeviceAndSwapChain(
            adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags,
            requested, ARRAYSIZE(requested), D3D11_SDK_VERSION, &description,
            &g_swap_chain, &g_device, nullptr, &g_context);
        if (FAILED(result) && flags != 0u) {
            record_diagnostic("warning", "D3D11",
                              "Debug layer unavailable; retrying without it (" +
                                  hresult_text(result) + ")");
            flags = 0u;
            result = D3D11CreateDeviceAndSwapChain(
                adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags,
                requested, ARRAYSIZE(requested), D3D11_SDK_VERSION,
                &description, &g_swap_chain, &g_device, nullptr, &g_context);
        }
        if (SUCCEEDED(result)) {
            created = true;
            g_graphics_report.device_mode = "Hardware";
            record_diagnostic("info", "D3D11", "Hardware device created on " +
                wide_to_utf8(adapter_desc.Description));
        } else {
            record_diagnostic("warning", "D3D11",
                              "Hardware adapter failed " +
                                  wide_to_utf8(adapter_desc.Description) +
                                  " (" + hresult_text(result) + ")");
            g_swap_chain.Reset();
            g_device.Reset();
            g_context.Reset();
        }
    }
    if (!created) {
        result = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, requested,
            ARRAYSIZE(requested), D3D11_SDK_VERSION, &description,
            &g_swap_chain, &g_device, nullptr, &g_context);
        if (SUCCEEDED(result)) {
            created = true;
            g_graphics_report.device_mode = "WARP fallback";
            record_diagnostic("warning", "D3D11",
                              "Hardware initialization failed; using WARP fallback");
        } else {
            record_diagnostic("error", "D3D11",
                              "WARP device creation failed " + hresult_text(result));
        }
    }
    if (!created) return false;
    create_render_target();
    if (!g_render_target) return false;
    g_graphics_device_lost = false;
    refresh_graphics_report();
    return true;
}

// ---- Separate remote-control window (Part C) ------------------------------
// An independent top-level window that shows the controlled client's live
// stream and forwards mouse/keyboard as RemoteInputPacket. It shares the main
// D3D11 device through a second swap chain plus a tiny texture-blit pipeline,
// so the Focus view stays display-only (plus annotation) with no embedded
// remote input. All access happens on the UI thread: the window is created,
// pumped, rendered, and torn down from the main loop's single thread.
struct RemoteImageRect {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
};

HWND g_remote_window = nullptr;
Microsoft::WRL::ComPtr<IDXGISwapChain> g_remote_swap_chain;
Microsoft::WRL::ComPtr<ID3D11RenderTargetView> g_remote_render_target;
Microsoft::WRL::ComPtr<ID3D11VertexShader> g_remote_vertex_shader;
Microsoft::WRL::ComPtr<ID3D11PixelShader> g_remote_pixel_shader;
Microsoft::WRL::ComPtr<ID3D11SamplerState> g_remote_sampler;
// A second, point-filtered sampler used only when the remote window is enlarged
// past the 720p stream. Bilinear upscaling blurs small text; nearest keeps it
// crisp. Downscaling still uses the linear sampler to avoid aliasing.
Microsoft::WRL::ComPtr<ID3D11SamplerState> g_remote_sampler_point;
std::uint64_t g_remote_target_client = 0;
nstu::server::ServerControlPlane* g_remote_control_plane = nullptr;
RemoteImageRect g_remote_image_rect;
std::atomic_bool g_remote_close_pending{false};
bool g_remote_class_registered = false;

bool ensure_remote_pipeline() {
    if (g_remote_vertex_shader && g_remote_pixel_shader && g_remote_sampler &&
        g_remote_sampler_point) {
        return true;
    }
    if (!g_device) {
        return false;
    }
    static const char kVertexShader[] =
        "struct VSOut{float4 pos:SV_POSITION;float2 uv:TEXCOORD0;};"
        "VSOut main(uint id:SV_VertexID){VSOut o;"
        "float2 uv=float2((id<<1)&2,id&2);o.uv=uv;"
        "o.pos=float4(uv*float2(2,-2)+float2(-1,1),0,1);return o;}";
    static const char kPixelShader[] =
        "Texture2D tex:register(t0);SamplerState smp:register(s0);"
        "float4 main(float4 pos:SV_POSITION,float2 uv:TEXCOORD0):SV_TARGET"
        "{return tex.Sample(smp,uv);}";
    Microsoft::WRL::ComPtr<ID3DBlob> vs_blob;
    Microsoft::WRL::ComPtr<ID3DBlob> ps_blob;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    HRESULT hr = D3DCompile(kVertexShader, sizeof(kVertexShader) - 1, "remote_vs",
                            nullptr, nullptr, "main", "vs_4_0", 0, 0, &vs_blob,
                            &errors);
    if (FAILED(hr)) {
        record_diagnostic("error", "D3D11",
                          "Remote vertex shader compile failed " + hresult_text(hr));
        return false;
    }
    hr = D3DCompile(kPixelShader, sizeof(kPixelShader) - 1, "remote_ps", nullptr,
                    nullptr, "main", "ps_4_0", 0, 0, &ps_blob, &errors);
    if (FAILED(hr)) {
        record_diagnostic("error", "D3D11",
                          "Remote pixel shader compile failed " + hresult_text(hr));
        return false;
    }
    if (FAILED(g_device->CreateVertexShader(vs_blob->GetBufferPointer(),
                                            vs_blob->GetBufferSize(), nullptr,
                                            &g_remote_vertex_shader)) ||
        FAILED(g_device->CreatePixelShader(ps_blob->GetBufferPointer(),
                                           ps_blob->GetBufferSize(), nullptr,
                                           &g_remote_pixel_shader))) {
        g_remote_vertex_shader.Reset();
        g_remote_pixel_shader.Reset();
        return false;
    }
    D3D11_SAMPLER_DESC sampler_desc{};
    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(g_device->CreateSamplerState(&sampler_desc, &g_remote_sampler))) {
        return false;
    }
    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    if (FAILED(g_device->CreateSamplerState(&sampler_desc,
                                            &g_remote_sampler_point))) {
        return false;
    }
    return true;
}

bool create_remote_render_target() {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> back_buffer;
    if (!g_remote_swap_chain ||
        FAILED(g_remote_swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer)))) {
        return false;
    }
    return SUCCEEDED(g_device->CreateRenderTargetView(
        back_buffer.Get(), nullptr, &g_remote_render_target));
}

void render_remote_window(ID3D11ShaderResourceView* srv, std::uint32_t tex_w,
                          std::uint32_t tex_h) {
    if (!g_remote_window || !g_remote_swap_chain || !g_remote_render_target ||
        !g_context || g_graphics_device_lost) {
        return;
    }
    RECT client{};
    GetClientRect(g_remote_window, &client);
    const float cw = static_cast<float>(client.right - client.left);
    const float ch = static_cast<float>(client.bottom - client.top);
    if (cw < 1.0f || ch < 1.0f) {
        return;
    }
    // Letterbox the client's stream aspect into the window client area so the
    // whole remote screen stays visible and input maps to the right pixels.
    float iw = cw;
    float ih = ch;
    float ix = 0.0f;
    float iy = 0.0f;
    if (tex_w > 0 && tex_h > 0) {
        const float ta = static_cast<float>(tex_w) / static_cast<float>(tex_h);
        const float ca = cw / ch;
        if (ca > ta) {
            ih = ch;
            iw = ch * ta;
            ix = (cw - iw) * 0.5f;
        } else {
            iw = cw;
            ih = cw / ta;
            iy = (ch - ih) * 0.5f;
        }
    }
    g_remote_image_rect = {ix, iy, iw, ih};
    const float clear_color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    g_context->OMSetRenderTargets(1, g_remote_render_target.GetAddressOf(),
                                  nullptr);
    g_context->ClearRenderTargetView(g_remote_render_target.Get(), clear_color);
    if (srv != nullptr && ensure_remote_pipeline()) {
        D3D11_VIEWPORT viewport{};
        viewport.TopLeftX = ix;
        viewport.TopLeftY = iy;
        viewport.Width = iw;
        viewport.Height = ih;
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;
        g_context->RSSetViewports(1, &viewport);
        g_context->IASetInputLayout(nullptr);
        g_context->IASetPrimitiveTopology(
            D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_context->VSSetShader(g_remote_vertex_shader.Get(), nullptr, 0);
        g_context->PSSetShader(g_remote_pixel_shader.Get(), nullptr, 0);
        ID3D11ShaderResourceView* views[1] = {srv};
        g_context->PSSetShaderResources(0, 1, views);
        // Enlarged past the source: nearest-filter keeps text crisp instead of
        // blurring it. At or below native, linear avoids downscale aliasing.
        const bool upscaling =
            tex_w > 0 && iw > static_cast<float>(tex_w);
        ID3D11SamplerState* sampler =
            (upscaling ? g_remote_sampler_point : g_remote_sampler).Get();
        g_context->PSSetSamplers(0, 1, &sampler);
        g_context->Draw(3, 0);
        ID3D11ShaderResourceView* null_views[1] = {nullptr};
        g_context->PSSetShaderResources(0, 1, null_views);
    }
    (void)g_remote_swap_chain->Present(0, 0);
}

LRESULT CALLBACK remote_window_proc(HWND hwnd, UINT message, WPARAM wparam,
                                    LPARAM lparam) {
    switch (message) {
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP: {
        if (g_remote_control_plane != nullptr && g_remote_target_client != 0) {
            const int mx = GET_X_LPARAM(lparam);
            const int my = GET_Y_LPARAM(lparam);
            const RemoteImageRect& rect = g_remote_image_rect;
            float rx = 0.5f;
            float ry = 0.5f;
            if (rect.w > 0.0f && rect.h > 0.0f) {
                rx = std::clamp((static_cast<float>(mx) - rect.x) / rect.w, 0.0f,
                                1.0f);
                ry = std::clamp((static_cast<float>(my) - rect.y) / rect.h, 0.0f,
                                1.0f);
            }
            nstu::wire::RemoteInputPacket packet{};
            packet.input_type =
                static_cast<std::uint8_t>(nstu::wire::RemoteInputType::mouse);
            packet.flags =
                static_cast<std::uint8_t>(
                    nstu::wire::RemoteInputFlags::mouse_absolute) |
                static_cast<std::uint8_t>(
                    nstu::wire::RemoteInputFlags::mouse_normalized);
            if (message == WM_LBUTTONDOWN) {
                packet.flags |= static_cast<std::uint8_t>(
                    nstu::wire::RemoteInputFlags::mouse_left_down);
                SetCapture(hwnd);
            } else if (message == WM_LBUTTONUP) {
                packet.flags |= static_cast<std::uint8_t>(
                    nstu::wire::RemoteInputFlags::mouse_left_up);
                ReleaseCapture();
            } else if (message == WM_RBUTTONDOWN) {
                packet.flags |= static_cast<std::uint8_t>(
                    nstu::wire::RemoteInputFlags::mouse_right_down);
                SetCapture(hwnd);
            } else if (message == WM_RBUTTONUP) {
                packet.flags |= static_cast<std::uint8_t>(
                    nstu::wire::RemoteInputFlags::mouse_right_up);
                ReleaseCapture();
            }
            packet.x = static_cast<std::int32_t>(rx * 65535.0f);
            packet.y = static_cast<std::int32_t>(ry * 65535.0f);
            std::string ignored;
            (void)g_remote_control_plane->send_remote_input(
                g_remote_target_client, packet, &ignored);
        }
        return 0;
    }
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP: {
        if (g_remote_control_plane != nullptr && g_remote_target_client != 0) {
            nstu::wire::RemoteInputPacket packet{};
            packet.input_type =
                static_cast<std::uint8_t>(nstu::wire::RemoteInputType::keyboard);
            packet.virtual_key = static_cast<std::uint16_t>(wparam);
            if (message == WM_KEYUP || message == WM_SYSKEYUP) {
                packet.flags = static_cast<std::uint8_t>(
                    nstu::wire::RemoteInputFlags::key_up);
            }
            std::string ignored;
            (void)g_remote_control_plane->send_remote_input(
                g_remote_target_client, packet, &ignored);
        }
        return 0;
    }
    case WM_SIZE:
        if (g_remote_swap_chain && wparam != SIZE_MINIMIZED) {
            g_remote_render_target.Reset();
            (void)g_remote_swap_chain->ResizeBuffers(
                0, LOWORD(lparam), HIWORD(lparam), DXGI_FORMAT_UNKNOWN, 0);
            create_remote_render_target();
        }
        return 0;
    case WM_CLOSE:
        // Tear-down runs on the main loop (COM release + control-plane stop)
        // rather than inside this synchronous window callback.
        g_remote_close_pending.store(true);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

void close_remote_window(nstu::server::ServerControlPlane& control_plane) {
    if (g_remote_target_client != 0) {
        (void)control_plane.stop_remote_control(g_remote_target_client, nullptr);
        g_remote_target_client = 0;
    }
    g_remote_render_target.Reset();
    g_remote_swap_chain.Reset();
    if (g_remote_window != nullptr) {
        DestroyWindow(g_remote_window);
        g_remote_window = nullptr;
    }
    g_remote_image_rect = {};
    g_remote_close_pending.store(false);
}

bool open_remote_window(nstu::server::ServerControlPlane& control_plane,
                        std::uint64_t client_id) {
    if (!g_device || client_id == 0) {
        return false;
    }
    g_remote_control_plane = &control_plane;
    if (!ensure_remote_pipeline()) {
        return false;
    }
    if (g_remote_window == nullptr) {
        if (!g_remote_class_registered) {
            WNDCLASSW window_class{};
            window_class.lpfnWndProc = remote_window_proc;
            window_class.hInstance = GetModuleHandleW(nullptr);
            window_class.lpszClassName = L"NstuRemoteWindow";
            window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
            window_class.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
            RegisterClassW(&window_class);
            g_remote_class_registered = true;
        }
        g_remote_window = CreateWindowW(
            L"NstuRemoteWindow", L"NSTU Remote control", WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT, 1120, 700, nullptr, nullptr,
            GetModuleHandleW(nullptr), nullptr);
        if (g_remote_window == nullptr) {
            return false;
        }
        Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
        Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
        Microsoft::WRL::ComPtr<IDXGIFactory> factory;
        if (FAILED(g_device.As(&dxgi_device)) ||
            FAILED(dxgi_device->GetAdapter(&adapter)) ||
            FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
            DestroyWindow(g_remote_window);
            g_remote_window = nullptr;
            return false;
        }
        DXGI_SWAP_CHAIN_DESC description{};
        description.BufferCount = 2;
        description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        description.OutputWindow = g_remote_window;
        description.SampleDesc.Count = 1;
        description.Windowed = TRUE;
        description.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        if (FAILED(factory->CreateSwapChain(g_device.Get(), &description,
                                            &g_remote_swap_chain)) ||
            !create_remote_render_target()) {
            g_remote_render_target.Reset();
            g_remote_swap_chain.Reset();
            DestroyWindow(g_remote_window);
            g_remote_window = nullptr;
            return false;
        }
        RECT client{};
        GetClientRect(g_remote_window, &client);
        g_remote_image_rect = {0.0f, 0.0f,
                               static_cast<float>(client.right - client.left),
                               static_cast<float>(client.bottom - client.top)};
        ShowWindow(g_remote_window, SW_SHOW);
    }
    if (g_remote_target_client != 0 && g_remote_target_client != client_id) {
        (void)control_plane.stop_remote_control(g_remote_target_client, nullptr);
        g_remote_target_client = 0;
    }
    std::string error;
    const bool ok = control_plane.start_remote_control(client_id, &error);
    if (ok) {
        g_remote_target_client = client_id;
        SetForegroundWindow(g_remote_window);
    } else {
        record_operation_failure("RemoteControl", "Start command failed", error);
    }
    return ok;
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam,
                             LPARAM lparam) {
    if (ImGui_ImplWin32_WndProcHandler(window, message, wparam, lparam)) {
        return 1;
    }
    if (message == WM_GETMINMAXINFO) {
        auto* minimum = reinterpret_cast<MINMAXINFO*>(lparam);
        minimum->ptMinTrackSize.x = 960;
        minimum->ptMinTrackSize.y = 640;
        return 0;
    }
    if (message == g_taskbar_created_message &&
        g_taskbar_created_message != 0) {
        add_tray_icon(window);
        return 0;
    }
    if (message == kTrayMessage) {
        if (LOWORD(lparam) == WM_LBUTTONDBLCLK) {
            show_main_window(window);
        } else if (LOWORD(lparam) == WM_RBUTTONUP ||
                   LOWORD(lparam) == WM_CONTEXTMENU) {
            show_tray_menu(window);
        }
        return 0;
    }
    if (message == WM_COMMAND) {
        if (LOWORD(wparam) == kTrayToggleWindow) {
            if (IsWindowVisible(window)) {
                ShowWindow(window, SW_HIDE);
            } else {
                show_main_window(window);
            }
            return 0;
        }
        if (LOWORD(wparam) == kTrayExit) {
            DestroyWindow(window);
            return 0;
        }
    }
    // Minimize behaves like a normal window and goes to the taskbar; only
    // closing hides NSTU to the system tray. Operators expect a plain minimize
    // here, so SC_MINIMIZE falls through to DefWindowProc's standard handling.
    if (message == WM_CLOSE) {
        ShowWindow(window, SW_HIDE);
        return 0;
    }
    if (message == WM_SIZE && g_swap_chain && wparam != SIZE_MINIMIZED) {
        g_render_target.Reset();
        const HRESULT resize_result = g_swap_chain->ResizeBuffers(
            0, LOWORD(lparam), HIWORD(lparam), DXGI_FORMAT_UNKNOWN, 0);
        if (FAILED(resize_result)) {
            record_diagnostic("error", "DXGI", "ResizeBuffers failed " +
                hresult_text(resize_result));
        }
        create_render_target();
        return 0;
    }
    if (message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

void apply_dashboard_style(bool dark_mode) {
    g_dark_mode = dark_mode;
    auto& style = ImGui::GetStyle();
    style.WindowPadding = {8.0f, 6.0f};
    style.FramePadding = {8.0f, 5.0f};
    style.CellPadding = {7.0f, 6.0f};
    style.ItemSpacing = {6.0f, 5.0f};
    style.ItemInnerSpacing = {5.0f, 4.0f};
    style.WindowRounding = 0.0f;
    style.ChildRounding = 3.0f;
    style.FrameRounding = 3.0f;
    style.PopupRounding = 3.0f;
    style.ScrollbarRounding = 3.0f;
    style.GrabRounding = 3.0f;
    style.WindowBorderSize = 0.0f;
    style.ChildBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;

    auto* colors = style.Colors;
    colors[ImGuiCol_Text] = dark_mode
        ? ImVec4{0.91f, 0.90f, 0.87f, 1.0f}
        : ImVec4{0.12f, 0.12f, 0.11f, 1.0f};
    colors[ImGuiCol_TextDisabled] = dark_mode
        ? ImVec4{0.61f, 0.60f, 0.57f, 1.0f}
        : ImVec4{0.46f, 0.45f, 0.43f, 1.0f};
    colors[ImGuiCol_WindowBg] = dark_mode
        ? ImVec4{0.075f, 0.075f, 0.070f, 1.0f}
        : ImVec4{0.975f, 0.971f, 0.958f, 1.0f};
    colors[ImGuiCol_ChildBg] = dark_mode
        ? ImVec4{0.105f, 0.105f, 0.098f, 1.0f}
        : ImVec4{1.0f, 1.0f, 1.0f, 1.0f};
    colors[ImGuiCol_PopupBg] = dark_mode
        ? ImVec4{0.12f, 0.12f, 0.11f, 1.0f}
        : ImVec4{1.0f, 1.0f, 1.0f, 1.0f};
    colors[ImGuiCol_Border] = dark_mode
        ? ImVec4{0.22f, 0.22f, 0.20f, 1.0f}
        : ImVec4{0.90f, 0.89f, 0.87f, 1.0f};
    colors[ImGuiCol_BorderShadow] = {0.0f, 0.0f, 0.0f, 0.0f};
    colors[ImGuiCol_FrameBg] = dark_mode
        ? ImVec4{0.14f, 0.14f, 0.13f, 1.0f}
        : ImVec4{0.985f, 0.982f, 0.973f, 1.0f};
    colors[ImGuiCol_FrameBgHovered] = dark_mode
        ? ImVec4{0.20f, 0.20f, 0.18f, 1.0f}
        : ImVec4{0.95f, 0.945f, 0.93f, 1.0f};
    colors[ImGuiCol_FrameBgActive] = dark_mode
        ? ImVec4{0.25f, 0.25f, 0.23f, 1.0f}
        : ImVec4{0.92f, 0.915f, 0.90f, 1.0f};
    colors[ImGuiCol_TitleBg] = colors[ImGuiCol_WindowBg];
    colors[ImGuiCol_TitleBgActive] = colors[ImGuiCol_WindowBg];
    colors[ImGuiCol_Button] = dark_mode
        ? ImVec4{0.17f, 0.17f, 0.16f, 1.0f}
        : ImVec4{0.93f, 0.925f, 0.91f, 1.0f};
    colors[ImGuiCol_ButtonHovered] = dark_mode
        ? ImVec4{0.24f, 0.24f, 0.22f, 1.0f}
        : ImVec4{0.88f, 0.875f, 0.86f, 1.0f};
    colors[ImGuiCol_ButtonActive] = dark_mode
        ? ImVec4{0.30f, 0.30f, 0.28f, 1.0f}
        : ImVec4{0.83f, 0.825f, 0.81f, 1.0f};
    colors[ImGuiCol_Header] = dark_mode
        ? ImVec4{0.17f, 0.24f, 0.27f, 1.0f}
        : ImVec4{0.88f, 0.93f, 0.95f, 1.0f};
    colors[ImGuiCol_HeaderHovered] = dark_mode
        ? ImVec4{0.21f, 0.31f, 0.35f, 1.0f}
        : ImVec4{0.82f, 0.90f, 0.94f, 1.0f};
    colors[ImGuiCol_HeaderActive] = dark_mode
        ? ImVec4{0.24f, 0.36f, 0.41f, 1.0f}
        : ImVec4{0.76f, 0.87f, 0.92f, 1.0f};
    colors[ImGuiCol_Separator] = colors[ImGuiCol_Border];
    colors[ImGuiCol_CheckMark] = dark_mode
        ? ImVec4{0.52f, 0.74f, 0.82f, 1.0f}
        : ImVec4{0.18f, 0.36f, 0.45f, 1.0f};
    colors[ImGuiCol_SliderGrab] = dark_mode
        ? ImVec4{0.72f, 0.71f, 0.68f, 1.0f}
        : ImVec4{0.18f, 0.18f, 0.17f, 1.0f};
    colors[ImGuiCol_SliderGrabActive] = dark_mode
        ? ImVec4{0.90f, 0.89f, 0.86f, 1.0f}
        : ImVec4{0.30f, 0.30f, 0.29f, 1.0f};
}

void load_dashboard_fonts() {
    std::array<char, MAX_PATH> windows_directory{};
    const UINT length = GetWindowsDirectoryA(
        windows_directory.data(), static_cast<UINT>(windows_directory.size()));
    if (length == 0 || length >= windows_directory.size()) {
        return;
    }
    const std::string fonts_directory =
        std::string(windows_directory.data()) + "\\Fonts\\";
    const auto regular = fonts_directory + "segoeui.ttf";
    const auto bold = fonts_directory + "segoeuib.ttf";
    auto& atlas = ImGui::GetIO().Fonts;
    const ImWchar* glyph_ranges = atlas->GetGlyphRangesVietnamese();
    if (GetFileAttributesA(regular.c_str()) != INVALID_FILE_ATTRIBUTES) {
        atlas->AddFontFromFileTTF(regular.c_str(), 15.0f, nullptr,
                                  glyph_ranges);
    }
    if (GetFileAttributesA(bold.c_str()) != INVALID_FILE_ATTRIBUTES) {
        g_heading_font = atlas->AddFontFromFileTTF(
            bold.c_str(), 17.0f, nullptr, glyph_ranges);
    }
}

ImVec4 status_text_color(nstu::server::ClientStatus status) {
    switch (status) {
    case nstu::server::ClientStatus::online:
        return g_dark_mode ? ImVec4{0.56f, 0.78f, 0.58f, 1.0f}
                           : ImVec4{0.20f, 0.40f, 0.23f, 1.0f};
    case nstu::server::ClientStatus::degraded:
    case nstu::server::ClientStatus::connecting:
        return g_dark_mode ? ImVec4{0.90f, 0.73f, 0.38f, 1.0f}
                           : ImVec4{0.58f, 0.39f, 0.06f, 1.0f};
    case nstu::server::ClientStatus::locked:
        return g_dark_mode ? ImVec4{0.94f, 0.56f, 0.55f, 1.0f}
                           : ImVec4{0.62f, 0.18f, 0.17f, 1.0f};
    case nstu::server::ClientStatus::offline:
        return g_dark_mode ? ImVec4{0.67f, 0.66f, 0.63f, 1.0f}
                           : ImVec4{0.40f, 0.39f, 0.37f, 1.0f};
    }
    return g_dark_mode ? ImVec4{0.67f, 0.66f, 0.63f, 1.0f}
                       : ImVec4{0.40f, 0.39f, 0.37f, 1.0f};
}

ImVec4 status_background_color(nstu::server::ClientStatus status) {
    switch (status) {
    case nstu::server::ClientStatus::online:
        return g_dark_mode ? ImVec4{0.13f, 0.23f, 0.14f, 1.0f}
                           : ImVec4{0.93f, 0.96f, 0.92f, 1.0f};
    case nstu::server::ClientStatus::degraded:
    case nstu::server::ClientStatus::connecting:
        return g_dark_mode ? ImVec4{0.25f, 0.20f, 0.10f, 1.0f}
                           : ImVec4{0.985f, 0.95f, 0.86f, 1.0f};
    case nstu::server::ClientStatus::locked:
        return g_dark_mode ? ImVec4{0.27f, 0.13f, 0.13f, 1.0f}
                           : ImVec4{0.99f, 0.92f, 0.92f, 1.0f};
    case nstu::server::ClientStatus::offline:
        return g_dark_mode ? ImVec4{0.18f, 0.18f, 0.17f, 1.0f}
                           : ImVec4{0.94f, 0.935f, 0.925f, 1.0f};
    }
    return g_dark_mode ? ImVec4{0.18f, 0.18f, 0.17f, 1.0f}
                       : ImVec4{0.94f, 0.935f, 0.925f, 1.0f};
}

const char* client_status_label(nstu::server::ClientStatus status,
                                const DashboardState& state) {
    if (state.language == Language::english) {
        return nstu::server::to_string(status);
    }
    switch (status) {
    case nstu::server::ClientStatus::online:
        return "Trực tuyến";
    case nstu::server::ClientStatus::connecting:
        return "Đang kết nối";
    case nstu::server::ClientStatus::degraded:
        return "Cần chú ý";
    case nstu::server::ClientStatus::locked:
        return "Đã khóa";
    case nstu::server::ClientStatus::offline:
        return "Ngoại tuyến";
    }
    return "Không rõ";
}

RoomCounts count_room_statuses(
    const std::vector<nstu::server::ClientRecord>& clients) {
    RoomCounts counts;
    for (const auto& client : clients) {
        switch (client.status) {
        case nstu::server::ClientStatus::online:
            ++counts.online;
            break;
        case nstu::server::ClientStatus::connecting:
        case nstu::server::ClientStatus::degraded:
            ++counts.attention;
            break;
        case nstu::server::ClientStatus::locked:
            ++counts.locked;
            break;
        case nstu::server::ClientStatus::offline:
            ++counts.offline;
            break;
        }
    }
    return counts;
}

std::string lower_ascii(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](char ch) {
        return static_cast<char>(
            std::tolower(static_cast<unsigned char>(ch)));
    });
    return result;
}

bool client_matches_filter(const nstu::server::ClientRecord& client,
                           const DashboardState& state) {
    if (!state.show_offline &&
        client.status == nstu::server::ClientStatus::offline) {
        return false;
    }
    switch (state.room_filter) {
    case RoomFilter::all:
        break;
    case RoomFilter::online:
        if (client.status != nstu::server::ClientStatus::online) {
            return false;
        }
        break;
    case RoomFilter::attention:
        if (client.status != nstu::server::ClientStatus::connecting &&
            client.status != nstu::server::ClientStatus::degraded) {
            return false;
        }
        break;
    case RoomFilter::locked:
        if (client.status != nstu::server::ClientStatus::locked) {
            return false;
        }
        break;
    case RoomFilter::offline:
        if (client.status != nstu::server::ClientStatus::offline) {
            return false;
        }
        break;
    }
    if (state.client_filter[0] == '\0') {
        return true;
    }
    const auto query = lower_ascii(state.client_filter.data());
    return lower_ascii(client.hostname).find(query) != std::string::npos ||
           lower_ascii(client.address).find(query) != std::string::npos;
}

void push_client_id(std::uint64_t id) {
    ImGui::PushID(static_cast<int>(id >> 32u));
    ImGui::PushID(static_cast<int>(id & 0xffffffffu));
}

void pop_client_id() {
    ImGui::PopID();
    ImGui::PopID();
}

void draw_icon(ImDrawList* draw_list, IconKind icon, ImVec2 center,
               float size, ImU32 color) {
    const float half = size * 0.5f;
    const float left = center.x - half;
    const float top = center.y - half;
    const float right = center.x + half;
    const float bottom = center.y + half;
    const float stroke = 1.8f;
    switch (icon) {
    case IconKind::grid: {
        const float cell = size * 0.34f;
        const float gap = size * 0.12f;
        for (int row = 0; row < 2; ++row) {
            for (int column = 0; column < 2; ++column) {
                const ImVec2 minimum{
                    left + column * (cell + gap),
                    top + row * (cell + gap)};
                draw_list->AddRect(
                    minimum, {minimum.x + cell, minimum.y + cell}, color,
                    1.5f, 0, stroke);
            }
        }
        break;
    }
    case IconKind::monitor:
        draw_list->AddRect({left, top + 1.0f}, {right, bottom - 4.0f}, color,
                           2.0f, 0, stroke);
        draw_list->AddLine({center.x, bottom - 4.0f}, {center.x, bottom}, color,
                           stroke);
        draw_list->AddLine({center.x - 5.0f, bottom},
                           {center.x + 5.0f, bottom}, color, stroke);
        break;
    case IconKind::camera:
        draw_list->AddRect({left, top + 3.0f}, {right, bottom}, color, 2.0f, 0,
                           stroke);
        draw_list->AddRect({left + 4.0f, top}, {left + 10.0f, top + 4.0f},
                           color, 1.0f, 0, stroke);
        draw_list->AddCircle(center, size * 0.21f, color, 16, stroke);
        break;
    case IconKind::stop:
        draw_list->AddRectFilled({left + 3.0f, top + 3.0f},
                                 {right - 3.0f, bottom - 3.0f}, color, 2.0f);
        break;
    case IconKind::lock:
    case IconKind::unlock: {
        const bool unlocked = icon == IconKind::unlock;
        draw_list->AddRect({left + 3.0f, center.y - 1.0f},
                           {right - 3.0f, bottom}, color, 2.0f, 0, stroke);
        draw_list->PathClear();
        const ImVec2 shackle_center{
            center.x + (unlocked ? 3.0f : 0.0f), center.y - 1.0f};
        draw_list->PathArcTo(shackle_center, size * 0.26f,
                             3.1415926f, 6.2831852f, 12);
        draw_list->PathStroke(color, 0, stroke);
        if (unlocked) {
            draw_list->AddLine({left + 2.0f, top + 7.0f},
                               {left + 2.0f, center.y - 1.0f}, color, stroke);
        }
        break;
    }
    case IconKind::pen:
        draw_list->AddLine({left + 3.0f, bottom - 2.0f},
                           {right - 2.0f, top + 3.0f}, color, 3.0f);
        draw_list->AddTriangleFilled({right - 2.0f, top + 3.0f},
                                     {right - 6.0f, top + 4.0f},
                                     {right - 3.0f, top + 7.0f}, color);
        break;
    case IconKind::erase:
        draw_list->AddQuad({left + 3.0f, bottom - 6.0f},
                           {center.x + 2.0f, top + 2.0f},
                           {right - 2.0f, top + 7.0f},
                           {center.x - 3.0f, bottom - 1.0f}, color, stroke);
        draw_list->AddLine({left + 5.0f, bottom - 4.0f},
                           {right - 1.0f, bottom - 4.0f}, color, stroke);
        break;
    case IconKind::broadcast:
        draw_list->AddCircleFilled(center, 2.4f, color);
        for (int ring = 1; ring <= 2; ++ring) {
            const float radius = size * (0.18f + ring * 0.15f);
            draw_list->PathClear();
            draw_list->PathArcTo(center, radius, -0.8f, 0.8f, 12);
            draw_list->PathStroke(color, 0, stroke);
            draw_list->PathClear();
            draw_list->PathArcTo(center, radius, 2.34f, 3.94f, 12);
            draw_list->PathStroke(color, 0, stroke);
        }
        break;
    case IconKind::chat:
        draw_list->AddRect({left, top}, {right, bottom - 4.0f}, color, 3.0f, 0,
                           stroke);
        draw_list->AddTriangleFilled({left + 4.0f, bottom - 4.0f},
                                     {left + 8.0f, bottom - 4.0f},
                                     {left + 4.0f, bottom}, color);
        draw_list->AddCircleFilled({center.x - 6.0f, center.y - 2.0f}, 1.5f,
                                   color);
        draw_list->AddCircleFilled({center.x, center.y - 2.0f}, 1.5f, color);
        draw_list->AddCircleFilled({center.x + 6.0f, center.y - 2.0f}, 1.5f,
                                   color);
        break;
    }
}

bool draw_icon_button(const char* id, const char* label, IconKind icon,
                      ImVec2 size, bool selected = false,
                      bool enabled = true, bool show_label = true,
                      bool badge = false) {
    ImGui::PushID(id);
    if (!enabled) {
        ImGui::BeginDisabled();
    }
    ImGui::InvisibleButton("##icon-button", size);
    const bool pressed = enabled && ImGui::IsItemClicked();
    const bool hovered = enabled && ImGui::IsItemHovered();
    const ImVec2 minimum = ImGui::GetItemRectMin();
    const ImVec2 maximum = ImGui::GetItemRectMax();
    auto* draw_list = ImGui::GetWindowDrawList();
    if (selected || hovered) {
        const ImU32 background = ImGui::GetColorU32(
            selected ? ImGuiCol_Header : ImGuiCol_FrameBgHovered);
        draw_list->AddRectFilled(minimum, maximum, background, 4.0f);
    }
    const ImU32 foreground = ImGui::GetColorU32(
        enabled ? ImGuiCol_Text : ImGuiCol_TextDisabled);
    const float label_height = show_label ? 20.0f : 0.0f;
    draw_icon(draw_list, icon,
              {minimum.x + size.x * 0.5f,
               minimum.y + (size.y - label_height) * 0.43f},
              show_label ? 22.0f : 20.0f, foreground);
    if (show_label) {
        const ImVec2 text_size = ImGui::CalcTextSize(label);
        draw_list->AddText(
            {minimum.x + std::max(3.0f, (size.x - text_size.x) * 0.5f),
             maximum.y - text_size.y - 5.0f},
            foreground, label);
    }
    if (hovered && !show_label) {
        ImGui::SetTooltip("%s", label);
    }
    if (badge) {
        // Small attention dot at the top-right corner; draws over the icon so it
        // reads as an unread marker regardless of label state.
        const ImVec2 center{maximum.x - 6.0f, minimum.y + 6.0f};
        draw_list->AddCircleFilled(center, 4.5f, IM_COL32(229, 72, 77, 255));
        draw_list->AddCircle(center, 4.5f, IM_COL32(255, 255, 255, 235), 12,
                             1.4f);
    }
    if (!enabled) {
        ImGui::EndDisabled();
    }
    ImGui::PopID();
    return pressed;
}

void draw_status_badge(nstu::server::ClientStatus status,
                       const DashboardState& state) {
    const char* label = client_status_label(status, state);
    const ImVec2 text_size = ImGui::CalcTextSize(label);
    const ImVec2 size{text_size.x + 16.0f, text_size.y + 8.0f};
    ImGui::PushID(label);
    ImGui::InvisibleButton("##status", size);
    const ImVec2 minimum = ImGui::GetItemRectMin();
    const ImVec2 maximum = ImGui::GetItemRectMax();
    auto* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddRectFilled(minimum, maximum,
                             ImGui::ColorConvertFloat4ToU32(
                                 status_background_color(status)),
                             4.0f);
    draw_list->AddText({minimum.x + 8.0f, minimum.y + 4.0f},
                       ImGui::ColorConvertFloat4ToU32(
                           status_text_color(status)),
                       label);
    ImGui::PopID();
}

SnapshotTexture* snapshot_texture(
    const nstu::server::ClientRecord& client) {
    if (client.snapshot_generation == 0 || !client.snapshot_jpeg ||
        client.snapshot_jpeg->empty() || g_graphics_device_lost || !g_device) {
        return nullptr;
    }
    auto& cached = g_snapshot_textures[client.id];
    if (cached.generation_gate.succeeded(client.snapshot_generation) &&
        cached.view) {
        return &cached;
    }
    if (!cached.generation_gate.begin(client.snapshot_generation)) {
        return nullptr;
    }
    cached.width = 0;
    cached.height = 0;
    cached.view.Reset();
    nstu::screen::BgraImage decoded;
    const auto bytes = std::span<const std::byte>(
        client.snapshot_jpeg->data(), client.snapshot_jpeg->size());
    std::string decode_error;
    if (!nstu::screen::decode_jpeg(
            bytes, client.snapshot_width, client.snapshot_height, decoded,
            &decode_error)) {
        cached.generation_gate.mark_failed(client.snapshot_generation);
        record_diagnostic(
            "warning", "Snapshot",
            "Client " + std::to_string(client.id) +
                " snapshot decode failed: " +
                (decode_error.empty() ? std::string("invalid JPEG")
                                      : decode_error));
        return nullptr;
    }
    D3D11_TEXTURE2D_DESC description{};
    description.Width = decoded.width;
    description.Height = decoded.height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_IMMUTABLE;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{};
    data.pSysMem = decoded.pixels.data();
    data.SysMemPitch = decoded.stride;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
    const HRESULT texture_result =
        g_device->CreateTexture2D(&description, &data, &texture);
    const HRESULT view_result = SUCCEEDED(texture_result)
        ? g_device->CreateShaderResourceView(texture.Get(), nullptr, &view)
        : E_FAIL;
    if (FAILED(texture_result) || FAILED(view_result)) {
        cached.generation_gate.mark_failed(client.snapshot_generation);
        const HRESULT failure = FAILED(texture_result)
            ? texture_result : view_result;
        const HRESULT removal_reason = g_device->GetDeviceRemovedReason();
        if (is_device_loss_hresult(failure) ||
            is_device_loss_hresult(removal_reason)) {
            g_graphics_device_lost = true;
            invalidate_snapshot_textures();
        }
        record_diagnostic(
            "warning", "Snapshot",
            "Client " + std::to_string(client.id) +
                " snapshot " +
                (FAILED(texture_result) ? std::string("texture")
                                         : std::string("view")) +
                " creation failed " + hresult_text(failure) +
                "; device reason " + hresult_text(removal_reason));
        return nullptr;
    }
    cached.width = decoded.width;
    cached.height = decoded.height;
    cached.view = std::move(view);
    cached.generation_gate.mark_succeeded(client.snapshot_generation);
    return &cached;
}

struct ScreenSurfaceResult {
    bool clicked = false;
    bool has_frame = false;
    ImVec2 image_min{};
    ImVec2 image_max{};
};

ScreenSurfaceResult draw_screen_surface(
    const nstu::server::ClientRecord& client, float height, const char* id,
    const DashboardState& state) {
    const ImVec2 size{ImGui::GetContentRegionAvail().x, height};
    ImGui::InvisibleButton(id, size);
    ScreenSurfaceResult result;
    result.clicked = ImGui::IsItemClicked();
    const ImVec2 minimum = ImGui::GetItemRectMin();
    const ImVec2 maximum = ImGui::GetItemRectMax();
    const bool offline = client.status == nstu::server::ClientStatus::offline;
    const ImU32 background = offline
        ? (g_dark_mode ? IM_COL32(43, 43, 40, 255)
                       : IM_COL32(230, 228, 223, 255))
        : (g_dark_mode ? IM_COL32(20, 20, 19, 255)
                       : IM_COL32(35, 35, 33, 255));
    const ImU32 foreground = offline
        ? (g_dark_mode ? IM_COL32(172, 170, 164, 255)
                       : IM_COL32(100, 98, 94, 255))
        : IM_COL32(240, 239, 235, 255);
    auto* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddRectFilled(minimum, maximum, background, 2.0f);
    auto* texture = snapshot_texture(client);
    if (!offline && texture != nullptr) {
        const float source_aspect = static_cast<float>(texture->width) /
                                    static_cast<float>(texture->height);
        const float target_aspect = size.x / size.y;
        ImVec2 image_size = size;
        if (source_aspect > target_aspect) {
            image_size.y = size.x / source_aspect;
        } else {
            image_size.x = size.y * source_aspect;
        }
        result.image_min = {
            minimum.x + (size.x - image_size.x) * 0.5f,
            minimum.y + (size.y - image_size.y) * 0.5f};
        result.image_max = {result.image_min.x + image_size.x,
                            result.image_min.y + image_size.y};
        draw_list->AddImage(
            reinterpret_cast<ImTextureID>(texture->view.Get()),
            result.image_min, result.image_max);
        result.has_frame = true;
    } else {
        const char* text = offline
            ? tr(state, "Offline", "Ngoại tuyến")
            : tr(state, "Waiting for snapshot", "Đang chờ ảnh chụp");
        const ImVec2 text_size = ImGui::CalcTextSize(text);
        draw_list->AddText({minimum.x + (size.x - text_size.x) * 0.5f,
                            minimum.y + (height - text_size.y) * 0.5f},
                           foreground, text);
    }
    return result;
}

void draw_client_card(const nstu::server::ClientRecord& client, float width,
                      DashboardState& state) {
    push_client_id(client.id);
    const bool selected = state.selected_client_id == client.id;
    ImGui::PushStyleColor(ImGuiCol_Border,
        selected ? (g_dark_mode ? ImVec4{0.42f, 0.68f, 0.78f, 1.0f}
                                : ImVec4{0.31f, 0.56f, 0.68f, 1.0f})
                 : ImGui::GetStyleColorVec4(ImGuiCol_Border));
    if (ImGui::BeginChild("client-card", {width, width * 0.5625f + 58.0f},
                          true, ImGuiWindowFlags_NoScrollbar)) {
        if (draw_screen_surface(client, width * 0.5625f, "##screen", state)
                .clicked) {
            state.selected_client_id = client.id;
            state.view = DashboardView::selected_client;
            state.annotation_enabled = false;
        }
        ImGui::TextUnformatted(client.hostname.c_str());
        const float status_width =
            ImGui::CalcTextSize(client_status_label(client.status, state)).x;
        ImGui::SameLine(ImGui::GetContentRegionMax().x - status_width);
        ImGui::TextColored(status_text_color(client.status), "%s",
                           client_status_label(client.status, state));
        ImGui::TextDisabled("%s", client.address.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%u ms  %.1f%%", client.latency_ms,
                            client.packet_loss_per_mille / 10.0f);
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    pop_client_id();
}

void draw_room_screen_wall(
    const std::vector<nstu::server::ClientRecord>& clients,
    DashboardState& state) {
    std::vector<const nstu::server::ClientRecord*> visible_clients;
    visible_clients.reserve(clients.size());
    for (const auto& client : clients) {
        if (client_matches_filter(client, state)) {
            visible_clients.push_back(&client);
        }
    }

    if (visible_clients.empty()) {
        const float offset = std::max(36.0f,
                                      ImGui::GetContentRegionAvail().y * 0.34f);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + offset);
        const char* heading = clients.empty()
            ? tr(state, "No clients connected", "Chưa có máy nào kết nối")
            : tr(state, "No clients match this view",
                 "Không có máy phù hợp với bộ lọc");
        const ImVec2 heading_size = ImGui::CalcTextSize(heading);
        ImGui::SetCursorPosX(
            std::max(20.0f, (ImGui::GetWindowWidth() - heading_size.x) * 0.5f));
        ImGui::TextDisabled("%s", heading);
        return;
    }

    const ImVec4 workspace_background = g_dark_mode
        ? ImVec4{0.065f, 0.085f, 0.095f, 1.0f}
        : ImVec4{0.91f, 0.965f, 0.985f, 1.0f};
    ImGui::PushStyleColor(ImGuiCol_ChildBg, workspace_background);
    if (ImGui::BeginChild("screen-wall", {0, 0}, false)) {
        constexpr float minimum_card_width = 205.0f;
        const float gap = ImGui::GetStyle().ItemSpacing.x;
        const float available = ImGui::GetContentRegionAvail().x;
        const int columns = std::clamp(
            static_cast<int>((available + gap) / (minimum_card_width + gap)),
            1, 6);
        const float card_width =
            (available - gap * static_cast<float>(columns - 1)) /
            static_cast<float>(columns);
        for (std::size_t index = 0; index < visible_clients.size(); ++index) {
            draw_client_card(*visible_clients[index], card_width, state);
            if ((index + 1) % static_cast<std::size_t>(columns) != 0) {
                ImGui::SameLine();
            }
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void draw_focus_client_list(
    const std::vector<nstu::server::ClientRecord>& clients,
    DashboardState& state) {
    if (ImGui::BeginChild("focus-client-list", {280.0f, 0}, true)) {
        ImGui::TextUnformatted(tr(state, "Clients", "Máy học sinh"));
        ImGui::Separator();
        if (ImGui::BeginTable("focus-clients", 2,
                              ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn(tr(state, "Client", "Máy"),
                                    ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn(tr(state, "Status", "Trạng thái"),
                                    ImGuiTableColumnFlags_WidthFixed, 94.0f);
            for (const auto& client : clients) {
                if (!client_matches_filter(client, state)) {
                    continue;
                }
                push_client_id(client.id);
                ImGui::TableNextRow(0, 40.0f);
                ImGui::TableSetColumnIndex(0);
                const bool selected = state.selected_client_id == client.id;
                if (ImGui::Selectable(client.hostname.c_str(), selected,
                                      ImGuiSelectableFlags_None,
                                      {0.0f, 36.0f})) {
                    state.selected_client_id = client.id;
                    state.annotation_enabled = false;
                }
                if (client_has_unread_chat(state, client.id)) {
                    const ImVec2 rect_min = ImGui::GetItemRectMin();
                    const ImVec2 rect_max = ImGui::GetItemRectMax();
                    ImGui::GetWindowDrawList()->AddCircleFilled(
                        {rect_max.x - 9.0f,
                         (rect_min.y + rect_max.y) * 0.5f},
                        4.0f, IM_COL32(229, 72, 77, 255));
                }
                ImGui::TableSetColumnIndex(1);
                ImGui::TextColored(status_text_color(client.status), "%s",
                                   client_status_label(client.status, state));
                pop_client_id();
            }
            ImGui::EndTable();
        }
    }
    ImGui::EndChild();
}

void draw_telemetry_card(const char* label, const char* value, float width) {
    ImGui::PushID(label);
    if (ImGui::BeginChild("telemetry", {width, 62.0f}, true,
                          ImGuiWindowFlags_NoScrollbar)) {
        ImGui::TextDisabled("%s", label);
        ImGui::TextUnformatted(value);
    }
    ImGui::EndChild();
    ImGui::PopID();
}

void draw_selected_client(
    const std::vector<nstu::server::ClientRecord>& clients,
    const nstu::server::ClientRecord* selected_client, DashboardState& state,
    nstu::server::ServerControlPlane& control_plane) {
    draw_focus_client_list(clients, state);
    ImGui::SameLine();
    if (!ImGui::BeginChild("focus-detail", {0, 0}, false)) {
        ImGui::EndChild();
        return;
    }
    if (selected_client == nullptr) {
        const char* empty = clients.empty()
            ? tr(state, "No clients connected", "Chưa có máy nào kết nối")
            : tr(state, "Select a client", "Chọn một máy học sinh");
        const float offset = std::max(36.0f,
                                      ImGui::GetContentRegionAvail().y * 0.38f);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + offset);
        const ImVec2 size = ImGui::CalcTextSize(empty);
        ImGui::SetCursorPosX(
            std::max(20.0f, (ImGui::GetWindowWidth() - size.x) * 0.5f));
        ImGui::TextDisabled("%s", empty);
        ImGui::EndChild();
        return;
    }

    if (g_heading_font != nullptr) {
        ImGui::PushFont(g_heading_font);
    }
    ImGui::TextUnformatted(selected_client->hostname.c_str());
    if (g_heading_font != nullptr) {
        ImGui::PopFont();
    }
    ImGui::SameLine();
    draw_status_badge(selected_client->status, state);
    ImGui::TextDisabled("%s", selected_client->address.c_str());
    bool managed = selected_client->frozen;
    if (ImGui::Checkbox(
            tr(state, "Managed mode", "Chế độ được quản lý"),
            &managed)) {
        std::string error;
        const bool sent = control_plane.set_frozen(
            selected_client->id, managed, &error);
        state.control_status = sent
            ? (managed
                   ? tr(state, "Managed mode request sent.",
                        "Đã gửi yêu cầu bật chế độ được quản lý.")
                   : tr(state, "Thaw request sent.",
                        "Đã gửi yêu cầu tắt chế độ được quản lý."))
            : error;
        if (!sent) {
            record_operation_failure(
                "ManagedMode", "Managed-mode command failed", error);
        }
        ImGui::OpenPopup("control-status");
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tr(
            state,
            "Blocks service stop and uninstall until disabled here or by a local administrator.",
            "Chặn dừng dịch vụ và gỡ cài đặt cho đến khi tắt tại đây hoặc bởi quản trị viên cục bộ."));
    }
    ImGui::SameLine();
    // Requirement check before activation: if the client has already reported
    // that its Windows edition/feature set cannot support reboot-to-restore,
    // do not let the operator arm it - the attempt would only fail on the box.
    const bool uwf_unsupported =
        selected_client->uwf.reported &&
        selected_client->uwf.phase == nstu::control::UwfFleetPhase::unsupported;
    ImGui::BeginDisabled(uwf_unsupported);
    if (ImGui::Button(tr(state, "Enable reboot-to-restore",
                         "Bật khôi phục sau reboot"))) {
        state.uwf_confirmation_client_id = selected_client->id;
        state.uwf_confirmation_open = true;
        ImGui::OpenPopup("uwf-confirmation");
    }
    ImGui::EndDisabled();
    if (uwf_unsupported &&
        ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", tr(state,
            "This computer does not meet reboot-to-restore requirements "
            "(unsupported Windows edition or missing Unified Write Filter).",
            "Máy này không đáp ứng yêu cầu khôi phục sau reboot (phiên bản "
            "Windows không hỗ trợ hoặc thiếu Unified Write Filter)."));
    }
    if (selected_client->uwf.reported ||
        selected_client->uwf.phase !=
            nstu::control::UwfFleetPhase::idle) {
        const bool proven =
            selected_client->uwf.proves_current_protection();
        ImGui::TextColored(
            proven ? (g_dark_mode ? ImVec4{0.40f, 0.85f, 0.53f, 1.0f}
                                  : ImVec4{0.10f, 0.55f, 0.24f, 1.0f})
                   : (g_dark_mode ? ImVec4{0.96f, 0.76f, 0.34f, 1.0f}
                                  : ImVec4{0.63f, 0.36f, 0.02f, 1.0f}),
            "%s", nstu::server::to_string(selected_client->uwf.phase));
        if (!selected_client->uwf.detail.empty()) {
            ImGui::TextWrapped("%s", selected_client->uwf.detail.c_str());
        }
        if (proven) {
            ImGui::TextColored(
                g_dark_mode ? ImVec4{0.40f, 0.85f, 0.53f, 1.0f}
                            : ImVec4{0.10f, 0.55f, 0.24f, 1.0f},
                "%s",
                tr(state,
                   "Reboot-to-restore protected in this session. Exam mode is "
                   "permitted.",
                   "Đã bảo vệ khôi phục sau reboot trong phiên này. Cho phép "
                   "chế độ thi."));
        }
    }
    if (state.uwf_confirmation_open &&
        ImGui::BeginPopupModal("uwf-confirmation", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("%s", tr(
            state,
            "This runs full client readiness checks, preserves NSTU data and registry state, then arms Unified Write Filter for the next restart. Create or verify a recovery checkpoint before continuing. A successful setup shows a 60-second countdown and restarts the client automatically.",
            "Thao tác này chạy đầy đủ kiểm tra sẵn sàng, giữ lại dữ liệu và registry NSTU, rồi chuẩn bị Unified Write Filter cho lần khởi động tiếp theo. Hãy tạo hoặc xác minh điểm khôi phục trước khi tiếp tục. Khi thiết lập thành công, máy client sẽ hiển thị đếm ngược 60 giây rồi tự khởi động lại."));
        if (ImGui::Button(tr(state, "I verified the checkpoint; enable",
                             "Đã xác minh điểm khôi phục; bật"))) {
            std::string error;
            // Fleet path: arm UWF and request the visible client restart, so a
            // fresh boot-bound probe can prove current-session protection.
            const bool sent = control_plane.configure_uwf_fleet(
                state.uwf_confirmation_client_id, true, true, &error);
            state.control_status = sent
                ? tr(state, "UWF readiness request sent.",
                     "Đã gửi yêu cầu kiểm tra và bật UWF.")
                : error;
            if (!sent) {
                record_operation_failure("UWF", "UWF command failed", error);
            }
            state.uwf_confirmation_open = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(tr(state, "Cancel", "Hủy"))) {
            state.uwf_confirmation_open = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    char latency[32]{};
    char packet_loss[32]{};
    sprintf_s(latency, "%u ms", selected_client->latency_ms);
    sprintf_s(packet_loss, "%.1f%%",
              selected_client->packet_loss_per_mille / 10.0f);
    char refresh[32]{};
    if (selected_client->snapshotting) {
        sprintf_s(refresh, tr(state, "%u seconds", "%u giây"),
                  selected_client->snapshot_interval_seconds);
    } else {
        strcpy_s(refresh, tr(state, "Paused", "Đã dừng"));
    }
    const float telemetry_gap = ImGui::GetStyle().ItemSpacing.x;
    const float telemetry_width =
        (ImGui::GetContentRegionAvail().x - telemetry_gap * 2.0f) / 3.0f;
    draw_telemetry_card(tr(state, "Latency", "Độ trễ"), latency,
                        telemetry_width);
    ImGui::SameLine();
    draw_telemetry_card(tr(state, "Packet loss", "Mất gói"), packet_loss,
                        telemetry_width);
    ImGui::SameLine();
    draw_telemetry_card(tr(state, "Snapshot refresh", "Chu kỳ chụp"),
                        refresh, telemetry_width);

    ImGui::TextUnformatted(tr(state, "Latest screen snapshot",
                              "Ảnh chụp màn hình mới nhất"));
    const bool live_now = selected_client->streaming;
    ImGui::SameLine();
    if (live_now) {
        const ImVec4 live_color = g_dark_mode
                                      ? ImVec4{0.40f, 0.85f, 0.53f, 1.0f}
                                      : ImVec4{0.10f, 0.55f, 0.24f, 1.0f};
        std::string live_label;
        if (state.measured_stream_fps > 0.0) {
            const long long tenths = static_cast<long long>(
                state.measured_stream_fps * 10.0 + 0.5);
            live_label = std::string(tr(state, "· Live ", "· Trực tiếp ")) +
                         std::to_string(tenths / 10) + "." +
                         std::to_string(tenths % 10) + " fps";
        } else {
            live_label =
                tr(state, "· Live (measuring)", "· Trực tiếp (đang đo)");
        }
        ImGui::TextColored(live_color, "%s", live_label.c_str());
    } else {
        ImGui::TextDisabled("%s", tr(state, "· Snapshot", "· Ảnh chụp"));
    }
    const float preview_width = ImGui::GetContentRegionAvail().x;
    const float preview_height = std::clamp(
        preview_width * 0.5625f, 220.0f,
        std::max(220.0f, ImGui::GetContentRegionAvail().y - 210.0f));
    const auto surface = draw_screen_surface(
        *selected_client, preview_height, "##focus-screen", state);
    // Remote input no longer rides on this embedded Focus surface: it lives in a
    // separate top-level window (see open_remote_window). The Focus view is a
    // read-only live preview plus teacher annotation.
    if (state.annotation_enabled && surface.has_frame) {
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const bool inside = mouse.x >= surface.image_min.x &&
                            mouse.x <= surface.image_max.x &&
                            mouse.y >= surface.image_min.y &&
                            mouse.y <= surface.image_max.y;
        const auto clamp_to_surface = [&](ImVec2 point) {
            return ImVec2{
                std::clamp(point.x, surface.image_min.x, surface.image_max.x),
                std::clamp(point.y, surface.image_min.y, surface.image_max.y)};
        };
        const auto normalized = [&](ImVec2 point) {
            const float x_ratio = std::clamp(
                (point.x - surface.image_min.x) /
                    (surface.image_max.x - surface.image_min.x),
                0.0f, 1.0f);
            const float y_ratio = std::clamp(
                (point.y - surface.image_min.y) /
                    (surface.image_max.y - surface.image_min.y),
                0.0f, 1.0f);
            return ImVec2{x_ratio * 65535.0f, y_ratio * 65535.0f};
        };
        const auto send_segment = [&](ImVec2 start, ImVec2 end) {
            const ImVec2 first = normalized(clamp_to_surface(start));
            const ImVec2 last = normalized(clamp_to_surface(end));
            const nstu::control::OverlayStroke stroke{
                .x0 = static_cast<std::uint16_t>(first.x),
                .y0 = static_cast<std::uint16_t>(first.y),
                .x1 = static_cast<std::uint16_t>(last.x),
                .y1 = static_cast<std::uint16_t>(last.y),
                .thickness = static_cast<std::uint16_t>(
                    state.annotation_thickness),
                .rgba = state.annotation_rgba,
            };
            std::string ignored_error;
            (void)control_plane.send_overlay_stroke(
                selected_client->id, stroke, &ignored_error);
        };
        const auto send_erase = [&](ImVec2 start, ImVec2 end) {
            const ImVec2 first = normalized(clamp_to_surface(start));
            const ImVec2 last = normalized(clamp_to_surface(end));
            const nstu::control::OverlayStroke path{
                .x0 = static_cast<std::uint16_t>(first.x),
                .y0 = static_cast<std::uint16_t>(first.y),
                .x1 = static_cast<std::uint16_t>(last.x),
                .y1 = static_cast<std::uint16_t>(last.y),
                .thickness = static_cast<std::uint16_t>(
                    state.annotation_thickness),
                // Colour is ignored for erase; a non-zero alpha only satisfies
                // the shared stroke codec.
                .rgba = 0xffffffffu,
            };
            std::string ignored_error;
            (void)control_plane.send_overlay_erase(
                selected_client->id, path, &ignored_error);
        };
        const auto send_shape = [&](ImVec2 start, ImVec2 end) {
            const float left = std::min(start.x, end.x);
            const float right = std::max(start.x, end.x);
            const float top = std::min(start.y, end.y);
            const float bottom = std::max(start.y, end.y);
            switch (state.annotation_tool) {
            case AnnotationTool::ruler:
                send_segment(start, end);
                break;
            case AnnotationTool::arrow: {
                send_segment(start, end);
                const float dx = end.x - start.x;
                const float dy = end.y - start.y;
                const float length = std::sqrt(dx * dx + dy * dy);
                if (length < 2.0f) {
                    break;
                }
                const float ux = dx / length;
                const float uy = dy / length;
                const float head = std::min(24.0f, length * 0.35f);
                const ImVec2 base{end.x - ux * head, end.y - uy * head};
                const ImVec2 perpendicular{-uy * head * 0.35f,
                                            ux * head * 0.35f};
                send_segment(end, {base.x + perpendicular.x,
                                    base.y + perpendicular.y});
                send_segment(end, {base.x - perpendicular.x,
                                    base.y - perpendicular.y});
                break;
            }
            case AnnotationTool::rectangle:
                send_segment({left, top}, {right, top});
                send_segment({right, top}, {right, bottom});
                send_segment({right, bottom}, {left, bottom});
                send_segment({left, bottom}, {left, top});
                break;
            case AnnotationTool::ellipse: {
                constexpr int segments = 24;
                const ImVec2 center{(left + right) * 0.5f,
                                    (top + bottom) * 0.5f};
                const ImVec2 radius{(right - left) * 0.5f,
                                    (bottom - top) * 0.5f};
                ImVec2 previous{center.x + radius.x, center.y};
                for (int index = 1; index <= segments; ++index) {
                    const float angle =
                        (2.0f * 3.14159265358979323846f * index) /
                        static_cast<float>(segments);
                    const ImVec2 next{center.x + std::cos(angle) * radius.x,
                                      center.y + std::sin(angle) * radius.y};
                    send_segment(previous, next);
                    previous = next;
                }
                break;
            }
            case AnnotationTool::pen:
            case AnnotationTool::eraser:
                break;
            }
        };
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && inside) {
            state.annotation_dragging = true;
            state.previous_annotation_point = mouse;
        }
        const bool freehand = state.annotation_tool == AnnotationTool::pen ||
                              state.annotation_tool == AnnotationTool::eraser;
        if (state.annotation_dragging &&
            ImGui::IsMouseDown(ImGuiMouseButton_Left) && freehand && inside) {
            const float delta_x = mouse.x - state.previous_annotation_point.x;
            const float delta_y = mouse.y - state.previous_annotation_point.y;
            if (delta_x * delta_x + delta_y * delta_y >= 9.0f) {
                if (state.annotation_tool == AnnotationTool::eraser) {
                    send_erase(state.previous_annotation_point, mouse);
                } else {
                    send_segment(state.previous_annotation_point, mouse);
                }
                state.previous_annotation_point = mouse;
            }
        }
        if (state.annotation_dragging &&
            ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            const ImVec2 end = inside ? mouse : state.previous_annotation_point;
            if (state.annotation_tool == AnnotationTool::eraser) {
                if (inside) {
                    send_erase(state.previous_annotation_point, end);
                }
            } else if (state.annotation_tool != AnnotationTool::pen) {
                send_shape(state.previous_annotation_point, end);
            } else if (inside) {
                send_segment(state.previous_annotation_point, end);
            }
            state.annotation_dragging = false;
        }
    } else {
        state.annotation_dragging = false;
    }

    ImGui::PushStyleColor(
        ImGuiCol_Button, g_dark_mode ? ImVec4{0.82f, 0.81f, 0.78f, 1.0f}
                                     : ImVec4{0.12f, 0.12f, 0.11f, 1.0f});
    ImGui::PushStyleColor(
        ImGuiCol_ButtonHovered,
        g_dark_mode ? ImVec4{0.90f, 0.89f, 0.86f, 1.0f}
                    : ImVec4{0.20f, 0.20f, 0.19f, 1.0f});
    ImGui::PushStyleColor(
        ImGuiCol_ButtonActive,
        g_dark_mode ? ImVec4{0.96f, 0.95f, 0.92f, 1.0f}
                    : ImVec4{0.25f, 0.25f, 0.24f, 1.0f});
    ImGui::PushStyleColor(
        ImGuiCol_Text, g_dark_mode ? ImVec4{0.10f, 0.10f, 0.09f, 1.0f}
                                   : ImVec4{1.0f, 1.0f, 1.0f, 1.0f});
    ImGui::BeginDisabled(state.auto_monitor);
    if (ImGui::Button(selected_client->snapshotting
                          ? tr(state, "Stop snapshots", "Dừng chụp")
                          : tr(state, "Start snapshots", "Bắt đầu chụp"))) {
        const bool enabled = !selected_client->snapshotting;
        std::string error;
        if (control_plane.set_snapshots(
                selected_client->id, enabled,
                static_cast<std::uint16_t>(
                    state.snapshot_interval_seconds), &error)) {
            state.control_status = enabled
                ? tr(state, "Snapshot refresh started.",
                     "Đã bắt đầu chụp màn hình.")
                : tr(state, "Snapshot refresh stopped.",
                     "Đã dừng chụp màn hình.");
        } else {
            state.control_status = error;
            record_operation_failure("Snapshots", "Snapshot command failed",
                                     error);
        }
        ImGui::OpenPopup("control-status");
    }
    ImGui::EndDisabled();
    if (state.auto_monitor &&
        ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", tr(state,
            "Automatic monitoring is on. Turn it off in Settings to control "
            "snapshots per computer.",
            "Đang bật theo dõi tự động. Tắt trong Cài đặt để điều khiển "
            "snapshot theo từng máy."));
    }
    ImGui::PopStyleColor(4);
    ImGui::SameLine();
    const bool remote_available = selected_client->status !=
        nstu::server::ClientStatus::offline;
    const bool remote_open_here = g_remote_window != nullptr &&
        g_remote_target_client == selected_client->id;
    if (ImGui::Button(remote_open_here
                          ? tr(state, "Stop remote", "Dừng điều khiển")
                          : tr(state, "Start remote", "Bắt đầu điều khiển"))) {
        if (remote_open_here) {
            close_remote_window(control_plane);
            state.control_status = tr(state, "Remote control window closed.",
                                      "Đã đóng cửa sổ điều khiển từ xa.");
        } else if (remote_available) {
            const bool ok =
                open_remote_window(control_plane, selected_client->id);
            state.control_status = ok
                ? tr(state, "Remote control window opened.",
                     "Đã mở cửa sổ điều khiển từ xa.")
                : tr(state, "Could not open the remote control window.",
                     "Không thể mở cửa sổ điều khiển từ xa.");
        }
        ImGui::OpenPopup("control-status");
    }
    // Keyboard entry and the offline auto-stop now belong to the separate
    // remote-control window: it forwards real key events and the main loop
    // closes it when the controlled client drops offline.
    ImGui::SameLine();
    if (ImGui::Button(state.annotation_enabled
                          ? tr(state, "Finish drawing", "Kết thúc vẽ")
                          : tr(state, "Draw on screen", "Vẽ lên màn hình"))) {
        state.annotation_enabled = !state.annotation_enabled;
        state.annotation_dragging = false;
    }
    ImGui::SameLine();
    if (ImGui::Button(tr(state, "Clear drawing", "Xóa nét vẽ"))) {
        std::string error;
        const bool cleared =
            control_plane.clear_overlay(selected_client->id, &error);
        state.control_status = cleared
            ? tr(state, "Student overlay cleared.",
                 "Đã xóa lớp vẽ trên máy học sinh.")
            : error;
        if (!cleared) {
            record_operation_failure("Overlay", "Clear command failed", error);
        }
        ImGui::OpenPopup("control-status");
    }
    ImGui::SameLine();
    ImGui::PushStyleColor(
        ImGuiCol_Button, g_dark_mode ? ImVec4{0.27f, 0.13f, 0.13f, 1.0f}
                                     : ImVec4{0.99f, 0.92f, 0.92f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
        g_dark_mode ? ImVec4{0.36f, 0.17f, 0.17f, 1.0f}
                    : ImVec4{0.97f, 0.86f, 0.86f, 1.0f});
    ImGui::PushStyleColor(
        ImGuiCol_Text, g_dark_mode ? ImVec4{0.94f, 0.56f, 0.55f, 1.0f}
                                   : ImVec4{0.62f, 0.18f, 0.17f, 1.0f});
    const bool is_locked =
        selected_client->status == nstu::server::ClientStatus::locked;
    if (ImGui::Button(is_locked
                           ? tr(state, "Unlock client", "Mở khóa máy")
                           : tr(state, "Lock client", "Khóa máy"))) {
        std::string error;
        const bool sent =
            control_plane.set_locked(selected_client->id, !is_locked, &error);
        state.control_status = sent
            ? (is_locked
                   ? tr(state, "Unlock command sent.",
                        "Đã gửi lệnh mở khóa.")
                   : tr(state, "Lock command sent.",
                        "Đã gửi lệnh khóa."))
            : error;
        if (!sent) {
            record_operation_failure("Lock", "Lock-state command failed",
                                     error);
        }
        ImGui::OpenPopup("control-status");
    }
    ImGui::PopStyleColor(3);
    if (state.annotation_enabled) {
        ImGui::TextUnformatted(tr(state, "Overlay tools", "Công cụ lớp phủ"));
        ImGui::SameLine();
        const char* tool_items =
            state.language == Language::vietnamese
                ? "Bút\0Thước\0Mũi tên\0Hình chữ nhật\0Elip\0Tẩy\0"
                : "Pen\0Ruler\0Arrow\0Rectangle\0Ellipse\0Eraser\0";
        int tool_index = static_cast<int>(state.annotation_tool);
        ImGui::SetNextItemWidth(150.0f);
        if (ImGui::Combo(tr(state, "Tool", "Công cụ"), &tool_index,
                         tool_items)) {
            state.annotation_tool = static_cast<AnnotationTool>(
                std::clamp(tool_index, 0,
                           static_cast<int>(AnnotationTool::eraser)));
            state.annotation_dragging = false;
        }
        ImGui::SameLine();
        float custom_color[4]{
            ((state.annotation_rgba >> 24u) & 0xffu) / 255.0f,
            ((state.annotation_rgba >> 16u) & 0xffu) / 255.0f,
            ((state.annotation_rgba >> 8u) & 0xffu) / 255.0f,
            (state.annotation_rgba & 0xffu) / 255.0f};
        if (ImGui::ColorEdit4(tr(state, "Color", "Màu"), custom_color,
                              ImGuiColorEditFlags_NoInputs |
                                  ImGuiColorEditFlags_AlphaBar)) {
            const auto channel = [](float value) {
                return static_cast<std::uint32_t>(
                    std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
            };
            state.annotation_rgba = (channel(custom_color[0]) << 24u) |
                                    (channel(custom_color[1]) << 16u) |
                                    (channel(custom_color[2]) << 8u) |
                                    channel(custom_color[3]);
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(tr(state, "Quick colors", "Màu nhanh"));
        ImGui::SameLine();
        constexpr std::array<std::uint32_t, 4> colors{
            0xe5484dffu, 0xf2c94cffu, 0x2f80edffu, 0x27ae60ffu};
        for (std::size_t index = 0; index < colors.size(); ++index) {
            const auto color = colors[index];
            const ImVec4 display{
                ((color >> 24u) & 0xffu) / 255.0f,
                ((color >> 16u) & 0xffu) / 255.0f,
                ((color >> 8u) & 0xffu) / 255.0f, 1.0f};
            ImGui::PushID(static_cast<int>(index));
            if (ImGui::ColorButton("##pen-color", display,
                                   ImGuiColorEditFlags_NoTooltip,
                                   {24.0f, 24.0f})) {
                state.annotation_rgba = color;
            }
            ImGui::PopID();
            if (index + 1 != colors.size()) {
                ImGui::SameLine();
            }
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(140.0f);
        ImGui::SliderInt(tr(state, "Thickness", "Độ dày"),
                         &state.annotation_thickness, 2, 12);
    }
    if (ImGui::BeginPopup("control-status")) {
        ImGui::TextUnformatted(state.control_status.c_str());
        ImGui::EndPopup();
    }

    if (ImGui::BeginChild("chat-panel", {0, 112.0f}, true)) {
        ImGui::TextUnformatted(tr(state, "Chat", "Trò chuyện"));
        const auto chat_log = control_plane.chat_history(selected_client->id);
        if (ImGui::BeginChild("chat-log", {0, 52.0f}, true)) {
            if (chat_log.empty()) {
                ImGui::TextDisabled(
                    "%s", tr(state, "No messages in this session.",
                             "Chưa có tin nhắn trong phiên này."));
            } else {
                for (const auto& entry : chat_log) {
                    const char* who =
                        entry.from_teacher
                            ? tr(state, "Teacher", "Giáo viên")
                            : selected_client->hostname.c_str();
                    ImGui::TextWrapped("%s: %s", who, entry.text.c_str());
                }
                if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f) {
                    ImGui::SetScrollHereY(1.0f);
                }
            }
        }
        ImGui::EndChild();
        ImGui::SetNextItemWidth(-78.0f);
        const bool submit = ImGui::InputText(
            "##chat-input", state.chat_input.data(), state.chat_input.size(),
            ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if ((submit || ImGui::Button(tr(state, "Send", "Gửi"),
                                     {68.0f, 0.0f})) &&
            state.chat_input[0] != '\0') {
            std::string error;
            if (control_plane.send_chat(selected_client->id,
                                        state.chat_input.data(), &error)) {
                state.chat_input[0] = '\0';
                state.control_status = tr(state, "Message sent.",
                                          "Đã gửi tin nhắn.");
            } else {
                state.control_status = error;
                record_operation_failure("Chat", "Chat command failed", error);
            }
            ImGui::OpenPopup("control-status");
        }
    }
    ImGui::EndChild();
    ImGui::EndChild();
}

bool draw_segment_option(const char* label, bool selected, float width) {
    if (selected) {
        ImGui::PushStyleColor(
            ImGuiCol_Button,
            g_dark_mode ? ImVec4{0.82f, 0.81f, 0.78f, 1.0f}
                        : ImVec4{0.12f, 0.12f, 0.11f, 1.0f});
        ImGui::PushStyleColor(
            ImGuiCol_Text,
            g_dark_mode ? ImVec4{0.10f, 0.10f, 0.09f, 1.0f}
                        : ImVec4{1.0f, 1.0f, 1.0f, 1.0f});
    }
    const bool pressed = ImGui::Button(label, {width, 28.0f});
    if (selected) {
        ImGui::PopStyleColor(2);
    }
    return pressed;
}

void draw_preferences(DashboardState& state) {
    constexpr float settings_width = 108.0f;
    ImGui::SameLine(ImGui::GetContentRegionMax().x - settings_width);
    if (ImGui::Button(tr(state, "Settings", "Cài đặt"),
                      {settings_width, 28.0f})) {
        ImGui::OpenPopup("settings-popup");
    }
    ImGui::SetNextWindowSize({390.0f, 0.0f}, ImGuiCond_Appearing);
    if (!ImGui::BeginPopup("settings-popup")) {
        return;
    }
    ImGui::TextUnformatted(tr(state, "Settings", "Cài đặt"));
    ImGui::Separator();
    ImGui::TextDisabled("%s", tr(state, "Language", "Ngôn ngữ"));
    if (draw_segment_option("EN", state.language == Language::english,
                            42.0f)) {
        state.language = Language::english;
        g_language = state.language;
        state.control_status.clear();
    }
    ImGui::SameLine();
    if (draw_segment_option("VI", state.language == Language::vietnamese,
                            42.0f)) {
        state.language = Language::vietnamese;
        g_language = state.language;
        state.control_status.clear();
    }
    ImGui::Spacing();
    ImGui::TextDisabled("%s", tr(state, "Appearance", "Giao diện"));
    if (draw_segment_option(tr(state, "Light", "Sáng"), !state.dark_mode,
                            68.0f)) {
        state.dark_mode = false;
        apply_dashboard_style(false);
    }
    ImGui::SameLine();
    if (draw_segment_option(tr(state, "Dark", "Tối"), state.dark_mode,
                            68.0f)) {
        state.dark_mode = true;
        apply_dashboard_style(true);
    }
    ImGui::Spacing();
    ImGui::TextDisabled("%s", tr(state, "Screen refresh", "Chu kỳ làm mới"));
    ImGui::SetNextItemWidth(184.0f);
    ImGui::SliderInt("##settings-refresh", &state.snapshot_interval_seconds,
                     kMinimumSnapshotInterval, kMaximumSnapshotInterval,
                     tr(state, "%d seconds", "%d giây"));
    ImGui::TextWrapped("%s", tr(state,
        "Snapshot commands use this interval for room monitoring and teacher broadcast.",
        "Chu kỳ này được dùng cho snapshot phòng máy và phát màn hình giáo viên."));
    ImGui::Checkbox(
        tr(state, "Monitor every computer automatically",
           "Tự động theo dõi mọi máy"),
        &state.auto_monitor);
    ImGui::TextWrapped("%s", tr(state,
        "When on, every online computer streams snapshots continuously; turn it off to start and stop snapshots per computer.",
        "Khi bật, mọi máy trực tuyến sẽ gửi snapshot liên tục; tắt để bật/tắt snapshot theo từng máy."));
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextDisabled("%s", tr(state, "Optional diagnostics",
                                  "Chẩn đoán tùy chọn"));
    bool collection_enabled = g_telemetry_policy.collect_in_background;
    if (ImGui::Checkbox(
            tr(state, "Collect privacy-filtered diagnostics locally",
               "Thu thập chẩn đoán đã lọc riêng tư trên máy"),
            &collection_enabled)) {
        auto policy = g_telemetry_policy;
        policy.collect_in_background = collection_enabled;
        (void)update_telemetry_policy(state, policy);
    }
    ImGui::BeginDisabled(!g_telemetry_policy.collect_in_background);
    bool prompt_on_error = g_telemetry_policy.prompt_on_error;
    if (ImGui::Checkbox(
            tr(state, "Prompt when an error report is ready",
               "Nhắc khi báo cáo lỗi đã sẵn sàng"),
            &prompt_on_error)) {
        auto policy = g_telemetry_policy;
        policy.prompt_on_error = prompt_on_error;
        (void)update_telemetry_policy(state, policy);
    }
    ImGui::EndDisabled();
    ImGui::TextWrapped("%s", tr(
        state,
        "Disabled by default. Events stay in memory and are discarded when NSTU exits. Nothing is uploaded automatically; review the report before posting it to the public GitHub issue tracker.",
        "Mặc định tắt. Sự kiện chỉ nằm trong bộ nhớ và bị xóa khi NSTU thoát. Không có dữ liệu nào tự động tải lên; hãy xem lại báo cáo trước khi đăng lên GitHub Issues công khai."));
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextDisabled("%s", tr(state, "About", "Giới thiệu"));
    ImGui::Text("NSTU %s", NSTU_PROJECT_VERSION);
    ImGui::TextWrapped("%s", tr(
        state,
        "Open-source classroom management under the MIT License. Developed with AI assistance; changes remain human-reviewed and publicly auditable.",
        "Phần mềm quản lý lớp học mã nguồn mở theo giấy phép MIT. Được phát triển với hỗ trợ AI; các thay đổi vẫn được con người rà soát và có thể kiểm tra công khai."));
    if (g_telemetry_policy.collect_in_background) {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextDisabled("%s", tr(state, "Diagnostic data",
                                      "Dữ liệu chẩn đoán"));
        ImGui::Text("%s: %llu / %llu",
                    tr(state, "Collected events", "Sự kiện đã thu thập"),
                    static_cast<unsigned long long>(g_telemetry_events.size()),
                    static_cast<unsigned long long>(
                        nstu::telemetry::kMaximumEvents));
        if (ImGui::Button(tr(state, "Review report", "Xem báo cáo"),
                          {116.0f, 0.0f})) {
            state.telemetry_report_requested = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(tr(state, "Clear data", "Xóa dữ liệu"),
                          {104.0f, 0.0f})) {
            g_telemetry_events.clear();
            g_telemetry_error_prompt_requested.store(false);
            g_telemetry_error_prompt_armed.store(
                g_telemetry_policy.prompt_on_error);
            state.telemetry_report_preview.clear();
            state.control_status = tr(state, "Collected diagnostics cleared.",
                                      "Đã xóa dữ liệu chẩn đoán.");
        }
    }
    ImGui::EndPopup();
}

void set_room_lock(const std::vector<nstu::server::ClientRecord>& clients,
                   DashboardState& state,
                   nstu::server::ServerControlPlane& control_plane,
                   bool locked) {
    std::size_t sent = 0;
    for (const auto& client : clients) {
        if (client.status != nstu::server::ClientStatus::offline &&
            control_plane.set_locked(client.id, locked, nullptr)) {
            ++sent;
        }
    }
    state.control_status = sent == 0
        ? tr(state, "No online clients are available.",
             "Không có máy trực tuyến để điều khiển.")
        : (locked ? tr(state, "Room lock command sent.",
                       "Đã gửi lệnh khóa toàn phòng.")
                  : tr(state, "Room unlock command sent.",
                       "Đã gửi lệnh mở khóa toàn phòng."));
}

void toggle_teacher_broadcast(DashboardState& state,
                              nstu::server::ServerControlPlane& control_plane) {
    if (state.broadcast_enabled) {
        std::string error;
        if (control_plane.stop_host_broadcast(&error)) {
            state.broadcast_enabled = false;
            state.control_status = tr(state, "Teacher broadcast stopped.",
                                      "Đã dừng phát màn hình giáo viên.");
        } else {
            state.control_status = error;
            record_operation_failure("Broadcast", "Stop command failed",
                                     error);
        }
        return;
    }
    state.broadcast_enabled = true;
    state.next_host_snapshot = {};
    state.control_status = tr(state, "Teacher broadcast started.",
                              "Đã bắt đầu phát màn hình giáo viên.");
}

void draw_diagnostics_popup(DashboardState& state) {
    constexpr float button_width = 118.0f;
    ImGui::SameLine(ImGui::GetContentRegionMax().x - button_width - 116.0f);
    if (ImGui::Button(tr(state, "Diagnostics", "Chẩn đoán"),
                      {button_width, 28.0f})) {
        refresh_graphics_report();
        ImGui::OpenPopup("diagnostics-popup");
    }
    if (!ImGui::BeginPopupModal("diagnostics-popup", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    ImGui::TextUnformatted(tr(state, "Graphics diagnostics",
                              "Chẩn đoán đồ họa"));
    ImGui::Separator();
    ImGui::Text("Device: %s", g_graphics_report.device_mode.empty()
        ? "Not initialized" : g_graphics_report.device_mode.c_str());
    ImGui::Text("Adapter: %s (%s)",
                g_graphics_report.selected_adapter.empty()
                    ? "Unknown" : g_graphics_report.selected_adapter.c_str(),
                g_graphics_report.selected_vendor.empty()
                    ? "Unknown" : g_graphics_report.selected_vendor.c_str());
    ImGui::Text("Feature level: %s",
                g_graphics_report.feature_level.empty()
                    ? "Unknown" : g_graphics_report.feature_level.c_str());
    ImGui::Text("Desktop Duplication: %s",
                g_graphics_report.desktop_duplication.empty()
                    ? "Unknown" : g_graphics_report.desktop_duplication.c_str());
    ImGui::Text("Hardware H.264 encoders: %s",
                g_graphics_report.h264_encoders.empty()
                    ? "Unknown" : g_graphics_report.h264_encoders.c_str());
    ImGui::Spacing();
    ImGui::TextUnformatted(tr(state, "Adapters", "Bộ điều hợp"));
    if (ImGui::BeginChild("diagnostic-adapters", {620.0f, 80.0f}, true)) {
        for (const auto& adapter : g_graphics_report.adapters) {
            ImGui::BulletText("%s", adapter.c_str());
        }
    }
    ImGui::EndChild();
    ImGui::Spacing();
    ImGui::TextUnformatted(tr(state, "Recent events", "Sự kiện gần đây"));
    if (ImGui::BeginChild("diagnostic-events", {620.0f, 180.0f}, true)) {
        for (const auto& event : g_diagnostics) {
            ImGui::TextWrapped("[%s] [%s] %s: %s", event.timestamp.c_str(),
                               event.severity.c_str(), event.source.c_str(),
                               event.message.c_str());
        }
    }
    ImGui::EndChild();
    if (ImGui::Button(tr(state, "Refresh", "Làm mới"), {90.0f, 0.0f})) {
        refresh_graphics_report();
    }
    if (g_telemetry_policy.collect_in_background) {
        ImGui::SameLine();
        if (ImGui::Button(tr(state, "Review report", "Xem báo cáo"),
                          {118.0f, 0.0f})) {
            state.telemetry_report_requested = true;
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(tr(state, "Close", "Đóng"), {90.0f, 0.0f})) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void draw_telemetry_report_popup(DashboardState& state,
                                 std::size_t client_count) {
    if (g_telemetry_error_prompt_requested.exchange(false)) {
        state.telemetry_report_requested = true;
    }
    if (state.telemetry_report_requested) {
        state.telemetry_report_requested = false;
        refresh_graphics_report();
        g_telemetry_error_prompt_requested.store(false);
        state.telemetry_report_preview = nstu::telemetry::build_public_markdown(
            make_telemetry_report(state, client_count));
        ImGui::OpenPopup("sanitized-diagnostic-report");
    }

    ImGui::SetNextWindowSize({760.0f, 600.0f}, ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("sanitized-diagnostic-report", nullptr,
                                ImGuiWindowFlags_NoResize)) {
        return;
    }
    ImGui::TextUnformatted(tr(state, "Review diagnostic report",
                              "Xem lại báo cáo chẩn đoán"));
    ImGui::Separator();
    ImGui::TextWrapped("%s", tr(
        state,
        "Nothing has been sent. GitHub Issues are public. Review the complete text and remove anything you do not want to disclose before submitting.",
        "Chưa có dữ liệu nào được gửi. GitHub Issues là công khai. Hãy xem toàn bộ nội dung và xóa mọi thông tin bạn không muốn công bố trước khi gửi."));
    ImGui::Spacing();
    if (ImGui::BeginChild("sanitized-report-preview", {0.0f, -48.0f}, true,
                          ImGuiWindowFlags_HorizontalScrollbar)) {
        ImGui::TextUnformatted(state.telemetry_report_preview.c_str());
    }
    ImGui::EndChild();

    if (ImGui::Button(tr(state, "Copy report", "Sao chép báo cáo"),
                      {126.0f, 0.0f})) {
        ImGui::SetClipboardText(state.telemetry_report_preview.c_str());
        state.control_status = tr(state, "Sanitized report copied.",
                                  "Đã sao chép báo cáo đã lọc.");
    }
    ImGui::SameLine();
    if (ImGui::Button(tr(state, "Copy and open GitHub",
                               "Sao chép và mở GitHub"),
                      {178.0f, 0.0f})) {
        ImGui::SetClipboardText(state.telemetry_report_preview.c_str());
        const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(
            nullptr, L"open", kDiagnosticIssueUrl, nullptr, nullptr,
            SW_SHOWNORMAL));
        state.control_status = result > 32
            ? tr(state, "Report copied; GitHub issue form opened.",
                 "Đã sao chép báo cáo và mở biểu mẫu GitHub Issue.")
            : tr(state, "Report copied, but GitHub could not be opened.",
                 "Đã sao chép báo cáo nhưng không thể mở GitHub.");
    }
    ImGui::SameLine();
    if (ImGui::Button(tr(state, "Clear", "Xóa"), {82.0f, 0.0f})) {
        g_telemetry_events.clear();
        g_telemetry_error_prompt_requested.store(false);
        g_telemetry_error_prompt_armed.store(
            g_telemetry_policy.prompt_on_error);
        state.telemetry_report_preview = nstu::telemetry::build_public_markdown(
            make_telemetry_report(state, client_count));
        state.control_status = tr(state, "Collected diagnostics cleared.",
                                  "Đã xóa dữ liệu chẩn đoán.");
    }
    ImGui::SameLine();
    if (ImGui::Button(tr(state, "Close", "Đóng"), {82.0f, 0.0f})) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// The computer name is the only label a teacher reliably recognises in a
// selection menu. The beacon sanitizes it again before it reaches the wire.
std::string local_server_name() {
    char name[MAX_COMPUTERNAME_LENGTH + 1]{};
    DWORD length = MAX_COMPUTERNAME_LENGTH + 1;
    if (GetComputerNameA(name, &length) && length > 0) {
        return std::string(name, length);
    }
    return "NSTU";
}

// The room name is a persisted display hint, not a secret, so it lives in a
// small plaintext file under the data root rather than the protected keyring. A
// missing or unreadable file means "no room configured" and the beacon falls
// back to the computer name. Only the first line is the label; trailing CR (a
// CRLF file) and surrounding spaces are trimmed so a hand-edited file still
// round-trips. The control plane sanitizes the value to the discovery bound
// before it reaches the wire, so a corrupt file can only mangle the label,
// never the pairing that the six-digit code secures.
std::string load_room_name(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }
    std::string line;
    std::getline(file, line);
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    const auto first = line.find_first_not_of(" \t");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = line.find_last_not_of(" \t");
    return line.substr(first, last - first + 1);
}

// Persists the room label as plaintext. Best-effort: a write failure only means
// the label will not survive a restart, so the caller surfaces it as a benign
// status rather than a hard error. The value is written verbatim; the control
// plane has already sanitized it to the discovery bound.
bool save_room_name(const std::filesystem::path& path, std::string_view name) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        return false;
    }
    file << name;
    return static_cast<bool>(file);
}

// Six digits compared out loud across a room are easier to read in two groups.
std::string grouped_code(const std::string& code) {
    return code.size() == 6 ? code.substr(0, 3) + " " + code.substr(3) : code;
}

// The room-name field lets the operator label this server so a student machine
// on a shared VLAN can target the right classroom by name. Editing it pushes
// each keystroke to the live beacon (a client sweeping right now sees the new
// label) and, once editing settles, persists the plaintext hint and reflects
// the sanitized value back so the field matches what is advertised and saved.
// The six-digit SAS the operator still compares is unaffected: the room name
// only routes, it never authorizes.
void draw_room_name_field(DashboardState& state,
                          nstu::server::ServerControlPlane& control_plane) {
    ImGui::TextUnformatted(tr(state,
        "Room name (optional label shown to computers)",
        "Tên phòng (nhãn tùy chọn hiển thị cho máy)"));
    ImGui::SetNextItemWidth(360.0f);
    if (ImGui::InputText("##room-name", state.room_name_input.data(),
                         state.room_name_input.size())) {
        control_plane.set_server_name(state.room_name_input.data());
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        const std::string sanitized = control_plane.server_name();
        std::snprintf(state.room_name_input.data(),
                      state.room_name_input.size(), "%s", sanitized.c_str());
        if (!state.room_name_path.empty() &&
            !save_room_name(state.room_name_path, sanitized)) {
            record_operation_failure("Pairing",
                                     "Room name could not be saved", "");
        }
    }
    ImGui::TextDisabled("%s", tr(state,
        "Leave blank to use the computer name.",
        "Để trống để dùng tên máy tính."));
}

// The operator's half of verified pairing. By the time a row appears here the
// key exchange is done and the client has already proved it derived the same
// secret, so what is left is the one thing arithmetic cannot settle: whether
// the machine on the other end of that exchange is the machine the teacher is
// standing next to. Comparing the six digits answers it, and nothing turns
// into a key without it.
void draw_pairing_requests(DashboardState& state,
                           nstu::server::ServerControlPlane& control_plane) {
    ImGui::TextUnformatted(tr(state,
        "New computers can find this server while this window is open.",
        "Máy mới có thể tìm thấy máy "
        "chủ này khi cửa sổ này đang mở."));
    ImGui::TextDisabled("%s", tr(state,
        "Approve only if the code matches the one shown on that computer.",
        "Chỉ duyệt khi mã trùng với mã "
        "hiển thị trên máy đó."));
    ImGui::Separator();
    const auto pending = control_plane.pending_pairings();
    if (pending.empty()) {
        ImGui::Dummy({656.0f, 8.0f});
        ImGui::TextDisabled("%s", tr(state, "Waiting for computers...",
                                     "Đang chờ máy..."));
        ImGui::Dummy({656.0f, 8.0f});
    } else if (ImGui::BeginTable("pairing-requests", 4,
                                 ImGuiTableFlags_RowBg |
                                     ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn(tr(state, "Computer", "Máy"),
                                ImGuiTableColumnFlags_WidthFixed, 300.0f);
        ImGui::TableSetupColumn(tr(state, "Code", "Mã"),
                                ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn(tr(state, "Time left", "Còn lại"),
                                ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("##pairing-actions",
                                ImGuiTableColumnFlags_WidthFixed, 176.0f);
        ImGui::TableHeadersRow();
        for (const auto& request : pending) {
            push_client_id(request.pairing_id);
            ImGui::TableNextRow(0, 46.0f);
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(request.hostname.c_str());
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", request.address.c_str());
            }
            ImGui::TextDisabled("%s", request.client_uuid.c_str());
            ImGui::TableSetColumnIndex(1);
            if (g_heading_font != nullptr) {
                ImGui::PushFont(g_heading_font);
            }
            ImGui::TextUnformatted(
                grouped_code(request.short_authentication_string).c_str());
            if (g_heading_font != nullptr) {
                ImGui::PopFont();
            }
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%u s",
                        static_cast<unsigned>(request.seconds_remaining));
            ImGui::TableSetColumnIndex(3);
            std::string error;
            if (ImGui::Button(tr(state, "Approve", "Duyệt"),
                              {84.0f, 30.0f})) {
                if (control_plane.approve_pairing(request.pairing_id,
                                                  &error)) {
                    state.pairing_status = request.hostname + " " +
                        tr(state, "is now enrolled.",
                           "đã được ghép "
                           "nối.");
                } else {
                    record_operation_failure("Pairing",
                                             "Approve request failed", error);
                    state.pairing_status = error;
                }
            }
            ImGui::SameLine();
            if (ImGui::Button(tr(state, "Reject", "Từ chối"),
                              {84.0f, 30.0f})) {
                if (control_plane.reject_pairing(request.pairing_id, &error)) {
                    state.pairing_status = request.hostname + " " +
                        tr(state, "was turned away.",
                           "đã bị từ chối.");
                } else {
                    record_operation_failure("Pairing",
                                             "Reject request failed", error);
                    state.pairing_status = error;
                }
            }
            pop_client_id();
        }
        ImGui::EndTable();
    }
    if (!state.pairing_status.empty()) {
        ImGui::Separator();
        ImGui::TextWrapped("%s", state.pairing_status.c_str());
    }
}

void draw_pairing_popup(DashboardState& state,
                        nstu::server::ServerControlPlane& control_plane) {
    constexpr float button_width = 140.0f;
    ImGui::SameLine(ImGui::GetContentRegionMax().x - button_width - 242.0f);
    bool focus_panel = false;
    if (ImGui::Button(
            tr(state,
               state.pairing_panel_open ? "Pairing active" : "Add computers",
               state.pairing_panel_open ? "Đang ghép nối" : "Thêm máy"),
            {button_width, 28.0f})) {
        if (!state.pairing_panel_open) {
            state.pairing_status.clear();
            state.pairing_panel_open = true;
        }
        focus_panel = true;
    }

    if (state.pairing_panel_open && !control_plane.pairing_window_open()) {
        // Seed the editable field from the live room label so it shows what the
        // beacon is currently advertising (empty means "using the computer
        // name"). The configured name already wins inside set_pairing_window,
        // so the computer name here is only the fallback.
        const std::string current_room = control_plane.server_name();
        std::snprintf(state.room_name_input.data(),
                      state.room_name_input.size(), "%s",
                      current_room.c_str());
        control_plane.set_pairing_window(true, local_server_name());
    }

    if (state.pairing_panel_open) {
        ImGui::SetNextWindowSize({720.0f, 0.0f}, ImGuiCond_FirstUseEver);
        if (focus_panel) {
            ImGui::SetNextWindowFocus();
        }
        if (ImGui::Begin(
                tr(state, "Add computers###pairing-window",
                   "Thêm máy###pairing-window"),
                &state.pairing_panel_open,
                ImGuiWindowFlags_AlwaysAutoResize)) {
            const auto discovery = control_plane.pairing_discovery_stats();
            ImGui::TextColored(
                discovery.beacon_enabled
                    ? (g_dark_mode ? ImVec4{0.40f, 0.85f, 0.53f, 1.0f}
                                       : ImVec4{0.10f, 0.55f, 0.24f, 1.0f})
                    : (g_dark_mode ? ImVec4{0.96f, 0.76f, 0.34f, 1.0f}
                                       : ImVec4{0.63f, 0.36f, 0.02f, 1.0f}),
                "%s", tr(state,
                           discovery.beacon_enabled
                               ? "Discovery active - listening for new computers"
                               : "Discovery inactive - set a room name",
                           discovery.beacon_enabled
                               ? "Đang tìm kiếm - chờ máy mới"
                               : "Chưa tìm kiếm - hãy đặt tên phòng"));
            ImGui::TextDisabled(
                "%s: %llu    %s: %llu",
                tr(state, "Probes received", "Tín hiệu đã nhận"),
                static_cast<unsigned long long>(discovery.probes_received),
                tr(state, "Replies sent", "Phản hồi đã gửi"),
                static_cast<unsigned long long>(discovery.beacons_sent));
            ImGui::TextDisabled("%s", tr(
                state,
                "Clients broadcast about every 10 seconds. Keep this window open.",
                "Máy khách phát tín hiệu khoảng mỗi 10 giây. Hãy giữ cửa sổ này mở."));
            draw_room_name_field(state, control_plane);
            ImGui::Separator();
            draw_pairing_requests(state, control_plane);
            ImGui::Separator();
            if (ImGui::Button(tr(state, "Done", "Xong"),
                              {110.0f, 30.0f})) {
                state.pairing_panel_open = false;
            }
        }
        ImGui::End();
    }

    // The beacon must not outlive the visible panel. Closing it explicitly
    // stops discovery and refuses unanswered requests.
    if (!state.pairing_panel_open && control_plane.pairing_window_open()) {
        control_plane.set_pairing_window(false);
    }
}

void draw_menu_strip(DashboardState& state, bool has_clients,
                     nstu::server::ServerControlPlane& control_plane) {
    if (!ImGui::BeginChild("menu-strip", {0, 31.0f}, false,
                           ImGuiWindowFlags_NoScrollbar)) {
        ImGui::EndChild();
        return;
    }
    if (g_heading_font != nullptr) {
        ImGui::PushFont(g_heading_font);
    }
    ImGui::TextUnformatted("NSTU School");
    if (g_heading_font != nullptr) {
        ImGui::PopFont();
    }
#if NSTU_DEV_UNPROTECTED_EXAM
    // Permanent, unmissable marker for the public DEV channel. This build starts
    // exams without proven reboot-to-restore protection; the operator must never
    // mistake it for a Release install.
    ImGui::SameLine();
    ImGui::TextColored(ImVec4{0.96f, 0.36f, 0.36f, 1.0f}, "%s",
                       tr(state, "DEV (UNPROTECTED) - exams run without UWF",
                          "DEV (KHÔNG BẢO VỆ) - thi không cần UWF"));
#endif
    ImGui::SameLine();
    if (draw_segment_option(tr(state, "Class", "Lớp"),
                            state.view == DashboardView::room_screens, 58.0f)) {
        state.view = DashboardView::room_screens;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!has_clients);
    if (draw_segment_option(tr(state, "Client", "Máy"),
                            state.view == DashboardView::selected_client,
                            60.0f)) {
        state.view = DashboardView::selected_client;
    }
    ImGui::EndDisabled();
    draw_pairing_popup(state, control_plane);
    draw_diagnostics_popup(state);
    draw_preferences(state);
    ImGui::EndChild();
}

void draw_ribbon_group_caption(float width, const char* label) {
    const ImVec2 text_size = ImGui::CalcTextSize(label);
    ImGui::SetCursorPosY(58.0f);
    ImGui::SetCursorPosX(std::max(
        ImGui::GetStyle().WindowPadding.x, (width - text_size.x) * 0.5f));
    ImGui::TextDisabled("%s", label);
}

void draw_ribbon_divider() {
    const ImVec2 start = ImGui::GetCursorScreenPos();
    ImGui::Dummy({1.0f, 64.0f});
    ImGui::GetWindowDrawList()->AddLine(
        {start.x, start.y + 3.0f}, {start.x, start.y + 61.0f},
        ImGui::GetColorU32(ImGuiCol_Separator));
}

void draw_ribbon(const std::vector<nstu::server::ClientRecord>& clients,
                 const nstu::server::ClientRecord* selected_client,
                 DashboardState& state,
                 nstu::server::ServerControlPlane& control_plane) {
    if (!ImGui::BeginChild("command-ribbon", {0, 82.0f}, true,
                           ImGuiWindowFlags_NoScrollbar)) {
        ImGui::EndChild();
        return;
    }
    const bool has_clients = !clients.empty();
    constexpr float student_width = 140.0f;
    if (ImGui::BeginChild("student-commands", {student_width, 74.0f}, false,
                          ImGuiWindowFlags_NoScrollbar)) {
        // Snapshots run automatically for the class view (auto_monitor), so the
        // manual Start/Stop capture buttons no longer live on the ribbon.
        if (draw_icon_button("lock-room", tr(state, "Lock", "Khóa"),
                             IconKind::lock, {58.0f, 54.0f}, false,
                             has_clients)) {
            set_room_lock(clients, state, control_plane, true);
        }
        ImGui::SameLine();
        if (draw_icon_button("unlock-room", tr(state, "Unlock", "Mở khóa"),
                             IconKind::unlock, {64.0f, 54.0f}, false,
                             has_clients)) {
            set_room_lock(clients, state, control_plane, false);
        }
        draw_ribbon_group_caption(student_width,
                                  tr(state, "Student", "Học sinh"));
    }
    ImGui::EndChild();

    ImGui::SameLine();
    draw_ribbon_divider();
    ImGui::SameLine();
    constexpr float teaching_width = 330.0f;
    if (ImGui::BeginChild("teaching-commands", {teaching_width, 74.0f}, false,
                          ImGuiWindowFlags_NoScrollbar)) {
        if (draw_icon_button("show-room", tr(state, "Screens", "Màn hình"),
                             IconKind::grid, {66.0f, 54.0f},
                             state.view == DashboardView::room_screens)) {
            state.view = DashboardView::room_screens;
        }
        ImGui::SameLine();
        if (draw_icon_button("focus-client", tr(state, "Focus", "Tập trung"),
                             IconKind::monitor, {66.0f, 54.0f},
                             state.view == DashboardView::selected_client,
                             selected_client != nullptr)) {
            state.view = DashboardView::selected_client;
        }
        ImGui::SameLine();
        if (draw_icon_button("draw-client", tr(state, "Draw", "Vẽ"),
                             IconKind::pen, {58.0f, 54.0f},
                             state.annotation_enabled,
                             selected_client != nullptr &&
                                 state.view ==
                                     DashboardView::selected_client)) {
            state.annotation_enabled = !state.annotation_enabled;
            state.annotation_dragging = false;
        }
        ImGui::SameLine();
        if (draw_icon_button("clear-client", tr(state, "Clear", "Xóa"),
                             IconKind::erase, {58.0f, 54.0f}, false,
                             selected_client != nullptr)) {
            std::string error;
            state.control_status = control_plane.clear_overlay(
                                       selected_client->id, &error)
                ? tr(state, "Student overlay cleared.", "Đã xóa lớp vẽ.")
                : error;
        }
        ImGui::SameLine();
        bool any_unread_chat = false;
        for (const auto& client : clients) {
            if (client_has_unread_chat(state, client.id)) {
                any_unread_chat = true;
                break;
            }
        }
        if (draw_icon_button("chat-client", tr(state, "Chat", "Chat"),
                             IconKind::chat, {58.0f, 54.0f}, false,
                             selected_client != nullptr, true,
                             any_unread_chat)) {
            state.view = DashboardView::selected_client;
        }
        draw_ribbon_group_caption(teaching_width,
                                  tr(state, "Teaching", "Giảng dạy"));
    }
    ImGui::EndChild();

    ImGui::SameLine();
    draw_ribbon_divider();
    ImGui::SameLine();
    constexpr float broadcast_width = 92.0f;
    if (ImGui::BeginChild("broadcast-commands", {broadcast_width, 74.0f}, false,
                          ImGuiWindowFlags_NoScrollbar)) {
        if (draw_icon_button(
                "broadcast-room",
                state.broadcast_enabled ? tr(state, "Stop", "Dừng")
                                        : tr(state, "Broadcast", "Phát"),
                state.broadcast_enabled ? IconKind::stop : IconKind::broadcast,
                {broadcast_width, 54.0f}, state.broadcast_enabled,
                has_clients)) {
            toggle_teacher_broadcast(state, control_plane);
        }
        draw_ribbon_group_caption(
            broadcast_width, tr(state, "Teacher screen", "Màn hình GV"));
    }
    ImGui::EndChild();
    ImGui::EndChild();
}

bool draw_filter_chip(const char* id, const char* label, std::size_t count,
                      bool selected) {
    char text[96]{};
    sprintf_s(text, "%s  %zu", label, count);
    ImGui::PushID(id);
    if (selected) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(
                                                    ImGuiCol_Header));
    }
    const bool pressed = ImGui::Button(text, {0, 28.0f});
    if (selected) {
        ImGui::PopStyleColor();
    }
    ImGui::PopID();
    return pressed;
}

void draw_workspace_toolbar(
    const std::vector<nstu::server::ClientRecord>& clients,
    DashboardState& state) {
    const auto counts = count_room_statuses(clients);
    if (draw_filter_chip("all", tr(state, "All", "Tất cả"), clients.size(),
                         state.room_filter == RoomFilter::all)) {
        state.room_filter = RoomFilter::all;
    }
    ImGui::SameLine();
    if (draw_filter_chip("online", tr(state, "Online", "Trực tuyến"),
                         counts.online,
                         state.room_filter == RoomFilter::online)) {
        state.room_filter = RoomFilter::online;
    }
    ImGui::SameLine();
    if (draw_filter_chip("attention", tr(state, "Attention", "Chú ý"),
                         counts.attention,
                         state.room_filter == RoomFilter::attention)) {
        state.room_filter = RoomFilter::attention;
    }
    ImGui::SameLine();
    if (draw_filter_chip("locked", tr(state, "Locked", "Đã khóa"),
                         counts.locked,
                         state.room_filter == RoomFilter::locked)) {
        state.room_filter = RoomFilter::locked;
    }
    ImGui::SameLine();
    if (draw_filter_chip("offline", tr(state, "Offline", "Ngoại tuyến"),
                         counts.offline,
                         state.room_filter == RoomFilter::offline)) {
        state.room_filter = RoomFilter::offline;
        state.show_offline = true;
    }
    constexpr float search_width = 210.0f;
    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 8.0f,
                             ImGui::GetContentRegionMax().x - search_width));
    ImGui::SetNextItemWidth(search_width);
    ImGui::InputTextWithHint("##client-filter",
                             tr(state, "Find client", "Tìm máy"),
                             state.client_filter.data(),
                             state.client_filter.size());
    ImGui::Separator();
}

void draw_navigation_rail(
    const std::vector<nstu::server::ClientRecord>& clients,
    const nstu::server::ClientRecord* selected_client, DashboardState& state,
    nstu::server::ServerControlPlane& control_plane) {
    if (!ImGui::BeginChild("navigation-rail", {48.0f, 0}, true,
                           ImGuiWindowFlags_NoScrollbar)) {
        ImGui::EndChild();
        return;
    }
    if (draw_icon_button("nav-room", tr(state, "Room screens", "Màn hình phòng"),
                         IconKind::grid, {36.0f, 38.0f},
                         state.view == DashboardView::room_screens, true,
                         false)) {
        state.view = DashboardView::room_screens;
    }
    if (draw_icon_button("nav-focus", tr(state, "Selected client", "Máy đang chọn"),
                         IconKind::monitor, {36.0f, 38.0f},
                         state.view == DashboardView::selected_client,
                         selected_client != nullptr, false)) {
        state.view = DashboardView::selected_client;
    }
    ImGui::Separator();
    if (draw_icon_button("nav-lock", tr(state, "Lock room", "Khóa phòng"),
                         IconKind::lock, {36.0f, 38.0f}, false,
                         !clients.empty(), false)) {
        set_room_lock(clients, state, control_plane, true);
    }
    if (draw_icon_button("nav-broadcast",
                         tr(state, "Teacher broadcast", "Phát màn hình giáo viên"),
                         IconKind::broadcast, {36.0f, 38.0f},
                         state.broadcast_enabled, !clients.empty(), false)) {
        toggle_teacher_broadcast(state, control_plane);
    }
    if (draw_icon_button("nav-chat", tr(state, "Client chat", "Chat với máy"),
                         IconKind::chat, {36.0f, 38.0f}, false,
                         selected_client != nullptr, false)) {
        state.view = DashboardView::selected_client;
    }
    ImGui::EndChild();
}

void draw_status_bar(const std::vector<nstu::server::ClientRecord>& clients,
                     DashboardState& state) {
    const auto counts = count_room_statuses(clients);
    if (!ImGui::BeginChild("status-bar", {0, 28.0f}, true,
                           ImGuiWindowFlags_NoScrollbar)) {
        ImGui::EndChild();
        return;
    }
    ImGui::TextColored(status_text_color(nstu::server::ClientStatus::online),
                       "●");
    ImGui::SameLine();
    ImGui::Text("%zu %s", counts.online,
                tr(state, "online", "trực tuyến"));
    if (!state.control_status.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", state.control_status.c_str());
    }
    const char* show_offline_label =
        tr(state, "Show offline", "Hiện ngoại tuyến");
    const float controls_width =
        ImGui::CalcTextSize(show_offline_label).x + 72.0f;
    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 8.0f,
                             ImGui::GetContentRegionMax().x - controls_width));
    ImGui::Checkbox(show_offline_label, &state.show_offline);
    /* Refresh interval is configured from Settings. */
    ImGui::EndChild();
}

void draw_dashboard_shell(
    const std::vector<nstu::server::ClientRecord>& clients,
    const nstu::server::ClientRecord* selected_client, DashboardState& state,
    nstu::server::ServerControlPlane& control_plane) {
    draw_menu_strip(state, !clients.empty(), control_plane);
    draw_telemetry_report_popup(state, clients.size());
    draw_ribbon(clients, selected_client, state, control_plane);
    draw_workspace_toolbar(clients, state);

    if (ImGui::BeginChild("main-workspace", {0, -32.0f}, false)) {
        draw_navigation_rail(clients, selected_client, state, control_plane);
        ImGui::SameLine();
        const ImVec4 content_background = g_dark_mode
            ? ImVec4{0.065f, 0.085f, 0.095f, 1.0f}
            : ImVec4{0.91f, 0.965f, 0.985f, 1.0f};
        ImGui::PushStyleColor(ImGuiCol_ChildBg, content_background);
        if (ImGui::BeginChild("workspace-content", {0, 0}, false)) {
            if (state.view == DashboardView::room_screens) {
                draw_room_screen_wall(clients, state);
            } else {
                draw_selected_client(clients, selected_client, state,
                                     control_plane);
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }
    ImGui::EndChild();
    draw_status_bar(clients, state);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, wchar_t*, int) {
    g_telemetry_policy = load_telemetry_policy();
    g_telemetry_events.clear();
    g_telemetry_error_prompt_requested.store(false);
    g_telemetry_error_prompt_armed.store(
        nstu::telemetry::should_prompt_for_error(g_telemetry_policy));
    const wchar_t* command_line = GetCommandLineW();
    const std::wstring_view arguments =
        command_line == nullptr ? std::wstring_view{} : command_line;
    g_graphics_debug = arguments.find(L"--graphics-debug") !=
                        std::wstring_view::npos;
    if (g_graphics_debug) {
        record_diagnostic("info", "D3D11",
                          "Graphics debug layer requested by command line");
    }
    g_taskbar_created_message = RegisterWindowMessageW(L"TaskbarCreated");
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.lpszClassName = L"NstuServerWindow";
    window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    window_class.hIcon =
        LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    RegisterClassW(&window_class);
    HWND window = CreateWindowW(window_class.lpszClassName, L"NSTU Server",
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                CW_USEDEFAULT, 1280, 820, nullptr, nullptr,
                                instance, nullptr);
    if (window == nullptr || !create_device(window)) {
        MessageBoxW(window, diagnostics_text().c_str(), L"NSTU graphics diagnostics",
                    MB_OK | MB_ICONERROR);
        return 1;
    }
    ShowWindow(window, SW_SHOWDEFAULT);
    g_main_window = window;
    add_tray_icon(window);

    DashboardState dashboard;
    if (PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_VIETNAMESE) {
        dashboard.language = Language::vietnamese;
    }
    if (arguments.find(L"--language=vi") != std::wstring_view::npos) {
        dashboard.language = Language::vietnamese;
    } else if (arguments.find(L"--language=en") != std::wstring_view::npos) {
        dashboard.language = Language::english;
    }
    dashboard.pairing_panel_open =
        arguments.find(L"--pairing") != std::wstring_view::npos;
    dashboard.dark_mode =
        arguments.find(L"--dark") != std::wstring_view::npos;
    g_language = dashboard.language;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    apply_dashboard_style(dashboard.dark_mode);
    load_dashboard_fonts();
    ImGui_ImplWin32_Init(window);
    ImGui_ImplDX11_Init(g_device.Get(), g_context.Get());

    nstu::server::ClientRegistry registry;
    nstu::security::KeyStore key_store;
    nstu::server::ServerControlPlane control_plane(registry, key_store);
    std::string deployment_error;
    const auto data_directory = nstu::deployment::data_root(&deployment_error);
    if (!data_directory.empty() &&
        nstu::deployment::ensure_data_root(data_directory, &deployment_error)) {
        nstu::server::ServerControlPlaneConfig control_config;
        control_config.keyring_path =
            (data_directory / L"server-keyring.bin").wstring();
        control_config.exam_journal_path =
            data_directory / L"exams" / L"answer-journal.bin";
        constexpr char entropy_text[] = "NSTU-SERVER-KEYRING-V1";
        control_config.keyring_entropy.assign(
            reinterpret_cast<const std::byte*>(entropy_text),
            reinterpret_cast<const std::byte*>(entropy_text) +
                sizeof(entropy_text) - 1);
        control_config.enrollment_secret = nstu::security::load_machine_secret(
            (data_directory / L"server-enrollment.bin").wstring(), {}, nullptr);
        // Operator-chosen room label (a display hint, not a secret): remember
        // the path so the "Add computers" field can rewrite it, and load the
        // plaintext file so the beacon advertises it. start() sanitizes it.
        dashboard.room_name_path = data_directory / L"server-room-name.txt";
        control_config.server_name = load_room_name(dashboard.room_name_path);
        std::string control_error;
        if (!control_plane.start(std::move(control_config), &control_error)) {
            dashboard.startup_error = control_error.empty()
                ? "control listener failed to start"
                : std::move(control_error);
            record_diagnostic("error", "ControlPlane",
                              dashboard.startup_error);
        }
    } else {
        dashboard.startup_error = deployment_error.empty()
            ? "protected data directory is unavailable"
            : std::move(deployment_error);
        record_diagnostic("error", "Deployment", dashboard.startup_error);
    }
    bool running = true;
    while (running) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
            if (message.message == WM_QUIT) {
                running = false;
            }
        }
        if (!running) {
            break;
        }
        if (g_telemetry_error_prompt_requested.load()) {
            show_main_window(window);
        }
        if (!IsWindowVisible(window)) {
            Sleep(50);
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("NSTU", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoBringToFrontOnFocus);

        const auto clients = registry.snapshot();
        // Chat notifications: refresh per-client inbound (student) counts, flash
        // the taskbar once when a new student line arrives while the manager is
        // not in the foreground, and expose unread state for the badges. The
        // first poll seeds the baselines so existing history neither flashes nor
        // badges at launch.
        dashboard.chat_counts = control_plane.chat_message_counts();
        {
            const std::uint64_t viewing_chat_id =
                dashboard.view == DashboardView::selected_client
                    ? dashboard.selected_client_id
                    : 0;
            if (!dashboard.chat_counts_initialized) {
                dashboard.chat_observed_counts = dashboard.chat_counts;
                dashboard.chat_seen_counts = dashboard.chat_counts;
                dashboard.chat_counts_initialized = true;
            } else {
                bool new_inbound = false;
                for (const auto& [id, count] : dashboard.chat_counts) {
                    const auto observed =
                        dashboard.chat_observed_counts.find(id);
                    const std::size_t previous =
                        observed == dashboard.chat_observed_counts.end()
                            ? 0
                            : observed->second;
                    if (count > previous && id != viewing_chat_id) {
                        new_inbound = true;
                    }
                    dashboard.chat_observed_counts[id] = count;
                }
                if (new_inbound && g_main_window != nullptr &&
                    GetForegroundWindow() != g_main_window) {
                    FLASHWINFO flash{};
                    flash.cbSize = sizeof(flash);
                    flash.hwnd = g_main_window;
                    flash.dwFlags = FLASHW_TRAY | FLASHW_TIMERNOFG;
                    flash.uCount = 3;
                    FlashWindowEx(&flash);
                }
            }
            // The focused client's chat panel is on screen, so its lines are
            // read as they arrive.
            if (viewing_chat_id != 0) {
                const auto current =
                    dashboard.chat_counts.find(viewing_chat_id);
                dashboard.chat_seen_counts[viewing_chat_id] =
                    current == dashboard.chat_counts.end()
                        ? 0
                        : current->second;
            }
        }
        // Requests the teacher never answered clear themselves rather
        // than sitting in the list until the window is closed.
        control_plane.expire_pending_pairings();
        const auto now = std::chrono::steady_clock::now();
        // Continuous monitoring: keep every online computer snapshotting without
        // the operator clicking "Start snapshots" per machine. The sweep is
        // throttled to at most once every 2 s, so a client that has not yet
        // acknowledged is re-armed and a reconnecting client is picked up
        // automatically. Turning off auto_monitor restores per-computer control.
        if (dashboard.auto_monitor &&
            now >= dashboard.next_auto_monitor_sweep) {
            for (const auto& monitored : clients) {
                if (monitored.status == nstu::server::ClientStatus::online &&
                    !monitored.snapshotting) {
                    std::string monitor_error;
                    (void)control_plane.set_snapshots(
                        monitored.id, true,
                        static_cast<std::uint16_t>(
                            dashboard.snapshot_interval_seconds),
                        &monitor_error);
                }
            }
            dashboard.next_auto_monitor_sweep = now + std::chrono::seconds(2);
        }
        if (dashboard.broadcast_enabled &&
            now >= dashboard.next_host_snapshot) {
            nstu::screen::JpegImage jpeg;
            std::string error;
            if (nstu::screen::capture_primary_screen_jpeg(
                    jpeg, 1280, 720, 72,
                    nstu::control::kMaximumSnapshotJpegBytes, &error)) {
                const auto captured_at = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count());
                nstu::control::SnapshotFrame frame{
                    .width = jpeg.width,
                    .height = jpeg.height,
                    .captured_at_unix_milliseconds = captured_at,
                    .jpeg = std::move(jpeg.bytes),
                };
                if (!control_plane.broadcast_host_snapshot(frame, &error)) {
                    record_operation_failure(
                        "Broadcast", "Teacher snapshot dispatch failed", error);
                    dashboard.control_status = std::move(error);
                }
            } else {
                record_operation_failure(
                    "Broadcast", "Teacher snapshot capture failed", error);
                dashboard.control_status = std::move(error);
            }
            dashboard.next_host_snapshot = now + std::chrono::seconds(
                dashboard.snapshot_interval_seconds);
        }
        if (clients.empty()) {
            dashboard.view = DashboardView::room_screens;
            dashboard.selected_client_id = 0;
        }
        if (dashboard.selected_client_id == 0 && !clients.empty()) {
            dashboard.selected_client_id = clients.front().id;
        }
        const auto selected_client = std::find_if(
            clients.begin(), clients.end(), [&dashboard](const auto& client) {
                return client.id == dashboard.selected_client_id;
            });
        const nstu::server::ClientRecord* selected =
            selected_client == clients.end() ? nullptr : &*selected_client;

        // Delivered-fps meter for the focused/streaming client (Epic B
        // diagnostic): sample snapshot_generation once per second so the Focus
        // label reports the real frame rate reaching the manager, not "~15fps".
        if (selected != nullptr && selected->streaming) {
            if (dashboard.fps_sample_client_id != selected->id) {
                dashboard.fps_sample_client_id = selected->id;
                dashboard.fps_sample_generation = selected->snapshot_generation;
                dashboard.fps_sample_time = now;
                dashboard.measured_stream_fps = 0.0;
            } else {
                const auto elapsed = now - dashboard.fps_sample_time;
                if (elapsed >= std::chrono::seconds(1)) {
                    const double secs =
                        std::chrono::duration<double>(elapsed).count();
                    const std::uint64_t current_generation =
                        selected->snapshot_generation;
                    // A reconnect under the same id can reset the generation
                    // below the stored baseline; guard the unsigned subtraction
                    // so this one sample reports 0 instead of underflowing to an
                    // absurd frame count.
                    const std::uint64_t frames =
                        current_generation >= dashboard.fps_sample_generation
                            ? current_generation -
                                  dashboard.fps_sample_generation
                            : 0;
                    dashboard.measured_stream_fps =
                        static_cast<double>(frames) / secs;
                    dashboard.fps_sample_generation = current_generation;
                    dashboard.fps_sample_time = now;
                }
            }
        } else {
            dashboard.fps_sample_client_id = 0;
            dashboard.measured_stream_fps = 0.0;
        }

        // Live streaming: the independent remote-control window keeps its
        // controlled client streaming regardless of the current view; otherwise
        // the client shown in Focus streams at ~15fps. Leaving Focus with no
        // remote window open, or the client going offline, stops the stream and
        // the class wall falls back to its slower snapshot cadence.
        std::uint64_t desired_stream_id = 0;
        if (g_remote_window != nullptr && g_remote_target_client != 0) {
            for (const auto& streamed : clients) {
                if (streamed.id == g_remote_target_client &&
                    streamed.status !=
                        nstu::server::ClientStatus::offline) {
                    desired_stream_id = streamed.id;
                    break;
                }
            }
        }
        if (desired_stream_id == 0 && selected != nullptr &&
            dashboard.view == DashboardView::selected_client &&
            selected->status != nstu::server::ClientStatus::offline) {
            desired_stream_id = selected->id;
        }
        if (dashboard.streaming_client_id != desired_stream_id) {
            if (dashboard.streaming_client_id != 0) {
                (void)control_plane.set_streaming(
                    dashboard.streaming_client_id, false, 0, nullptr);
            }
            if (desired_stream_id != 0) {
                std::string stream_error;
                if (!control_plane.set_streaming(desired_stream_id, true, 15,
                                                 &stream_error)) {
                    record_operation_failure(
                        "Focus", "Live stream start failed", stream_error);
                }
            }
            dashboard.streaming_client_id = desired_stream_id;
        }

        // Self-heal a stalled live stream. The arming above is edge-triggered on
        // the streamed client id, so a start_stream dropped across a client or
        // agent reconnect leaves the server believing the client streams while
        // its agent has stopped capturing -- the Focus surface and the remote
        // window freeze on the last frame with no edge to re-fire. Re-assert
        // set_streaming when the wanted client stops delivering frames (its
        // snapshot_generation stops advancing) for longer than the stall window.
        constexpr auto kStreamStallRearm = std::chrono::milliseconds(2000);
        std::uint64_t desired_generation = 0;
        if (desired_stream_id != 0) {
            for (const auto& candidate : clients) {
                if (candidate.id == desired_stream_id) {
                    desired_generation = candidate.snapshot_generation;
                    break;
                }
            }
        }
        const nstu::server::StreamRearm rearm =
            dashboard.stream_watchdog.update(desired_stream_id,
                                             desired_generation, now,
                                             kStreamStallRearm);
        if (rearm != nstu::server::StreamRearm::none) {
            std::string rearm_error;
            if (!control_plane.set_streaming(desired_stream_id, true, 15,
                                             &rearm_error)) {
                record_operation_failure("Focus", "Live stream re-arm failed",
                                         rearm_error);
            } else if (rearm == nstu::server::StreamRearm::first) {
                // Log once per stall episode; a still-dark client re-arms
                // quietly (`repeat`) so it can't churn the diagnostics ring.
                record_diagnostic("info", "Focus",
                                  "Re-armed a stalled live stream");
            }
        }

        draw_dashboard_shell(clients, selected, dashboard, control_plane);
        ImGui::End();

        ImGui::Render();
        const ImVec4 background =
            ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
        const float clear_color[4] = {
            background.x, background.y, background.z, background.w};
        if (g_context && g_render_target && g_swap_chain) {
            g_context->OMSetRenderTargets(1, g_render_target.GetAddressOf(),
                                           nullptr);
            g_context->ClearRenderTargetView(g_render_target.Get(), clear_color);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            const HRESULT present_result = g_swap_chain->Present(1, 0);
            if (FAILED(present_result)) {
                const HRESULT removal_reason = g_device
                    ? g_device->GetDeviceRemovedReason() : E_FAIL;
                record_diagnostic("error", "DXGI", "Present failed " +
                    hresult_text(present_result) + "; device reason " +
                    hresult_text(removal_reason));
                if (is_device_loss_hresult(present_result)) {
                    if (!g_graphics_device_lost) {
                        g_graphics_device_lost = true;
                        invalidate_snapshot_textures();
                    }
                    dashboard.control_status = tr(
                        dashboard, "Graphics device lost. Open Diagnostics.",
                        "Mất thiết bị đồ họa. Hãy mở Chẩn đoán.");
                }
            }
        }

        // Independent remote-control window: process a pending close (posted by
        // its WM_CLOSE) and otherwise blit the controlled client's latest frame
        // through the shared device's second swap chain.
        if (g_remote_close_pending.load()) {
            close_remote_window(control_plane);
        } else if (g_remote_window != nullptr) {
            const nstu::server::ClientRecord* remote_client = nullptr;
            for (const auto& candidate : clients) {
                if (candidate.id == g_remote_target_client) {
                    remote_client = &candidate;
                    break;
                }
            }
            if (remote_client == nullptr ||
                remote_client->status ==
                    nstu::server::ClientStatus::offline) {
                close_remote_window(control_plane);
            } else {
                SnapshotTexture* remote_texture =
                    snapshot_texture(*remote_client);
                render_remote_window(
                    remote_texture ? remote_texture->view.Get() : nullptr,
                    remote_texture ? remote_texture->width : 0u,
                    remote_texture ? remote_texture->height : 0u);
            }
        }
    }

    close_remote_window(control_plane);
    if (dashboard.streaming_client_id != 0) {
        (void)control_plane.set_streaming(dashboard.streaming_client_id, false,
                                          0, nullptr);
    }
    control_plane.stop();
    Shell_NotifyIconW(NIM_DELETE, &g_tray_icon);
    g_snapshot_textures.clear();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    g_render_target.Reset();
    g_swap_chain.Reset();
    g_context.Reset();
    g_device.Reset();
    if (IsWindow(window)) {
        DestroyWindow(window);
    }
    UnregisterClassW(window_class.lpszClassName, instance);
    return 0;
}
