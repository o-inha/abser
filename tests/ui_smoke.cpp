// Exercises the shipped executable and its real message loop on Windows.
#ifndef UNICODE
#define UNICODE
#define _UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commctrl.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <vector>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
constexpr int kPath = 101, kList = 103, kFilter = 200;
constexpr wchar_t kWindowClass[] = L"AbserWindow";

void require(bool condition, const char* error) {
    if (!condition) throw std::runtime_error(error);
}

template<class Predicate> void waitFor(Predicate predicate, const char* error, int timeoutMs = 5000) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    do {
        if (predicate()) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < until);
    throw std::runtime_error(error);
}

void put16(std::ostream& output, uint16_t n) {
    output.put(static_cast<char>(n)); output.put(static_cast<char>(n >> 8));
}
void put32(std::ostream& output, uint32_t n) {
    put16(output, static_cast<uint16_t>(n)); put16(output, static_cast<uint16_t>(n >> 16));
}
void makeWave(const std::filesystem::path& file) {
    constexpr uint32_t frames = 16000, rate = 8000, dataBytes = frames * 2;
    std::ofstream output(file, std::ios::binary);
    output.write("RIFF", 4); put32(output, 36 + dataBytes); output.write("WAVEfmt ", 8);
    put32(output, 16); put16(output, 1); put16(output, 1); put32(output, rate);
    put32(output, rate * 2); put16(output, 2); put16(output, 16);
    output.write("data", 4); put32(output, dataBytes);
    for (uint32_t i = 0; i < frames; ++i)
        put16(output, static_cast<uint16_t>(static_cast<int16_t>(std::sin(i * 0.17) * 1000)));
    require(static_cast<bool>(output), "Could not create smoke-test WAV fixture");
}

struct Process {
    PROCESS_INFORMATION info{};
    HWND window{};
    ~Process() {
        if (!info.hProcess) return;
        if (WaitForSingleObject(info.hProcess, 0) == WAIT_TIMEOUT) {
            if (window) PostMessageW(window, WM_CLOSE, 0, 0);
            if (WaitForSingleObject(info.hProcess, 5000) == WAIT_TIMEOUT) {
                TerminateProcess(info.hProcess, 91);
                WaitForSingleObject(info.hProcess, 1000);
            }
        }
        CloseHandle(info.hThread); CloseHandle(info.hProcess);
    }
    bool alive() const { return WaitForSingleObject(info.hProcess, 0) == WAIT_TIMEOUT; }
};

BOOL CALLBACK findWindow(HWND window, LPARAM parameter) {
    auto& process = *reinterpret_cast<Process*>(parameter);
    DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
    if (pid != process.info.dwProcessId) return TRUE;
    wchar_t name[128]{}; GetClassNameW(window, name, 128);
    if (std::wstring(name) != kWindowClass || !IsWindowVisible(window)) return TRUE;
    process.window = window; return FALSE;
}

struct PopupMenu { DWORD processId; HWND window{}; HMENU menu{}; };
BOOL CALLBACK findPopupMenu(HWND window, LPARAM parameter) {
    auto& popup = *reinterpret_cast<PopupMenu*>(parameter);
    DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
    if (pid != popup.processId || !IsWindowVisible(window)) return TRUE;
    wchar_t name[64]{}; GetClassNameW(window, name, 64);
    if (std::wstring(name) != L"#32768") return TRUE;
    MENUBARINFO info{sizeof(info)};
    if (!GetMenuBarInfo(window, OBJID_CLIENT, 0, &info) || !info.hMenu) return TRUE;
    if (GetMenuItemCount(info.hMenu) != 11 || GetMenuItemID(info.hMenu, 1) != 1001) return TRUE;
    popup.window = window; popup.menu = info.hMenu; return FALSE;
}

