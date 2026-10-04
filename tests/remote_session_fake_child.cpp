#include <windows.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

std::wstring argument(int index, int argc, wchar_t** argv) {
    return index < argc ? argv[index] : std::wstring{};
}

int integer_argument(int index, int argc, wchar_t** argv, int fallback) {
    if (index >= argc) return fallback;
    try {
        return std::stoi(argv[index]);
    } catch (...) {
        return fallback;
    }
}

void append_arguments(const std::filesystem::path& path, int argc,
                      wchar_t** argv) {
    std::wofstream output(path, std::ios::app);
    for (int index = 0; index < argc; ++index) {
        output << index << L':' << argv[index] << L'\n';
    }
}

void spawn_descendant(const std::filesystem::path& marker) {
    wchar_t module[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, module, MAX_PATH) == 0) return;
    std::wstring command = L"\"" + std::wstring(module) +
        L"\" descendant \"" + marker.wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(module, command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        return;
    }
    std::ofstream(marker) << process.dwProcessId;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) return 64;
    const auto mode = argument(1, argc, argv);
    if (mode == L"--version") return 0;
    if (mode == L"descendant") {
        std::this_thread::sleep_for(std::chrono::seconds(30));
        return 0;
    }
    if (mode == L"list") {
        const auto host = argument(2, argc, argv);
        if (host == L"preflight-fail") {
            fputs("unpaired host\n", stderr);
            return 2;
        }
        if (host == L"preflight-timeout") {
            std::this_thread::sleep_for(std::chrono::seconds(30));
            return 3;
        }
        fputs("Desktop\n", stdout);
        return 0;
    }
    if (mode != L"stream") return 65;

    for (int index = 2; index < argc; ++index) {
        if (std::wstring_view(argv[index]) == L"--record-args" &&
            index + 1 < argc) {
            append_arguments(argv[++index], argc, argv);
        } else if (std::wstring_view(argv[index]) == L"--diagnostic-bytes" &&
                   index + 1 < argc) {
            const int count = integer_argument(++index, argc, argv, 0);
            const std::string chunk(static_cast<std::size_t>(
                count > 0 ? count : 0), 'x');
            fwrite(chunk.data(), 1, chunk.size(), stderr);
            fflush(stderr);
        } else if (std::wstring_view(argv[index]) == L"--spawn-descendant" &&
                   index + 1 < argc) {
            spawn_descendant(argv[++index]);
        } else if (std::wstring_view(argv[index]) == L"--sleep-ms" &&
                   index + 1 < argc) {
            const int milliseconds = integer_argument(++index, argc, argv, 0);
            std::this_thread::sleep_for(
                std::chrono::milliseconds(milliseconds));
        } else if (std::wstring_view(argv[index]) == L"--exit-code" &&
                   index + 1 < argc) {
            return integer_argument(++index, argc, argv, 0);
        }
    }
    return 0;
}
