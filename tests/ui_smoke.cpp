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
#include <ole2.h>
#include <shellapi.h>
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
#include <sstream>
#include <string>
#include <thread>

namespace {
constexpr int kPath = 101, kList = 103, kVolume = 104, kPlay = 105, kFilter = 200;
constexpr int kFixtureCount = 64, kVerticalRail = 0x6A11, kHorizontalRail = 0x6A12;
constexpr wchar_t kWindowClass[] = L"AbserWindow";

void require(bool condition, const char* error) {
    if (!condition) throw std::runtime_error(error);
}

void pumpMessages() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
}

template<class Predicate> void waitFor(Predicate predicate, const char* error, int timeoutMs = 5000) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    do {
        pumpMessages();
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
    for (uint32_t i = 0; i < frames; ++i) {
        const double envelope = 0.2 + 0.8 * (1.0 - static_cast<double>(i) / frames);
        put16(output, static_cast<uint16_t>(static_cast<int16_t>(std::sin(i * 0.17) * 1000 * envelope)));
    }
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

void focusControl(HWND window, HWND control);
void key(HWND window, UINT code);
void clickScreen(POINT point);
void captureWindow(HWND window, const std::filesystem::path& filename);

struct ChildLookup { int id; HWND result{}; };
BOOL CALLBACK findChild(HWND window, LPARAM parameter) {
    auto& lookup = *reinterpret_cast<ChildLookup*>(parameter);
    if (GetDlgCtrlID(window) != lookup.id) return TRUE;
    lookup.result = window; return FALSE;
}
HWND childControl(HWND parent, int id) {
    if (const auto direct = GetDlgItem(parent, id)) return direct;
    ChildLookup lookup{id}; EnumChildWindows(parent, findChild, reinterpret_cast<LPARAM>(&lookup));
    return lookup.result;
}

RECT remoteRect(const Process& process, HWND control, UINT message, WPARAM parameter, RECT initial = {}) {
    void* remote = VirtualAllocEx(process.info.hProcess, nullptr, sizeof(RECT), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    require(remote != nullptr, "Could not allocate native-control test data");
    SIZE_T copied = 0;
    const BOOL written = WriteProcessMemory(process.info.hProcess, remote, &initial, sizeof(initial), &copied);
    if (!written || copied != sizeof(initial)) { VirtualFreeEx(process.info.hProcess, remote, 0, MEM_RELEASE); throw std::runtime_error("Could not write native-control test data"); }
    SendMessageW(control, message, parameter, reinterpret_cast<LPARAM>(remote));
    RECT result{};
    const BOOL read = ReadProcessMemory(process.info.hProcess, remote, &result, sizeof(result), &copied);
    VirtualFreeEx(process.info.hProcess, remote, 0, MEM_RELEASE);
    require(read && copied == sizeof(result), "Could not read native-control rectangle");
    return result;
}

void moveScreen(POINT point) {
    const int left = GetSystemMetrics(SM_XVIRTUALSCREEN), top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int width = GetSystemMetrics(SM_CXVIRTUALSCREEN), height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    INPUT event{}; event.type = INPUT_MOUSE;
    event.mi.dx = MulDiv(point.x - left, 65535, width - 1);
    event.mi.dy = MulDiv(point.y - top, 65535, height - 1);
    event.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    require(SendInput(1, &event, sizeof(event)) == 1, "Could not move native test pointer");
}
void mouseButton(DWORD flags) {
    INPUT event{}; event.type = INPUT_MOUSE; event.mi.dwFlags = flags;
    require(SendInput(1, &event, sizeof(event)) == 1, "Could not change native test mouse button");
}
void pumpFor(int milliseconds) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (std::chrono::steady_clock::now() < until) {
        pumpMessages(); std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

class DropTarget final : public IDropTarget {
public:
    ULONG references = 1;
    bool enterCalled = false, entered = false, dropped = false, matchedSource = false;
    HRESULT queryResult = E_PENDING;
    DWORD offeredEffects = DROPEFFECT_NONE, effect = DROPEFFECT_NONE, requestedEffect = DROPEFFECT_COPY;
    std::wstring path, error;
    std::filesystem::path destination, expectedSource;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, IID_IDropTarget)) { *object = static_cast<IDropTarget*>(this); AddRef(); return S_OK; }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override { const ULONG result = --references; if (!result) delete this; return result; }
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD, POINTL, DWORD* allowed) override {
        FORMATETC format{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        enterCalled = true; offeredEffects = *allowed;
        queryResult = data ? data->QueryGetData(&format) : E_POINTER;
        entered = queryResult == S_OK;
        *allowed = entered && (*allowed & requestedEffect) ? requestedEffect : DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD* allowed) override {
        *allowed = entered && (*allowed & requestedEffect) ? requestedEffect : DROPEFFECT_NONE; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override { entered = false; return S_OK; }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD, POINTL, DWORD* allowed) override {
        FORMATETC format{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL}; STGMEDIUM medium{};
        *allowed = DROPEFFECT_NONE;
        if (!data || FAILED(data->GetData(&format, &medium))) return DV_E_FORMATETC;
        const HDROP files = reinterpret_cast<HDROP>(medium.hGlobal);
        if (DragQueryFileW(files, 0xffffffffu, nullptr, 0) == 1) {
            const UINT length = DragQueryFileW(files, 0, nullptr, 0);
            path.resize(static_cast<size_t>(length) + 1);
            DragQueryFileW(files, 0, path.data(), length + 1); path.resize(length);
            std::error_code identityError;
            matchedSource = std::filesystem::equivalent(std::filesystem::path(path), expectedSource, identityError) && !identityError;
            bool complete = matchedSource;
            if (complete && requestedEffect == DROPEFFECT_MOVE) {
                complete = CopyFileW(path.c_str(), destination.c_str(), TRUE) != FALSE;
                if (!complete) error = L"Target copy failed: " + std::to_wstring(GetLastError());
                if (complete) {
                    // A real shell move target writes the performed effect only
                    // after the original bytes are safely present at destination.
                    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
                    if (!memory) complete = false;
                    else {
                        void* locked = GlobalLock(memory);
                        if (!locked) { GlobalFree(memory); complete = false; }
                        else {
                            *static_cast<DWORD*>(locked) = DROPEFFECT_MOVE; GlobalUnlock(memory);
                            FORMATETC performed{static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"Performed DropEffect")), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
                            STGMEDIUM stored{}; stored.tymed = TYMED_HGLOBAL; stored.hGlobal = memory;
                            const HRESULT accepted = data->SetData(&performed, &stored, TRUE);
                            if (FAILED(accepted)) { GlobalFree(memory); complete = false; error = L"Source rejected performed MOVE: " + std::to_wstring(accepted); }
                        }
                    }
                }
            }
            if (complete) { dropped = true; effect = requestedEffect; *allowed = requestedEffect; }
        }
        ReleaseStgMedium(&medium); return dropped ? S_OK : DV_E_FORMATETC;
    }
};

