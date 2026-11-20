#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <memory>

namespace abser {

// Wraps a report ListView without replacing its native navigation, header or
// scrolling model. The host clips its standard nonclient scrollbars; two narrow
// overlay thumbs operate the same native scroll positions.
class SlimScrollbars {
public:
    static constexpr int HostId = 0x6A10;
    static constexpr int VerticalId = 0x6A11;
    static constexpr int HorizontalId = 0x6A12;
    static constexpr int AnimationId = 0x6A13;
    static constexpr const wchar_t* HostClass = L"Abser.SlimScrollHost";
    static constexpr const wchar_t* ThumbClass = L"Abser.SlimScrollThumb";
    static constexpr const wchar_t* AnimationClass = L"Abser.SmoothScrollOverlay";

    SlimScrollbars();
    ~SlimScrollbars();
    SlimScrollbars(const SlimScrollbars&) = delete;
    SlimScrollbars& operator=(const SlimScrollbars&) = delete;

    bool attach(HWND list, HWND originalParent);
    void layout(const RECT& area, unsigned dpi = 96);
    void refresh();
    void detach();
    HWND host() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace abser
