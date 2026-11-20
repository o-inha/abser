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
#include <windowsx.h>
#include <commctrl.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <dwmapi.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include "audio.h"
#include "catalog.h"
#include "file_drag.h"
#include "slim_scrollbars.h"

namespace {
using namespace abser;
constexpr UINT WM_SCAN = WM_APP + 1, WM_DURATION = WM_APP + 2, WM_AUDIO = WM_APP + 3, WM_AUDIO_READY = WM_APP + 4, WM_QUERY_AUDIO = WM_APP + 5;
constexpr UINT TIMER_UI = 1, TIMER_SAVE = 2;
constexpr int ID_PATH = 101, ID_OPEN = 102, ID_LIST = 103, ID_VOLUME = 104, ID_PLAY = 105, ID_FILTER = 200;
constexpr int LIST_TOP = 72, FOOTER_HEIGHT = 152;
const COLORREF BG = RGB(23,25,28), PANEL = RGB(29,32,36), TEXT = RGB(228,231,234), MUTED = RGB(146,153,161), LINE = RGB(48,53,58), ACCENT = RGB(195,219,164), SELECTED = RGB(48,60,56);
const COLORREF COLORS[11] = {RGB(146,153,161),RGB(235,107,111),RGB(235,159,91),RGB(224,202,100),RGB(155,202,113),RGB(92,194,159),RGB(92,190,208),RGB(110,150,224),RGB(164,132,220),RGB(213,129,192),RGB(207,188,163)};
const wchar_t* COLOR_NAMES[11] = {L"Unassigned",L"Red",L"Orange",L"Yellow",L"Lime",L"Green",L"Cyan",L"Blue",L"Violet",L"Pink",L"Sand"};
struct ScanMessage { uint64_t generation; ScanResult result; };
struct DurationMessage { uint64_t generation; std::vector<std::pair<std::wstring,double>> values; };

class FolderWorker {
public:
    explicit FolderWorker(HWND window) : window_(window), thread_([this]{ run(); }) {}
    ~FolderWorker() { { std::lock_guard<std::mutex> lock(mutex_); quit_ = true; ++generation_; } cv_.notify_one(); thread_.join(); }
    uint64_t request(std::wstring path, const MetadataStore& store) {
        std::lock_guard<std::mutex> lock(mutex_); path_ = std::move(path); store_ = store; pending_ = true;
        auto result = ++generation_; cv_.notify_one(); return result;
    }
private:
    void run() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        for (;;) {
            std::wstring path; MetadataStore store; uint64_t generation;
            { std::unique_lock<std::mutex> lock(mutex_); cv_.wait(lock,[this]{ return quit_ || pending_; });
              if (quit_) return; pending_ = false; path = path_; store = store_; generation = generation_.load(); }
            auto scan = std::make_unique<ScanMessage>(); scan->generation = generation; scan->result = scanDirectory(path,store);
            std::vector<FileEntry> files = scan->result.files;
            if (generation_.load() != generation) continue;
            if (PostMessageW(window_,WM_SCAN,0,reinterpret_cast<LPARAM>(scan.get()))) scan.release();
            auto batch = std::make_unique<DurationMessage>(); batch->generation = generation;
            auto lastPost = std::chrono::steady_clock::now();
            for (const auto& file : files) {
                if (generation_.load() != generation) break;
                const double duration = probeDuration(file.path);
                if (generation_.load() != generation) break;
                batch->values.emplace_back(file.path,duration);
                if (batch->values.size() >= 16 || std::chrono::steady_clock::now() - lastPost > std::chrono::milliseconds(140)) {
                    if (PostMessageW(window_,WM_DURATION,0,reinterpret_cast<LPARAM>(batch.get()))) batch.release();
                    batch = std::make_unique<DurationMessage>(); batch->generation = generation; lastPost = std::chrono::steady_clock::now();
                }
            }
            if (!batch->values.empty() && generation_.load() == generation && PostMessageW(window_,WM_DURATION,0,reinterpret_cast<LPARAM>(batch.get()))) batch.release();
        }
    }
    HWND window_; std::mutex mutex_; std::condition_variable cv_; bool quit_ = false, pending_ = false;
    std::atomic<uint64_t> generation_{0}; std::wstring path_; MetadataStore store_; std::thread thread_;
};

