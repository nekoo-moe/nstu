#include "nstu/client_registry.hpp"
#include "nstu/control_plane.hpp"
#include "nstu/deployment.hpp"
#include "nstu/key_store.hpp"
#include "nstu/screen_snapshot.hpp"
#include "nstu/secret_store.hpp"
#include "nstu/snapshot_generation_gate.hpp"
#include "nstu/protocol_headers.h"

#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>
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
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <deque>
#include <iomanip>
#include <sstream>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

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
};

Language g_language = Language::english;

struct DashboardState {
    DashboardView view = DashboardView::room_screens;
    RoomFilter room_filter = RoomFilter::all;
    std::uint64_t selected_client_id = 0;
    int snapshot_interval_seconds = 7;
    Language language = Language::english;
    bool dark_mode = false;
    bool show_offline = true;
    bool broadcast_enabled = false;
    bool remote_control_enabled = false;
    std::uint64_t remote_control_client_id = 0;
    bool remote_pointer_down = false;
    ImVec2 remote_pointer_last{};
    std::array<char, 64> remote_keyboard_input{};
    bool annotation_enabled = false;
    bool annotation_dragging = false;
    ImVec2 previous_annotation_point{};
    AnnotationTool annotation_tool = AnnotationTool::pen;
    std::uint32_t annotation_rgba = 0xe5484dffu;
    int annotation_thickness = 4;
    std::chrono::steady_clock::time_point next_host_snapshot{};
    std::string control_status;
    std::string startup_error;
    std::array<char, 96> client_filter{};
    std::array<char, 512> chat_input{};
};