class DropWindow {
public:
    HWND window{}; DropTarget* target = new DropTarget;
    explicit DropWindow(const RECT& source, DWORD effect, const std::filesystem::path& destination, const std::filesystem::path& expected) {
        target->requestedEffect = effect; target->destination = destination; target->expectedSource = expected;
        WNDCLASSW cls{}; cls.lpfnWndProc = DefWindowProcW; cls.hInstance = GetModuleHandleW(nullptr);
        cls.hCursor = LoadCursorW(nullptr, IDC_ARROW); cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1); cls.lpszClassName = L"AbserSmokeDropTarget";
        RegisterClassW(&cls);
        RECT work{}; SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        const int width = 230, height = 160;
        int x = source.right + 30; if (x + width > work.right) x = std::max(work.left, source.left - width - 30);
        window = CreateWindowExW(WS_EX_TOPMOST, cls.lpszClassName, L"Native file drop test", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                x, source.top + 90, width, height, nullptr, nullptr, cls.hInstance, nullptr);
        require(window && SUCCEEDED(RegisterDragDrop(window, target)), "Could not register native file-drop target");
        UpdateWindow(window);
    }
    ~DropWindow() { if (window) { RevokeDragDrop(window); DestroyWindow(window); } target->Release(); }
};

std::string readText(const std::filesystem::path& path);
void testFileDrag(Process& process, HWND list, const std::filesystem::path& expected,
                  DWORD effect = DROPEFFECT_COPY, const std::filesystem::path& destination = {}) {
    const std::string original = readText(expected);
    focusControl(process.window, list); key(list, VK_HOME);
    SendMessageW(list, LVM_ENSUREVISIBLE, 0, FALSE);
    waitFor([&] { return SendMessageW(list, LVM_GETNEXTITEM, static_cast<WPARAM>(-1), LVNI_SELECTED) == 0 && SendMessageW(list, LVM_GETTOPINDEX, 0, 0) == 0; },
            "Could not position first filename for drag");
    RECT sourceWindow{}; GetWindowRect(process.window, &sourceWindow); DropWindow sink(sourceWindow, effect, destination, expected);
    RECT row = remoteRect(process, list, LVM_GETITEMRECT, 0);
    require(row.bottom > row.top, "Could not locate native filename row for dragging");
    const int catWidth = static_cast<int>(SendMessageW(list, LVM_GETCOLUMNWIDTH, 0, 0));
    POINT source{catWidth + 35, (row.top + row.bottom) / 2}; ClientToScreen(list, &source);
    RECT dropRect{}; GetClientRect(sink.window, &dropRect);
    POINT drop{(dropRect.left + dropRect.right) / 2, (dropRect.top + dropRect.bottom) / 2}; ClientToScreen(sink.window, &drop);
    require(WindowFromPoint(source) == list, "Filename drag start is covered by another native window");
    require(WindowFromPoint(drop) == sink.window, "File-drop target is covered by another native window");
    std::cout << "Native drag source=" << source.x << ',' << source.y << " target=" << drop.x << ',' << drop.y << '\n';
    focusControl(process.window, list);
    // A completed filename click restarts the fixture before the drag, keeping
    // the decoder's real file handle open while the MOVE target completes.
    clickScreen(source); pumpFor(80);
    moveScreen(source); mouseButton(MOUSEEVENTF_LEFTDOWN); pumpFor(50);
    moveScreen({source.x + 25, source.y + 2}); pumpFor(80); moveScreen(drop);
    try {
        waitFor([&] { require(process.alive(), "App exited starting file drag"); return sink.target->enterCalled; },
                "Dragged filename did not enter native CF_HDROP target", 4000);
        require(sink.target->entered, "Native file drag did not offer CF_HDROP");
        require((sink.target->offeredEffects & effect) != 0, "Native file drag did not offer requested copy/move effect");
        mouseButton(MOUSEEVENTF_LEFTUP);
        waitFor([&] { return sink.target->dropped; }, "Native file drop was not completed", 3000);
        waitFor([&] {
            require(process.alive(), "App exited completing native file drag");
            return GetPropW(process.window, L"Abser.Smoke.DragActive") == nullptr;
        }, "Source OLE drag loop did not return after the drop", 4000);
    } catch (...) {
        mouseButton(MOUSEEVENTF_LEFTUP);
        try {
            if (IsWindow(process.window)) captureWindow(process.window, L"ui-smoke-drag-source-failed.bmp");
            if (IsWindow(sink.window)) captureWindow(sink.window, L"ui-smoke-drag-target-failed.bmp");
        } catch (...) { /* Preserve the original drag failure if capture is unavailable. */ }
        POINT cursor{}; GetCursorPos(&cursor);
        GUITHREADINFO sourceGui{sizeof(sourceGui)};
        GetGUIThreadInfo(GetWindowThreadProcessId(process.window, nullptr), &sourceGui);
        std::wcerr << L"Native drop error: " << sink.target->error
                   << L"; entered=" << sink.target->enterCalled << L"; formatHRESULT=" << sink.target->queryResult
                   << L"; offeredEffects=" << sink.target->offeredEffects
                   << L"; dragSerial=" << reinterpret_cast<UINT_PTR>(GetPropW(process.window, L"Abser.Smoke.DragSerial"))
                   << L"; dragActive=" << reinterpret_cast<UINT_PTR>(GetPropW(process.window, L"Abser.Smoke.DragActive"))
                   << L"; dragHRESULT=" << reinterpret_cast<UINT_PTR>(GetPropW(process.window, L"Abser.Smoke.DragResult"))
                   << L"; cursor=" << cursor.x << L',' << cursor.y
                   << L"; capture=" << sourceGui.hwndCapture << L"; focus=" << sourceGui.hwndFocus << L'\n';
        throw;
    }
    require(sink.target->matchedSource, "File drag supplied a different original file");
    require(sink.target->effect == effect, "File drag did not honor native target effect");
    if (effect == DROPEFFECT_MOVE) {
        waitFor([&] {
            std::error_code error;
            const bool exists = std::filesystem::exists(expected, error);
            return !error && !exists;
        }, "Completed native MOVE did not remove original file");
        require(std::filesystem::is_regular_file(destination) && readText(destination) == original, "Native MOVE destination bytes do not match original");
        waitFor([&] { return SendMessageW(list, LVM_GETITEMCOUNT, 0, 0) == kFixtureCount - 1; }, "File list did not refresh after a native move");
        std::cout << "Native CF_HDROP MOVE passed with exact destination bytes.\n";
    } else {
        require(std::filesystem::is_regular_file(expected) && readText(expected) == original, "Copy drag removed or changed source file");
        std::cout << "Native CF_HDROP COPY passed.\n";
    }
}

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