struct App {
    HWND window{}, path{}, open{}, list{}, volume{}, play{}, tooltip{}, filters[11]{};
    HFONT font{}, smallFont{}; HBRUSH bgBrush{}, panelBrush{}; HIMAGELIST rowImages{};
    UINT dpi = 96; MetadataStore store; AudioEngine audio; std::unique_ptr<FolderWorker> worker;
    SlimScrollbars scrollbars;
    std::thread audioInit; bool audioReady = false, audioStarting = true, smokeTest = false, queuedPaused = false; std::wstring queuedPlay;
    std::vector<FileEntry> files; std::vector<size_t> visible;
    std::unordered_map<std::wstring,size_t> fileIndex;
    uint64_t generation = 0; bool suppressSelection = false, ready = false, loading = false, closing = false;
    bool durationSortDirty = false; std::wstring folder, notice; AudioSnapshot playback; WaveformSnapshot waveform;
    uint64_t dragSerial = 0;
    std::wstring savedSelection; std::chrono::steady_clock::time_point selectionChanged{};
    int scale(int value) const { return MulDiv(value,static_cast<int>(dpi),96); }
    int logical(int value) const { return MulDiv(value,96,static_cast<int>(dpi)); }
    int selected() const { return ListView_GetNextItem(list,-1,LVNI_SELECTED); }
    const FileEntry* entry(int row) const { return row >= 0 && static_cast<size_t>(row) < visible.size() && visible[row]<files.size() ? &files[visible[row]] : nullptr; }
    void dirty() { if (!closing) SetTimer(window,TIMER_SAVE,500,nullptr); }
    void invalidateTransport() { RECT r{};GetClientRect(window,&r);r.top=std::max(0L,r.bottom-scale(FOOTER_HEIGHT));r.bottom-=scale(44);InvalidateRect(window,&r,FALSE); }
    void save() {
        if (!ready) return;
        for (int i=0;i<4;++i) store.settings.columnWidths[i] = logical(ListView_GetColumnWidth(list,i));
        WINDOWPLACEMENT placement{sizeof(placement)};
        if (GetWindowPlacement(window,&placement)) { auto r=placement.rcNormalPosition; store.settings.windowX=r.left; store.settings.windowY=r.top;
            store.settings.windowWidth=logical(r.right-r.left); store.settings.windowHeight=logical(r.bottom-r.top); }
        std::wstring error; if (!store.save(&error)) { notice = error; InvalidateRect(window,nullptr,FALSE); }
    }
    void headerSort() {
        HWND header=ListView_GetHeader(list);
        for(int i=0;i<4;++i) { HDITEMW item{}; item.mask=HDI_FORMAT; Header_GetItem(header,i,&item); item.fmt &= ~(HDF_SORTUP|HDF_SORTDOWN);
            if(i==static_cast<int>(store.settings.sortColumn)) item.fmt |= store.settings.ascending?HDF_SORTUP:HDF_SORTDOWN;
            Header_SetItem(header,i,&item); }
    }
    void rebuild(bool sort = true) {
        std::wstring selectedPath; if (auto e=entry(selected())) selectedPath=e->path;
        suppressSelection=true;
        ListView_SetItemState(list,-1,0,LVIS_SELECTED|LVIS_FOCUSED);
        if(sort) sortFiles(files,store.settings.sortColumn,store.settings.ascending);
        fileIndex.clear();
        for(size_t i=0;i<files.size();++i) fileIndex.emplace(files[i].path,i);
        visible.clear();
        for(size_t i=0;i<files.size();++i) if(store.settings.categoryMask & (1u<<files[i].category)) visible.push_back(i);
        ListView_SetItemCountEx(list,static_cast<int>(visible.size()),LVSICF_NOSCROLL|LVSICF_NOINVALIDATEALL);
        for(size_t i=0;i<visible.size();++i) if(files[visible[i]].path==selectedPath) { ListView_SetItemState(list,static_cast<int>(i),LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED); ListView_EnsureVisible(list,static_cast<int>(i),FALSE); break; }
        suppressSelection=false; headerSort(); scrollbars.refresh();InvalidateRect(list,nullptr,FALSE); InvalidateRect(window,nullptr,FALSE);
    }
    void playRow(int row) {
        if (auto e=entry(row)) { notice.clear(); selectionChanged=std::chrono::steady_clock::now();
            if(audioStarting) {queuedPlay=e->path;queuedPaused=false;playback={e->path,L"",AudioState::Loading,-1.0,0.0};} else audio.play(e->path);
            invalidateTransport(); }
    }
    void navigate(int step) {
        if(visible.empty()) return; int row=selected(); row = row<0 ? (step<0?static_cast<int>(visible.size())-1:0) : std::clamp(row+step,0,static_cast<int>(visible.size())-1);
        ListView_SetItemState(list,row,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED); ListView_EnsureVisible(list,row,FALSE);
        SetFocus(list);
    }
    void togglePlay() {
        if(audioStarting&&!queuedPlay.empty()) {queuedPaused=!queuedPaused;playback.state=queuedPaused?AudioState::Paused:AudioState::Loading;InvalidateRect(window,nullptr,FALSE);return;}
        if(!audioStarting)playback=audio.snapshot();
        if(playback.state==AudioState::Stopped || playback.state==AudioState::Error || playback.state==AudioState::Finished) { int row=selected(); if(row<0 && !visible.empty()) { navigate(1); return; } playRow(row); } else audio.togglePause();
    }
    void load(const std::wstring& input) {
        if(input.empty()) return; notice.clear(); loading=true; savedSelection.clear();
        if(auto e=entry(selected())) savedSelection=e->path;
        queuedPlay.clear();queuedPaused=false;audio.stop(); generation=worker->request(input,store); InvalidateRect(window,nullptr,FALSE);
    }
    void browse() {
        IFileOpenDialog* dialog=nullptr;
        if(SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dialog)))) {
            DWORD options=0; dialog->GetOptions(&options); dialog->SetOptions(options|FOS_PICKFOLDERS|FOS_FORCEFILESYSTEM|FOS_PATHMUSTEXIST|FOS_NOCHANGEDIR);
            dialog->SetTitle(L"Open audio folder");
            IShellItem* initial=nullptr; if(!folder.empty() && SUCCEEDED(SHCreateItemFromParsingName(folder.c_str(),nullptr,IID_PPV_ARGS(&initial)))) { dialog->SetFolder(initial); initial->Release(); }
            if(SUCCEEDED(dialog->Show(window))) { IShellItem* item=nullptr; if(SUCCEEDED(dialog->GetResult(&item))) { PWSTR name=nullptr;
                if(SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH,&name))) { SetWindowTextW(path,name); load(name); CoTaskMemFree(name); } item->Release(); } }
            dialog->Release();
        }
    }
    void assignCategory(int row, POINT point) {
        const auto* e=entry(row); if(!e) return; const std::wstring pathValue=e->path;
        suppressSelection=true; ListView_SetItemState(list,row,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED); suppressSelection=false;
        HMENU menu=CreatePopupMenu();
        for(int i=0;i<11;++i) { MENUITEMINFOW mi{sizeof(mi)}; mi.fMask=MIIM_ID|MIIM_FTYPE|MIIM_DATA|MIIM_STATE; mi.wID=1000+i;
            mi.fType=MFT_OWNERDRAW; mi.dwItemData=i; mi.fState=e->category==i?MFS_CHECKED:MFS_UNCHECKED; InsertMenuItemW(menu,i,TRUE,&mi); }
        int id=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,point.x,point.y,0,window,nullptr); DestroyMenu(menu);
        if(id>=1000 && id<=1010) { store.setCategory(pathValue,id-1000); for(auto& f:files) if(f.path==pathValue) { f.category=id-1000; break; } rebuild(); dirty(); }
        SetFocus(list);
    }
    void categoryForSelection() { int row=selected(); if(row<0) return; RECT r{}; ListView_GetSubItemRect(list,row,0,LVIR_BOUNDS,&r); POINT p{r.left,r.bottom}; ClientToScreen(list,&p); assignCategory(row,p); }
    void beginDrag(int row) {
        const auto* file=entry(row);if(!file)return;const std::wstring value=file->path;++dragSerial;
        // The initial press still auditions immediately. Only movement beyond
        // Windows' drag threshold reaches this point and pauses that preview.
        if(audioStarting) {if(!queuedPlay.empty()){queuedPaused=true;playback.state=AudioState::Paused;}}
        else audio.pause();
        updateAudio();
        if(smokeTest) {SetPropW(window,L"Abser.Smoke.DragSerial",reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(dragSerial)));SetPropW(window,L"Abser.Smoke.DragActive",reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(1)));}
        auto result=dragFile(window,value);
        if(smokeTest) {SetPropW(window,L"Abser.Smoke.DragResult",reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(static_cast<uint32_t>(result.result))));RemovePropW(window,L"Abser.Smoke.DragActive");}
        if(FAILED(result.result)) {notice=L"Could not drag this file.";invalidateTransport();}
        if(((result.effect&DROPEFFECT_MOVE)!=0||result.sourceRemoved)&&!folder.empty())load(folder);
    }
    void updateAudio() {
        const auto before=playback;
        if(!audioStarting||queuedPlay.empty())playback=audio.snapshot();
        auto next=audio.waveformSnapshot();
        const bool changed=next.path!=waveform.path||next.loading!=waveform.loading||next.peaks!=waveform.peaks;
        waveform=std::move(next);
        if(waveform.path==playback.path&&waveform.durationSeconds>0&&playback.durationSeconds<0)playback.durationSeconds=waveform.durationSeconds;
        if(changed||before.state!=playback.state||before.path!=playback.path||before.error!=playback.error||before.durationSeconds!=playback.durationSeconds||std::abs(before.positionSeconds-playback.positionSeconds)>0.0005)invalidateTransport();
        if(before.state!=playback.state)InvalidateRect(play,nullptr,FALSE);
        if(smokeTest)SetPropW(window,L"Abser.Smoke.AudioState",reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(playback.state)+1));
    }
};
App* app = nullptr;