void clickScreen(POINT point) {
    const int left = GetSystemMetrics(SM_XVIRTUALSCREEN), top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int width = GetSystemMetrics(SM_CXVIRTUALSCREEN), height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    require(width > 1 && height > 1, "Native test desktop has invalid dimensions");
    INPUT input[3]{};
    for (auto& event : input) event.type = INPUT_MOUSE;
    input[0].mi.dx = MulDiv(point.x - left, 65535, width - 1);
    input[0].mi.dy = MulDiv(point.y - top, 65535, height - 1);
    input[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    input[1].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    input[2].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    require(SendInput(3, input, sizeof(INPUT)) == 3, "Could not click the native category menu");
}

void captureWindow(HWND window, const std::filesystem::path& filename) {
    RECT rect{}; GetWindowRect(window, &rect);
    const int width = rect.right - rect.left, height = rect.bottom - rect.top;
    HDC screen = GetDC(nullptr), memory = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, width, height);
    HGDIOBJ old = SelectObject(memory, bitmap);
    const BOOL printed = PrintWindow(window, memory, 2);
    SelectObject(memory, old);
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = height;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
    std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 4);
    const int copied = GetDIBits(memory, bitmap, 0, height, pixels.data(), &info, DIB_RGB_COLORS);
    DeleteObject(bitmap); DeleteDC(memory); ReleaseDC(nullptr, screen);
    require(printed && copied == height, "Could not capture native UI screenshot");
    BITMAPFILEHEADER header{}; header.bfType = 0x4d42; header.bfOffBits = sizeof(header) + sizeof(BITMAPINFOHEADER);
    header.bfSize = header.bfOffBits + static_cast<DWORD>(pixels.size());
    std::ofstream output(filename, std::ios::binary);
    output.write(reinterpret_cast<const char*>(&header), sizeof(header));
    output.write(reinterpret_cast<const char*>(&info.bmiHeader), sizeof(info.bmiHeader));
    output.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
    require(static_cast<bool>(output), "Could not write native UI screenshot");
}

void focusControl(HWND window, HWND control) {
    const DWORD targetThread = GetWindowThreadProcessId(window, nullptr);
    const DWORD currentThread = GetCurrentThreadId();
    MSG message{}; PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE); // Create our input queue.
    require(AttachThreadInput(currentThread, targetThread, TRUE) != FALSE, "Could not attach smoke-test input queue");
    SetForegroundWindow(window); SetFocus(control);
    AttachThreadInput(currentThread, targetThread, FALSE);
    waitFor([&] { GUITHREADINFO state{sizeof(state)}; return GetGUIThreadInfo(targetThread, &state) && state.hwndFocus == control; },
            "Could not focus native control", 2000);
}

void key(HWND window, UINT code) {
    require(PostMessageW(window, WM_KEYDOWN, code, 1) != FALSE, "Could not post test key");
    PostMessageW(window, WM_KEYUP, code, static_cast<LPARAM>(0xc0000001u));
}

void openByEnter(Process& process, HWND path, HWND list, const std::filesystem::path& folder, int expectedCount) {
    require(SendMessageW(path, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(folder.c_str())) != FALSE, "Could not set address field");
    wchar_t entered[32768]{};
    SendMessageW(path, WM_GETTEXT, 32768, reinterpret_cast<LPARAM>(entered));
    require(folder.wstring() == entered, "Address field did not retain the entered path");
    focusControl(process.window, path); key(path, VK_RETURN);
    try {
        waitFor([&] { require(process.alive(), "App exited while changing folders");
                      return SendMessageW(list, LVM_GETITEMCOUNT, 0, 0) == expectedCount; },
                "Address Enter did not load expected folder");
    } catch (...) {
        wchar_t actual[32768]{};
        SendMessageW(path, WM_GETTEXT, 32768, reinterpret_cast<LPARAM>(actual));
        std::wcerr << L"Expected folder: " << folder.wstring() << L"; address text: " << actual
                   << L"; list count: " << SendMessageW(list, LVM_GETITEMCOUNT, 0, 0) << L'\n';
        throw;
    }
    const DWORD thread = GetWindowThreadProcessId(process.window, nullptr);
    waitFor([&] { GUITHREADINFO state{sizeof(state)}; return GetGUIThreadInfo(thread, &state) && state.hwndFocus == list; },
            "Opening a folder did not return focus to file list");
}

