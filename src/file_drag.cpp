#include "file_drag.h"

#ifdef _WIN32
#include <shellapi.h>
#include <shlobj.h>
#include <shldisp.h>

#include <cstring>
#include <vector>

namespace abser {
namespace {

template<class T> struct ComOwner {
    T* value = nullptr;
    ComOwner() = default;
    ComOwner(const ComOwner&) = delete;
    ComOwner& operator=(const ComOwner&) = delete;
    ~ComOwner() { if (value) value->Release(); }
    T** put() { return &value; }
    T* operator->() const { return value; }
};

struct PidlOwner {
    PIDLIST_ABSOLUTE value = nullptr;
    ~PidlOwner() { CoTaskMemFree(value); }
};

struct HandleOwner {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~HandleOwner() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};

HRESULT lastError() {
    const DWORD error = GetLastError();
    return error ? HRESULT_FROM_WIN32(error) : E_FAIL;
}

HRESULT fullFilePath(const std::wstring& input, std::wstring& output) {
    if (input.empty() || input.find(L'\0') != std::wstring::npos) return E_INVALIDARG;
    const DWORD size = GetFullPathNameW(input.c_str(), 0, nullptr, nullptr);
    if (!size) return lastError();
    if (size > 32768) return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
    std::vector<wchar_t> buffer(size);
    const DWORD written = GetFullPathNameW(input.c_str(), size, buffer.data(), nullptr);
    if (!written) return lastError();
    if (written >= size) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    output.assign(buffer.data(), written);
    const DWORD attributes = GetFileAttributesW(output.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return lastError();
    if (attributes & FILE_ATTRIBUTE_DIRECTORY) return HRESULT_FROM_WIN32(ERROR_DIRECTORY);
    return S_OK;
}

HRESULT shellObject(const std::wstring& fullPath, IDataObject** object) {
    PidlOwner absolute;
    HRESULT hr = SHParseDisplayName(fullPath.c_str(), nullptr, &absolute.value, 0, nullptr);
    if (FAILED(hr)) return hr;
    ComOwner<IShellFolder> parent;
    PCUITEMID_CHILD child = nullptr;
    hr = SHBindToParent(absolute.value, IID_IShellFolder,
                       reinterpret_cast<void**>(parent.put()), &child);
    if (FAILED(hr)) return hr;
    // Asking the actual filesystem folder for its IDataObject preserves all
    // Explorer transfer formats, rather than emulating only CF_HDROP.
    hr = parent->GetUIObjectOf(nullptr, 1, &child, IID_IDataObject, nullptr,
                              reinterpret_cast<void**>(object));
    if (FAILED(hr)) return hr;
    // Keep completion synchronous so the target's performed-effect signal is
    // final before deciding whether an unoptimized move needs source cleanup.
    ComOwner<IDataObjectAsyncCapability> asynchronous;
    if (SUCCEEDED((*object)->QueryInterface(IID_IDataObjectAsyncCapability,
                        reinterpret_cast<void**>(asynchronous.put())))) {
        hr = asynchronous->SetAsyncMode(FALSE);
        if (FAILED(hr)) {
            (*object)->Release();
            *object = nullptr;
            return hr;
        }
    }
    return S_OK;
}

bool readEffect(IDataObject* object, const wchar_t* format, DWORD& effect) {
    const UINT clipboardFormat = RegisterClipboardFormatW(format);
    if (!clipboardFormat) return false;
    FORMATETC requested{static_cast<CLIPFORMAT>(clipboardFormat), nullptr,
                        DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium{};
    if (FAILED(object->GetData(&requested, &medium))) return false;
    bool found = false;
    if (medium.tymed == TYMED_HGLOBAL && medium.hGlobal &&
        GlobalSize(medium.hGlobal) >= sizeof(DWORD)) {
        if (const void* data = GlobalLock(medium.hGlobal)) {
            std::memcpy(&effect, data, sizeof(effect));
            GlobalUnlock(medium.hGlobal);
            found = true;
        }
    }
    ReleaseStgMedium(&medium);
    return found;
}

HRESULT readIdentity(const std::wstring& path, BY_HANDLE_FILE_INFORMATION& identity) {
    HandleOwner file;
    file.value = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file.value == INVALID_HANDLE_VALUE) return lastError();
    if (!GetFileInformationByHandle(file.value, &identity)) return lastError();
    return S_OK;
}

bool sameIdentity(const BY_HANDLE_FILE_INFORMATION& a, const BY_HANDLE_FILE_INFORMATION& b) {
    return a.dwVolumeSerialNumber == b.dwVolumeSerialNumber &&
           a.nFileIndexHigh == b.nFileIndexHigh && a.nFileIndexLow == b.nFileIndexLow &&
           a.nFileSizeHigh == b.nFileSizeHigh && a.nFileSizeLow == b.nFileSizeLow &&
           a.ftLastWriteTime.dwHighDateTime == b.ftLastWriteTime.dwHighDateTime &&
           a.ftLastWriteTime.dwLowDateTime == b.ftLastWriteTime.dwLowDateTime;
}

HRESULT removeUnoptimizedMoveSource(const std::wstring& path,
                                   const BY_HANDLE_FILE_INFORMATION& original) {
    HandleOwner source;
    // Excluding FILE_SHARE_DELETE protects the verified identity against a
    // rename/replacement between checking it and removing that exact handle.
    source.value = CreateFileW(path.c_str(), DELETE | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (source.value == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        // Some targets already finish the move despite returning MOVE.
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return S_OK;
        return HRESULT_FROM_WIN32(error);
    }
    BY_HANDLE_FILE_INFORMATION current{};
    if (!GetFileInformationByHandle(source.value, &current)) return lastError();
    if (!sameIdentity(original, current)) return HRESULT_FROM_WIN32(ERROR_FILE_INVALID);
    FILE_DISPOSITION_INFO disposition{TRUE};
    if (!SetFileInformationByHandle(source.value, FileDispositionInfo, &disposition,
                                   sizeof(disposition))) return lastError();
    return S_OK;
}

bool missingFile(const std::wstring& path) {
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return false;
    const DWORD error = GetLastError();
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

} // namespace

HRESULT createFileDragDataObject(const std::wstring& path, IDataObject** object) {
    if (!object) return E_POINTER;
    *object = nullptr;
    std::wstring fullPath;
    HRESULT hr = fullFilePath(path, fullPath);
    if (FAILED(hr)) return hr;
    return shellObject(fullPath, object);
}

namespace detail {
bool shouldDeleteDraggedSource(HRESULT result, DWORD returnedEffect,
                               bool hasPerformedEffect, DWORD performedEffect) noexcept {
    // https://learn.microsoft.com/en-us/windows/win32/shell/datascenarios
    // Both signals must request MOVE; optimized moves require no source delete.
    return result == DRAGDROP_S_DROP && returnedEffect == DROPEFFECT_MOVE &&
           hasPerformedEffect && performedEffect == DROPEFFECT_MOVE;
}
} // namespace detail

FileDragResult dragFile(HWND owner, const std::wstring& path) {
    FileDragResult result;
    std::wstring fullPath;
    result.result = fullFilePath(path, fullPath);
    if (FAILED(result.result)) return result;
    BY_HANDLE_FILE_INFORMATION original{};
    result.result = readIdentity(fullPath, original);
    if (FAILED(result.result)) return result;
    ComOwner<IDataObject> object;
    result.result = shellObject(fullPath, object.put());
    if (FAILED(result.result)) return result;

    DWORD returnedEffect = DROPEFFECT_NONE;
    result.result = SHDoDragDrop(owner, object.value, nullptr,
        DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK, &returnedEffect);
    if (result.result != DRAGDROP_S_DROP) return result;

    DWORD performed = DROPEFFECT_NONE;
    const bool hasPerformed = readEffect(object.value, L"Performed DropEffect", performed);
    DWORD logical = DROPEFFECT_NONE;
    const bool hasLogical = readEffect(object.value, L"Logical Performed DropEffect", logical);
    result.effect = (hasLogical ? logical : returnedEffect) &
                    (DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK);

    if (detail::shouldDeleteDraggedSource(result.result, returnedEffect, hasPerformed, performed)) {
        const HRESULT cleanup = removeUnoptimizedMoveSource(fullPath, original);
        if (FAILED(cleanup)) result.result = cleanup;
        else result.sourceRemoved = true;
    }
    if (missingFile(fullPath)) {
        result.sourceRemoved = true;
        // Optimized Shell moves can return COPY/NONE because the target already
        // moved the file and no source-side deletion is needed.
        result.effect = DROPEFFECT_MOVE;
    }
    return result;
}

} // namespace abser
#endif
