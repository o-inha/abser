#include "file_drag.h"

#include <ole2.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shldisp.h>

#include <cstring>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace abser;

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct OleFixture {
    HRESULT hr = OleInitialize(nullptr);
    ~OleFixture() { if (SUCCEEDED(hr)) OleUninitialize(); }
};

struct DataObjectOwner {
    IDataObject* object = nullptr;
    ~DataObjectOwner() { if (object) object->Release(); }
};

struct MediumOwner {
    STGMEDIUM medium{};
    ~MediumOwner() { if (medium.tymed != TYMED_NULL) ReleaseStgMedium(&medium); }
};

struct Fixture {
    std::wstring folder;
    std::wstring file;
    const char* payload = "Abser original audio fixture - never rewritten by drag setup";
    Fixture() {
        std::vector<wchar_t> temporary(32768);
        const DWORD length = GetTempPathW(static_cast<DWORD>(temporary.size()), temporary.data());
        require(length != 0 && length < temporary.size(), "Could not locate temporary folder");
        folder.assign(temporary.data(), length);
        folder += L"Abser-drag-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                  std::to_wstring(GetTickCount64()) + L"-日本語 ääni";
        require(CreateDirectoryW(folder.c_str(), nullptr) != FALSE, "Could not create Unicode fixture folder");
        file = folder + L"\\音声 ääni 01.wav";
        HANDLE handle = CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE,
                                   nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        require(handle != INVALID_HANDLE_VALUE, "Could not create Unicode fixture file");
        DWORD written = 0;
        const BOOL success = WriteFile(handle, payload, static_cast<DWORD>(std::strlen(payload)), &written, nullptr);
        CloseHandle(handle);
        require(success && written == std::strlen(payload), "Could not write fixture file");
    }
    ~Fixture() {
        if (!file.empty()) DeleteFileW(file.c_str());
        if (!folder.empty()) RemoveDirectoryW(folder.c_str());
    }
    void verifyOriginal() const {
        HANDLE handle = CreateFileW(file.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        require(handle != INVALID_HANDLE_VALUE, "Drag setup/completion decision removed the original file");
        char bytes[256]{};
        DWORD read = 0;
        const BOOL success = ReadFile(handle, bytes, sizeof(bytes), &read, nullptr);
        CloseHandle(handle);
        require(success && read == std::strlen(payload) && std::memcmp(bytes, payload, read) == 0,
                "Drag setup/completion decision changed the original file");
    }
};

void testShellFormats(Fixture& fixture) {
    DataObjectOwner data;
    require(SUCCEEDED(createFileDragDataObject(fixture.file, &data.object)) && data.object,
            "Could not create real Shell file IDataObject");

    FORMATETC drop{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    require(data.object->QueryGetData(&drop) == S_OK, "Shell data object did not advertise CF_HDROP");
    MediumOwner medium;
    require(SUCCEEDED(data.object->GetData(&drop, &medium.medium)), "Could not obtain CF_HDROP data");
    require(medium.medium.tymed == TYMED_HGLOBAL, "CF_HDROP has incorrect storage medium");
    const HDROP handle = reinterpret_cast<HDROP>(medium.medium.hGlobal);
    require(DragQueryFileW(handle, 0xffffffff, nullptr, 0) == 1, "CF_HDROP did not contain exactly one file");
    const UINT length = DragQueryFileW(handle, 0, nullptr, 0);
    require(length != 0, "CF_HDROP contained an empty path");
    std::vector<wchar_t> dropped(length + 1);
    require(DragQueryFileW(handle, 0, dropped.data(), static_cast<UINT>(dropped.size())) == length,
            "Could not obtain Unicode drop path");
    require(CompareStringOrdinal(fixture.file.c_str(), -1, dropped.data(), -1, TRUE) == CSTR_EQUAL,
            "CF_HDROP path differs from the actual original Unicode file");
    const auto* dropHeader = static_cast<const DROPFILES*>(GlobalLock(medium.medium.hGlobal));
    require(dropHeader != nullptr, "Could not lock DROPFILES structure");
    const bool isUnicode = dropHeader->fWide != FALSE;
    GlobalUnlock(medium.medium.hGlobal);
    require(isUnicode, "Shell file transfer should preserve Unicode paths");

    const UINT shellFormat = RegisterClipboardFormatW(L"Shell IDList Array");
    require(shellFormat != 0, "Could not register Shell IDList format");
    FORMATETC idList{static_cast<CLIPFORMAT>(shellFormat), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    require(data.object->QueryGetData(&idList) == S_OK, "Shell IDList format is missing");
    MediumOwner identifiers;
    require(SUCCEEDED(data.object->GetData(&idList, &identifiers.medium)) &&
            identifiers.medium.tymed == TYMED_HGLOBAL, "Could not obtain Shell IDList data");
    const SIZE_T size = GlobalSize(identifiers.medium.hGlobal);
    const auto* cida = static_cast<const CIDA*>(GlobalLock(identifiers.medium.hGlobal));
    require(cida != nullptr, "Could not lock Shell IDList data");
    const bool valid = size >= sizeof(UINT) * 3 && cida->cidl == 1 &&
                       cida->aoffset[0] < size && cida->aoffset[1] < size;
    GlobalUnlock(identifiers.medium.hGlobal);
    require(valid, "Shell IDList did not contain a valid parent and one child");

    IDataObjectAsyncCapability* asynchronous = nullptr;
    if (SUCCEEDED(data.object->QueryInterface(IID_IDataObjectAsyncCapability,
                        reinterpret_cast<void**>(&asynchronous)))) {
        BOOL enabled = TRUE;
        const HRESULT hr = asynchronous->GetAsyncMode(&enabled);
        asynchronous->Release();
        require(SUCCEEDED(hr) && !enabled, "Drag completion must wait for target extraction");
    }
    fixture.verifyOriginal();
}

void testInvalidPaths(const Fixture& fixture) {
    IDataObject* data = reinterpret_cast<IDataObject*>(static_cast<uintptr_t>(1));
    require(FAILED(createFileDragDataObject(L"", &data)) && data == nullptr,
            "Empty path should fail and clear output pointer");
    require(FAILED(createFileDragDataObject(fixture.folder, &data)) && data == nullptr,
            "Dragging a directory is outside the single-audio-file API");
    require(FAILED(createFileDragDataObject(fixture.file + L".missing", &data)) && data == nullptr,
            "Missing files must not produce a fabricated file transfer");
    require(createFileDragDataObject(fixture.file, nullptr) == E_POINTER, "Null output pointer should fail cleanly");
    const auto invalidDrag = dragFile(nullptr, fixture.file + L".missing");
    require(FAILED(invalidDrag.result) && invalidDrag.effect == DROPEFFECT_NONE && !invalidDrag.sourceRemoved,
            "Invalid drag should not enter an interactive drag loop or imply a drop");
}

void testMoveCompletionDecision(Fixture& fixture) {
    using abser::detail::shouldDeleteDraggedSource;
    const DWORD effects[] = {DROPEFFECT_NONE, DROPEFFECT_COPY, DROPEFFECT_MOVE, DROPEFFECT_LINK,
                             DROPEFFECT_COPY | DROPEFFECT_MOVE};
    const HRESULT outcomes[] = {DRAGDROP_S_DROP, DRAGDROP_S_CANCEL, S_OK, E_FAIL};
    for (const HRESULT outcome : outcomes) {
        for (const DWORD returned : effects) {
            for (const DWORD performed : effects) {
                const bool expected = outcome == DRAGDROP_S_DROP && returned == DROPEFFECT_MOVE &&
                                      performed == DROPEFFECT_MOVE;
                require(shouldDeleteDraggedSource(outcome, returned, true, performed) == expected,
                        "Unoptimized move cleanup was permitted without both successful MOVE signals");
                require(!shouldDeleteDraggedSource(outcome, returned, false, performed),
                        "Missing Shell completion flag must never permit source deletion");
            }
        }
    }
    // Decision checks themselves and optimized/cancelled/copy/link setup have no
    // file side effects. Actual deletion is reachable only after SHDoDragDrop.
    fixture.verifyOriginal();
}

} // namespace

int main() {
    try {
        OleFixture ole;
        require(SUCCEEDED(ole.hr), "OleInitialize failed");
        Fixture fixture;
        testShellFormats(fixture);
        testInvalidPaths(fixture);
        testMoveCompletionDecision(fixture);
        std::cout << "Native file drag tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "File drag test failed: " << error.what() << '\n';
        return 1;
    }
}