void fill(HDC dc, const RECT& rect, COLORREF color) { HBRUSH brush=CreateSolidBrush(color); FillRect(dc,&rect,brush); DeleteObject(brush); }
void line(HDC dc,int x1,int y1,int x2,int y2,COLORREF color,int width=1) { HPEN pen=CreatePen(PS_SOLID,width,color); auto old=SelectObject(dc,pen); MoveToEx(dc,x1,y1,nullptr); LineTo(dc,x2,y2); SelectObject(dc,old); DeleteObject(pen); }
void swatch(HDC dc,RECT r,int category,bool enabled=true) {
    COLORREF c=COLORS[category]; if(!enabled) c=RGB((GetRValue(c)+23)/3,(GetGValue(c)+25)/3,(GetBValue(c)+28)/3);
    HBRUSH brush=category?CreateSolidBrush(c):static_cast<HBRUSH>(GetStockObject(HOLLOW_BRUSH)); HPEN pen=CreatePen(PS_SOLID,1,c);
    auto ob=SelectObject(dc,brush); auto op=SelectObject(dc,pen); RoundRect(dc,r.left,r.top,r.right,r.bottom,3,3); SelectObject(dc,op); SelectObject(dc,ob); DeleteObject(pen); if(category) DeleteObject(brush);
}
void textDraw(HDC dc,const std::wstring& text,RECT rect,COLORREF color,UINT flags=DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS) { SetBkMode(dc,TRANSPARENT); SetTextColor(dc,color); DrawTextW(dc,text.c_str(),static_cast<int>(text.size()),&rect,flags); }
template<class Draw> void bufferedPaint(HDC target,const RECT& area,Draw draw) {
    const int width=area.right-area.left,height=area.bottom-area.top;if(width<=0||height<=0)return;
    HDC memory=CreateCompatibleDC(target);HBITMAP bitmap=CreateCompatibleBitmap(target,width,height);
    if(!memory||!bitmap) {if(bitmap)DeleteObject(bitmap);if(memory)DeleteDC(memory);draw(target);return;}
    auto old=SelectObject(memory,bitmap);SetWindowOrgEx(memory,area.left,area.top,nullptr);draw(memory);
    BitBlt(target,area.left,area.top,width,height,memory,area.left,area.top,SRCCOPY);
    SelectObject(memory,old);DeleteObject(bitmap);DeleteDC(memory);
}
std::wstring durationText(double seconds) {
    if(seconds<0 || !std::isfinite(seconds)) return L"\u2014";
    if(seconds<10.0) {wchar_t value[32];swprintf(value,32,seconds<1.0?L"%.3fs":L"%.2fs",seconds);return value;}
    auto s=static_cast<int64_t>(std::round(seconds)); wchar_t value[64];
    if(s>=3600) swprintf(value,64,L"%lld:%02lld:%02lld",static_cast<long long>(s/3600),static_cast<long long>((s/60)%60),static_cast<long long>(s%60));
    else swprintf(value,64,L"%lld:%02lld",static_cast<long long>(s/60),static_cast<long long>(s%60)); return value;
}
std::wstring modifiedText(int64_t unixSeconds) {
    const time_t value=static_cast<time_t>(unixSeconds); std::tm local{}; localtime_s(&local,&value);
    SYSTEMTIME st{}; st.wYear=static_cast<WORD>(local.tm_year+1900); st.wMonth=static_cast<WORD>(local.tm_mon+1); st.wDay=static_cast<WORD>(local.tm_mday);
    wchar_t date[64]{}; GetDateFormatEx(LOCALE_NAME_USER_DEFAULT,DATE_SHORTDATE,&st,nullptr,date,64,nullptr);
    wchar_t time[32]{};swprintf(time,32,L" %02d:%02d",local.tm_hour,local.tm_min);
    return std::wstring(date)+time;
}
void tooltip(HWND owner,HWND control,const wchar_t* text) { TOOLINFOW ti{sizeof(ti)}; ti.uFlags=TTF_IDISHWND|TTF_SUBCLASS; ti.hwnd=owner; ti.uId=reinterpret_cast<UINT_PTR>(control); ti.lpszText=const_cast<LPWSTR>(text); SendMessageW(app->tooltip,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&ti)); }

