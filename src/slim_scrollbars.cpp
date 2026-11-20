#include "slim_scrollbars.h"

#include <windowsx.h>
#include <commctrl.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

namespace abser {
namespace {
constexpr UINT kRefreshMessage = WM_APP + 0x60;
constexpr UINT_PTR kSubclassId = 0x6A14;
constexpr UINT_PTR kAnimationTimer = 0x6A15;
constexpr COLORREF kPanel = RGB(29,32,36);
constexpr COLORREF kThumb = RGB(91,99,107);
constexpr COLORREF kThumbHot = RGB(141,151,160);

struct Bitmap {
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ previous = nullptr;
    int width = 0, height = 0;
    Bitmap() = default;
    ~Bitmap() { reset(); }
    Bitmap(const Bitmap&) = delete;
    Bitmap& operator=(const Bitmap&) = delete;
    Bitmap(Bitmap&& other) noexcept { swap(other); }
    Bitmap& operator=(Bitmap&& other) noexcept { if(this != &other) {reset();swap(other);}return *this; }
    void swap(Bitmap& other) noexcept {
        std::swap(dc,other.dc);std::swap(bitmap,other.bitmap);std::swap(previous,other.previous);
        std::swap(width,other.width);std::swap(height,other.height);
    }
    void reset() {
        if(dc && previous)SelectObject(dc,previous);
        if(bitmap)DeleteObject(bitmap);
        if(dc)DeleteDC(dc);
        dc=nullptr;bitmap=nullptr;previous=nullptr;width=height=0;
    }
    bool create(HDC reference,int w,int h) {
        reset();if(w<=0||h<=0)return false;
        dc=CreateCompatibleDC(reference);if(!dc)return false;
        bitmap=CreateCompatibleBitmap(reference,w,h);if(!bitmap){reset();return false;}
        previous=SelectObject(dc,bitmap);width=w;height=h;return true;
    }
};
void fill(HDC dc,const RECT& rect,COLORREF color) {
    const HBRUSH brush=CreateSolidBrush(color);FillRect(dc,&rect,brush);DeleteObject(brush);
}
int boundedInt(int64_t value) {
    return static_cast<int>(std::clamp<int64_t>(value,std::numeric_limits<int>::min(),std::numeric_limits<int>::max()));
}
} // namespace

struct SlimScrollbars::Impl {
    struct Rail {
        Impl* owner = nullptr;
        HWND window = nullptr;
        bool vertical = false, hot = false, dragging = false, visible = false;
        int dragOffset = 0, minimum = 0, maximum = 0, position = 0, page = 0;
        int extent = 0, thumbStart = 0, thumbLength = 0;
        RECT bounds{};
        Bitmap canvas;
    };

    HWND list = nullptr, parent = nullptr, clip = nullptr, overlay = nullptr;
    RECT originalRect{}, area{};
    Rail vertical{}, horizontal{};
    unsigned dpi = 96;
    int width = 0, height = 0, bodyTop = 0;
    bool refreshing = false, refreshPosted = false, scrolling = false;
    bool animating = false;
    ULONGLONG animationStarted = 0;
    double animationFrom = 0, animationTo = 0, animationShift = 0;
    double wheelRowsRemainder = 0;
    Bitmap previousFrame, nextFrame, composedFrame;

    int scale(int value) const { return std::max(1,MulDiv(value,static_cast<int>(dpi),96)); }
    int railSize() const { return scale(8); }
    int bodyHeight() const { return std::max(0,height-bodyTop); }
    int rowHeight() const {
        RECT rect{};const int count=ListView_GetItemCount(list);
        if(count>0&&ListView_GetItemRect(list,std::max(0,ListView_GetTopIndex(list)),&rect,LVIR_BOUNDS))
            return std::max(1,static_cast<int>(rect.bottom-rect.top));
        return scale(29);
    }
    double progress() const {
        if(!animating)return 1;
        const double t=std::clamp(static_cast<double>(GetTickCount64()-animationStarted)/120.0,0.0,1.0);
        return 1.0-(1.0-t)*(1.0-t)*(1.0-t);
    }
    double visualPosition() const { return animationFrom+(animationTo-animationFrom)*progress(); }