const char* tr(const DashboardState& state, const char* english,
               const char* vietnamese) {
    return state.language == Language::vietnamese ? vietnamese : english;
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
    if (message == WM_SYSCOMMAND &&
        (wparam & 0xfff0u) == SC_MINIMIZE) {
        ShowWindow(window, SW_HIDE);
        return 0;
    }
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
                      bool enabled = true, bool show_label = true) {
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
    const float preview_width = ImGui::GetContentRegionAvail().x;
    const float preview_height = std::clamp(
        preview_width * 0.5625f, 220.0f,
        std::max(220.0f, ImGui::GetContentRegionAvail().y - 210.0f));
    const auto surface = draw_screen_surface(
        *selected_client, preview_height, "##focus-screen", state);
    if (state.remote_control_enabled && surface.has_frame) {
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const bool inside = mouse.x >= surface.image_min.x &&
                            mouse.x <= surface.image_max.x &&
                            mouse.y >= surface.image_min.y &&
                            mouse.y <= surface.image_max.y;
        const auto send_mouse = [&](ImVec2 point, std::uint8_t flags,
                                    bool require_inside) {
            if (require_inside && !inside) {
                return;
            }
            const float x_ratio = std::clamp(
                (point.x - surface.image_min.x) /
                    (surface.image_max.x - surface.image_min.x),
                0.0f, 1.0f);
            const float y_ratio = std::clamp(
                (point.y - surface.image_min.y) /
                    (surface.image_max.y - surface.image_min.y),
                0.0f, 1.0f);
            nstu::wire::RemoteInputPacket packet{};
            packet.input_type = static_cast<std::uint8_t>(
                nstu::wire::RemoteInputType::mouse);
            packet.flags = flags | static_cast<std::uint8_t>(
                nstu::wire::RemoteInputFlags::mouse_absolute) |
                static_cast<std::uint8_t>(
                    nstu::wire::RemoteInputFlags::mouse_normalized);
            packet.x = static_cast<std::int32_t>(x_ratio * 65535.0f);
            packet.y = static_cast<std::int32_t>(y_ratio * 65535.0f);
            std::string ignored_error;
            (void)control_plane.send_remote_input(
                selected_client->id, packet, &ignored_error);
        };
        if (inside && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            state.remote_pointer_down = true;
            state.remote_pointer_last = mouse;
            send_mouse(mouse, static_cast<std::uint8_t>(
                nstu::wire::RemoteInputFlags::mouse_left_down), true);
        }
        if (state.remote_pointer_down && inside &&
            ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            state.remote_pointer_last = mouse;
            send_mouse(mouse, 0, true);
        }
        if (state.remote_pointer_down &&
            ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            send_mouse(inside ? mouse : state.remote_pointer_last,
                       static_cast<std::uint8_t>(
                           nstu::wire::RemoteInputFlags::mouse_left_up), false);
            state.remote_pointer_down = false;
        }
    } else {
        state.remote_pointer_down = false;
    }
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
                break;
            }
        };
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && inside) {
            state.annotation_dragging = true;
            state.previous_annotation_point = mouse;
        }
        if (state.annotation_dragging &&
            ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
            state.annotation_tool == AnnotationTool::pen && inside) {
            const float delta_x = mouse.x - state.previous_annotation_point.x;
            const float delta_y = mouse.y - state.previous_annotation_point.y;
            if (delta_x * delta_x + delta_y * delta_y >= 9.0f) {
                send_segment(state.previous_annotation_point, mouse);
                state.previous_annotation_point = mouse;
            }
        }
        if (state.annotation_dragging &&
            ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            const ImVec2 end = inside ? mouse : state.previous_annotation_point;
            if (state.annotation_tool != AnnotationTool::pen) {
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
        }
        ImGui::OpenPopup("control-status");
    }
    ImGui::PopStyleColor(4);
    ImGui::SameLine();
    const bool remote_available = selected_client->status !=
        nstu::server::ClientStatus::offline;
    if (ImGui::Button(state.remote_control_enabled
                          ? tr(state, "Stop remote", "Dừng điều khiển")
                          : tr(state, "Start remote", "Bắt đầu điều khiển"))) {
        std::string error;
        if (state.remote_control_enabled) {
            const bool ok = control_plane.stop_remote_control(
                selected_client->id, &error);
            state.control_status = ok
                ? tr(state, "Remote control stopped.",
                     "Đã dừng điều khiển từ xa.")
                : error;
            state.remote_control_enabled = false;
            state.remote_control_client_id = 0;
            state.remote_pointer_down = false;
        } else if (remote_available) {
            const bool ok = control_plane.start_remote_control(
                selected_client->id, &error);
            state.control_status = ok
                ? tr(state, "Remote control enabled.",
                     "Đã bật điều khiển từ xa.")
                : error;
            state.remote_control_enabled = ok;
            state.remote_control_client_id = ok ? selected_client->id : 0;
        }
        ImGui::OpenPopup("control-status");
    }
    if (!remote_available && state.remote_control_enabled) {
        (void)control_plane.stop_remote_control(selected_client->id, nullptr);
        state.remote_control_enabled = false;
        state.remote_control_client_id = 0;
        state.remote_pointer_down = false;
    }
    if (state.remote_control_enabled) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(220.0f);
        const bool submit_key = ImGui::InputText(
            "##remote-key", state.remote_keyboard_input.data(),
            state.remote_keyboard_input.size(),
            ImGuiInputTextFlags_EnterReturnsTrue);
        if (submit_key && state.remote_keyboard_input[0] != '\0') {
            for (const unsigned char character : std::string_view(
                     state.remote_keyboard_input.data())) {
                const SHORT key = VkKeyScanA(character);
                if (key == -1) {
                    continue;
                }
                nstu::wire::RemoteInputPacket down{};
                down.input_type = static_cast<std::uint8_t>(
                    nstu::wire::RemoteInputType::keyboard);
                down.virtual_key = LOBYTE(key);
                nstu::wire::RemoteInputPacket up = down;
                up.flags = static_cast<std::uint8_t>(
                    nstu::wire::RemoteInputFlags::key_up);
                std::string ignored_error;
                (void)control_plane.send_remote_input(
                    selected_client->id, down, &ignored_error);
                (void)control_plane.send_remote_input(
                    selected_client->id, up, &ignored_error);
            }
            state.remote_keyboard_input[0] = '\0';
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Type ASCII text and press Enter to send it");
        }
    }
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
        state.control_status =
            control_plane.clear_overlay(selected_client->id, &error)
                ? tr(state, "Student overlay cleared.",
                     "Đã xóa lớp vẽ trên máy học sinh.")
                : error;
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
        state.control_status =
            control_plane.set_locked(selected_client->id, !is_locked, &error)
                ? (is_locked
                       ? tr(state, "Unlock command sent.",
                            "Đã gửi lệnh mở khóa.")
                       : tr(state, "Lock command sent.",
                            "Đã gửi lệnh khóa."))
                : error;
        ImGui::OpenPopup("control-status");
    }
    ImGui::PopStyleColor(3);
    if (state.annotation_enabled) {
        ImGui::TextUnformatted(tr(state, "Overlay tools", "Công cụ lớp phủ"));
        ImGui::SameLine();
        const char* tool_items =
            state.language == Language::vietnamese
                ? "Bút\0Thước\0Mũi tên\0Hình chữ nhật\0Elip\0"
                : "Pen\0Ruler\0Arrow\0Rectangle\0Ellipse\0";
        int tool_index = static_cast<int>(state.annotation_tool);
        ImGui::SetNextItemWidth(150.0f);
        if (ImGui::Combo(tr(state, "Tool", "Công cụ"), &tool_index,
                         tool_items)) {
            state.annotation_tool = static_cast<AnnotationTool>(
                std::clamp(tool_index, 0,
                           static_cast<int>(AnnotationTool::ellipse)));
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
        ImGui::TextDisabled("%s", tr(state, "No messages in this session.",
                                      "Chưa có tin nhắn trong phiên này."));
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
    ImGui::EndPopup();
}

void set_room_snapshots(
    const std::vector<nstu::server::ClientRecord>& clients,
    DashboardState& state, nstu::server::ServerControlPlane& control_plane,
    bool enabled) {
    std::size_t sent = 0;
    std::string last_error;
    for (const auto& client : clients) {
        if (client.status == nstu::server::ClientStatus::offline) {
            continue;
        }
        std::string error;
        if (control_plane.set_snapshots(
                client.id, enabled,
                static_cast<std::uint16_t>(state.snapshot_interval_seconds),
                &error)) {
            ++sent;
        } else {
            last_error = std::move(error);
        }
    }
    state.control_status = sent == 0
        ? (last_error.empty()
               ? tr(state, "No online clients are available.",
                    "Không có máy trực tuyến để điều khiển.")
               : std::move(last_error))
        : (enabled ? tr(state, "Room snapshots started.",
                        "Đã bắt đầu chụp toàn phòng.")
                   : tr(state, "Room snapshots stopped.",
                        "Đã dừng chụp toàn phòng."));
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
    ImGui::SameLine();
    if (ImGui::Button(tr(state, "Close", "Đóng"), {90.0f, 0.0f})) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void draw_menu_strip(DashboardState& state, bool has_clients) {
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
    constexpr float student_width = 256.0f;
    if (ImGui::BeginChild("student-commands", {student_width, 74.0f}, false,
                          ImGuiWindowFlags_NoScrollbar)) {
        if (draw_icon_button("start-room", tr(state, "Start", "Chụp"),
                             IconKind::camera, {58.0f, 54.0f}, false,
                             has_clients)) {
            set_room_snapshots(clients, state, control_plane, true);
        }
        ImGui::SameLine();
        if (draw_icon_button("stop-room", tr(state, "Stop", "Dừng"),
                             IconKind::stop, {58.0f, 54.0f}, false,
                             has_clients)) {
            set_room_snapshots(clients, state, control_plane, false);
        }
        ImGui::SameLine();
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
                             selected_client != nullptr)) {
            state.view = DashboardView::selected_client;
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
        if (draw_icon_button("chat-client", tr(state, "Chat", "Chat"),
                             IconKind::chat, {58.0f, 54.0f}, false,
                             selected_client != nullptr)) {
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
    if (draw_icon_button("nav-snapshot", tr(state, "Start snapshots", "Bắt đầu chụp"),
                         IconKind::camera, {36.0f, 38.0f}, false,
                         !clients.empty(), false)) {
        set_room_snapshots(clients, state, control_plane, true);
    }
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
    draw_menu_strip(state, !clients.empty());
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
        constexpr char entropy_text[] = "NSTU-SERVER-KEYRING-V1";
        control_config.keyring_entropy.assign(
            reinterpret_cast<const std::byte*>(entropy_text),
            reinterpret_cast<const std::byte*>(entropy_text) +
                sizeof(entropy_text) - 1);
        control_config.enrollment_secret = nstu::security::load_machine_secret(
            (data_directory / L"server-enrollment.bin").wstring(), {}, nullptr);
        std::string control_error;
        if (!control_plane.start(std::move(control_config), &control_error)) {
            dashboard.startup_error = control_error.empty()
                ? "control listener failed to start"
                : std::move(control_error);
        }
    } else {
        dashboard.startup_error = deployment_error.empty()
            ? "protected data directory is unavailable"
            : std::move(deployment_error);
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
        const auto now = std::chrono::steady_clock::now();
        if (dashboard.broadcast_enabled &&
            now >= dashboard.next_host_snapshot) {
            nstu::screen::JpegImage jpeg;
            std::string error;
            if (nstu::screen::capture_primary_screen_jpeg(
                    jpeg, 480, 270, 52,
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
                    dashboard.control_status = std::move(error);
                }
            } else {
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

        if (dashboard.remote_control_enabled &&
            (selected == nullptr || dashboard.view != DashboardView::selected_client ||
             dashboard.remote_control_client_id != selected->id)) {
            (void)control_plane.stop_remote_control(
                dashboard.remote_control_client_id, nullptr);
            dashboard.remote_control_enabled = false;
            dashboard.remote_control_client_id = 0;
            dashboard.remote_pointer_down = false;
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
    }

    if (dashboard.remote_control_enabled) {
        (void)control_plane.stop_remote_control(
            dashboard.remote_control_client_id, nullptr);
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
