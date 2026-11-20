#pragma once

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objidl.h>
#include <oleidl.h>
#include <string>

namespace abser {

struct FileDragResult {
    // DRAGDROP_S_DROP or DRAGDROP_S_CANCEL, or a failing HRESULT. A failed
    // unoptimized move cleanup is returned here even if the target copied it.
    HRESULT result = E_FAIL;
    // Logical outcome, including optimized Shell moves. Zero on cancellation.
    DWORD effect = DROPEFFECT_NONE;
    bool sourceRemoved = false;
};

// Gets the filesystem Shell's real data object, including Unicode CF_HDROP,
// Shell IDList and the other formats that Explorer offers. The caller owns the
// returned interface and must Release it. Does not modify the source file.
// These functions require an STA UI thread initialized with OleInitialize.
HRESULT createFileDragDataObject(const std::wstring& path, IDataObject** object);

// Enters the native OLE drag loop. Supports Explorer copy/move/link modifiers,
// cancellation and native drag feedback. Call in response to LVN_BEGINDRAG.
FileDragResult dragFile(HWND owner, const std::wstring& path);

namespace detail {
// A source is deleted only after a successful drop where BOTH independent
// Shell completion signals explicitly request an unoptimized move.
bool shouldDeleteDraggedSource(HRESULT result, DWORD returnedEffect,
                               bool hasPerformedEffect, DWORD performedEffect) noexcept;
}

} // namespace abser
#endif