LRESULT CALLBACK PathProc(HWND hwnd,UINT message,WPARAM wParam,LPARAM lParam,UINT_PTR,DWORD_PTR) {
    if(message==WM_KEYDOWN && wParam==VK_RETURN) { int length=GetWindowTextLengthW(hwnd); std::wstring value(length+1,L'\0'); GetWindowTextW(hwnd,value.data(),length+1); value.resize(length); app->load(value); SetFocus(app->list); return 0; }
    if(message==WM_KEYDOWN && wParam==VK_ESCAPE) { SetWindowTextW(hwnd,app->folder.c_str()); SetFocus(app->list); return 0; }
    return DefSubclassProc(hwnd,message,wParam,lParam);
}
LRESULT CALLBACK ButtonProc(HWND hwnd,UINT message,WPARAM wParam,LPARAM lParam,UINT_PTR,DWORD_PTR) {
    if(message==WM_ERASEBKGND)return 1;
    return DefSubclassProc(hwnd,message,wParam,lParam);
}
RECT volumeChannel(HWND hwnd) {
    RECT r{};SendMessageW(hwnd,TBM_GETCHANNELRECT,0,reinterpret_cast<LPARAM>(&r));
    RECT client{};GetClientRect(hwnd,&client);
    if(r.right<=r.left) {r.left=app->scale(6);r.right=client.right-app->scale(6);}
    return r;
}
void volumeAt(HWND hwnd,int x,UINT event=TB_THUMBTRACK) {
    const RECT channel=volumeChannel(hwnd);const int position=std::clamp(MulDiv(x-channel.left,100,std::max(1L,channel.right-channel.left)),0,100);
    SendMessageW(hwnd,TBM_SETPOS,TRUE,position);InvalidateRect(hwnd,nullptr,FALSE);
    SendMessageW(app->window,WM_HSCROLL,MAKEWPARAM(event,position),reinterpret_cast<LPARAM>(hwnd));
}
void paintVolume(HWND hwnd,HDC dc) {
    RECT r{};GetClientRect(hwnd,&r);fill(dc,r,BG);const auto channel=volumeChannel(hwnd);
    const int value=static_cast<int>(SendMessageW(hwnd,TBM_GETPOS,0,0)),y=(r.top+r.bottom)/2;
    const int x=channel.left+MulDiv(value,channel.right-channel.left,100);
    line(dc,channel.left,y,channel.right,y,LINE,app->scale(3));line(dc,channel.left,y,x,y,ACCENT,app->scale(2));
    RECT thumb{x-app->scale(4),y-app->scale(7),x+app->scale(4),y+app->scale(7)};
    HBRUSH brush=CreateSolidBrush(ACCENT);auto oldBrush=SelectObject(dc,brush);auto oldPen=SelectObject(dc,GetStockObject(NULL_PEN));
    RoundRect(dc,thumb.left,thumb.top,thumb.right,thumb.bottom,app->scale(3),app->scale(3));SelectObject(dc,oldPen);SelectObject(dc,oldBrush);DeleteObject(brush);
    if(GetFocus()==hwnd) {RECT focus=r;InflateRect(&focus,-1,-1);DrawFocusRect(dc,&focus);}
}
LRESULT CALLBACK VolumeProc(HWND hwnd,UINT message,WPARAM wParam,LPARAM lParam,UINT_PTR,DWORD_PTR) {
    if(message==WM_ERASEBKGND)return 1;
    if(message==WM_PAINT) {PAINTSTRUCT ps{};HDC dc=BeginPaint(hwnd,&ps);bufferedPaint(dc,ps.rcPaint,[&](HDC memory){paintVolume(hwnd,memory);});EndPaint(hwnd,&ps);return 0;}
    if(message==WM_PRINTCLIENT) {paintVolume(hwnd,reinterpret_cast<HDC>(wParam));return 0;}
    if(message==WM_LBUTTONDOWN) {SetFocus(hwnd);SetCapture(hwnd);volumeAt(hwnd,GET_X_LPARAM(lParam));return 0;}
    if(message==WM_MOUSEMOVE&&GetCapture()==hwnd) {volumeAt(hwnd,GET_X_LPARAM(lParam));return 0;}
    if(message==WM_LBUTTONUP&&GetCapture()==hwnd) {volumeAt(hwnd,GET_X_LPARAM(lParam),TB_ENDTRACK);ReleaseCapture();return 0;}
    if(message==WM_SETFOCUS||message==WM_KILLFOCUS)InvalidateRect(hwnd,nullptr,FALSE);
    return DefSubclassProc(hwnd,message,wParam,lParam);
}
void paintHeader(HWND hwnd,HDC dc) {
    RECT client{};GetClientRect(hwnd,&client);fill(dc,client,BG);SelectObject(dc,app->smallFont);
    for(int i=0;i<4;++i) {
        RECT cell{};if(!Header_GetItemRect(hwnd,i,&cell))continue;
        wchar_t label[64]{};HDITEMW item{};item.mask=HDI_TEXT;item.pszText=label;item.cchTextMax=64;Header_GetItem(hwnd,i,&item);
        RECT text=cell;text.left+=app->scale(7);text.right-=app->scale(5);
        std::wstring value=label;if(i==static_cast<int>(app->store.settings.sortColumn))value+=app->store.settings.ascending?L" \u2191":L" \u2193";
        textDraw(dc,value,text,MUTED);line(dc,cell.right-1,cell.top+app->scale(5),cell.right-1,cell.bottom-app->scale(5),LINE);
    }
    line(dc,0,client.bottom-1,client.right,client.bottom-1,LINE);
}
LRESULT CALLBACK HeaderProc(HWND hwnd,UINT message,WPARAM wParam,LPARAM lParam,UINT_PTR,DWORD_PTR) {
    if(message==WM_ERASEBKGND)return 1;
    if(message==WM_PAINT){PAINTSTRUCT ps{};HDC dc=BeginPaint(hwnd,&ps);bufferedPaint(dc,ps.rcPaint,[&](HDC memory){paintHeader(hwnd,memory);});EndPaint(hwnd,&ps);return 0;}
    if(message==WM_PRINTCLIENT){paintHeader(hwnd,reinterpret_cast<HDC>(wParam));return 0;}
    return DefSubclassProc(hwnd,message,wParam,lParam);
}
LRESULT CALLBACK ListProc(HWND hwnd,UINT message,WPARAM wParam,LPARAM lParam,UINT_PTR,DWORD_PTR) {
    if(message==WM_NOTIFY) {
        auto header=reinterpret_cast<NMHDR*>(lParam);
        if(header->hwndFrom==ListView_GetHeader(hwnd)&&(header->code==NM_CUSTOMDRAW||header->code==HDN_ENDTRACKW||header->code==HDN_ENDTRACKA))
            return SendMessageW(app->window,WM_NOTIFY,wParam,lParam);
    }
    if(message==WM_LBUTTONDOWN||message==WM_LBUTTONDBLCLK) {
        LVHITTESTINFO hit{}; hit.pt={GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)}; ListView_SubItemHitTest(hwnd,&hit);
        if(hit.iItem>=0 && hit.iSubItem==0) { POINT point=hit.pt; ClientToScreen(hwnd,&point); app->assignCategory(hit.iItem,point); return 0; }
        if(hit.iItem>=0) {
            const int row=hit.iItem;const auto* file=app->entry(row);const std::wstring selectedPath=file?file->path:L"";
            const bool same=app->selected()==row;SetFocus(hwnd);
            ListView_SetItemState(hwnd,row,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);
            if(same)app->playRow(row);UpdateWindow(hwnd);
            // Explicitly detect drags in every text column, including a second
            // rapid click. Native report views do not handle all these cases.
            if(DragDetect(hwnd,hit.pt)) {
                for(size_t i=0;i<app->visible.size();++i)if(app->files[app->visible[i]].path==selectedPath){app->beginDrag(static_cast<int>(i));break;}
            }
            return 0;
        }
    }
    if(message==WM_CONTEXTMENU) {
        POINT point{GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)};
        if(point.x==-1 && point.y==-1) app->categoryForSelection();
        else { POINT local=point; ScreenToClient(hwnd,&local); LVHITTESTINFO hit{}; hit.pt=local; ListView_SubItemHitTest(hwnd,&hit); if(hit.iItem>=0) app->assignCategory(hit.iItem,point); } return 0;
    }
    return DefSubclassProc(hwnd,message,wParam,lParam);
}
void setFonts(App& a) {
    if(a.font) DeleteObject(a.font); if(a.smallFont) DeleteObject(a.smallFont);
    a.font=CreateFontW(-a.scale(12),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    a.smallFont=CreateFontW(-a.scale(11),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    for(HWND child : {a.path,a.open,a.list,a.volume,a.play}) SendMessageW(child,WM_SETFONT,reinterpret_cast<WPARAM>(a.font),TRUE);
    SendMessageW(ListView_GetHeader(a.list),WM_SETFONT,reinterpret_cast<WPARAM>(a.smallFont),TRUE);
    if(a.rowImages) ImageList_Destroy(a.rowImages);
    a.rowImages=ImageList_Create(1,a.scale(29),ILC_COLOR32,1,1); ListView_SetImageList(a.list,a.rowImages,LVSIL_SMALL);
}
void layout(App& a) {
    RECT r{}; GetClientRect(a.window,&r); int w=r.right,h=r.bottom, p=a.scale(10), top=a.scale(8), bh=a.scale(24);
    const int pathWidth=std::max(a.scale(60),w-3*p-bh);
    MoveWindow(a.path,p+a.scale(6),top+a.scale(4),pathWidth-a.scale(12),a.scale(16),TRUE); MoveWindow(a.open,w-p-bh,top,bh,bh,TRUE);
    const int y=a.scale(38), available=w-2*p, cell=std::min(a.scale(27),available/11);
    for(int i=0;i<11;++i) MoveWindow(a.filters[i],p+i*cell,y,cell-1,a.scale(26),TRUE);
    const int listTop=a.scale(LIST_TOP), bottom=a.scale(FOOTER_HEIGHT);
    RECT listArea{0,listTop,w,std::max(listTop+a.scale(50),h-bottom)};a.scrollbars.layout(listArea,a.dpi);
    MoveWindow(a.play,p,h-a.scale(88),a.scale(26),a.scale(30),TRUE);
    MoveWindow(a.volume,p+a.scale(27),h-a.scale(36),std::max(a.scale(40),w-2*p-a.scale(71)),a.scale(24),TRUE);
    InvalidateRect(a.window,nullptr,FALSE);
}
void drawButtonContent(const DRAWITEMSTRUCT& d) {
    App& a=*app; HDC dc=d.hDC; RECT r=d.rcItem; bool pressed=(d.itemState&ODS_SELECTED)!=0,focused=(d.itemState&ODS_FOCUS)!=0;
    fill(dc,r,pressed?SELECTED:BG);
    if(d.CtlID>=ID_FILTER && d.CtlID<ID_FILTER+11) {
        int i=d.CtlID-ID_FILTER; bool active=(a.store.settings.categoryMask&(1u<<i))!=0;
        RECT s{r.left+a.scale(4),r.top+a.scale(5),r.left+a.scale(19),r.top+a.scale(20)}; swatch(dc,s,i,active);
        if(active) { COLORREF c=i?BG:TEXT; int x=s.left,y=s.top; line(dc,x+a.scale(3),y+a.scale(7),x+a.scale(6),y+a.scale(10),c,a.scale(1)); line(dc,x+a.scale(6),y+a.scale(10),x+a.scale(12),y+a.scale(4),c,a.scale(1)); }
    } else if(d.CtlID==ID_OPEN) {
        fill(dc,r,PANEL); int x=(r.left+r.right)/2,y=(r.top+r.bottom)/2;
        line(dc,x-a.scale(8),y+a.scale(6),x+a.scale(8),y+a.scale(6),TEXT); line(dc,x-a.scale(8),y-a.scale(5),x-a.scale(8),y+a.scale(6),TEXT);
        line(dc,x-a.scale(8),y-a.scale(5),x-a.scale(2),y-a.scale(5),TEXT); line(dc,x-a.scale(2),y-a.scale(5),x,y-a.scale(2),TEXT);
        line(dc,x,y-a.scale(2),x+a.scale(8),y-a.scale(2),TEXT); line(dc,x+a.scale(8),y-a.scale(2),x+a.scale(8),y+a.scale(6),TEXT);
    } else if(d.CtlID==ID_PLAY) {
        int x=(r.left+r.right)/2,y=(r.top+r.bottom)/2;
        if(a.playback.state==AudioState::Playing) { RECT p{x-a.scale(5),y-a.scale(6),x-a.scale(1),y+a.scale(6)}; fill(dc,p,ACCENT); p.left=x+a.scale(2);p.right=x+a.scale(6);fill(dc,p,ACCENT); }
        else { POINT pts[3]{{x-a.scale(4),y-a.scale(7)},{x+a.scale(7),y},{x-a.scale(4),y+a.scale(7)}}; HBRUSH b=CreateSolidBrush(ACCENT); auto old=SelectObject(dc,b); auto pen=SelectObject(dc,GetStockObject(NULL_PEN)); Polygon(dc,pts,3); SelectObject(dc,pen); SelectObject(dc,old);DeleteObject(b); }
    }
    if(focused) { RECT f=r; InflateRect(&f,-1,-1); DrawFocusRect(dc,&f); }
}
void drawButton(const DRAWITEMSTRUCT& d) {
    bufferedPaint(d.hDC,d.rcItem,[&](HDC memory){auto item=d;item.hDC=memory;drawButtonContent(item);});
}
void paintWaveform(App& a,HDC dc,const RECT& area) {
    fill(dc,area,PANEL);
    const int width=area.right-area.left,middle=(area.top+area.bottom)/2,half=(area.bottom-area.top)/2-a.scale(4);
    if(width<=0||half<=0)return;
    line(dc,area.left,middle,area.right,middle,LINE);
    const bool matching=a.waveform.path==a.playback.path&&!a.playback.path.empty();
    double duration=a.playback.durationSeconds;
    if(matching&&a.waveform.durationSeconds>0)duration=a.waveform.durationSeconds;
    const double progress=duration>0?std::clamp(a.playback.positionSeconds/duration,0.0,1.0):0.0;
    const int playhead=area.left+static_cast<int>(std::round(progress*std::max(0,width-1)));
    if(matching&&!a.waveform.peaks.empty()) {
        const auto& peaks=a.waveform.peaks;const float maximum=*std::max_element(peaks.begin(),peaks.end());
        const float displayGain=maximum>0?1.0f/maximum:1.0f;
        for(int x=0;x<width;++x) {
            size_t first=static_cast<size_t>(x)*peaks.size()/width,last=std::min(peaks.size(),std::max(first+1,static_cast<size_t>(x+1)*peaks.size()/width));
            float peak=0;for(size_t i=first;i<last;++i)peak=std::max(peak,peaks[i]);
            const int height=std::max(1,static_cast<int>(std::round(std::clamp(peak*displayGain,0.0f,1.0f)*half)));
            line(dc,area.left+x,middle-height,area.left+x,middle+height+1,area.left+x<playhead?ACCENT:RGB(105,119,126));
        }
    }
    if(!a.playback.path.empty())line(dc,playhead,area.top+a.scale(2),playhead,area.bottom-a.scale(2),TEXT);
}
void paint(App& a,HDC dc) {
    RECT r{};GetClientRect(a.window,&r); fill(dc,r,BG); SelectObject(dc,a.smallFont);
    int w=r.right,h=r.bottom,p=a.scale(10);
    RECT field{p,a.scale(8),w-2*p-a.scale(24),a.scale(32)};fill(dc,field,PANEL);
    line(dc,0,a.scale(LIST_TOP-3),w,a.scale(LIST_TOP-3),LINE);
    line(dc,0,h-a.scale(FOOTER_HEIGHT),w,h-a.scale(FOOTER_HEIGHT),LINE);
    RECT wave{p,h-a.scale(144),w-p,h-a.scale(98)};paintWaveform(a,dc,wave);
    RECT now{p+a.scale(34),h-a.scale(89),w-p,h-a.scale(68)};
    std::wstring name,detail;
    if(!a.notice.empty()) { name=a.notice; detail=L""; }
    else if(a.playback.state==AudioState::Error) { name=L"Cannot play this file"; detail=a.playback.error; }
    else if(!a.playback.path.empty()) { name=std::filesystem::path(a.playback.path).filename().wstring();
        const wchar_t* state=L"Stopped"; switch(a.playback.state) { case AudioState::Loading:state=L"Opening";break;case AudioState::Playing:state=L"Playing";break;case AudioState::Paused:state=L"Paused";break;case AudioState::Finished:state=L"Finished";break;default:break; }
        detail=std::wstring(state)+L"  "+durationText(a.playback.positionSeconds)+L" / "+durationText(a.playback.durationSeconds);
    } else { name=a.loading?L"Opening folder\u2026":L"Click a file to listen"; detail=L"\u2191 \u2193  Browse     Space  Pause / play"; }
    textDraw(dc,name,now,TEXT); RECT status{p+a.scale(34),h-a.scale(69),w-p,h-a.scale(52)};
    textDraw(dc,detail,status,MUTED);
    RECT vol{w-p-a.scale(39),h-a.scale(36),w-p,h-a.scale(12)}; textDraw(dc,std::to_wstring(static_cast<int>(std::round(a.store.settings.volume*100)))+L"%",vol,MUTED,DT_RIGHT|DT_VCENTER|DT_SINGLELINE);
    // Small speaker icon.
    int sx=p+a.scale(6),sy=h-a.scale(24); RECT speaker{sx-a.scale(3),sy-a.scale(3),sx,sy+a.scale(3)}; fill(dc,speaker,MUTED);
    POINT speakerPts[4]{{sx,sy-a.scale(3)},{sx+a.scale(5),sy-a.scale(7)},{sx+a.scale(5),sy+a.scale(7)},{sx,sy+a.scale(3)}};
    HBRUSH b=CreateSolidBrush(MUTED); auto old=SelectObject(dc,b); auto pen=SelectObject(dc,GetStockObject(NULL_PEN)); Polygon(dc,speakerPts,4);SelectObject(dc,pen);SelectObject(dc,old);DeleteObject(b);
}

LRESULT CALLBACK WindowProc(HWND hwnd,UINT message,WPARAM wParam,LPARAM lParam) {
    App* a=app;
    switch(message) {
    case WM_CREATE: {
        a->window=hwnd; a->dpi=GetDpiForWindow(hwnd); a->bgBrush=CreateSolidBrush(BG);a->panelBrush=CreateSolidBrush(PANEL);
        BOOL dark=TRUE; DwmSetWindowAttribute(hwnd,20,&dark,sizeof(dark));
        a->path=CreateWindowExW(0,L"EDIT",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|ES_AUTOHSCROLL,0,0,0,0,hwnd,reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_PATH)),nullptr,nullptr);
        SendMessageW(a->path,EM_SETCUEBANNER,TRUE,reinterpret_cast<LPARAM>(L"Folder path")); SetWindowSubclass(a->path,PathProc,1,0);
        a->open=CreateWindowW(L"BUTTON",L"Open folder",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,0,0,0,0,hwnd,reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_OPEN)),nullptr,nullptr);
        a->list=CreateWindowW(WC_LISTVIEWW,L"Audio files",WS_CHILD|WS_VISIBLE|WS_TABSTOP|LVS_REPORT|LVS_OWNERDATA|LVS_SINGLESEL|LVS_SHOWSELALWAYS|LVS_SHAREIMAGELISTS,0,0,0,0,hwnd,reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_LIST)),nullptr,nullptr);
        ListView_SetExtendedListViewStyle(a->list,LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER|LVS_EX_INFOTIP);
        ListView_SetBkColor(a->list,PANEL);ListView_SetTextBkColor(a->list,PANEL);ListView_SetTextColor(a->list,TEXT);
        SetWindowTheme(a->list,L"",L""); SetWindowTheme(ListView_GetHeader(a->list),L"",L""); SetWindowSubclass(a->list,ListProc,1,0);SetWindowSubclass(ListView_GetHeader(a->list),HeaderProc,1,0);
        const wchar_t* columns[]{L"Cat",L"Filename",L"Duration",L"Date modified"};
        for(int i=0;i<4;++i) { LVCOLUMNW c{};c.mask=LVCF_TEXT|LVCF_WIDTH|LVCF_FMT;c.pszText=const_cast<LPWSTR>(columns[i]);c.cx=a->scale(a->store.settings.columnWidths[i]);c.fmt=i==2?LVCFMT_RIGHT:LVCFMT_LEFT;ListView_InsertColumn(a->list,i,&c); }
        for(int i=0;i<11;++i) { std::wstring label=L"Show "+std::wstring(COLOR_NAMES[i])+L" category";
            a->filters[i]=CreateWindowW(L"BUTTON",label.c_str(),WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,0,0,0,0,hwnd,reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_FILTER+i)),nullptr,nullptr);
            SendMessageW(a->filters[i],BM_SETCHECK,(a->store.settings.categoryMask&(1u<<i))?BST_CHECKED:BST_UNCHECKED,0); }
        a->play=CreateWindowW(L"BUTTON",L"Play / pause",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,0,0,0,0,hwnd,reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_PLAY)),nullptr,nullptr);
        a->volume=CreateWindowW(TRACKBAR_CLASSW,L"Volume",WS_CHILD|WS_VISIBLE|WS_TABSTOP|TBS_HORZ|TBS_NOTICKS,0,0,0,0,hwnd,reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_VOLUME)),nullptr,nullptr);
        SendMessageW(a->volume,TBM_SETRANGE,TRUE,MAKELPARAM(0,100));SendMessageW(a->volume,TBM_SETPOS,TRUE,static_cast<int>(a->store.settings.volume*100));
        SendMessageW(a->volume,TBM_SETPAGESIZE,0,5);SetWindowTheme(a->volume,L"",L"");SetWindowSubclass(a->volume,VolumeProc,1,0);
        for(HWND child:{a->open,a->play})SetWindowSubclass(child,ButtonProc,1,0);
        for(HWND child:a->filters)SetWindowSubclass(child,ButtonProc,1,0);
        a->tooltip=CreateWindowExW(WS_EX_TOPMOST,TOOLTIPS_CLASSW,nullptr,WS_POPUP|TTS_ALWAYSTIP|TTS_NOPREFIX,CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,hwnd,nullptr,nullptr,nullptr);
        tooltip(hwnd,a->open,L"Open folder (Ctrl+O)");tooltip(hwnd,a->play,L"Play / pause (Space)");tooltip(hwnd,a->path,L"Paste a folder path and press Enter. Ctrl+L to focus.");
        for(int i=0;i<11;++i) tooltip(hwnd,a->filters[i],COLOR_NAMES[i]);
        setFonts(*a);a->scrollbars.attach(a->list,hwnd);layout(*a);a->headerSort();DragAcceptFiles(hwnd,TRUE);
        a->worker=std::make_unique<FolderWorker>(hwnd);a->audio.setNotify([hwnd]{PostMessageW(hwnd,WM_AUDIO,0,0);});
        a->audio.setVolume(a->store.settings.volume);
        a->audioInit=std::thread([a,hwnd]{bool success=a->audio.initialize(a->smokeTest);PostMessageW(hwnd,WM_AUDIO_READY,success?1:0,0);});
        a->ready=true;SetTimer(hwnd,TIMER_UI,33,nullptr);
        if(!a->store.settings.lastFolder.empty()) {SetWindowTextW(a->path,a->store.settings.lastFolder.c_str());a->load(a->store.settings.lastFolder);}
        return 0;
    }
    case WM_GETMINMAXINFO: {auto m=reinterpret_cast<MINMAXINFO*>(lParam);m->ptMinTrackSize={a->scale(280),a->scale(360)};return 0;}
    case WM_SIZE: if(a->list) {layout(*a);if(wParam!=SIZE_MINIMIZED)a->dirty();} return 0;
    case WM_MOVE: if(a->ready)a->dirty();return 0;
    case WM_DPICHANGED: {
        a->dpi=HIWORD(wParam);auto r=reinterpret_cast<RECT*>(lParam);SetWindowPos(hwnd,nullptr,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER|SWP_NOACTIVATE);
        setFonts(*a);for(int i=0;i<4;++i)ListView_SetColumnWidth(a->list,i,a->scale(a->store.settings.columnWidths[i]));layout(*a);return 0;
    }
    case WM_COMMAND: {
        int id=LOWORD(wParam);
        if(id==ID_OPEN) {a->browse();return 0;}
        if(id==ID_PLAY) {a->togglePlay();SetFocus(a->list);return 0;}
        if(id>=ID_FILTER&&id<ID_FILTER+11&&HIWORD(wParam)==BN_CLICKED) {int i=id-ID_FILTER;a->store.settings.categoryMask^=1u<<i;
            SendMessageW(a->filters[i],BM_SETCHECK,(a->store.settings.categoryMask&(1u<<i))?BST_CHECKED:BST_UNCHECKED,0);InvalidateRect(a->filters[i],nullptr,FALSE);a->rebuild(false);a->dirty();return 0;}
        return 0;
    }
    case WM_HSCROLL: if(reinterpret_cast<HWND>(lParam)==a->volume) {a->store.settings.volume=static_cast<float>(SendMessageW(a->volume,TBM_GETPOS,0,0))/100.f;a->audio.setVolume(a->store.settings.volume);a->dirty();RECT changed{};GetClientRect(hwnd,&changed);changed.top=changed.bottom-a->scale(42);InvalidateRect(hwnd,&changed,FALSE);}return 0;
    case WM_NOTIFY: {
        auto hdr=reinterpret_cast<NMHDR*>(lParam);
        if(hdr->hwndFrom==a->list) {
            switch(hdr->code) {
            case LVN_GETDISPINFOW: {auto info=reinterpret_cast<NMLVDISPINFOW*>(lParam);const auto* e=a->entry(info->item.iItem);if(!e)return 0;
                if(info->item.mask&LVIF_TEXT) {std::wstring text;switch(info->item.iSubItem){case 0:break;case 1:text=e->name;break;case 2:text=durationText(e->duration);break;case 3:text=modifiedText(e->modified);break;}
                    lstrcpynW(info->item.pszText,text.c_str(),info->item.cchTextMax);}return 0;}
            case LVN_GETINFOTIPW: {auto tip=reinterpret_cast<NMLVGETINFOTIPW*>(lParam);if(auto e=a->entry(tip->iItem)){std::wstring value=e->name+L"\n"+COLOR_NAMES[e->category]+L"  \u00b7  "+durationText(e->duration)+L"\nModified: "+modifiedText(e->modified);lstrcpynW(tip->pszText,value.c_str(),tip->cchTextMax);}return 0;}
            case LVN_ITEMCHANGED: {auto change=reinterpret_cast<NMLISTVIEW*>(lParam);if(!a->suppressSelection&&(change->uChanged&LVIF_STATE)&&(change->uNewState&LVIS_SELECTED)&&!(change->uOldState&LVIS_SELECTED))a->playRow(change->iItem);return 0;}
            case LVN_BEGINDRAG: {auto drag=reinterpret_cast<NMLISTVIEW*>(lParam);a->beginDrag(drag->iItem);return 0;}
            case LVN_COLUMNCLICK: {auto click=reinterpret_cast<NMLISTVIEW*>(lParam);auto col=static_cast<SortColumn>(click->iSubItem);if(col==a->store.settings.sortColumn)a->store.settings.ascending=!a->store.settings.ascending;else{a->store.settings.sortColumn=col;a->store.settings.ascending=true;}a->rebuild();a->dirty();return 0;}
            case NM_CUSTOMDRAW: {
                auto cd=reinterpret_cast<NMLVCUSTOMDRAW*>(lParam);
                if(cd->nmcd.dwDrawStage==CDDS_PREPAINT)return CDRF_NOTIFYITEMDRAW|CDRF_NOTIFYPOSTPAINT;
                if(cd->nmcd.dwDrawStage==CDDS_POSTPAINT) {if(a->visible.empty()){RECT r{};GetClientRect(a->list,&r);r.top=a->scale(62);r.left=a->scale(12);r.right-=a->scale(12);std::wstring msg=a->loading?L"Opening folder\u2026":a->folder.empty()?L"Open an audio folder\n\nPaste a path above and press Enter,\nor click the folder button.":a->files.empty()?L"No audio files in this folder":L"No files match the selected colours";SelectObject(cd->nmcd.hdc,a->font);textDraw(cd->nmcd.hdc,msg,r,MUTED,DT_CENTER|DT_TOP|DT_WORDBREAK);}return CDRF_DODEFAULT;}
                if(cd->nmcd.dwDrawStage==CDDS_ITEMPREPAINT) {return CDRF_NOTIFYSUBITEMDRAW;}
                if(cd->nmcd.dwDrawStage==(CDDS_ITEMPREPAINT|CDDS_SUBITEM)) {const auto* e=a->entry(static_cast<int>(cd->nmcd.dwItemSpec));if(!e)return CDRF_DODEFAULT;
                    RECT r{};ListView_GetSubItemRect(a->list,static_cast<int>(cd->nmcd.dwItemSpec),cd->iSubItem,LVIR_BOUNDS,&r);
                    const bool selected=(ListView_GetItemState(a->list,static_cast<int>(cd->nmcd.dwItemSpec),LVIS_SELECTED)&LVIS_SELECTED)!=0;
                    if(cd->iSubItem==0) {r.right=r.left+ListView_GetColumnWidth(a->list,0);fill(cd->nmcd.hdc,r,selected?SELECTED:PANEL);
                        int size=a->scale(11);RECT s{r.left+(r.right-r.left-size)/2,r.top+(r.bottom-r.top-size)/2,0,0};s.right=s.left+size;s.bottom=s.top+size;swatch(cd->nmcd.hdc,s,e->category);return CDRF_SKIPDEFAULT;}
                    fill(cd->nmcd.hdc,r,selected?SELECTED:PANEL);r.left+=a->scale(7);r.right-=a->scale(7);SelectObject(cd->nmcd.hdc,a->font);
                    std::wstring value=cd->iSubItem==1?e->name:cd->iSubItem==2?durationText(e->duration):modifiedText(e->modified);
                    textDraw(cd->nmcd.hdc,value,r,cd->iSubItem==1?TEXT:MUTED,(cd->iSubItem==2?DT_RIGHT:DT_LEFT)|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
                    return CDRF_SKIPDEFAULT;
                }
                return CDRF_DODEFAULT;
            }
            case LVN_ODFINDITEMW: {auto f=reinterpret_cast<NMLVFINDITEMW*>(lParam);if(!f->lvfi.psz)return -1;int start=std::max(0,f->iStart);std::wstring prefix=f->lvfi.psz;
                for(size_t offset=0;offset<a->visible.size();++offset){int row=(start+static_cast<int>(offset))%static_cast<int>(a->visible.size());auto e=a->entry(row);if(e&&_wcsnicmp(e->name.c_str(),prefix.c_str(),prefix.size())==0)return row;}return -1;}
            }
        }
        if(hdr->hwndFrom==ListView_GetHeader(a->list)) {
            if(hdr->code==HDN_ENDTRACKW||hdr->code==HDN_ENDTRACKA){a->scrollbars.refresh();a->dirty();return 0;}
            if(hdr->code==NM_CUSTOMDRAW) {auto cd=reinterpret_cast<NMCUSTOMDRAW*>(lParam);if(cd->dwDrawStage==CDDS_PREPAINT){RECT r{};GetClientRect(hdr->hwndFrom,&r);fill(cd->hdc,r,BG);return CDRF_NOTIFYITEMDRAW;}
                if(cd->dwDrawStage==CDDS_ITEMPREPAINT){RECT r=cd->rc;fill(cd->hdc,r,BG);SelectObject(cd->hdc,a->smallFont);int i=static_cast<int>(cd->dwItemSpec);wchar_t label[64]{};HDITEMW item{};item.mask=HDI_TEXT;item.pszText=label;item.cchTextMax=64;Header_GetItem(hdr->hwndFrom,i,&item);
                    r.left+=a->scale(8);r.right-=a->scale(5);std::wstring text=label;if(i==static_cast<int>(a->store.settings.sortColumn))text+=a->store.settings.ascending?L" \u2191":L" \u2193";textDraw(cd->hdc,text,r,MUTED);line(cd->hdc,cd->rc.right-1,cd->rc.top+a->scale(7),cd->rc.right-1,cd->rc.bottom-a->scale(7),LINE);return CDRF_SKIPDEFAULT;}}
        }
        return 0;
    }
    case WM_DRAWITEM: {auto d=reinterpret_cast<DRAWITEMSTRUCT*>(lParam);if(d->CtlType==ODT_MENU) {int i=static_cast<int>(d->itemData);fill(d->hDC,d->rcItem,(d->itemState&ODS_SELECTED)?SELECTED:BG);RECT s{d->rcItem.left+a->scale(10),d->rcItem.top+a->scale(7),d->rcItem.left+a->scale(23),d->rcItem.top+a->scale(20)};swatch(d->hDC,s,i);RECT text=d->rcItem;text.left+=a->scale(34);SelectObject(d->hDC,a->font);textDraw(d->hDC,COLOR_NAMES[i],text,TEXT);if(d->itemState&ODS_CHECKED)textDraw(d->hDC,L"\u2713",d->rcItem,ACCENT,DT_RIGHT|DT_VCENTER|DT_SINGLELINE);return TRUE;}drawButton(*d);return TRUE;}
    case WM_MEASUREITEM: {auto m=reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);if(m->CtlType==ODT_MENU){m->itemWidth=a->scale(150);m->itemHeight=a->scale(27);return TRUE;}break;}
    case WM_CTLCOLOREDIT: SetTextColor(reinterpret_cast<HDC>(wParam),TEXT);SetBkColor(reinterpret_cast<HDC>(wParam),PANEL);return reinterpret_cast<LRESULT>(a->panelBrush);
    case WM_CTLCOLORSTATIC: case WM_CTLCOLORBTN: SetTextColor(reinterpret_cast<HDC>(wParam),TEXT);SetBkColor(reinterpret_cast<HDC>(wParam),BG);return reinterpret_cast<LRESULT>(a->bgBrush);
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {PAINTSTRUCT ps{};HDC dc=BeginPaint(hwnd,&ps);bufferedPaint(dc,ps.rcPaint,[&](HDC memory){paint(*a,memory);});EndPaint(hwnd,&ps);return 0;}
    case WM_PRINTCLIENT: paint(*a,reinterpret_cast<HDC>(wParam));return 0;
    case WM_SCAN: {
        std::unique_ptr<ScanMessage> result(reinterpret_cast<ScanMessage*>(lParam));if(result->generation!=a->generation)return 0;a->loading=false;
        if(!result->result.error.empty()) {a->notice=result->result.error;InvalidateRect(hwnd,nullptr,FALSE);InvalidateRect(a->list,nullptr,FALSE);return 0;}
        a->suppressSelection=true;ListView_SetItemState(a->list,-1,0,LVIS_SELECTED|LVIS_FOCUSED);a->visible.clear();a->suppressSelection=false;
        a->folder=result->result.folder;a->store.settings.lastFolder=a->folder;SetWindowTextW(a->path,a->folder.c_str());a->files=std::move(result->result.files);
        for(auto& file:a->files)file.category=a->store.category(file.path);
        a->rebuild();a->suppressSelection=true;ListView_SetItemState(a->list,-1,0,LVIS_SELECTED|LVIS_FOCUSED);
        for(size_t i=0;i<a->visible.size();++i)if(a->files[a->visible[i]].path==a->savedSelection){ListView_SetItemState(a->list,static_cast<int>(i),LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);break;}a->suppressSelection=false;
        std::wstring title=L"Abser  \u00b7  "+std::filesystem::path(a->folder).filename().wstring();SetWindowTextW(hwnd,title.c_str());a->dirty();SetFocus(a->list);return 0;
    }
    case WM_DURATION: {
        std::unique_ptr<DurationMessage> result(reinterpret_cast<DurationMessage*>(lParam));if(result->generation!=a->generation)return 0;
        for(const auto& value:result->values) {auto found=a->fileIndex.find(value.first);if(found!=a->fileIndex.end())a->files[found->second].duration=value.second;}
        if(a->store.settings.sortColumn==SortColumn::Duration)a->durationSortDirty=true;
        InvalidateRect(a->list,nullptr,FALSE);return 0;
    }
    case WM_AUDIO: a->updateAudio();return 0;
    case WM_QUERY_AUDIO: return a->smokeTest?static_cast<LRESULT>(a->audioStarting?a->playback.state:a->audio.snapshot().state)+1:0;
    case WM_AUDIO_READY:
        if(a->audioInit.joinable())a->audioInit.join();a->audioStarting=false;a->audioReady=wParam!=0;
        if(a->audioReady&&!a->queuedPlay.empty()) {a->audio.play(a->queuedPlay);if(a->queuedPaused)a->audio.pause();}
        a->queuedPlay.clear();a->queuedPaused=false;a->updateAudio();return 0;
    case WM_TIMER: if(wParam==TIMER_SAVE){KillTimer(hwnd,TIMER_SAVE);a->save();}else if(wParam==TIMER_UI){a->updateAudio();if(a->durationSortDirty){a->durationSortDirty=false;a->rebuild();}}return 0;
    case WM_DROPFILES: {HDROP drop=reinterpret_cast<HDROP>(wParam);wchar_t path[32768];if(DragQueryFileW(drop,0,path,32768)){std::filesystem::path value(path);std::error_code ec;if(!std::filesystem::is_directory(value,ec))value=value.parent_path();SetWindowTextW(a->path,value.c_str());a->load(value.wstring());}DragFinish(drop);return 0;}
    case WM_CLOSE: a->closing=true;a->save();DestroyWindow(hwnd);return 0;
    case WM_DESTROY: {
        KillTimer(hwnd,TIMER_UI);KillTimer(hwnd,TIMER_SAVE);if(a->audioInit.joinable())a->audioInit.join();a->audio.setNotify({});a->audio.shutdown();a->worker.reset();
        MSG queued{};while(PeekMessageW(&queued,hwnd,WM_SCAN,WM_DURATION,PM_REMOVE)){if(queued.message==WM_SCAN)delete reinterpret_cast<ScanMessage*>(queued.lParam);else if(queued.message==WM_DURATION)delete reinterpret_cast<DurationMessage*>(queued.lParam);}
        PostQuitMessage(0);return 0;
    }
    }
    return DefWindowProcW(hwnd,message,wParam,lParam);
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if(FAILED(OleInitialize(nullptr)))return 1;INITCOMMONCONTROLSEX common{sizeof(common),ICC_LISTVIEW_CLASSES|ICC_BAR_CLASSES|ICC_STANDARD_CLASSES};InitCommonControlsEx(&common);
    App application;app=&application;
    int argc=0;LPWSTR* argv=CommandLineToArgvW(GetCommandLineW(),&argc);std::wstring startupFolder;
    if(argv&&argc>=3&&std::wstring(argv[1])==L"--smoke-test-data") {application.smokeTest=true;application.store=MetadataStore(std::filesystem::path(argv[2]));if(argc>=4)startupFolder=argv[3];}
    else if(argv&&argc>1)startupFolder=argv[1];
    if(argv)LocalFree(argv);
    std::wstring error;application.store.load(&error);if(!error.empty())application.notice=error;
    WNDCLASSEXW wc{sizeof(wc)};wc.style=CS_HREDRAW|CS_VREDRAW;wc.lpfnWndProc=WindowProc;wc.hInstance=instance;wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.lpszClassName=L"AbserWindow";
    wc.hIcon=LoadIconW(instance,MAKEINTRESOURCEW(1));if(!wc.hIcon)wc.hIcon=LoadIconW(nullptr,IDI_APPLICATION);wc.hIconSm=wc.hIcon;RegisterClassExW(&wc);
    auto& s=application.store.settings;UINT dpi=GetDpiForSystem();RECT r{s.windowX,s.windowY,s.windowX+MulDiv(s.windowWidth,dpi,96),s.windowY+MulDiv(s.windowHeight,dpi,96)};
    HMONITOR monitor=MonitorFromRect(&r,MONITOR_DEFAULTTONEAREST);MONITORINFO info{sizeof(info)};GetMonitorInfoW(monitor,&info);int width=std::min(r.right-r.left,info.rcWork.right-info.rcWork.left),height=std::min(r.bottom-r.top,info.rcWork.bottom-info.rcWork.top);
    int x=std::clamp(static_cast<int>(r.left),static_cast<int>(info.rcWork.left),static_cast<int>(info.rcWork.right)-width),y=std::clamp(static_cast<int>(r.top),static_cast<int>(info.rcWork.top),static_cast<int>(info.rcWork.bottom)-height);
    HWND window=CreateWindowExW(0,wc.lpszClassName,L"Abser",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,x,y,width,height,nullptr,nullptr,instance,nullptr);
    if(!window){OleUninitialize();return 1;}
    // An optional command-line folder opens directly without changing audio files.
    if(!startupFolder.empty())application.load(startupFolder);
    ShowWindow(window,show);UpdateWindow(window);SetFocus(application.list);
    MSG msg{};while(GetMessageW(&msg,nullptr,0,0)>0) {
        if(msg.message==WM_KEYDOWN) {
            const bool ctrl=(GetKeyState(VK_CONTROL)&0x8000)!=0;HWND focus=GetFocus();
            if(ctrl&&(msg.wParam=='O'||msg.wParam=='L')){if(msg.wParam=='O')application.browse();else{SetFocus(application.path);SendMessageW(application.path,EM_SETSEL,0,-1);}continue;}
            if(focus==application.path) {
                if(msg.wParam==VK_RETURN) {int length=GetWindowTextLengthW(application.path);std::wstring value(length+1,L'\0');GetWindowTextW(application.path,value.data(),length+1);value.resize(length);application.load(value);SetFocus(application.list);continue;}
                if(msg.wParam==VK_ESCAPE) {SetWindowTextW(application.path,application.folder.c_str());SetFocus(application.list);continue;}
                if(ctrl&&msg.wParam=='A') {SendMessageW(application.path,EM_SETSEL,0,-1);continue;}
            }
            if(focus!=application.path) {
                if(msg.wParam==VK_F5){if(!application.folder.empty())application.load(application.folder);continue;}
                if(msg.wParam==VK_SPACE&&(focus==application.list||focus==window||focus==application.play||focus==application.volume)){application.togglePlay();continue;}
                if(focus==application.list&&(msg.wParam==VK_UP||msg.wParam==VK_DOWN)){application.navigate(msg.wParam==VK_UP?-1:1);continue;}
                if(focus==application.list&&msg.wParam=='C'&&!ctrl){application.categoryForSelection();continue;}
                if(focus==application.list&&msg.wParam==VK_ESCAPE){application.queuedPlay.clear();application.queuedPaused=false;application.audio.stop();continue;}
            }
        }
        if(!IsDialogMessageW(window,&msg)){TranslateMessage(&msg);DispatchMessageW(&msg);}
    }
    if(application.rowImages)ImageList_Destroy(application.rowImages);if(application.font)DeleteObject(application.font);if(application.smallFont)DeleteObject(application.smallFont);if(application.bgBrush)DeleteObject(application.bgBrush);if(application.panelBrush)DeleteObject(application.panelBrush);
    OleUninitialize();return static_cast<int>(msg.wParam);
}