std::string readText(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

int run(const std::filesystem::path& executable) {
    wchar_t tempPath[32768]{};
    require(GetTempPathW(32768, tempPath) > 0, "Could not locate temporary directory");
    const auto root = std::filesystem::path(tempPath) /
        (L"Abser UI smoke " + std::to_wstring(GetCurrentProcessId()) + L" " + std::to_wstring(GetTickCount64()));
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); } } cleanup{root};
    const auto audio = root / L"Audio files \u65e5\u672c\u8a9e";
    const auto empty = root / L"Empty folder";
    const auto metadata = root / L"App data" / L"metadata.txt";
    std::filesystem::create_directories(audio); std::filesystem::create_directories(empty);
    for (int i = 0; i < 16; ++i) makeWave(audio / (L"take" + std::to_wstring(i + 1) + L".wav"));

    Process process;
    std::wstring command = L"\"" + executable.wstring() + L"\" --smoke-test-data \"" + metadata.wstring() + L"\" \"" + audio.wstring() + L"\"";
    STARTUPINFOW startup{sizeof(startup)};
    require(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process.info) != FALSE,
            "Could not launch Abser executable");
    // A top-level HWND exists before WM_CREATE finishes. Wait for the visible,
    // fully created window and its children rather than assuming enumeration
    // means that initialization has completed.
    WaitForInputIdle(process.info.hProcess, 5000);
    waitFor([&] { require(process.alive(), "Abser exited during startup"); EnumWindows(findWindow, reinterpret_cast<LPARAM>(&process)); return process.window != nullptr; },
            "Abser did not create its visible main window");
    HWND path = nullptr, list = nullptr;
    waitFor([&] {
        require(process.alive(), "Abser exited creating native controls");
        path = GetDlgItem(process.window, kPath); list = GetDlgItem(process.window, kList);
        if (!path || !list || !GetDlgItem(process.window, kFilter + 10)) return false;
        const HWND header = reinterpret_cast<HWND>(SendMessageW(list, LVM_GETHEADER, 0, 0));
        return header && SendMessageW(header, HDM_GETITEMCOUNT, 0, 0) == 4;
    }, "Required native controls were not created");
    waitFor([&] { require(process.alive(), "Abser exited loading initial folder"); return SendMessageW(list, LVM_GETITEMCOUNT, 0, 0) == 16; },
            "Initial folder was not loaded");
    HWND header = reinterpret_cast<HWND>(SendMessageW(list, LVM_GETHEADER, 0, 0));
    require(header && SendMessageW(header, HDM_GETITEMCOUNT, 0, 0) == 4, "File list must have four columns");
    RECT windowRect{}; GetWindowRect(process.window, &windowRect);
    const UINT dpi = GetDpiForWindow(process.window);
    require(windowRect.right - windowRect.left <= MulDiv(600, dpi, 96), "Default window is unexpectedly wide");
    captureWindow(process.window, L"ui-smoke-default.bmp");

    // Native Down travels through the application's outer keyboard loop.
    focusControl(process.window, list); key(list, VK_DOWN);
    waitFor([&] { return SendMessageW(list, LVM_GETNEXTITEM, static_cast<WPARAM>(-1), LVNI_SELECTED) == 0; }, "Down did not select the first file");
    key(list, VK_END);
    waitFor([&] { return SendMessageW(list, LVM_GETNEXTITEM, static_cast<WPARAM>(-1), LVNI_SELECTED) == 15; }, "End did not select the final file");
    key(list, VK_SPACE); key(list, VK_SPACE);

    // Previously selected index15 must not be used against the new empty model.
    openByEnter(process, path, list, empty, 0);
    openByEnter(process, path, list, audio, 16);

    // All fixture files start unassigned. Its checkbox must hide/reveal all rows.
    HWND unassigned = GetDlgItem(process.window, kFilter);
    require(unassigned != nullptr, "Unassigned colour filter is missing");
    SendMessageW(unassigned, BM_CLICK, 0, 0);
    waitFor([&] { return SendMessageW(list, LVM_GETITEMCOUNT, 0, 0) == 0; }, "Unassigned filter did not hide files");
    SendMessageW(unassigned, BM_CLICK, 0, 0);
    waitFor([&] { return SendMessageW(list, LVM_GETITEMCOUNT, 0, 0) == 16; }, "Unassigned filter did not restore files");

    // Assign Red by clicking the real popup item through the native input route.
    focusControl(process.window, list); key(list, VK_DOWN);
    waitFor([&] { return SendMessageW(list, LVM_GETNEXTITEM, static_cast<WPARAM>(-1), LVNI_SELECTED) >= 0; }, "Could not select a file for categorization");
    const DWORD uiThread = GetWindowThreadProcessId(process.window, nullptr);
    auto menuActive = [&] { GUITHREADINFO state{sizeof(state)};
        return GetGUIThreadInfo(uiThread, &state) && (state.flags & (GUI_INMENUMODE | GUI_POPUPMENUMODE)); };
    require(PostMessageW(list, WM_CONTEXTMENU, reinterpret_cast<WPARAM>(list), MAKELPARAM(-1, -1)) != FALSE,
            "Could not open category context menu");
    waitFor(menuActive, "Category context menu did not open", 2000);
    PopupMenu popup{process.info.dwProcessId};
    waitFor([&] { EnumWindows(findPopupMenu, reinterpret_cast<LPARAM>(&popup)); return popup.window != nullptr; },
            "Could not locate Abser category popup window", 2000);
    RECT red{};
    require(GetMenuItemRect(process.window, popup.menu, 1, &red) != FALSE && red.right > red.left && red.bottom > red.top,
            "Could not locate Red category menu item");
    captureWindow(popup.window, L"ui-smoke-menu.bmp");
    const POINT redCenter{(red.left + red.right) / 2, (red.top + red.bottom) / 2};
    std::cout << "Category menu item1001 at " << redCenter.x << ',' << redCenter.y << '\n';
    clickScreen(redCenter);
    waitFor([&] { require(process.alive(), "App exited choosing a category"); return !menuActive(); },
            "Category context menu did not accept a native mouse click", 2000);
    SendMessageW(unassigned, BM_CLICK, 0, 0);
    try {
        waitFor([&] { return SendMessageW(list, LVM_GETITEMCOUNT, 0, 0) == 1; },
                "Red assignment was not applied by the native menu");
    } catch (...) {
        captureWindow(process.window, L"ui-smoke-category-failed.bmp");
        std::cerr << "After Red click, visible file count=" << SendMessageW(list, LVM_GETITEMCOUNT, 0, 0)
                  << "; saved metadata=" << readText(metadata) << '\n';
        throw;
    }
    RedrawWindow(process.window, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    captureWindow(process.window, L"ui-smoke-category.bmp");
    SendMessageW(unassigned, BM_CLICK, 0, 0);
    waitFor([&] { return SendMessageW(list, LVM_GETITEMCOUNT, 0, 0) == 16; },
            "Assigned and unassigned files did not restore together");

    // Exercise native header clicks and resizable columns on the shipped control.
    const int catWidth = static_cast<int>(SendMessageW(list, LVM_GETCOLUMNWIDTH, 0, 0));
    const int x = catWidth + 15;
    SendMessageW(header, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, 8));
    SendMessageW(header, WM_LBUTTONUP, 0, MAKELPARAM(x, 8));
    SendMessageW(list, LVM_SETCOLUMNWIDTH, 1, 177);
    require(SendMessageW(list, LVM_GETCOLUMNWIDTH, 1, 0) == 177, "Filename column is not resizable");
    const int desiredWidth = MulDiv(310, dpi, 96);
    SetWindowPos(process.window, nullptr, windowRect.left, windowRect.top, desiredWidth, windowRect.bottom - windowRect.top, SWP_NOZORDER | SWP_NOACTIVATE);
    GetWindowRect(process.window, &windowRect);
    require(windowRect.right - windowRect.left == desiredWidth, "Window cannot be resized to a narrow portrait width");
    require(process.alive(), "App exited during list interactions");
    RedrawWindow(process.window, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    captureWindow(process.window, L"ui-smoke-narrow.bmp");

    PostMessageW(process.window, WM_CLOSE, 0, 0);
    require(WaitForSingleObject(process.info.hProcess, 8000) == WAIT_OBJECT_0, "App did not exit cleanly");
    DWORD exitCode = 0; GetExitCodeProcess(process.info.hProcess, &exitCode);
    require(exitCode == 0, "App returned an error exit code");
    require(std::filesystem::is_regular_file(metadata), "Settings were not written to isolated AppData store");
    const std::string saved = readText(metadata);
    require(saved.find("ABSER 1") == 0, "Saved metadata format is invalid");
    require(saved.find("S folder ") != std::string::npos, "Last folder was not remembered");
    require(saved.find("S col1 ") != std::string::npos, "Column width was not remembered");
    require(saved.find("S ascending 0") != std::string::npos, "Header click did not reverse the filename sort");
    require(saved.find("C 1 ") != std::string::npos, "Red file category was not persisted");
    for (const auto& file : std::filesystem::directory_iterator(audio))
        require(file.path().extension() == L".wav", "App wrote metadata into the audio folder");
    std::cout << "UI smoke passed: Unicode paths, Enter, selection, empty folder, filters, category menu, sorting, resizing, persistence, clean exit.\n";
    return 0;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    try {
        require(argc == 2, "Usage: abser_ui_smoke.exe <Abser.exe>");
        return run(std::filesystem::absolute(argv[1]));
    } catch (const std::exception& error) {
        std::cerr << "UI smoke failed: " << error.what() << '\n';
        return 1;
    }
}