bool renderedWaveform(HWND window) {
    RECT client{}; GetClientRect(window, &client); const UINT dpi = GetDpiForWindow(window);
    const int width = client.right, height = client.bottom;
    if (width <= 0 || height <= MulDiv(150, dpi, 96)) return false;
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height; info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
    HDC screen = GetDC(nullptr), memory = CreateCompatibleDC(screen); void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0); ReleaseDC(nullptr, screen);
    if (!memory || !bitmap || !pixels) { if (bitmap) DeleteObject(bitmap); if (memory) DeleteDC(memory); return false; }
    HGDIOBJ old = SelectObject(memory, bitmap); const BOOL printed = PrintWindow(window, memory, PW_CLIENTONLY | 2); GdiFlush();
    const int x1 = MulDiv(12, dpi, 96), x2 = width - x1;
    const int middle = height - MulDiv(121, dpi, 96), y = middle - MulDiv(5, dpi, 96);
    int columns = 0;
    if (printed && y >= 0 && y < height) {
        const auto* row = static_cast<const uint32_t*>(pixels) + static_cast<size_t>(y) * width;
        for (int x = x1; x < x2; ++x) {
            const uint32_t rgb = row[x] & 0xffffffu;
            if (rgb == 0x69777eu || rgb == 0xc3dba4u) ++columns;
        }
    }
    SelectObject(memory, old); DeleteObject(bitmap); DeleteDC(memory);
    return columns > (x2 - x1) / 2;
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
    for (int i = 0; i < kFixtureCount; ++i) makeWave(audio / (L"take" + std::to_wstring(i + 1) + L".wav"));

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
        path = childControl(process.window, kPath); list = childControl(process.window, kList);
        if (!path || !list || !GetDlgItem(process.window, kFilter + 10)) return false;
        const HWND header = reinterpret_cast<HWND>(SendMessageW(list, LVM_GETHEADER, 0, 0));
        return header && SendMessageW(header, HDM_GETITEMCOUNT, 0, 0) == 4;
    }, "Required native controls were not created");
    waitFor([&] { require(process.alive(), "Abser exited loading initial folder"); return SendMessageW(list, LVM_GETITEMCOUNT, 0, 0) == kFixtureCount; },
            "Initial folder was not loaded");
    HWND header = reinterpret_cast<HWND>(SendMessageW(list, LVM_GETHEADER, 0, 0));
    require(header && SendMessageW(header, HDM_GETITEMCOUNT, 0, 0) == 4, "File list must have four columns");
    RECT windowRect{}; GetWindowRect(process.window, &windowRect);
    const UINT dpi = GetDpiForWindow(process.window);
    require(windowRect.right - windowRect.left <= MulDiv(600, dpi, 96), "Default window is unexpectedly wide");
    captureWindow(process.window, L"ui-smoke-default.bmp");
    RECT pathRect{}, playRect{}, volumeRect{};
    GetWindowRect(path, &pathRect);
    require(pathRect.bottom - pathRect.top <= MulDiv(24, dpi, 96), "Address field is not compact");
    require((GetWindowLongPtrW(path, GWL_EXSTYLE) & WS_EX_CLIENTEDGE) == 0 &&
            (GetWindowLongPtrW(path, GWL_STYLE) & WS_BORDER) == 0, "Address field still has a native border");
    HWND play = childControl(process.window, kPlay), volume = childControl(process.window, kVolume);
    require(play && volume, "Transport or volume control is missing");
    GetWindowRect(play, &playRect); GetWindowRect(volume, &volumeRect);
    require(volumeRect.top - playRect.bottom >= MulDiv(10, dpi, 96), "Transport and volume need a visible margin");
    // Start far from the desired point so a native five-step page change cannot pass.
    SendMessageW(volume, TBM_SETPOS, TRUE, 5);
    const RECT channel = remoteRect(process, volume, TBM_GETCHANNELRECT, 0);
    require(channel.right > channel.left, "Volume channel rectangle is invalid");
    POINT volumeClick{channel.left + (channel.right - channel.left) * 3 / 4, (channel.top + channel.bottom) / 2};
    ClientToScreen(volume, &volumeClick); clickScreen(volumeClick);
    waitFor([&] { const auto value = SendMessageW(volume, TBM_GETPOS, 0, 0); return value >= 71 && value <= 79; },
            "Volume track click did not jump to the clicked position");

    // Native Down travels through the application's outer keyboard loop.
    focusControl(process.window, list); key(list, VK_DOWN);
    waitFor([&] { return SendMessageW(list, LVM_GETNEXTITEM, static_cast<WPARAM>(-1), LVNI_SELECTED) == 0; }, "Down did not select the first file");
    waitFor([&] { return renderedWaveform(process.window); }, "Audio waveform was not rendered in the footer", 4000);
    pumpFor(100); captureWindow(process.window, L"ui-smoke-waveform.bmp");
    key(list, VK_END);
    waitFor([&] { return SendMessageW(list, LVM_GETNEXTITEM, static_cast<WPARAM>(-1), LVNI_SELECTED) == kFixtureCount - 1; }, "End did not select the final file");
    key(list, VK_SPACE); key(list, VK_SPACE);

    // Previously selected final index must not be used against the new empty model.
    openByEnter(process, path, list, empty, 0);
    openByEnter(process, path, list, audio, kFixtureCount);

    // All fixture files start unassigned. Its checkbox must hide/reveal all rows.
    HWND unassigned = GetDlgItem(process.window, kFilter);
    require(unassigned != nullptr, "Unassigned colour filter is missing");
    SendMessageW(unassigned, BM_CLICK, 0, 0);
    waitFor([&] { return SendMessageW(list, LVM_GETITEMCOUNT, 0, 0) == 0; }, "Unassigned filter did not hide files");
    SendMessageW(unassigned, BM_CLICK, 0, 0);
    waitFor([&] { return SendMessageW(list, LVM_GETITEMCOUNT, 0, 0) == kFixtureCount; }, "Unassigned filter did not restore files");

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
    waitFor([&] { return SendMessageW(list, LVM_GETITEMCOUNT, 0, 0) == kFixtureCount; },
            "Assigned and unassigned files did not restore together");

    HWND vertical = nullptr;
    waitFor([&] { vertical = childControl(process.window, kVerticalRail); return vertical && IsWindowVisible(vertical); },
            "Custom vertical scrollbar is missing");
    HWND smooth = childControl(process.window, 0x6A13);
    require(smooth != nullptr, "Smooth-scroll compositor is missing");
    SendMessageW(list, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(-WHEEL_DELTA)), 0);
    require(IsWindowVisible(smooth), "Wheel scrolling did not start the smooth compositor");
    captureWindow(process.window, L"ui-smoke-smooth-scroll.bmp");
    waitFor([&] { return !IsWindowVisible(smooth); }, "Smooth-scroll compositor did not settle", 2000);
    require(SendMessageW(list, LVM_GETTOPINDEX, 0, 0) > 0, "Wheel scroll did not change the file list position");
    RECT verticalRect{}; GetWindowRect(vertical, &verticalRect);
    require(verticalRect.right - verticalRect.left <= MulDiv(10, dpi, 96), "Vertical scrollbar is not slim");
    clickScreen({(verticalRect.left + verticalRect.right) / 2,
                 verticalRect.top + (verticalRect.bottom - verticalRect.top) * 4 / 5});
    waitFor([&] { return SendMessageW(list, LVM_GETTOPINDEX, 0, 0) > 0; }, "Custom vertical scrollbar did not move the file list");
    captureWindow(process.window, L"ui-smoke-vertical-scroll.bmp");
    testFileDrag(process, list, audio / L"take1.wav");
    const auto moved = root / L"Moved file \u65e5\u672c\u8a9e.wav";
    testFileDrag(process, list, audio / L"take1.wav", DROPEFFECT_MOVE, moved);

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
    HWND horizontal = nullptr;
    waitFor([&] { horizontal = childControl(process.window, kHorizontalRail); return horizontal && IsWindowVisible(horizontal); },
            "Custom horizontal scrollbar is missing at narrow width");
    RECT horizontalRect{}; GetWindowRect(horizontal, &horizontalRect);
    require(horizontalRect.bottom - horizontalRect.top <= MulDiv(10, dpi, 96), "Horizontal scrollbar is not slim");
    clickScreen({horizontalRect.left + (horizontalRect.right - horizontalRect.left) * 4 / 5,
                 (horizontalRect.top + horizontalRect.bottom) / 2});
    waitFor([&] { return GetScrollPos(list, SB_HORZ) > 0; }, "Custom horizontal scrollbar did not move the columns");
    captureWindow(process.window, L"ui-smoke-horizontal-scroll.bmp");

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
    const auto volumeStart = saved.find("S volume ");
    require(volumeStart != std::string::npos, "Volume setting was not saved");
    std::istringstream volumeSetting(saved.substr(volumeStart + 9)); float savedVolume = 0;
    volumeSetting >> savedVolume;
    require(savedVolume >= 0.71f && savedVolume <= 0.79f, "Clicked volume position was not persisted");
    for (const auto& file : std::filesystem::directory_iterator(audio))
        require(file.path().extension() == L".wav", "App wrote metadata into the audio folder");
    std::cout << "UI smoke passed: Unicode paths, Enter, selection, empty folder, filters, category menu, compact path, waveform capture, click-volume, slim scrolling, native file drag, sorting, resizing, persistence, clean exit.\n";
    return 0;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HRESULT initialized = OleInitialize(nullptr);
    if (FAILED(initialized)) { std::cerr << "Could not initialize native drop test.\n"; return 1; }
    int result = 1;
    try {
        require(argc == 2, "Usage: abser_ui_smoke.exe <Abser.exe>");
        result = run(std::filesystem::absolute(argv[1]));
    } catch (const std::exception& error) {
        std::cerr << "UI smoke failed: " << error.what() << '\n';
    }
    OleUninitialize(); return result;
}