    static bool registerClasses() {
        const auto registerOne=[](const wchar_t* name,WNDPROC procedure) {
            WNDCLASSEXW wc{};wc.cbSize=sizeof(wc);wc.hInstance=GetModuleHandleW(nullptr);
            wc.lpszClassName=name;wc.lpfnWndProc=procedure;wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);
            return RegisterClassExW(&wc)!=0||GetLastError()==ERROR_CLASS_ALREADY_EXISTS;
        };
        return registerOne(HostClass,HostProc)&&registerOne(ThumbClass,RailProc)&&registerOne(AnimationClass,OverlayProc);
    }
    bool attach(HWND nativeList,HWND originalParent) {
        detach();if(!IsWindow(nativeList)||!IsWindow(originalParent)||!registerClasses())return false;
        list=nativeList;parent=originalParent;
        GetWindowRect(list,&originalRect);MapWindowPoints(HWND_DESKTOP,parent,reinterpret_cast<POINT*>(&originalRect),2);
        clip=CreateWindowExW(WS_EX_CONTROLPARENT,HostClass,L"Audio files viewport",WS_CHILD|WS_VISIBLE|WS_CLIPCHILDREN|WS_CLIPSIBLINGS,
            originalRect.left,originalRect.top,std::max(1,static_cast<int>(originalRect.right-originalRect.left)),
            std::max(1,static_cast<int>(originalRect.bottom-originalRect.top)),parent,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(HostId)),GetModuleHandleW(nullptr),this);
        if(!clip){list=nullptr;parent=nullptr;return false;}
        SetParent(list,clip);
        SetWindowLongPtrW(list,GWL_STYLE,GetWindowLongPtrW(list,GWL_STYLE)|WS_CLIPSIBLINGS);
        if(!SetWindowSubclass(list,ListProc,kSubclassId,reinterpret_cast<DWORD_PTR>(this))){detach();return false;}
        overlay=CreateWindowExW(WS_EX_TRANSPARENT|WS_EX_NOACTIVATE,AnimationClass,L"Smooth scrolling",WS_CHILD|WS_CLIPSIBLINGS,
            0,0,0,0,clip,reinterpret_cast<HMENU>(static_cast<INT_PTR>(AnimationId)),GetModuleHandleW(nullptr),this);
        vertical.owner=this;vertical.vertical=true;
        horizontal.owner=this;horizontal.vertical=false;
        vertical.window=createRail(vertical,VerticalId,L"Vertical scroll");
        horizontal.window=createRail(horizontal,HorizontalId,L"Horizontal scroll");
        if(!overlay||!vertical.window||!horizontal.window){detach();return false;}
        area=originalRect;refresh();return true;
    }
    HWND createRail(Rail& rail,int id,const wchar_t* name) {
        return CreateWindowExW(WS_EX_NOACTIVATE,ThumbClass,name,WS_CHILD|WS_CLIPSIBLINGS,
            0,0,0,0,clip,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),&rail);
    }
    void detach() {
        cancelAnimation();
        if(IsWindow(list)) {
            RemoveWindowSubclass(list,ListProc,kSubclassId);
            if(IsWindow(parent)){
                SetParent(list,parent);
                SetWindowPos(list,nullptr,originalRect.left,originalRect.top,
                    std::max(1,static_cast<int>(originalRect.right-originalRect.left)),
                    std::max(1,static_cast<int>(originalRect.bottom-originalRect.top)),SWP_NOZORDER|SWP_NOACTIVATE);
            }
        }
        if(IsWindow(clip))DestroyWindow(clip);
        list=parent=clip=overlay=nullptr;vertical=Rail{};horizontal=Rail{};
        refreshing=refreshPosted=false;width=height=bodyTop=0;
    }
    void layout(const RECT& rect,unsigned newDpi) {
        if(!IsWindow(clip))return;
        cancelAnimation();dpi=newDpi?newDpi:96;area=rect;
        SetWindowPos(clip,nullptr,rect.left,rect.top,std::max(0,static_cast<int>(rect.right-rect.left)),
            std::max(0,static_cast<int>(rect.bottom-rect.top)),SWP_NOZORDER|SWP_NOACTIVATE);
        refresh();
    }
    void scheduleRefresh() {
        if(refreshing||refreshPosted||!IsWindow(clip))return;
        refreshPosted=PostMessageW(clip,kRefreshMessage,0,0)!=FALSE;
    }
    void queryRail(Rail& rail) {
        SCROLLINFO info{};info.cbSize=sizeof(info);info.fMask=SIF_RANGE|SIF_PAGE|SIF_POS;
        const int bar=rail.vertical?SB_VERT:SB_HORZ;
        const LONG_PTR style=GetWindowLongPtrW(list,GWL_STYLE);
        const bool nativeVisible=(style&(rail.vertical?WS_VSCROLL:WS_HSCROLL))!=0;
        const bool hasInfo=GetScrollInfo(list,bar,&info)!=FALSE;
        rail.minimum=hasInfo?info.nMin:0;
        rail.maximum=hasInfo?std::max(info.nMin,boundedInt(static_cast<int64_t>(info.nMax)-std::max<UINT>(info.nPage,1)+1)):0;
        rail.page=hasInfo?boundedInt(info.nPage):0;
        rail.position=hasInfo?std::clamp(info.nPos,rail.minimum,rail.maximum):0;
        rail.visible=nativeVisible&&hasInfo&&rail.maximum>rail.minimum;
    }
    void geometry(Rail& rail) {
        const RECT oldBounds=rail.bounds;
        const int oldStart=rail.thumbStart,oldLength=rail.thumbLength;
        const int inset=scale(3),size=railSize();
        if(rail.vertical)rail.bounds={width-size,bodyTop+inset,width,height-(horizontal.visible?size:0)-inset};
        else rail.bounds={inset,height-size,width-(vertical.visible?size:0)-inset,height};
        rail.extent=std::max(0,static_cast<int>(rail.vertical?rail.bounds.bottom-rail.bounds.top:rail.bounds.right-rail.bounds.left));
        const int64_t total=static_cast<int64_t>(rail.maximum)-rail.minimum+std::max(rail.page,1);
        rail.thumbLength=std::min(rail.extent,std::max(scale(22),static_cast<int>(std::lround(static_cast<double>(rail.extent)*std::max(rail.page,1)/std::max<int64_t>(1,total)))));
        double pos=rail.vertical&&animating?visualPosition():rail.position;
        const double fraction=rail.maximum>rail.minimum?std::clamp((pos-rail.minimum)/(rail.maximum-rail.minimum),0.0,1.0):0;
        rail.thumbStart=static_cast<int>(std::lround(fraction*std::max(0,rail.extent-rail.thumbLength)));
        if(!rail.visible||rail.extent<=0){ShowWindow(rail.window,SW_HIDE);return;}
        SetWindowPos(rail.window,HWND_TOP,rail.bounds.left,rail.bounds.top,
            std::max(0,static_cast<int>(rail.bounds.right-rail.bounds.left)),std::max(0,static_cast<int>(rail.bounds.bottom-rail.bounds.top)),SWP_NOACTIVATE|SWP_SHOWWINDOW);
        if(oldStart!=rail.thumbStart||oldLength!=rail.thumbLength||!EqualRect(&oldBounds,&rail.bounds))InvalidateRect(rail.window,nullptr,FALSE);
    }
    void refresh() {
        if(refreshing||!IsWindow(list)||!IsWindow(clip))return;
        refreshing=true;
        RECT client{};GetClientRect(clip,&client);width=static_cast<int>(client.right);height=static_cast<int>(client.bottom);
        if(width>0&&height>0){
            // Keep the native *client* exactly as large as the visible viewport.
            // Its auto-added nonclient bars then live beyond the parent's clip.
            // Recheck because adding one native bar can cause the other to appear.
            for(int attempt=0;attempt<5;++attempt){
                RECT outer{},inside{};GetWindowRect(list,&outer);GetClientRect(list,&inside);
                const int extraW=std::max(0,static_cast<int>((outer.right-outer.left)-inside.right));
                const int extraH=std::max(0,static_cast<int>((outer.bottom-outer.top)-inside.bottom));
                const int desiredW=width+extraW,desiredH=height+extraH;
                POINT origin{0,0};ClientToScreen(clip,&origin);
                if(outer.left==origin.x&&outer.top==origin.y&&outer.right-outer.left==desiredW&&outer.bottom-outer.top==desiredH)break;
                SetWindowPos(list,HWND_BOTTOM,0,0,desiredW,desiredH,SWP_NOACTIVATE);
            }
            RECT header{};const HWND headerWindow=ListView_GetHeader(list);
            if(IsWindow(headerWindow)&&IsWindowVisible(headerWindow)){
                GetWindowRect(headerWindow,&header);MapWindowPoints(HWND_DESKTOP,list,reinterpret_cast<POINT*>(&header),2);
                bodyTop=std::clamp(static_cast<int>(header.bottom),0,height);
            }else bodyTop=0;
            queryRail(vertical);queryRail(horizontal);geometry(vertical);geometry(horizontal);
        }
        refreshing=false;
    }
    void scrollTo(Rail& rail,int requested) {
        if(!IsWindow(list))return;
        queryRail(rail);const int target=std::clamp(requested,rail.minimum,rail.maximum);
        scrolling=true;
        if(rail.vertical){
            const int current=ListView_GetTopIndex(list);
            const int64_t pixels=(static_cast<int64_t>(target)-current)*rowHeight();
            ListView_Scroll(list,0,boundedInt(pixels));
        }else ListView_Scroll(list,target-rail.position,0);
        scrolling=false;refresh();
    }
    bool nativeFrame(Bitmap& frame) {
        if(bodyHeight()<=0||width<=0)return false;
        HDC reference=GetDC(list);Bitmap complete;
        const bool allocated=complete.create(reference,width,height)&&frame.create(reference,width,bodyHeight());
        ReleaseDC(list,reference);if(!allocated)return false;
        const RECT full{0,0,width,height};fill(complete.dc,full,kPanel);
        const BOOL printed=PrintWindow(list,complete.dc,PW_CLIENTONLY);
        if(!printed){HDC source=GetDC(list);BitBlt(complete.dc,0,0,width,height,source,0,0,SRCCOPY);ReleaseDC(list,source);}
        BitBlt(frame.dc,0,0,width,bodyHeight(),complete.dc,0,bodyTop,SRCCOPY);return true;
    }
    void paintAnimation(HDC dc) const {
        const RECT rect{0,0,width,bodyHeight()};fill(dc,rect,kPanel);
        const int offset=static_cast<int>(std::lround(animationShift*progress()));
        const int shift=static_cast<int>(std::lround(animationShift));
        if(nextFrame.dc)BitBlt(dc,0,shift-offset,nextFrame.width,nextFrame.height,nextFrame.dc,0,0,SRCCOPY);
        if(previousFrame.dc)BitBlt(dc,0,-offset,previousFrame.width,previousFrame.height,previousFrame.dc,0,0,SRCCOPY);
    }
    void paintBufferedAnimation(HDC destination) {
        if(!composedFrame.dc)return;
        paintAnimation(composedFrame.dc);
        BitBlt(destination,0,0,width,bodyHeight(),composedFrame.dc,0,0,SRCCOPY);
    }
    bool visualFrame(Bitmap& frame) {
        if(!animating)return nativeFrame(frame);
        HDC reference=GetDC(list);const bool allocated=frame.create(reference,width,bodyHeight());ReleaseDC(list,reference);
        if(allocated)paintAnimation(frame.dc);return allocated;
    }
    void cancelAnimation() {
        if(IsWindow(clip))KillTimer(clip,kAnimationTimer);
        animating=false;previousFrame.reset();nextFrame.reset();composedFrame.reset();
        if(IsWindow(overlay))ShowWindow(overlay,SW_HIDE);
    }
    void settleVisualPosition() {
        // Mouse hit testing must start at the content currently being shown,
        // rather than at a wheel animation's already-committed destination.
        // Report ListViews require complete rows, so stop at the nearest row.
        const bool wasAnimating=animating;
        const int position=wasAnimating?static_cast<int>(std::lround(visualPosition())):0;
        cancelAnimation();
        if(wasAnimating&&IsWindow(list))scrollTo(vertical,position);
    }
    void smoothTo(int target) {
        refresh();if(!vertical.visible){cancelAnimation();return;}
        target=std::clamp(target,vertical.minimum,vertical.maximum);
        if(target==ListView_GetTopIndex(list))return;
        const double start=animating?visualPosition():ListView_GetTopIndex(list);
        Bitmap before;if(!visualFrame(before)){cancelAnimation();scrollTo(vertical,target);return;}
        scrollTo(vertical,target);
        const int actual=ListView_GetTopIndex(list);
        Bitmap after;if(actual==start||!nativeFrame(after)){cancelAnimation();return;}
        cancelAnimation();previousFrame=std::move(before);nextFrame=std::move(after);
        HDC reference=GetDC(list);const bool buffered=composedFrame.create(reference,width,bodyHeight());ReleaseDC(list,reference);
        if(!buffered){cancelAnimation();return;}
        animationFrom=start;animationTo=actual;
        // Normal wheel movements use their exact pixel distance. Very large
        // retargets retain a single continuous viewport transition, without
        // allocating a bitmap as tall as the complete folder contents.
        animationShift=std::clamp((actual-start)*rowHeight(),-static_cast<double>(bodyHeight()),static_cast<double>(bodyHeight()));
        animationStarted=GetTickCount64();animating=true;
        SetWindowPos(overlay,HWND_TOP,0,bodyTop,width,bodyHeight(),SWP_NOACTIVATE|SWP_SHOWWINDOW);
        if(vertical.visible)SetWindowPos(vertical.window,HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
        if(horizontal.visible)SetWindowPos(horizontal.window,HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
        InvalidateRect(overlay,nullptr,FALSE);UpdateWindow(overlay);SetTimer(clip,kAnimationTimer,16,nullptr);refresh();
    }
    void wheel(WPARAM value) {
        UINT lines=3;if(!SystemParametersInfoW(SPI_GETWHEELSCROLLLINES,0,&lines,0))lines=3;
        if(lines==0)return;
        const int delta=GET_WHEEL_DELTA_WPARAM(value);
        if(GET_KEYSTATE_WPARAM(value)&MK_SHIFT){cancelAnimation();ListView_Scroll(list,-MulDiv(delta,scale(48),WHEEL_DELTA),0);refresh();return;}
        const int step=lines==WHEEL_PAGESCROLL?std::max(1,ListView_GetCountPerPage(list)):boundedInt(lines);
        wheelRowsRemainder-=static_cast<double>(delta)*step/WHEEL_DELTA;
        const int rows=boundedInt(static_cast<int64_t>(std::trunc(wheelRowsRemainder)));
        if(rows==0)return;wheelRowsRemainder-=rows;
        smoothTo(boundedInt(static_cast<int64_t>(ListView_GetTopIndex(list))+rows));
    }
    void animationTick() {
        if(!animating)return;
        if(GetTickCount64()-animationStarted>=120){cancelAnimation();refresh();return;}
        InvalidateRect(overlay,nullptr,FALSE);geometry(vertical);
    }
    void railPaint(Rail& rail,HDC dc) {
        RECT full{};GetClientRect(rail.window,&full);
        if(rail.canvas.width!=full.right||rail.canvas.height!=full.bottom)
            rail.canvas.create(dc,static_cast<int>(full.right),static_cast<int>(full.bottom));
        HDC destination=dc;if(rail.canvas.dc)dc=rail.canvas.dc;
        fill(dc,full,kPanel);
        const int thickness=scale(rail.hot||rail.dragging?6:4);
        RECT thumb=full;
        if(rail.vertical){thumb.left=(full.right-thickness)/2;thumb.right=thumb.left+thickness;thumb.top=rail.thumbStart;thumb.bottom=thumb.top+rail.thumbLength;}
        else {thumb.top=(full.bottom-thickness)/2;thumb.bottom=thumb.top+thickness;thumb.left=rail.thumbStart;thumb.right=thumb.left+rail.thumbLength;}
        const HBRUSH brush=CreateSolidBrush(rail.hot||rail.dragging?kThumbHot:kThumb);
        const HGDIOBJ oldBrush=SelectObject(dc,brush),oldPen=SelectObject(dc,GetStockObject(NULL_PEN));
        RoundRect(dc,thumb.left,thumb.top,thumb.right,thumb.bottom,thickness,thickness);
        SelectObject(dc,oldPen);SelectObject(dc,oldBrush);DeleteObject(brush);
        if(rail.canvas.dc)BitBlt(destination,0,0,full.right,full.bottom,dc,0,0,SRCCOPY);
    }
    void railDown(Rail& rail,int coordinate) {
        SetFocus(list);settleVisualPosition();refresh();
        if(coordinate>=rail.thumbStart&&coordinate<rail.thumbStart+rail.thumbLength){
            rail.dragging=true;rail.dragOffset=coordinate-rail.thumbStart;SetCapture(rail.window);InvalidateRect(rail.window,nullptr,FALSE);
        }else{
            const int direction=coordinate<rail.thumbStart?-1:1;
            const int target=boundedInt(static_cast<int64_t>(rail.position)+direction*std::max(rail.page,1));
            if(rail.vertical)smoothTo(target);else scrollTo(rail,target);
        }
    }
    void railMove(Rail& rail,int coordinate) {
        if(!rail.hot){rail.hot=true;TRACKMOUSEEVENT tracking{sizeof(tracking),TME_LEAVE,rail.window,0};TrackMouseEvent(&tracking);InvalidateRect(rail.window,nullptr,FALSE);}
        if(!rail.dragging)return;
        const int travel=std::max(0,rail.extent-rail.thumbLength);
        const double fraction=travel?std::clamp(static_cast<double>(coordinate-rail.dragOffset)/travel,0.0,1.0):0;
        const int target=rail.minimum+static_cast<int>(std::lround(fraction*(rail.maximum-rail.minimum)));
        scrollTo(rail,target);
    }

    static LRESULT CALLBACK ListProc(HWND hwnd,UINT message,WPARAM wParam,LPARAM lParam,UINT_PTR,DWORD_PTR data) {
        auto* self=reinterpret_cast<Impl*>(data);
        if(message==WM_NCDESTROY){RemoveWindowSubclass(hwnd,ListProc,kSubclassId);if(self->list==hwnd)self->list=nullptr;return DefSubclassProc(hwnd,message,wParam,lParam);}
        if(message==WM_MOUSEWHEEL){self->wheel(wParam);return 0;}
        if(!self->scrolling){
            switch(message){
            case WM_LBUTTONDOWN:case WM_LBUTTONDBLCLK:case WM_RBUTTONDOWN:self->settleVisualPosition();break;
            case WM_KEYDOWN:
            case LVM_ENSUREVISIBLE:case LVM_SETITEMCOUNT:case LVM_SETITEMSTATE:case LVM_SETCOLUMNWIDTH:
            case LVM_SETIMAGELIST:case WM_SETFONT:self->cancelAnimation();break;
            default:break;
            }
        }
        const LRESULT result=DefSubclassProc(hwnd,message,wParam,lParam);
        switch(message){
        case WM_SIZE:case WM_WINDOWPOSCHANGED:case WM_STYLECHANGED:case WM_NCCALCSIZE:
        case WM_VSCROLL:case WM_HSCROLL:case WM_MOUSEHWHEEL:case WM_KEYDOWN:
        case LVM_SCROLL:case LVM_ENSUREVISIBLE:case LVM_SETITEMCOUNT:case LVM_SETCOLUMNWIDTH:
        case LVM_INSERTCOLUMNW:case LVM_INSERTCOLUMNA:case LVM_DELETECOLUMN:case LVM_SETIMAGELIST:case WM_SETFONT:
            self->scheduleRefresh();break;
        default:break;
        }
        return result;
    }
    static LRESULT CALLBACK HostProc(HWND hwnd,UINT message,WPARAM wParam,LPARAM lParam) {
        auto* self=reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
        if(message==WM_NCCREATE){self=static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}
        if(!self)return DefWindowProcW(hwnd,message,wParam,lParam);
        switch(message){
        case WM_NOTIFY:case WM_COMMAND:case WM_CTLCOLORSTATIC:case WM_CTLCOLORBTN:
            return IsWindow(self->parent)?SendMessageW(self->parent,message,wParam,lParam):0;
        case kRefreshMessage:self->refreshPosted=false;self->refresh();return 0;
        case WM_TIMER:if(wParam==kAnimationTimer){self->animationTick();return 0;}break;
        case WM_SIZE:self->scheduleRefresh();return 0;
        case WM_SETFOCUS:if(IsWindow(self->list))SetFocus(self->list);return 0;
        case WM_MOUSEWHEEL:self->wheel(wParam);return 0;
        case WM_MOUSEHWHEEL:return SendMessageW(self->list,message,wParam,lParam);
        case WM_ERASEBKGND:return 1;
        case WM_PAINT:{PAINTSTRUCT ps{};HDC dc=BeginPaint(hwnd,&ps);RECT rect{};GetClientRect(hwnd,&rect);fill(dc,rect,kPanel);EndPaint(hwnd,&ps);return 0;}
        case WM_DESTROY:self->cancelAnimation();return 0;
        case WM_NCDESTROY:if(self->clip==hwnd)self->clip=nullptr;break;
        default:break;
        }
        return DefWindowProcW(hwnd,message,wParam,lParam);
    }
    static LRESULT CALLBACK RailProc(HWND hwnd,UINT message,WPARAM wParam,LPARAM lParam) {
        auto* rail=reinterpret_cast<Rail*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
        if(message==WM_NCCREATE){rail=static_cast<Rail*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(rail));}
        if(!rail||!rail->owner)return DefWindowProcW(hwnd,message,wParam,lParam);
        auto* self=rail->owner;
        switch(message){
        case WM_ERASEBKGND:return 1;
        case WM_PAINT:{PAINTSTRUCT ps{};HDC dc=BeginPaint(hwnd,&ps);self->railPaint(*rail,dc);EndPaint(hwnd,&ps);return 0;}
        case WM_PRINTCLIENT:self->railPaint(*rail,reinterpret_cast<HDC>(wParam));return 0;
        case WM_LBUTTONDOWN:case WM_LBUTTONDBLCLK:self->railDown(*rail,rail->vertical?GET_Y_LPARAM(lParam):GET_X_LPARAM(lParam));return 0;
        case WM_MOUSEMOVE:self->railMove(*rail,rail->vertical?GET_Y_LPARAM(lParam):GET_X_LPARAM(lParam));return 0;
        case WM_LBUTTONUP:if(rail->dragging){rail->dragging=false;ReleaseCapture();InvalidateRect(hwnd,nullptr,FALSE);}return 0;
        case WM_CAPTURECHANGED:rail->dragging=false;InvalidateRect(hwnd,nullptr,FALSE);return 0;
        case WM_MOUSELEAVE:rail->hot=false;InvalidateRect(hwnd,nullptr,FALSE);return 0;
        case WM_MOUSEWHEEL:self->wheel(wParam);return 0;
        case WM_MOUSEHWHEEL:return SendMessageW(self->list,message,wParam,lParam);
        case WM_NCDESTROY:rail->window=nullptr;break;
        default:break;
        }
        return DefWindowProcW(hwnd,message,wParam,lParam);
    }
    static LRESULT CALLBACK OverlayProc(HWND hwnd,UINT message,WPARAM wParam,LPARAM lParam) {
        auto* self=reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
        if(message==WM_NCCREATE){self=static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}
        if(!self)return DefWindowProcW(hwnd,message,wParam,lParam);
        switch(message){
        case WM_NCHITTEST:return HTTRANSPARENT;
        case WM_ERASEBKGND:return 1;
        case WM_PAINT:{PAINTSTRUCT ps{};HDC dc=BeginPaint(hwnd,&ps);self->paintBufferedAnimation(dc);EndPaint(hwnd,&ps);return 0;}
        case WM_PRINTCLIENT:self->paintBufferedAnimation(reinterpret_cast<HDC>(wParam));return 0;
        case WM_NCDESTROY:if(self->overlay==hwnd)self->overlay=nullptr;break;
        default:break;
        }
        return DefWindowProcW(hwnd,message,wParam,lParam);
    }
};

SlimScrollbars::SlimScrollbars():impl_(std::make_unique<Impl>()){}
SlimScrollbars::~SlimScrollbars(){impl_->detach();}
bool SlimScrollbars::attach(HWND list,HWND parent){return impl_->attach(list,parent);}
void SlimScrollbars::layout(const RECT& area,unsigned dpi){impl_->layout(area,dpi);}
void SlimScrollbars::refresh(){impl_->refresh();}
void SlimScrollbars::detach(){impl_->detach();}
HWND SlimScrollbars::host()const{return impl_->clip;}
} // namespace abser
