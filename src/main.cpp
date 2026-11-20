#include "capture.h"
#include "displays.h"
#include "input_bridge.h"
#include "resource.h"
#include <windows.graphics.directx.direct3d11.interop.h>
#include <commctrl.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <dwmapi.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <optional>
#include <windowsx.h>

using namespace winrt;
using namespace winrt::Windows::Graphics::Capture;
namespace {
constexpr COLORREF Background=RGB(14,19,28),Panel=RGB(23,30,42),Ink=RGB(231,237,247),Muted=RGB(150,167,191),Accent=RGB(63,218,180);
enum ControlId { ID_WINDOWS=100,ID_REFRESH,ID_START,ID_SIZE,ID_RESIZE,ID_RESTORE,ID_FILTER,ID_SHARP,ID_LINEAR,ID_COMPARE,ID_FULL,ID_SAVE,ID_STOP,ID_PATTERN,ID_MONITOR,ID_PARK,ID_NATIVE_INPUT };
constexpr int HOT_EMERGENCY=1,HOT_FULL=2,HOT_COMPARE=3,HOT_START=4,HOT_MOUSE=5;
constexpr UINT WM_RENDER_PENDING=WM_APP+3;
constexpr UINT_PTR PresentRetryTimer=3,DisplayRefreshTimer=4;
constexpr wchar_t MainClass[]=L"Upscalp.Control", PreviewClass[]=L"Upscalp.Preview", PatternClass[]=L"Upscalp.Pattern";
struct SourceEntry { HWND hwnd{}; HMONITOR monitor{}; std::wstring title,displayIdentity; };
struct SubmissionMetrics {
    unsigned long long frames{};
    double medianMs{-1},p95Ms{-1},filterGpuMs{-1};
};
bool ClosePreviousInstances() {
    std::vector<HWND> windows;
    EnumWindows([](HWND hwnd,LPARAM data)->BOOL {
        wchar_t name[80]{}; GetClassNameW(hwnd,name,80);
        DWORD pid{}; GetWindowThreadProcessId(hwnd,&pid);
        if (wcscmp(name,MainClass)==0 && pid!=GetCurrentProcessId())
            reinterpret_cast<std::vector<HWND>*>(data)->push_back(hwnd);
        return TRUE;
    },reinterpret_cast<LPARAM>(&windows));
    for (HWND hwnd : windows) {
        DWORD pid{}; GetWindowThreadProcessId(hwnd,&pid);
        handle process{OpenProcess(SYNCHRONIZE,FALSE,pid)};
        if (!PostMessageW(hwnd,WM_CLOSE,0,0) && IsWindow(hwnd)) return false;
        if (process && WaitForSingleObject(process.get(),5000)!=WAIT_OBJECT_0) return false;
        if (!process) {
            auto deadline=GetTickCount64()+5000;
            while (IsWindow(hwnd) && GetTickCount64()<deadline) Sleep(20);
            if (IsWindow(hwnd)) return false;
        }
    }
    return true;
}
std::wstring ErrorText() {
    try { throw; }
    catch (const hresult_error& e) {
        std::wostringstream out; out << e.message().c_str() << L" (0x" << std::hex << static_cast<unsigned>(e.code().value) << L")"; return out.str();
    }
    catch (const std::exception& e) { return to_hstring(e.what()).c_str(); }
    catch (...) { return L"Bilinmeyen hata"; }
}
std::string JsonString(const std::wstring& value) {
    std::string escaped;
    constexpr char hex[]="0123456789abcdef";
    for (unsigned char c : to_string(value)) {
        if (c=='"' || c=='\\') escaped+='\\';
        if (c<32) {
            escaped+="\\u00";
            escaped+=hex[c>>4]; escaped+=hex[c&15];
        } else escaped+=static_cast<char>(c);
    }
    return escaped;
}
void SmokeReport(const std::wstring& path,bool passed,unsigned width,unsigned height,const std::wstring& detail,
                 const SubmissionMetrics* metrics=nullptr) {
    if (path.empty()) return;
    std::ofstream file(std::filesystem::path(path),std::ios::binary);
    file << "{\"passed\":" << (passed ? "true" : "false") << ",\"sourceWidth\":" << width
         << ",\"sourceHeight\":" << height << ",\"outputWidth\":1920,\"outputHeight\":1080,\"detail\":\"" << JsonString(detail) << "\"";
    if (metrics) {
        file << ",\"submittedFrames\":" << metrics->frames << ",\"arrivalToSubmitMedianMs\":" << metrics->medianMs
             << ",\"arrivalToSubmitP95Ms\":" << metrics->p95Ms << ",\"lastFilterGpuMs\":" << metrics->filterGpuMs;
    }
    file << "}\n";
}
class App {
public:
    HINSTANCE instance{};
    HWND main{},preview{},pattern{};
    std::wstring smokeReport,smokeImage,startupSourceDisplay,startupOutputDisplay;
    std::wstring sessionReport;
    int exitCode{};
    bool smoke{},monitorSmoke{};
    void Initialize();
    int Run();
    LRESULT Handle(UINT message,WPARAM wparam,LPARAM lparam);
private:
    int D(int value) const { return MulDiv(value,static_cast<int>(dpi_),96); }
    HWND Control(int id) const { return GetDlgItem(main,id); }
    void Add(const wchar_t* type,const wchar_t* text,int id,DWORD style=0);
    void UpdateFonts();
    void Layout();
    void Paint();
    void Text(HDC dc,const std::wstring& text,RECT bounds,HFONT font,COLORREF color,UINT flags=DT_LEFT|DT_TOP|DT_WORDBREAK) const;
    void RefreshWindows();
    void UpdateSourceControls();
    void RefreshDisplays();
    void SetDisplays(std::vector<DisplayTarget> displays);
    void QueueDisplayRefresh();
    void RefreshDisplayTopology();
    const DisplayTarget* SelectedDisplay() const;
    void ChangeDisplay();
    void WriteSessionReport();
    std::vector<RECT> DisplayBounds() const;
    const SourceEntry* SelectedSource() const;
    HWND SelectedWindow() const;
    void Start(HWND target=nullptr);
    void Stop(bool restore=true);
    bool ResizeTarget();
    void BeginSupersampling();
    void BeginNativeWindow(HWND target,const DisplayTarget& output);
    HMONITOR NativeInputMonitor() const;
    void FocusNativeSource(std::optional<POINT> outputPoint=std::nullopt);
    void ReleaseNativeClip();
    void UpdateNativeClip();
    void SaveTargetPlacement(HWND target);
    bool RestoreTarget();
    void Frame();
    void Redraw() { redrawPending_=true; }
    void Draw();
    void SetPreviewVisible(bool visible);
    void RecordSubmission();
    void ResetSubmissions();
    void UpdateSubmissionStats();
    void UpdateOptions();
    void ToggleFullscreen();
    void ToggleUpscale();
    void ToggleMouse();
    void Save();
    void MakePattern(HMONITOR target=nullptr);
    void Tick();
    void SetStatus(std::wstring text) { status_=std::move(text); InvalidateRect(main,nullptr,FALSE); }
    Renderer renderer_,patternRenderer_;
    Capture capture_;
    InputBridge input_;
    DisplaySurface previewDisplay_,fullDisplay_,patternDisplay_;
    RenderOptions options_;
    std::vector<SourceEntry> sources_;
    std::vector<DisplayTarget> displays_;
    std::wstring preferredDisplay_,settingsPath_;
    std::wstring captureDisplayIdentity_;
    HWND nativeMouseWindow_{};
    std::optional<RECT> nativeClip_;
    HWND resizedTarget_{};
    HMONITOR nativeWindowMonitor_{};
    DWORD resizedPid_{};
    WINDOWPLACEMENT savedPlacement_{sizeof(WINDOWPLACEMENT)};
    HFONT font_{},titleFont_{},smallFont_{},numberFont_{};
    HBRUSH panelBrush_{};
    UINT dpi_{96};
    RECT previewBounds_{};
    std::wstring status_=L"Bir oyun penceresi seçip yakalamayı başlatın. 4K test deseni ile önce görüntü yolunu deneyebilirsiniz.";
    std::wstring hotkeyWarning_;
    unsigned long long lastFrame_{},fpsStart_{},smokeStart_{},patternStart_{};
    unsigned fpsFrames_{},outputFpsFrames_{},smokeFrames_{};
    unsigned smokePhase_{};
    bool smokeClickForwarded_{};
    HWND smokeMirrorWindow_{};
    unsigned long long smokeCaptureGeneration_{};
    SubmissionMetrics smokeMetrics_;
    double fps_{},outputFps_{};
    bool closing_{};
    bool parked_{};
    bool pendingOutput_{};
    bool framePending_{},redrawPending_{},optionsDirty_{},previewVisible_{true};
    unsigned long long parkedAt_{},displayRefreshDeadline_{};
    bool presentRetry_{};
    long long sourceArrivalQpc_{},submittedArrivalQpc_{};
    double millisecondsPerQpcTick_{};
    std::array<double,120> submissionAges_{};
    size_t submissionCount_{},submissionIndex_{};
    double submitMedianMs_{-1},submitP95Ms_{-1};
    unsigned long long submittedFrames_{};
    static LRESULT CALLBACK MainProc(HWND hwnd,UINT message,WPARAM wp,LPARAM lp) {
        App* app=reinterpret_cast<App*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
        if (message==WM_NCCREATE) {
            app=static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            app->main=hwnd; SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(app));
        }
        if (app) {
            try { return app->Handle(message,wp,lp); }
            catch (...) {
                auto error=ErrorText();
                try { app->Stop(); } catch (...) {}
                app->SetStatus(L"İşlem tamamlanamadı: "+error);
                if (app->smoke && !app->closing_) {
                    SmokeReport(app->smokeReport,false,0,0,error); app->exitCode=1;
                    PostMessageW(hwnd,WM_CLOSE,0,0);
                }
                return 0;
            }
        }
        return DefWindowProcW(hwnd,message,wp,lp);
    }
    static LRESULT CALLBACK SurfaceProc(HWND hwnd,UINT message,WPARAM wp,LPARAM lp) {
        if (message==WM_NCCREATE) {
            auto self=static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));
        }
        auto self=reinterpret_cast<App*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
        if (message==WM_NCHITTEST) return self ? HTCLIENT : HTTRANSPARENT;
        if (self && message==WM_MOUSEACTIVATE) return MA_NOACTIVATE;
        if (self && message==WM_SETCURSOR) { SetCursor(LoadCursorW(nullptr,IDC_ARROW)); return TRUE; }
        if (self && (message==WM_MOUSEMOVE || message==WM_LBUTTONDOWN || message==WM_LBUTTONUP ||
            message==WM_RBUTTONDOWN || message==WM_RBUTTONUP || message==WM_MBUTTONDOWN || message==WM_MBUTTONUP ||
            message==WM_XBUTTONDOWN || message==WM_XBUTTONUP || message==WM_MOUSEWHEEL || message==WM_MOUSEHWHEEL)) {
            try {
                if (hwnd==self->fullDisplay_.hwnd && self->NativeInputMonitor()) {
                    if (message==WM_LBUTTONDOWN) self->FocusNativeSource(POINT{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)});
                } else {
                    self->input_.Forward(message,wp,lp);
                }
            }
            catch (...) { self->input_.Release(); }
            return 0;
        }
        if (message==WM_ERASEBKGND) return 1;
        if (message==WM_PAINT) {
            PAINTSTRUCT paint{}; HDC dc=BeginPaint(hwnd,&paint);
            if (!GetPropW(hwnd,L"Upscalp.Presented")) FillRect(dc,&paint.rcPaint,static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            EndPaint(hwnd,&paint); return 0;
        }
        if (message==WM_NCDESTROY) RemovePropW(hwnd,L"Upscalp.Presented");
        return DefWindowProcW(hwnd,message,wp,lp);
    }
    static LRESULT CALLBACK PatternProc(HWND hwnd,UINT message,WPARAM wp,LPARAM lp) {
        if (message==WM_NCCREATE)
            SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
        if (message==WM_GETMINMAXINFO) {
            auto info=reinterpret_cast<MINMAXINFO*>(lp);
            info->ptMaxTrackSize=GetPropW(hwnd,L"Upscalp.TestClamp") ? POINT{4000,1700} : POINT{16384,16384}; return 0;
        }
        if (message==WM_KEYDOWN && wp==VK_ESCAPE) { PostMessageW(hwnd,WM_CLOSE,0,0); return 0; }
        if (message==WM_CLOSE) {
            if (auto app=reinterpret_cast<App*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA))) app->patternDisplay_={};
            DestroyWindow(hwnd); return 0;
        }
        if (message==WM_LBUTTONDOWN) {
            SetPropW(hwnd,L"Upscalp.TestClick",reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(lp))); return 0;
        }
        if (message==WM_ERASEBKGND) return 1;
        return DefWindowProcW(hwnd,message,wp,lp);
    }
};
void App::Add(const wchar_t* type,const wchar_t* text,int id,DWORD style) {
    HWND hwnd=CreateWindowExW(0,type,text,WS_CHILD|WS_VISIBLE|WS_TABSTOP|style,0,0,10,10,main,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),instance,nullptr);
    if (!hwnd) throw_last_error();
    SendMessageW(hwnd,WM_SETFONT,reinterpret_cast<WPARAM>(font_),TRUE);
    SetWindowTheme(hwnd,L"DarkMode_Explorer",nullptr);
}
void App::UpdateFonts() {
    HFONT oldFonts[]={font_,titleFont_,smallFont_,numberFont_};
    auto makeFont=[&](int points,int weight) { return CreateFontW(-MulDiv(points,static_cast<int>(dpi_),72),0,0,0,weight,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI"); };
    font_=makeFont(10,FW_NORMAL); titleFont_=makeFont(25,FW_SEMIBOLD); smallFont_=makeFont(9,FW_NORMAL); numberFont_=makeFont(17,FW_SEMIBOLD);
    if (main) for (int id=ID_WINDOWS;id<=ID_NATIVE_INPUT;++id) SendMessageW(Control(id),WM_SETFONT,reinterpret_cast<WPARAM>(font_),TRUE);
    for (HFONT font : oldFonts) if (font) DeleteObject(font);
}
void App::Initialize() {
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_STANDARD_CLASSES|ICC_BAR_CLASSES}; InitCommonControlsEx(&controls);
    renderer_.Initialize();
    LARGE_INTEGER frequency{};
    if (QueryPerformanceFrequency(&frequency) && frequency.QuadPart>0)
        millisecondsPerQpcTick_=1000.0/static_cast<double>(frequency.QuadPart);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.hInstance=instance; wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);
    wc.hIcon=LoadIconW(instance,MAKEINTRESOURCEW(IDI_UPSCALP));
    wc.lpfnWndProc=MainProc; wc.lpszClassName=MainClass; RegisterClassExW(&wc);
    wc.hIcon=nullptr;
    wc.lpfnWndProc=SurfaceProc; wc.lpszClassName=PreviewClass; wc.hbrBackground=static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)); RegisterClassExW(&wc);
    wc.lpfnWndProc=PatternProc; wc.lpszClassName=PatternClass; RegisterClassExW(&wc);
    dpi_=GetDpiForSystem();
    UpdateFonts();
    panelBrush_=CreateSolidBrush(Panel);
    main=CreateWindowExW(WS_EX_APPWINDOW,MainClass,L"Upscalp · Supersampling Lab",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,
        CW_USEDEFAULT,CW_USEDEFAULT,D(1200),D(850),nullptr,nullptr,instance,this);
    if (const UINT windowDpi=GetDpiForWindow(main); windowDpi && windowDpi!=dpi_) {
        dpi_=windowDpi;
        UpdateFonts();
    }
    if (!main) throw_last_error();
    BOOL dark=TRUE; DwmSetWindowAttribute(main,DWMWA_USE_IMMERSIVE_DARK_MODE,&dark,sizeof(dark));
    Add(WC_COMBOBOXW,L"",ID_WINDOWS,CBS_DROPDOWNLIST|WS_VSCROLL);
    Add(WC_BUTTONW,L"Yenile",ID_REFRESH,BS_OWNERDRAW);
    Add(WC_BUTTONW,L"Yakalamayı başlat",ID_START,BS_OWNERDRAW);
    Add(WC_COMBOBOXW,L"",ID_SIZE,CBS_DROPDOWNLIST);
    for (auto text : {L"2560 × 1440  ·  Hafif",L"3200 × 1800  ·  Dengeli",L"3840 × 2160  ·  4K"}) SendMessageW(Control(ID_SIZE),CB_ADDSTRING,0,reinterpret_cast<LPARAM>(text));
    SendMessageW(Control(ID_SIZE),CB_SETCURSEL,2,0);
    Add(WC_BUTTONW,L"Seçili ekranda başlat",ID_RESIZE,BS_OWNERDRAW);
    Add(WC_BUTTONW,L"Pencereyi eski boyutuna döndür",ID_RESTORE,BS_OWNERDRAW);
    Add(WC_COMBOBOXW,L"",ID_FILTER,CBS_DROPDOWNLIST);
    for (auto text : {L"Alan ortalaması  ·  Yumuşak",L"Mitchell  ·  Dengeli",L"Lanczos 2  ·  Keskin",L"Bilinear  ·  Referans",
        L"Lanczos 3  ·  Hale kontrollü"}) SendMessageW(Control(ID_FILTER),CB_ADDSTRING,0,reinterpret_cast<LPARAM>(text));
    SendMessageW(Control(ID_FILTER),CB_SETCURSEL,static_cast<WPARAM>(options_.filter),0);
    Add(TRACKBAR_CLASSW,L"",ID_SHARP,TBS_HORZ|TBS_NOTICKS);
    SendMessageW(Control(ID_SHARP),TBM_SETRANGE,TRUE,MAKELPARAM(0,100)); SendMessageW(Control(ID_SHARP),TBM_SETPOS,TRUE,15);
    Add(WC_BUTTONW,L"Lineer ışıkta küçült",ID_LINEAR,BS_AUTOCHECKBOX);
    Add(WC_BUTTONW,L"Sol: bilinear / sağ: seçilen filtre",ID_COMPARE,BS_AUTOCHECKBOX);
    SendMessageW(Control(ID_LINEAR),BM_SETCHECK,BST_CHECKED,0);
    Add(WC_BUTTONW,L"TV'de tam ekran göster",ID_FULL,BS_OWNERDRAW);
    Add(WC_BUTTONW,L"1080p karşılaştırma karesini kaydet",ID_SAVE,BS_OWNERDRAW);
    Add(WC_BUTTONW,L"Durdur ve pencereyi geri yükle",ID_STOP,BS_OWNERDRAW);
    Add(WC_BUTTONW,L"4K test deseni",ID_PATTERN,BS_OWNERDRAW);
    Add(WC_COMBOBOXW,L"",ID_MONITOR,CBS_DROPDOWNLIST|WS_VSCROLL);
    Add(WC_BUTTONW,L"Kaynağı ekran dışında tut",ID_PARK,BS_AUTOCHECKBOX);
    Add(WC_BUTTONW,L"Gerçek fare (ayrı kaynak ekran)",ID_NATIVE_INPUT,BS_AUTOCHECKBOX);
    SendMessageW(Control(ID_PARK),BM_SETCHECK,BST_CHECKED,0);
    preview=CreateWindowExW(0,PreviewClass,L"",WS_CHILD|WS_VISIBLE,0,0,100,100,main,nullptr,instance,nullptr);
    previewDisplay_.hwnd=preview;
    bool emergency=RegisterHotKey(main,HOT_EMERGENCY,MOD_CONTROL|MOD_ALT|MOD_NOREPEAT,VK_END)!=FALSE;
    bool full=RegisterHotKey(main,HOT_FULL,MOD_CONTROL|MOD_ALT|MOD_NOREPEAT,VK_F9)!=FALSE;
    bool compare=RegisterHotKey(main,HOT_COMPARE,MOD_CONTROL|MOD_ALT|MOD_NOREPEAT,VK_F8)!=FALSE;
    bool start=RegisterHotKey(main,HOT_START,MOD_CONTROL|MOD_ALT|MOD_NOREPEAT,VK_F7)!=FALSE;
    bool mouse=RegisterHotKey(main,HOT_MOUSE,MOD_CONTROL|MOD_ALT|MOD_NOREPEAT,VK_F10)!=FALSE;
    if (!emergency || !full || !compare || !start || !mouse) hotkeyWarning_=L"Bazı kısayollar başka uygulamada kullanımda. Durdur düğmesini kullanın.";
    if (!emergency) EnableWindow(Control(ID_FULL),FALSE);
    SetTimer(main,1,250,nullptr);
    wchar_t exe[MAX_PATH]{}; GetModuleFileNameW(nullptr,exe,MAX_PATH);
    settingsPath_=(std::filesystem::path(exe).parent_path()/L"settings.ini").wstring();
    wchar_t savedDisplay[1024]{};
    GetPrivateProfileStringW(L"display",L"target",L"",savedDisplay,1024,settingsPath_.c_str());
    preferredDisplay_=savedDisplay;
    Layout(); RefreshDisplays(); RefreshWindows();

    auto findDisplay=[&](const std::wstring& name) {
        auto match=displays_.end();
        for (auto display=displays_.begin();display!=displays_.end();++display) {
            if (_wcsicmp(display->device.c_str(),name.c_str())!=0 &&
                _wcsicmp(display->name.c_str(),name.c_str())!=0) continue;
            if (match!=displays_.end()) throw hresult_error(E_INVALIDARG,L"Ekranı seçmek için Windows ekran kimliğini kullanın.");
            match=display;
        }
        if (match==displays_.end()) throw hresult_error(E_INVALIDARG,L"İstenen ekran Windows'ta etkin değil.");
        return match;
    };
    auto selectSource=[&](HMONITOR monitor) {
        for (size_t i=0;i<sources_.size();++i)
            if (sources_[i].monitor==monitor) SendMessageW(Control(ID_WINDOWS),CB_SETCURSEL,i,0);
        UpdateSourceControls();
    };
    if (!startupOutputDisplay.empty()) {
        const auto target=findDisplay(startupOutputDisplay);
        SendMessageW(Control(ID_MONITOR),CB_SETCURSEL,target-displays_.begin(),0);
    }
    if (!startupSourceDisplay.empty()) {
        const auto source=findDisplay(startupSourceDisplay);
        const auto* output=SelectedDisplay();
        if (output && source->monitor==output->monitor) {
            const auto alternate=std::find_if(displays_.begin(),displays_.end(),[&](const auto& display) {
                return display.monitor!=source->monitor;
            });
            if (!startupOutputDisplay.empty() || alternate==displays_.end())
                throw hresult_error(E_INVALIDARG,L"Kaynak ekran ve çıktı ekranı farklı olmalı.");
            SendMessageW(Control(ID_MONITOR),CB_SETCURSEL,alternate-displays_.begin(),0);
        }
        selectSource(source->monitor);
        SetStatus(L"Kaynak ekran hazır. Ctrl+Alt+F7 ile TV'ye yansıtın.");
    }

    // A shell launcher may set STARTUPINFO to hidden. A second explicit call
    // ensures this interactive control window is still visible.
    ShowWindow(main,SW_SHOW); ShowWindow(main,SW_SHOWNORMAL); UpdateWindow(main);
    if (smoke) {
        smokeStart_=GetTickCount64();
        if (monitorSmoke) {
            const auto* output=SelectedDisplay();
            if (!output) throw hresult_error(E_FAIL,L"Monitor smoke test needs an output display.");
            const auto mainMonitor=MonitorFromWindow(main,MONITOR_DEFAULTTONEAREST);
            const auto source=std::find_if(displays_.begin(),displays_.end(),[&](const auto& display) {
                return display.monitor!=output->monitor && display.monitor!=mainMonitor;
            });
            const auto fallback=std::find_if(displays_.begin(),displays_.end(),[&](const auto& display) {
                return display.monitor!=output->monitor;
            });
            const auto chosen=startupSourceDisplay.empty() ? (source!=displays_.end() ? source : fallback)
                                                           : findDisplay(startupSourceDisplay);
            if (chosen==displays_.end()) throw hresult_error(E_FAIL,L"Monitor smoke test needs two displays.");
            if (chosen->monitor==output->monitor) throw hresult_error(E_INVALIDARG,L"Test source and output must be distinct.");
            MakePattern(chosen->monitor);
            selectSource(chosen->monitor);
            BeginSupersampling();
        } else {
            MakePattern();
            Start(pattern);
        }
    }
}
void App::Layout() {
    RECT rect{}; GetClientRect(main,&rect);
    auto move=[&](int id,int x,int y,int w,int h) { MoveWindow(Control(id),D(x),D(y),D(w),D(h),TRUE); };
    move(ID_WINDOWS,36,157,278,220); move(ID_REFRESH,36,195,82,34); move(ID_START,126,195,188,34);
    move(ID_SIZE,36,280,278,160); move(ID_RESIZE,36,319,278,32); move(ID_RESTORE,36,355,278,30);
    move(ID_PARK,36,390,278,22);
    move(ID_FILTER,36,439,278,180); move(ID_SHARP,32,510,286,28);
    move(ID_LINEAR,36,550,278,26); move(ID_COMPARE,36,582,278,26);
    move(ID_FULL,36,627,278,34); move(ID_SAVE,36,669,278,34); move(ID_STOP,36,711,278,34);
    MoveWindow(Control(ID_PATTERN),rect.right-D(184),D(35),D(148),D(36),TRUE);
    MoveWindow(Control(ID_NATIVE_INPUT),D(366),D(39),D(270),D(28),TRUE);
    MoveWindow(Control(ID_MONITOR),D(366),D(155),std::max(D(200),static_cast<int>(rect.right)-D(402)),D(190),TRUE);
    int availableWidth=std::max(D(200),static_cast<int>(rect.right)-D(402));
    int availableHeight=std::max(D(120),static_cast<int>(rect.bottom)-D(463));
    int width=std::min(availableWidth,availableHeight*16/9);
    int height=width*9/16;
    int left=D(366)+(availableWidth-width)/2;
    previewBounds_={left,D(206),left+width,D(206)+height};
    MoveWindow(preview,previewBounds_.left,previewBounds_.top,width,height,TRUE);
    InvalidateRect(main,nullptr,FALSE);
    Redraw();
}
void App::Text(HDC dc,const std::wstring& text,RECT bounds,HFONT font,COLORREF color,UINT flags) const {
    auto old=SelectObject(dc,font); SetTextColor(dc,color); SetBkMode(dc,TRANSPARENT);
    DrawTextW(dc,text.c_str(),static_cast<int>(text.size()),&bounds,flags); SelectObject(dc,old);
}
void App::Paint() {
    PAINTSTRUCT ps{}; HDC dc=BeginPaint(main,&ps);
    RECT rect{}; GetClientRect(main,&rect);
    HBRUSH background=CreateSolidBrush(Background); FillRect(dc,&rect,background); DeleteObject(background);
    auto label=[&](std::wstring text,int x,int y,int w,int h,HFONT font,COLORREF color) { Text(dc,text,{D(x),D(y),D(x+w),D(y+h)},font,color); };
    label(L"upscalp",28,22,270,45,titleFont_,Ink);
    label(L"SUPERSAMPLING LAB  /  0.4",30,72,380,22,smallFont_,Accent);
    RECT left{D(20),D(112),D(330),rect.bottom-D(20)}; FillRect(dc,&left,panelBrush_);
    RECT right{D(350),D(112),rect.right-D(20),rect.bottom-D(20)}; FillRect(dc,&right,panelBrush_);
    label(L"KAYNAK PENCERE VEYA EKRAN",36,132,278,20,smallFont_,Muted);
    label(L"02   KAYNAK BOYUTUNU DENE",36,255,278,20,smallFont_,Muted);
    label(L"03   KÜÇÜLTME FİLTRESİ",36,414,278,20,smallFont_,Muted);
    auto sharp=static_cast<int>(SendMessageW(Control(ID_SHARP),TBM_GETPOS,0,0));
    label(L"Keskinlik  ·  "+std::to_wstring(sharp)+L"%",36,486,278,21,font_,Ink);
    label(L"Ctrl+Alt+F7  Başlat / durdur",36,756,280,19,smallFont_,Accent);
    label(L"Ctrl+Alt+End  Acil durdur",36,775,280,18,smallFont_,Muted);
    Text(dc,L"ÇIKTI EKRANI / TV",{D(366),D(132),rect.right-D(36),D(153)},smallFont_,Muted);
    int y=previewBounds_.bottom+D(20);
    int x=D(366),width=rect.right-D(402);
    if (!previewVisible_)
        Text(dc,fullDisplay_.hwnd ? L"TV çıktısı etkin" : L"Önizleme için kontrolü başka bir ekrana taşıyın.",
            previewBounds_,font_,Muted,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    std::wstring source=renderer_.HasSource() ? std::to_wstring(renderer_.SourceWidth())+L" × "+std::to_wstring(renderer_.SourceHeight()) : L"Kaynak bekleniyor";
    Text(dc,source+L"  →  1920 × 1080",{x,y,x+width,y+D(32)},numberFont_,Ink);
    std::wostringstream metrics;
    metrics << std::fixed << std::setprecision(1) << L"İşlenen " << fps_ << L" kare/sn";
    if (fullDisplay_.hwnd) metrics << L"   ·   TV'ye sunulan " << outputFps_ << L" kare/sn";
    metrics << L"   ·   GPU ";
    if (renderer_.LastGpuMs()<0) metrics << L"—"; else metrics << std::setprecision(2) << renderer_.LastGpuMs() << L" ms";
    metrics << L"     Atlanan: " << capture_.Dropped();
    Text(dc,metrics.str(),{x,y+D(40),x+width,y+D(66)},font_,Accent);
    std::wostringstream submission;
    if (submitMedianMs_>=0) {
        submission << std::fixed << std::setprecision(1) << L"Uygulama içi: ortanca " << submitMedianMs_
            << L" ms   ·   P95 " << submitP95Ms_ << L" ms";
    } else {
        submission << renderer_.AdapterName();
    }
    Text(dc,submission.str(),{x,y+D(73),x+width,y+D(95)},smallFont_,Muted);
    Text(dc,status_,{x,y+D(109),x+width,rect.bottom-D(77)},font_,Ink);
    std::wstring footer=hotkeyWarning_.empty() ? L"Ctrl+Alt+F7  Başlat / durdur  ·  Ctrl+Alt+F10  Fareyi bırak / yakala\nCtrl+Alt+F9  Çıktı  ·  Ctrl+Alt+F8  Karşılaştırma" : hotkeyWarning_;
    Text(dc,footer,{x,rect.bottom-D(72),x+width,rect.bottom-D(25)},smallFont_,Muted);
    EndPaint(main,&ps);
}
void App::RefreshWindows() {
    const auto* old=SelectedSource();
    const HWND selected=old ? old->hwnd : nullptr;
    const std::wstring selectedDisplay=old ? old->displayIdentity : std::wstring{};
    sources_.clear(); SendMessageW(Control(ID_WINDOWS),CB_RESETCONTENT,0,0);
    EnumWindows([](HWND hwnd,LPARAM data)->BOOL {
        auto self=reinterpret_cast<App*>(data);
        if (hwnd==self->main || hwnd==self->fullDisplay_.hwnd || !IsWindowVisible(hwnd) || GetWindow(hwnd,GW_OWNER)) return TRUE;
        if (hwnd!=self->pattern && (GetWindowLongPtrW(hwnd,GWL_EXSTYLE)&(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE))) return TRUE;
        DWORD cloaked{}; DwmGetWindowAttribute(hwnd,DWMWA_CLOAKED,&cloaked,sizeof(cloaked));
        if (cloaked) return TRUE;
        int length=GetWindowTextLengthW(hwnd); if (length<1) return TRUE;
        std::wstring title(static_cast<size_t>(length)+1,L'\0'); GetWindowTextW(hwnd,title.data(),length+1); title.resize(length);
        self->sources_.push_back({hwnd,nullptr,std::move(title)}); return TRUE;
    },reinterpret_cast<LPARAM>(this));
    for (const auto& display : displays_)
        sources_.push_back({nullptr,display.monitor,L"Ekran · "+display.label,display.identity});
    int selection=selectedDisplay.empty() ? 0 : -1;
    for (size_t i=0;i<sources_.size();++i) {
        SendMessageW(Control(ID_WINDOWS),CB_ADDSTRING,0,reinterpret_cast<LPARAM>(sources_[i].title.c_str()));
        if ((selected && sources_[i].hwnd==selected) ||
            (!selectedDisplay.empty() && sources_[i].displayIdentity==selectedDisplay)) selection=static_cast<int>(i);
    }
    if (!sources_.empty()) SendMessageW(Control(ID_WINDOWS),CB_SETCURSEL,selection,0);
    UpdateSourceControls();
}
void App::UpdateSourceControls() {
    const auto* source=SelectedSource();
    const bool monitor=source && source->monitor;
    const bool native=SendMessageW(Control(ID_NATIVE_INPUT),BM_GETCHECK,0,0)==BST_CHECKED;
    EnableWindow(Control(ID_SIZE),!monitor && !native);
    EnableWindow(Control(ID_RESTORE),!monitor);
    EnableWindow(Control(ID_PARK),!monitor && !native);
    EnableWindow(Control(ID_NATIVE_INPUT),!monitor && !capture_.Running());
    SetWindowTextW(Control(ID_RESIZE),monitor ? L"Ekranı TV'ye yansıt" :
        native ? L"Gerçek fareyle başlat" : L"Seçili ekranda başlat");
}
const SourceEntry* App::SelectedSource() const {
    auto index=SendMessageW(Control(ID_WINDOWS),CB_GETCURSEL,0,0);
    return index>=0 && static_cast<size_t>(index)<sources_.size() ? &sources_[index] : nullptr;
}
HWND App::SelectedWindow() const {
    const auto* source=SelectedSource();
    return source ? source->hwnd : nullptr;
}
std::vector<RECT> App::DisplayBounds() const {
    std::vector<RECT> bounds;
    for (const auto& display : displays_) bounds.push_back(display.bounds);
    return bounds;
}
const DisplayTarget* App::SelectedDisplay() const {
    auto index=SendMessageW(Control(ID_MONITOR),CB_GETCURSEL,0,0);
    return index>=0 && static_cast<size_t>(index)<displays_.size() ? &displays_[index] : nullptr;
}
void App::RefreshDisplays() {
    SetDisplays(EnumerateDisplays());
}
void App::SetDisplays(std::vector<DisplayTarget> displays) {
    if (const auto* previous=SelectedDisplay()) preferredDisplay_=previous->identity;
    displays_=std::move(displays); SendMessageW(Control(ID_MONITOR),CB_RESETCONTENT,0,0);
    int selection=-1,samsung=-1;
    for (size_t i=0;i<displays_.size();++i) {
        const auto& display=displays_[i];
        SendMessageW(Control(ID_MONITOR),CB_ADDSTRING,0,reinterpret_cast<LPARAM>(display.label.c_str()));
        if (display.identity==preferredDisplay_) selection=static_cast<int>(i);
        // Prefer the TV's exact EDID name only when no explicit choice was saved.
        if (_wcsicmp(display.name.c_str(),L"SAMSUNG")==0) samsung=static_cast<int>(i);
    }
    if (selection<0 && preferredDisplay_.empty()) selection=samsung;
    if (selection<0 && !displays_.empty()) {
        HMONITOR current=MonitorFromWindow(main,MONITOR_DEFAULTTONEAREST);
        selection=0;
        for (size_t i=0;i<displays_.size();++i) if (displays_[i].monitor==current) selection=static_cast<int>(i);
    }
    if (selection>=0) SendMessageW(Control(ID_MONITOR),CB_SETCURSEL,selection,0);
}
void App::QueueDisplayRefresh() {
    if (!displayRefreshDeadline_) displayRefreshDeadline_=GetTickCount64()+3000;
    SetTimer(main,DisplayRefreshTimer,150,nullptr);
}
void App::RefreshDisplayTopology() {
    KillTimer(main,DisplayRefreshTimer);
    auto displays=EnumerateDisplays();
    auto find=[&](const std::wstring& identity) {
        return std::find_if(displays.begin(),displays.end(),[&](const auto& display) {
            return display.identity==identity;
        });
    };
    const auto* previousOutput=SelectedDisplay();
    const auto output=previousOutput ? find(previousOutput->identity) : displays.end();
    const auto source=find(captureDisplayIdentity_);
    const auto previousNative=std::find_if(displays_.begin(),displays_.end(),[&](const auto& display) {
        return display.monitor==nativeWindowMonitor_;
    });
    const auto native=previousNative!=displays_.end() ? find(previousNative->identity) : displays.end();
    const bool outputMissing=(fullDisplay_.hwnd || pendingOutput_) && output==displays.end();
    const bool sourceMissing=(!captureDisplayIdentity_.empty() && source==displays.end()) ||
                             (nativeWindowMonitor_ && native==displays.end());
    if (outputMissing || sourceMissing) {
        ReleaseNativeClip();
        if (GetTickCount64()<displayRefreshDeadline_) { QueueDisplayRefresh(); return; }
        Stop();
        SetDisplays(std::move(displays)); RefreshWindows();
        SetStatus(L"Kaynak veya çıktı ekranı bağlantısı kesildi. Ekranları seçip yansıtmayı yeniden başlatın.");
        return;
    }
    if (source!=displays.end() && (!capture_.Running() || capture_.SourceMonitor()!=source->monitor)) {
        try {
            framePending_=false;
            ReleaseNativeClip();
            capture_.StartMonitor(source->monitor,main,renderer_.Device());
        } catch (const hresult_error&) {
            if (GetTickCount64()<displayRefreshDeadline_) { QueueDisplayRefresh(); return; }
            throw;
        }
        lastFrame_=GetTickCount64();
    }
    if (nativeWindowMonitor_ && native!=displays.end()) nativeWindowMonitor_=native->monitor;
    if (fullDisplay_.hwnd && output!=displays.end()) {
        RECT old{};
        const RECT& bounds=output->bounds;
        if (GetWindowRect(fullDisplay_.hwnd,&old) && !EqualRect(&old,&bounds)) {
            if (!SetWindowPos(fullDisplay_.hwnd,nullptr,bounds.left,bounds.top,bounds.right-bounds.left,
                              bounds.bottom-bounds.top,SWP_NOZORDER|SWP_NOACTIVATE)) throw_last_error();
            if (!NativeInputMonitor() && capture_.Source()) {
                const bool armed=input_.Armed();
                input_.Start(capture_.Source(),fullDisplay_.hwnd);
                if (!armed) input_.Release();
            }
        }
    }
    displayRefreshDeadline_=0;
    SetDisplays(std::move(displays)); RefreshWindows();
    UpdateNativeClip();
    Redraw();
}
void App::ChangeDisplay() {
    const auto* selected=SelectedDisplay(); if (!selected) return;
    preferredDisplay_=selected->identity;
    WritePrivateProfileStringW(L"display",L"target",preferredDisplay_.c_str(),settingsPath_.c_str());
    bool showing=fullDisplay_.hwnd!=nullptr;
    if (showing) ToggleFullscreen();
    if (NativeInputMonitor()==selected->monitor) {
        pendingOutput_=false;
        SetStatus(L"Kaynak ve çıktı aynı ekran olamaz. Çıktı için farklı bir ekran seçin.");
        WriteSessionReport();
        return;
    }
    if (showing) ToggleFullscreen();
    SetStatus(L"Çıktı ekranı: "+selected->label+L". Kontrol penceresini taşımak bu seçimi değiştirmez.");
    WriteSessionReport();
}
void App::WriteSessionReport() {
    if (sessionReport.empty()) return;
    RECT source{},client{},output{};
    HWND sourceWindow=capture_.Source();
    if (sourceWindow) { GetWindowRect(sourceWindow,&source); GetClientRect(sourceWindow,&client); }
    if (fullDisplay_.hwnd) GetWindowRect(fullDisplay_.hwnd,&output);
    auto rectangle=[](const RECT& r) { return "["+std::to_string(r.left)+","+std::to_string(r.top)+","+std::to_string(r.right)+","+std::to_string(r.bottom)+"]"; };
    const auto* selected=SelectedDisplay();
    std::ofstream file(std::filesystem::path(sessionReport),std::ios::binary);
    file << "{\"captureActive\":" << (capture_.Running() ? "true" : "false")
         << ",\"sourceKind\":\"" << (capture_.SourceMonitor() ? "monitor" : sourceWindow ? "window" : "none") << "\""
         << ",\"sourceRect\":" << rectangle(source) << ",\"sourceClient\":[" << client.right << "," << client.bottom << "]"
         << ",\"sourceIntersectsDisplay\":" << (IntersectsAnyDisplay(source,DisplayBounds()) ? "true" : "false")
         << ",\"sourceParked\":" << (parked_ ? "true" : "false")
         << ",\"capturedSize\":[" << renderer_.SourceWidth() << "," << renderer_.SourceHeight() << "]"
         << ",\"captureFps\":" << fps_ << ",\"outputSubmitFps\":" << (fullDisplay_.hwnd ? outputFps_ : 0)
         << ",\"lastFrameAgeMs\":" << (lastFrame_ ? GetTickCount64()-lastFrame_ : 0)
         << ",\"fullscreen\":" << (fullDisplay_.hwnd ? "true" : "false")
         << ",\"waitableOutput\":" << (fullDisplay_.frameLatency ? "true" : "false")
         << ",\"submittedFrames\":" << submittedFrames_
         << ",\"arrivalToSubmitMedianMs\":" << submitMedianMs_
         << ",\"arrivalToSubmitP95Ms\":" << submitP95Ms_
         << ",\"droppedCaptureFrames\":" << capture_.Dropped()
         << ",\"outputPending\":" << (pendingOutput_ ? "true" : "false")
         << ",\"mouseArmed\":" << (input_.Armed() ? "true" : "false")
         << ",\"nativeMouse\":" << (NativeInputMonitor() ? "true" : "false")
         << ",\"mouseConfined\":" << (input_.Confined() || nativeClip_ ? "true" : "false")
         << ",\"mouseMessages\":" << input_.Forwarded()
         << ",\"outputRect\":" << rectangle(output)
         << ",\"selectedDisplayRect\":" << rectangle(selected ? selected->bounds : RECT{}) << ",\"displays\":[";
    for (size_t i=0;i<displays_.size();++i) {
        if (i) file << ',';
        file << "{\"name\":\"" << JsonString(displays_[i].name) << "\",\"rect\":" << rectangle(displays_[i].bounds) << "}";
    }
    file << "]}\n";
}
void App::Start(HWND target) {
    HMONITOR monitor{};
    if (!target) {
        if (const auto* source=SelectedSource()) { target=source->hwnd; monitor=source->monitor; }
    }
    if (!monitor && !IsWindow(target)) { SetStatus(L"Önce açık bir oyun penceresi veya kaynak ekran seçin."); return; }
    Stop();
    if (!renderer_.Device() || FAILED(renderer_.Device()->GetDeviceRemovedReason())) {
        previewDisplay_={}; previewDisplay_.hwnd=preview;
        Renderer replacement;
        replacement.Initialize();
        renderer_=std::move(replacement);
    }
    renderer_.ResetSource();
    if (monitor) {
        const auto display=std::find_if(displays_.begin(),displays_.end(),[&](const auto& entry) { return entry.monitor==monitor; });
        if (display==displays_.end()) throw hresult_error(E_INVALIDARG,L"Kaynak ekran artık etkin değil.");
        capture_.StartMonitor(monitor,main,renderer_.Device());
        captureDisplayIdentity_=display->identity;
    }
    else capture_.Start(target,main,renderer_.Device());
    ResetSubmissions();
    fpsFrames_=outputFpsFrames_=0; fps_=outputFps_=0; fpsStart_=GetTickCount64(); lastFrame_=fpsStart_;
    EnableWindow(Control(ID_WINDOWS),FALSE); EnableWindow(Control(ID_REFRESH),FALSE);
    UpdateSourceControls();
    SetStatus(monitor ? L"Kaynak ekran yakalanıyor. Oyunu bu ekrana taşıyıp odağı oyunda tutun."
        : L"Yakalama başladı. İlk kare bekleniyor. Oyun simge durumuna küçültülmemeli.");
}
bool App::RestoreTarget() {
    if (!resizedTarget_) { parked_=false; return true; }
    DWORD pid{}; GetWindowThreadProcessId(resizedTarget_,&pid);
    if (!IsWindow(resizedTarget_) || pid!=resizedPid_) { resizedTarget_=nullptr; parked_=false; return true; }
    if (!SetWindowPlacement(resizedTarget_,&savedPlacement_)) return false;
    resizedTarget_=nullptr; parked_=false; return true;
}
void App::Stop(bool restore) {
    KillTimer(main,PresentRetryTimer); KillTimer(main,DisplayRefreshTimer);
    presentRetry_=false; displayRefreshDeadline_=0;
    ReleaseNativeClip(); nativeMouseWindow_=nullptr;
    captureDisplayIdentity_.clear();
    pendingOutput_=false;
    framePending_=false;
    input_.Stop();
    capture_.Stop();
    nativeWindowMonitor_=nullptr;
    if (fullDisplay_.hwnd) {
        HWND hwnd=fullDisplay_.hwnd; fullDisplay_={}; DestroyWindow(hwnd);
    }
    const bool restored=!restore || RestoreTarget();
    fps_=0;
    if (IsWindow(main)) {
        EnableWindow(Control(ID_WINDOWS),TRUE); EnableWindow(Control(ID_REFRESH),TRUE);
        UpdateSourceControls();
        SetWindowTextW(Control(ID_FULL),L"TV'de tam ekran göster");
        SetStatus(restored ? L"Yakalama durduruldu. Son kare önizlemede kalır." : L"Yakalama durduruldu; pencere geri yüklenemedi. Eski boyuta döndür düğmesini tekrar deneyin.");
    }
    Redraw();
    WriteSessionReport();
}
void App::BeginSupersampling() {
    const auto* selectedSource=SelectedSource();
    const auto* output=SelectedDisplay();
    HMONITOR sourceMonitor=capture_.Running() ? capture_.SourceMonitor() :
        selectedSource ? selectedSource->monitor : nullptr;
    if (sourceMonitor) {
        if (!output || output->monitor==sourceMonitor) {
            SetStatus(L"Kaynak ekran ve çıktı ekranı farklı olmalı."); return;
        }
        if (!IsWindowEnabled(Control(ID_FULL))) {
            SetStatus(L"Acil durdurma kısayolu kullanımda. TV çıktısı başlatılamadı."); return;
        }
        if (!capture_.Running()) Start();
        if (!capture_.Running()) return;
        pendingOutput_=!renderer_.HasSource();
        if (!pendingOutput_ && !fullDisplay_.hwnd) ToggleFullscreen();
        return;
    }
    HWND target=capture_.Running() ? capture_.Source() : SelectedWindow();
    if (!IsWindow(target) || IsIconic(target)) {
        SetStatus(L"Önce açık ve küçültülmemiş bir oyun penceresi seçin."); return;
    }
    // Validate the output before taking the source off the visible desktop.
    if (!output) { SetStatus(L"Önce çıktı ekranını seçin."); return; }
    if (!IsWindowEnabled(Control(ID_FULL))) {
        SetStatus(L"Acil durdurma kısayolu kullanımda. TV çıktısı başlatılamadığı için oyun taşınmadı."); return;
    }
    if (SendMessageW(Control(ID_NATIVE_INPUT),BM_GETCHECK,0,0)==BST_CHECKED) {
        BeginNativeWindow(target,*output);
        return;
    }
    if (!capture_.Running()) Start(target);
    if (!capture_.Running()) return;
    pendingOutput_=true;
    if (!ResizeTarget()) { pendingOutput_=false; return; }
    if (renderer_.HasSource() && !fullDisplay_.hwnd) {
        pendingOutput_=false; ToggleFullscreen();
    }
    SetStatus(pendingOutput_ ? L"Yakalama başladı. İlk kare gelir gelmez seçili TV'de çıktı açılacak." :
        L"Görüntü seçili ekrana gönderiliyor. Ctrl+Alt+F10 fareyi bırakır; Ctrl+Alt+F7 upscale'i durdurur.");
}
void App::SaveTargetPlacement(HWND target) {
    if (resizedTarget_==target) return;
    if (resizedTarget_ && !RestoreTarget()) throw hresult_error(E_FAIL,L"Önce önceki pencereyi geri yükleyin.");
    savedPlacement_={sizeof(WINDOWPLACEMENT)};
    if (!GetWindowPlacement(target,&savedPlacement_)) throw_last_error();
    resizedTarget_=target;
    GetWindowThreadProcessId(target,&resizedPid_);
}
void App::BeginNativeWindow(HWND target,const DisplayTarget& output) {
    const HMONITOR current=MonitorFromWindow(target,MONITOR_DEFAULTTONULL);
    const DisplayTarget* source=nullptr;
    int best=-1;
    for (const auto& display : displays_) {
        if (display.monitor==output.monitor) continue;
        const bool active4k=display.bounds.right-display.bounds.left>=3840 &&
            display.bounds.bottom-display.bounds.top>=2160;
        bool offers4k60=false;
        for (DWORD index=0;!active4k && index<4096;++index) {
            DEVMODEW mode{};
            mode.dmSize=sizeof(mode);
            if (!EnumDisplaySettingsExW(display.device.c_str(),index,&mode,0)) break;
            if (mode.dmPelsWidth==3840 && mode.dmPelsHeight==2160 && mode.dmDisplayFrequency==60) {
                offers4k60=true;
                break;
            }
        }
        const int rank=(active4k ? 4 : offers4k60 ? 2 : 0)+(display.monitor==current ? 1 : 0);
        if (rank>best) { best=rank; source=&display; }
    }
    if (!source) {
        SetStatus(L"Gerçek fare için TV'den ayrı bir kaynak ekran gerekli.");
        return;
    }
    Start(target);
    if (!capture_.Running()) return;
    SaveTargetPlacement(target);
    if (IsZoomed(target)) ShowWindow(target,SW_RESTORE);
    MONITORINFO info{sizeof(info)};
    if (!GetMonitorInfoW(source->monitor,&info)) throw_last_error();
    const LONG_PTR style=GetWindowLongPtrW(target,GWL_STYLE);
    const RECT area=(style&WS_CAPTION)==WS_CAPTION ? info.rcWork : info.rcMonitor;
    if (!SetWindowPos(target,nullptr,area.left,area.top,area.right-area.left,area.bottom-area.top,
        SWP_NOACTIVATE|SWP_NOZORDER))
        throw_last_error();
    if (MonitorFromWindow(target,MONITOR_DEFAULTTONULL)!=source->monitor)
        throw hresult_error(E_FAIL,L"Oyun kaynak ekrana geçmedi. Oyunu pencereli moda alıp yeniden deneyin.");
    nativeWindowMonitor_=source->monitor;
    pendingOutput_=true;
    SetStatus(L"Gerçek fare kaynağı: "+source->label+L". İlk kare bekleniyor.");
}
bool App::ResizeTarget() {
    HWND target=capture_.Running() ? capture_.Source() : SelectedWindow();
    if (!IsWindow(target) || IsIconic(target)) { SetStatus(L"Boyut denemesi için açık ve küçültülmemiş bir pencere seçin."); return false; }
    SaveTargetPlacement(target);
    if (IsZoomed(target)) ShowWindow(target,SW_RESTORE);
    int index=static_cast<int>(SendMessageW(Control(ID_SIZE),CB_GETCURSEL,0,0));
    constexpr int widths[]={2560,3200,3840},heights[]={1440,1800,2160};
    index=std::clamp(index,0,2);
    RECT rect{0,0,widths[index],heights[index]};
    auto style=static_cast<DWORD>(GetWindowLongPtrW(target,GWL_STYLE));
    auto ex=static_cast<DWORD>(GetWindowLongPtrW(target,GWL_EXSTYLE));
    if (!AdjustWindowRectExForDpi(&rect,style,GetMenu(target)!=nullptr,ex,GetDpiForWindow(target))) throw_last_error();
    const auto* display=SelectedDisplay();
    if (!display) throw hresult_error(E_FAIL,L"Önce çıktı ekranını seçin.");
    LONG x=display->bounds.left+rect.left,y=display->bounds.top+rect.top;
    bool park=SendMessageW(Control(ID_PARK),BM_GETCHECK,0,0)==BST_CHECKED;
    if (park) {
        auto offscreen=ParkOutsideDisplays(DisplayBounds(),rect.right-rect.left,rect.bottom-rect.top,display->bounds.left);
        x=offscreen.left; y=offscreen.top;
    }
    if (!SetWindowPos(target,nullptr,x,y,rect.right-rect.left,rect.bottom-rect.top,SWP_NOZORDER|SWP_NOACTIVATE)) throw_last_error();
    RECT actual{};
    if (GetClientRect(target,&actual) && (actual.right!=widths[index] || actual.bottom!=heights[index])) {
        // Default WM_WINDOWPOSCHANGING processing can clamp a decorated 4K
        // window to the virtual desktop's max tracking height. Keep WM_SIZE
        // delivery but retry without that pre-change clamp.
        if (!SetWindowPos(target,nullptr,x,y,rect.right-rect.left,rect.bottom-rect.top,
            SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOSENDCHANGING)) throw_last_error();
    }
    parked_=park; parkedAt_=GetTickCount64(); lastFrame_=parkedAt_;
    SetStatus(park ? L"Büyük kaynak pencere ekranların dışında tutuluyor. Çıktı yalnızca seçili ekrana açılır. İlk denemede gamepad kullanın; menü için pencereyi geri yükleyin." : L"Boyut isteği gönderildi. Ekran dışında tut kapalı: büyük kaynak pencere komşu ekranlara taşabilir.");
    WriteSessionReport();
    return true;
}
void App::Frame() {
    if (!capture_.Running()) return;
    auto captured=capture_.TakeLatest(); if (!captured.frame) return;
    auto size=captured.frame.ContentSize();
    if (capture_.ResizePoolIfNeeded(captured,size)) return;
    {
        auto access=captured.frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        com_ptr<ID3D11Texture2D> texture;
        check_hresult(access->GetInterface(__uuidof(ID3D11Texture2D),texture.put_void()));
        RECT crop=capture_.SourceMonitor() ? RECT{0,0,size.Width,size.Height} :
            Capture::ClientCrop(capture_.Source(),size.Width,size.Height);
        renderer_.CopyFrame(texture.get(),crop);
    }
    captured.frame.Close();
    renderer_.Process(options_);
    optionsDirty_=false;
    sourceArrivalQpc_=captured.arrivalQpc;
    if (pendingOutput_ && !fullDisplay_.hwnd) {
        pendingOutput_=false; ToggleFullscreen();
    }
    Redraw();
    lastFrame_=GetTickCount64(); ++fpsFrames_;
    if (fpsFrames_==1 && fps_==0) {
        if (nativeWindowMonitor_) {
            const bool is4K=renderer_.SourceWidth()>=3840 && renderer_.SourceHeight()>=2160;
            SetStatus(is4K ? L"Gerçek fare etkin. 4K oyun penceresi TV'ye yansıtılıyor." :
                L"Gerçek fare etkin. Tam 4K için kaynak ekranı Windows'ta 3840×2160, oyunu çerçevesiz yapın.");
        } else SetStatus(renderer_.SourceWidth()>1920 && renderer_.SourceHeight()>1080 ?
            L"Yüksek çözünürlüklü kaynak alınıyor. Oyunda dinamik çözünürlük ve upscaling ayarlarını ayrıca kontrol edin. Karşılaştırma aynı kaynağın bilinear küçültmesine göredir." :
            L"Kaynak 1080p veya daha düşük. Henüz ek supersampling elde edilmedi. Kaynak boyutunu deneyin veya oyunun kendi render ölçeğini kullanın.");
    }
    if (monitorSmoke) {
        if (!smokePhase_ && submittedFrames_>=20) {
            smokePhase_=1;
            smokeMirrorWindow_=fullDisplay_.hwnd;
            smokeCaptureGeneration_=capture_.Generation();
            PostMessageW(main,WM_DISPLAYCHANGE,32,0);
            PostMessageW(main,WM_CAPTURE_CLOSED,static_cast<WPARAM>(smokeCaptureGeneration_),0);
        }
        if (smokePhase_==1 && submittedFrames_>=60) {
            RECT output{};
            const auto* selected=SelectedDisplay();
            if (fullDisplay_.hwnd!=smokeMirrorWindow_ || capture_.Generation()==smokeCaptureGeneration_ ||
                !capture_.SourceMonitor() || capture_.Source() || !selected || !fullDisplay_.frameLatency || input_.Armed() ||
                !GetWindowRect(fullDisplay_.hwnd,&output) || !EqualRect(&output,&selected->bounds) ||
                renderer_.SourceWidth()!=patternDisplay_.width || renderer_.SourceHeight()!=patternDisplay_.height)
                throw hresult_error(E_FAIL,L"Monitor smoke test: output, source size or input isolation mismatch.");
            if (submissionCount_<60) throw hresult_error(E_FAIL,L"Monitor smoke test: fewer than 60 application latency samples.");
            UpdateSubmissionStats();
            smokeMetrics_={submittedFrames_,submitMedianMs_,submitP95Ms_,renderer_.LastGpuMs()};
            SmokeReport(smokeReport,true,renderer_.SourceWidth(),renderer_.SourceHeight(),
                L"Monitor capture resumed after display-change and capture-close messages without replacing the TV output window.",
                &smokeMetrics_);
            PostMessageW(main,WM_CLOSE,0,0);
        }
    } else if (smoke) {
        const bool is4K=renderer_.SourceWidth()==3840 && renderer_.SourceHeight()==2160;
        const bool is1440p=renderer_.SourceWidth()==2560 && renderer_.SourceHeight()==1440;
        if (smokePhase_==0 && is4K && ++smokeFrames_>=8) {
            smokeFrames_=0; smokePhase_=1;
            Stop(false);
            SendMessageW(Control(ID_SIZE),CB_SETCURSEL,0,0); BeginSupersampling();
        } else if (smokePhase_==1 && is1440p && submittedFrames_>=3 && ++smokeFrames_>=4) {
            RECT output{},source{};
            const auto* selected=SelectedDisplay();
            if (!fullDisplay_.hwnd || !GetWindowRect(fullDisplay_.hwnd,&output) || !selected || !EqualRect(&output,&selected->bounds))
                throw hresult_error(E_FAIL,L"Smoke test: automatic output did not match the selected display.");
            GetWindowRect(capture_.Source(),&source);
            if (IntersectsAnyDisplay(source,DisplayBounds()))
                throw hresult_error(E_FAIL,L"Smoke test: source spilled onto a physical display.");
            // Route a click through the real output window procedure. The
            // independent expected position is the 2560x1440 source center.
            RECT client{}; GetClientRect(fullDisplay_.hwnd,&client);
            auto click=MAKELPARAM(client.right/2,client.bottom/2);
            const auto messages=input_.Forwarded();
            SendMessageW(fullDisplay_.hwnd,WM_MOUSEMOVE,0,click);
            SendMessageW(fullDisplay_.hwnd,WM_LBUTTONDOWN,MK_LBUTTON,click);
            SendMessageW(fullDisplay_.hwnd,WM_LBUTTONUP,0,click);
            smokeClickForwarded_=input_.Forwarded()>messages;
            DWORD foregroundPid{};
            GetWindowThreadProcessId(GetForegroundWindow(),&foregroundPid);
            if (!smokeClickForwarded_ && foregroundPid==GetCurrentProcessId())
                throw hresult_error(E_FAIL,L"Smoke test: focused test pattern did not receive any forwarded messages.");
            smokeFrames_=0; smokePhase_=2;
            SetPropW(pattern,L"Upscalp.TestClamp",reinterpret_cast<HANDLE>(1));
            SendMessageW(Control(ID_SIZE),CB_SETCURSEL,2,0); ResizeTarget();
        } else if (smokePhase_==2 && is4K && smokeFrames_==0) {
            ResetSubmissions();
            sourceArrivalQpc_=captured.arrivalQpc;
            smokeFrames_=1;
        } else if (smokePhase_==2 && is4K && submittedFrames_>=60) {
            auto click=reinterpret_cast<ULONG_PTR>(GetPropW(pattern,L"Upscalp.TestClick"));
            if (smokeClickForwarded_ && (LOWORD(click)!=1280 || HIWORD(click)!=720))
                throw hresult_error(E_FAIL,L"Smoke test: output click did not reach the correct source coordinates.");
            if (submissionCount_<60) throw hresult_error(E_FAIL,L"Smoke test: fewer than 60 application latency samples.");
            UpdateSubmissionStats();
            smokeMetrics_={submittedFrames_,submitMedianMs_,submitP95Ms_,renderer_.LastGpuMs()};
            // This 4K request must survive the synthetic 1700px tracking cap.
            RemovePropW(pattern,L"Upscalp.TestClamp");
            smokeFrames_=0; smokePhase_=3;
            ToggleFullscreen();
            if (!RestoreTarget()) throw hresult_error(E_FAIL,L"Smoke test: window restore failed.");
        } else if (smokePhase_==3 && is4K && ++smokeFrames_>=4) {
            if (!smokeImage.empty()) renderer_.SaveOutput(smokeImage,options_);
            std::wstring detail=L"WGC, 4K GPU filtering, waitable output, selected monitor bounds, offscreen containment and window restoration verified. ";
            detail+=smokeClickForwarded_ ? L"Test click reached the correct source coordinates." :
                L"Live mouse forwarding was not tested because Windows kept another application in the foreground.";
            SmokeReport(smokeReport,true,renderer_.SourceWidth(),renderer_.SourceHeight(),detail,&smokeMetrics_);
            PostMessageW(main,WM_CLOSE,0,0);
        }
    }
}
void App::Draw() {
    const bool previewVisible=!fullDisplay_.hwnd &&
        capture_.SourceMonitor()!=MonitorFromWindow(main,MONITOR_DEFAULTTONEAREST);
    SetPreviewVisible(previewVisible);
    if (!renderer_.HasSource()) return;
    if (fullDisplay_.hwnd) {
        if (presentRetry_) return;
        if (renderer_.Present(fullDisplay_,options_,true)) RecordSubmission();
        else if (!fullDisplay_.lowLatency || fullDisplay_.frameSlotReady) {
            presentRetry_=SetTimer(main,PresentRetryTimer,16,nullptr)!=0;
        }
    } else if (previewVisible && !IsIconic(main)) {
        renderer_.Present(previewDisplay_,options_,false);
    }
}
void App::SetPreviewVisible(bool visible) {
    if (previewVisible_==visible) return;
    previewVisible_=visible;
    if (IsWindow(preview)) ShowWindow(preview,visible ? SW_SHOWNA : SW_HIDE);
    InvalidateRect(main,&previewBounds_,FALSE);
}
void App::RecordSubmission() {
    if (sourceArrivalQpc_<=0 || sourceArrivalQpc_==submittedArrivalQpc_) return;
    submittedArrivalQpc_=sourceArrivalQpc_;
    ++submittedFrames_;
    ++outputFpsFrames_;
    LARGE_INTEGER now{};
    if (millisecondsPerQpcTick_<=0 || !QueryPerformanceCounter(&now) || now.QuadPart<sourceArrivalQpc_) return;
    const double age=static_cast<double>(now.QuadPart-sourceArrivalQpc_)*millisecondsPerQpcTick_;
    submissionAges_[submissionIndex_]=age;
    submissionIndex_=(submissionIndex_+1)%submissionAges_.size();
    submissionCount_=std::min(submissionCount_+1,submissionAges_.size());
}
void App::ResetSubmissions() {
    submittedFrames_=0;
    submissionCount_=submissionIndex_=0;
    submitMedianMs_=submitP95Ms_=-1;
    sourceArrivalQpc_=submittedArrivalQpc_=0;
}
void App::UpdateSubmissionStats() {
    if (!submissionCount_) return;
    auto ages=submissionAges_;
    std::sort(ages.begin(),ages.begin()+submissionCount_);
    const size_t middle=submissionCount_/2;
    submitMedianMs_=submissionCount_%2 ? ages[middle] : (ages[middle-1]+ages[middle])/2;
    submitP95Ms_=ages[(submissionCount_*95+99)/100-1];
}
void App::UpdateOptions() {
    options_.filter=static_cast<Filter>(std::clamp(static_cast<int>(SendMessageW(Control(ID_FILTER),CB_GETCURSEL,0,0)),0,
        static_cast<int>(Filter::LanczosGuarded)));
    options_.linearLight=SendMessageW(Control(ID_LINEAR),BM_GETCHECK,0,0)==BST_CHECKED;
    options_.comparison=SendMessageW(Control(ID_COMPARE),BM_GETCHECK,0,0)==BST_CHECKED;
    options_.sharpness=static_cast<float>(SendMessageW(Control(ID_SHARP),TBM_GETPOS,0,0))/100;
    optionsDirty_=true; Redraw(); InvalidateRect(main,nullptr,FALSE);
}
void App::ToggleFullscreen() {
    pendingOutput_=false;
    if (fullDisplay_.hwnd) {
        KillTimer(main,PresentRetryTimer); presentRetry_=false;
        ReleaseNativeClip(); nativeMouseWindow_=nullptr;
        input_.Stop();
        HWND hwnd=fullDisplay_.hwnd; fullDisplay_={}; DestroyWindow(hwnd);
        SetWindowTextW(Control(ID_FULL),L"TV'de tam ekran göster"); Redraw(); return;
    }
    if (!capture_.Running() || !renderer_.HasSource()) { SetStatus(L"Tam ekran için önce bir kaynaktan görüntü yakalayın."); return; }
    if (!IsWindowEnabled(Control(ID_FULL))) { SetStatus(L"Acil çıkış kısayolu kullanımda; tam ekran açılamadı."); return; }
    const auto* selected=SelectedDisplay();
    if (!selected) { SetStatus(L"Önce çıktı ekranını seçin."); return; }
    if (selected->monitor==NativeInputMonitor()) { SetStatus(L"Kaynak ve çıktı aynı ekran olamaz."); return; }
    auto r=selected->bounds;
    fullDisplay_.lowLatency=true;
    fullDisplay_.hwnd=CreateWindowExW(WS_EX_TOPMOST|WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW,PreviewClass,L"Upscalp Output",
        WS_POPUP,r.left,r.top,r.right-r.left,r.bottom-r.top,nullptr,nullptr,instance,this);
    if (!fullDisplay_.hwnd) throw_last_error();
    renderer_.PrepareDisplay(fullDisplay_);
    ShowWindow(fullDisplay_.hwnd,SW_SHOWNOACTIVATE);
    SetPreviewVisible(false);
    Redraw();
    if (nativeWindowMonitor_) FocusNativeSource();
    else if (capture_.Source()) input_.Start(capture_.Source(),fullDisplay_.hwnd);
    SetWindowTextW(Control(ID_FULL),L"Tam ekran çıktıyı kapat");
    WriteSessionReport();
}
void App::ToggleUpscale() {
    if (fullDisplay_.hwnd || pendingOutput_ || parked_) Stop();
    else BeginSupersampling();
}
HMONITOR App::NativeInputMonitor() const {
    return nativeWindowMonitor_ ? nativeWindowMonitor_ : capture_.SourceMonitor();
}
void App::ReleaseNativeClip() {
    if (!nativeClip_) return;
    RECT current{};
    if (GetClipCursor(&current) && EqualRect(&current,&*nativeClip_)) ClipCursor(nullptr);
    nativeClip_.reset();
}
void App::UpdateNativeClip() {
    const auto monitor=NativeInputMonitor();
    HWND foreground=GetForegroundWindow();
    DWORD targetPid{},foregroundPid{};
    GetWindowThreadProcessId(nativeMouseWindow_,&targetPid);
    GetWindowThreadProcessId(foreground,&foregroundPid);
    MONITORINFO info{sizeof(info)};
    if (!targetPid || targetPid!=foregroundPid || !fullDisplay_.hwnd || !monitor ||
        MonitorFromWindow(foreground,MONITOR_DEFAULTTONULL)!=monitor || !GetMonitorInfoW(monitor,&info)) {
        ReleaseNativeClip();
        return;
    }

    const auto& bounds=info.rcMonitor;
    if (nativeClip_ && !EqualRect(&bounds,&*nativeClip_)) ReleaseNativeClip();
    RECT current{};
    if (!GetClipCursor(&current)) return;
    if (nativeClip_ && !EqualRect(&current,&*nativeClip_)) nativeClip_.reset();
    RECT overlap{};
    if (IntersectRect(&overlap,&current,&bounds) && EqualRect(&current,&overlap)) return;
    const LONG x=GetSystemMetrics(SM_XVIRTUALSCREEN), y=GetSystemMetrics(SM_YVIRTUALSCREEN);
    const RECT desktop{x,y,x+GetSystemMetrics(SM_CXVIRTUALSCREEN),y+GetSystemMetrics(SM_CYVIRTUALSCREEN)};
    if (EqualRect(&current,&desktop) && ClipCursor(&bounds)) nativeClip_=bounds;
}
void App::FocusNativeSource(std::optional<POINT> outputPoint) {
    const HMONITOR monitor=NativeInputMonitor();
    MONITORINFO info{sizeof(info)};
    if (!monitor || !GetMonitorInfoW(monitor,&info)) return;
    const RECT bounds=info.rcMonitor;
    const SIZE sourceSize{bounds.right-bounds.left,bounds.bottom-bounds.top};
    POINT point{bounds.left+sourceSize.cx/2,bounds.top+sourceSize.cy/2};
    if (outputPoint) {
        RECT client{};
        POINT mapped{};
        if (!GetClientRect(fullDisplay_.hwnd,&client) ||
            !MapOutputPoint(*outputPoint,SIZE{client.right,client.bottom},sourceSize,mapped)) return;
        point={bounds.left+mapped.x,bounds.top+mapped.y};
    }

    auto onSource=[&](HWND window) {
        if (!window || window==main || window==GetShellWindow() || window==GetDesktopWindow() ||
            !IsWindowVisible(window) || IsIconic(window) || MonitorFromWindow(window,MONITOR_DEFAULTTONULL)!=monitor) return false;
        wchar_t name[64]{};
        GetClassNameW(window,name,64);
        return wcscmp(name,L"WorkerW")!=0 && wcscmp(name,L"Progman")!=0 &&
            wcscmp(name,L"Shell_TrayWnd")!=0 && wcscmp(name,L"Shell_SecondaryTrayWnd")!=0;
    };
    HWND game=nativeWindowMonitor_ ? capture_.Source() : GetAncestor(WindowFromPoint(point),GA_ROOT);
    HWND foreground=GetForegroundWindow();
    if (!outputPoint && onSource(foreground)) game=foreground;
    if (!onSource(game)) game=nullptr;
    nativeMouseWindow_=game;
    if (game) {
        SetForegroundWindow(game);
        if (GetForegroundWindow()==game) {
            // Release a stale game clip left behind on the old monitor.
            RECT current{},intersection{};
            if (GetClipCursor(&current) && !IntersectRect(&intersection,&current,&bounds)) ClipCursor(nullptr);
        }
    }
    SetWindowPos(fullDisplay_.hwnd,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
    SetCursorPos(point.x,point.y);
    UpdateNativeClip();
    POINT position{};
    if (!GetCursorPos(&position) || !PtInRect(&bounds,position)) {
        SetStatus(L"Fareyi oyun tutuyor olabilir. Alt+Tab ile odağı bırakıp tekrar deneyin.");
    } else {
        SetStatus(game && GetForegroundWindow()==game ? L"Oyun odakta. Ctrl+Alt+F10 ile kontrole dönün." :
            L"Fare kaynak ekranda. Ekrandaki oyuna tıklayarak odağı verin.");
    }
}
void App::ToggleMouse() {
    if (!fullDisplay_.hwnd) { SetStatus(L"Fareyi bağlamak için önce upscale'i başlatın."); return; }
    if (const auto source=NativeInputMonitor()) {
        POINT cursor{};
        if (!GetCursorPos(&cursor) || MonitorFromPoint(cursor,MONITOR_DEFAULTTONULL)!=source) {
            FocusNativeSource();
            return;
        }
        ReleaseNativeClip(); nativeMouseWindow_=nullptr;
        const auto* output=SelectedDisplay();
        const HMONITOR controlMonitor=MonitorFromWindow(main,MONITOR_DEFAULTTONEAREST);
        if (controlMonitor==source || (output && controlMonitor==output->monitor)) {
            const auto alternate=std::find_if(displays_.begin(),displays_.end(),[&](const auto& display) {
                return display.monitor!=source && (!output || display.monitor!=output->monitor);
            });
            const HMONITOR destination=alternate!=displays_.end() ? alternate->monitor : output ? output->monitor : nullptr;
            MONITORINFO info{sizeof(info)};
            if (destination && GetMonitorInfoW(destination,&info)) {
                ShowWindow(main,SW_RESTORE);
                SetWindowPos(main,nullptr,info.rcWork.left,info.rcWork.top,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
            }
            if (output && destination==output->monitor) {
                SetWindowPos(fullDisplay_.hwnd,HWND_NOTOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
            }
        }
        ShowWindow(main,SW_SHOWNORMAL);
        SetForegroundWindow(main);
        MONITORINFO sourceInfo{sizeof(sourceInfo)};
        RECT clip{}, inside{};
        if (GetForegroundWindow()==main && GetMonitorInfoW(source,&sourceInfo) && GetClipCursor(&clip) &&
            IntersectRect(&inside,&clip,&sourceInfo.rcMonitor) && EqualRect(&clip,&inside)) ClipCursor(nullptr);
        RECT client{};
        if (GetClientRect(main,&client)) {
            POINT control={client.right/2,client.bottom/2};
            if (ClientToScreen(main,&control)) SetCursorPos(control.x,control.y);
        }
        if (GetCursorPos(&cursor) && MonitorFromPoint(cursor,MONITOR_DEFAULTTONULL)!=source) {
            SetStatus(L"Fare kontrol ekranında.");
        } else SetStatus(L"Fareyi oyun tutuyor. Alt+Tab ile odağı bırakıp tekrar deneyin.");
        return;
    }
    if (input_.Armed()) {
        input_.Release(); ShowWindow(main,SW_SHOWNORMAL); SetForegroundWindow(main); ClipCursor(nullptr);
        SetStatus(L"Fare serbest. Ctrl+Alt+F10 ile yeniden TV'ye bağlayın veya TV görüntüsüne tıklayın.");
    }
    else SetStatus(input_.Acquire() ? L"Fare TV'ye bağlı. Ctrl+Alt+F10 serbest bırakır." : L"Oyun odağı alınamadı. TV görüntüsüne yeniden tıklayın.");
    WriteSessionReport();
}
void App::Save() {
    if (!renderer_.HasSource()) { SetStatus(L"Kaydetmek için önce bir kare yakalayın."); return; }
    SYSTEMTIME now{}; GetLocalTime(&now); wchar_t name[96]{};
    swprintf_s(name,L"upscalp-%04u%02u%02u-%02u%02u%02u-%03u.png",now.wYear,now.wMonth,now.wDay,now.wHour,now.wMinute,now.wSecond,now.wMilliseconds);
    wchar_t exe[MAX_PATH]{}; GetModuleFileNameW(nullptr,exe,MAX_PATH);
    auto directory=std::filesystem::path(exe).parent_path()/L"captures"; std::filesystem::create_directories(directory);
    if (optionsDirty_) { renderer_.Process(options_); optionsDirty_=false; }
    auto path=directory/name; renderer_.SaveOutput(path.wstring(),options_);
    SetStatus(L"1920×1080 kare kaydedildi:\n"+path.wstring());
}
void App::MakePattern(HMONITOR target) {
    if (IsWindow(pattern)) { SetStatus(L"Test deseni zaten açık. Kaynak listesinden seçebilirsiniz."); return; }
    if (!patternRenderer_.Device()) patternRenderer_.Initialize();
    MONITORINFO monitor{sizeof(monitor)};
    if (!GetMonitorInfoW(target ? target : MonitorFromWindow(main,MONITOR_DEFAULTTONEAREST),&monitor))
        throw_last_error();
    RECT outer{0,0,3840,2160};
    DWORD style=WS_OVERLAPPEDWINDOW|WS_VISIBLE;
    if (target) {
        outer=monitor.rcMonitor;
        style=WS_POPUP|WS_VISIBLE;
    } else if (!AdjustWindowRectExForDpi(&outer,style,FALSE,WS_EX_NOACTIVATE,GetDpiForWindow(main))) {
        throw_last_error();
    }
    pattern=CreateWindowExW(WS_EX_NOACTIVATE,PatternClass,L"Upscalp · 4K Test Deseni",style,
        monitor.rcMonitor.left,monitor.rcMonitor.top,outer.right-outer.left,outer.bottom-outer.top,nullptr,nullptr,instance,this);
    if (!pattern) throw_last_error();
    patternDisplay_={}; patternDisplay_.hwnd=pattern;
    patternStart_=GetTickCount64();
    SetWindowPos(pattern,target ? HWND_TOPMOST : HWND_BOTTOM,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
    SetTimer(main,2,16,nullptr); RefreshWindows();
    for (size_t i=0;i<sources_.size();++i) if (sources_[i].hwnd==pattern) SendMessageW(Control(ID_WINDOWS),CB_SETCURSEL,i,0);
    UpdateSourceControls();
    SetStatus(L"3840×2160 test deseni açıldı. Yakalamayı başlat düğmesiyle görüntü yolunu deneyin. Desene odaklanıp Esc ile kapatabilirsiniz.");
}
void App::Tick() {
    auto now=GetTickCount64();
    UpdateNativeClip();
    if (!displayRefreshDeadline_ && nativeWindowMonitor_ && !IsIconic(capture_.Source()) &&
        MonitorFromWindow(capture_.Source(),MONITOR_DEFAULTTONULL)!=nativeWindowMonitor_) {
        Stop();
        SetStatus(L"Oyun kaynak ekrandan ayrıldı. Pencere geri yüklendi. Pencereli modda yeniden deneyin.");
    }
    if (parked_ && !smoke && now-parkedAt_>3000) {
        RECT source{}; GetWindowRect(resizedTarget_,&source);
        if (!IsIconic(resizedTarget_) && IntersectsAnyDisplay(source,DisplayBounds())) {
            Stop();
            SetStatus(L"Oyun ekran dışı yerleşimi korumadı. Pencere geri yüklendi. Oyunu Pencereli moda alın; çerçevesiz mod yerleşimi sıfırlayabilir.");
        }
    }
    if (capture_.Running() && now-fpsStart_>=1000) {
        const double seconds=static_cast<double>(now-fpsStart_)/1000.0;
        fps_=fpsFrames_/seconds; outputFps_=outputFpsFrames_/seconds;
        fpsFrames_=outputFpsFrames_=0; fpsStart_=now;
        UpdateSubmissionStats();
        InvalidateRect(main,nullptr,FALSE);
        if (now-lastFrame_>2500) SetStatus(L"Yeni kare gelmiyor. Oyun arka planda çizimi durdurmuş, küçültülmüş veya yakalamayı engelliyor olabilir. Son kare gösteriliyor.");
    }
    if (smoke && now-smokeStart_>20000) {
        RECT client{}; GetClientRect(pattern,&client);
        std::wstring detail=L"Capture smoke test timed out at phase "+std::to_wstring(smokePhase_)+L"; source client="+std::to_wstring(client.right)+L"x"+std::to_wstring(client.bottom);
        SmokeReport(smokeReport,false,renderer_.SourceWidth(),renderer_.SourceHeight(),detail); exitCode=1; PostMessageW(main,WM_CLOSE,0,0);
    }
    WriteSessionReport();
}
LRESULT App::Handle(UINT message,WPARAM wp,LPARAM lp) {
    switch (message) {
    case WM_GETMINMAXINFO: {
        auto info=reinterpret_cast<MINMAXINFO*>(lp); info->ptMinTrackSize={D(1050),D(840)}; return 0;
    }
    case WM_SIZE: if (preview && wp!=SIZE_MINIMIZED) Layout(); return 0;
    case WM_MOVE: if (capture_.SourceMonitor()) Redraw(); return 0;
    case WM_DISPLAYCHANGE:
        if (!closing_) QueueDisplayRefresh();
        return 0;
    case WM_DPICHANGED: {
        // Native controls and the layout scale with the window's monitor DPI.
        dpi_=HIWORD(wp); UpdateFonts(); auto r=reinterpret_cast<RECT*>(lp);
        SetWindowPos(main,nullptr,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER|SWP_NOACTIVATE); Layout(); return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: Paint(); return 0;
    case WM_CTLCOLORSTATIC: case WM_CTLCOLORBTN: case WM_CTLCOLORLISTBOX: {
        HDC dc=reinterpret_cast<HDC>(wp); SetTextColor(dc,Ink); SetBkColor(dc,Panel); return reinterpret_cast<LRESULT>(panelBrush_);
    }
    case WM_DRAWITEM: {
        auto item=reinterpret_cast<DRAWITEMSTRUCT*>(lp);
        if (item->CtlType!=ODT_BUTTON) break;
        bool enabled=(item->itemState&ODS_DISABLED)==0;
        bool primary=item->CtlID==ID_START || item->CtlID==ID_FULL;
        COLORREF color=primary ? RGB(40,103,96) : RGB(37,48,65);
        if (item->itemState&ODS_SELECTED) color=RGB(40,76,80);
        if (!enabled) color=RGB(32,38,48);
        HBRUSH brush=CreateSolidBrush(color); FillRect(item->hDC,&item->rcItem,brush); DeleteObject(brush);
        wchar_t text[160]{}; GetWindowTextW(item->hwndItem,text,160);
        Text(item->hDC,text,item->rcItem,font_,enabled ? Ink : Muted,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        if (item->itemState&ODS_FOCUS) { RECT focus=item->rcItem; InflateRect(&focus,-3,-3); DrawFocusRect(item->hDC,&focus); }
        return TRUE;
    }
    case WM_HSCROLL: UpdateOptions(); return 0;
    case WM_COMMAND: {
        int id=LOWORD(wp),code=HIWORD(wp);
        if (code==CBN_SELCHANGE && id==ID_WINDOWS) { UpdateSourceControls(); return 0; }
        if (code==CBN_SELCHANGE && id==ID_FILTER) { UpdateOptions(); return 0; }
        if (code==CBN_SELCHANGE && id==ID_MONITOR) { ChangeDisplay(); return 0; }
        if (code!=BN_CLICKED) return 0;
        switch (id) {
        case ID_NATIVE_INPUT: UpdateSourceControls(); break;
        case ID_REFRESH: RefreshDisplays(); RefreshWindows(); break;
        case ID_START: Start(); break;
        case ID_RESIZE: BeginSupersampling(); break;
        case ID_RESTORE:
            if (!resizedTarget_) { SetStatus(L"Geri yüklenecek pencere yok."); break; }
            if (nativeWindowMonitor_) { Stop(); break; }
            pendingOutput_=false;
            if (fullDisplay_.hwnd) ToggleFullscreen();
            SetStatus(RestoreTarget() ? L"Pencere eski boyutuna döndürüldü." : L"Pencere geri yüklenemedi."); break;
        case ID_LINEAR: case ID_COMPARE: UpdateOptions(); break;
        case ID_FULL: ToggleFullscreen(); break;
        case ID_SAVE: Save(); break;
        case ID_STOP: Stop(); break;
        case ID_PATTERN: MakePattern(); break;
        }
        return 0;
    }
    case WM_HOTKEY:
        if (wp==HOT_EMERGENCY) Stop();
        else if (wp==HOT_START) ToggleUpscale();
        else if (wp==HOT_MOUSE) ToggleMouse();
        else if (wp==HOT_FULL) ToggleFullscreen();
        else if (wp==HOT_COMPARE) { SendMessageW(Control(ID_COMPARE),BM_SETCHECK,options_.comparison ? BST_UNCHECKED : BST_CHECKED,0); UpdateOptions(); }
        return 0;
    case WM_CAPTURE_FRAME:
        if (capture_.Running() && wp==capture_.Generation()) framePending_=true;
        return 0;
    case WM_RENDER_PENDING:
        if (std::exchange(framePending_,false)) Frame();
        if (std::exchange(optionsDirty_,false)) renderer_.Process(options_);
        if (redrawPending_ && (!fullDisplay_.frameLatency || fullDisplay_.frameSlotReady)) {
            redrawPending_=false;
            Draw();
        }
        return 0;
    case WM_CAPTURE_CLOSED:
        if (capture_.Running() && wp==capture_.Generation()) {
            if (captureDisplayIdentity_.empty()) {
                Stop(); SetStatus(L"Kaynak yakalama oturumu kapandı; yakalama durduruldu.");
            } else {
                framePending_=false;
                ReleaseNativeClip();
                capture_.Stop();
                QueueDisplayRefresh();
            }
        }
        return 0;
    case WM_TIMER:
        if (wp==1) Tick();
        if (wp==2) {
            if (IsWindow(pattern)) patternRenderer_.DrawPattern(patternDisplay_,static_cast<float>(GetTickCount64()-patternStart_)/1000);
            else { pattern=nullptr; patternDisplay_={}; KillTimer(main,2); RefreshWindows(); }
        }
        if (wp==PresentRetryTimer) { KillTimer(main,PresentRetryTimer); presentRetry_=false; Redraw(); }
        if (wp==DisplayRefreshTimer && displayRefreshDeadline_) RefreshDisplayTopology();
        return 0;
    case WM_CLOSE:
        closing_=true; Stop(); previewDisplay_={}; patternDisplay_={};
        if (IsWindow(pattern)) DestroyWindow(pattern);
        DestroyWindow(main); return 0;
    case WM_DESTROY:
        for (int id : {HOT_EMERGENCY,HOT_FULL,HOT_COMPARE,HOT_START,HOT_MOUSE}) UnregisterHotKey(main,id);
        KillTimer(main,1); KillTimer(main,2);
        previewDisplay_={}; patternDisplay_={};
        for (HFONT font : {font_,titleFont_,smallFont_,numberFont_}) DeleteObject(font);
        DeleteObject(panelBrush_); PostQuitMessage(exitCode); return 0;
    }
    return DefWindowProcW(main,message,wp,lp);
}
int App::Run() {
    for (;;) {
        MSG message{};
        for (unsigned i=0;i<16 && PeekMessageW(&message,nullptr,0,0,PM_REMOVE);++i) {
            if (message.message==WM_QUIT) return static_cast<int>(message.wParam);
            if (!IsDialogMessageW(main,&message)) { TranslateMessage(&message); DispatchMessageW(&message); }
        }
        const bool work=framePending_ || redrawPending_;
        const HANDLE latency=fullDisplay_.frameSlotReady ? nullptr : fullDisplay_.frameLatency.get();
        if (work && (!latency || WaitForSingleObject(latency,0)==WAIT_OBJECT_0)) {
            if (latency) fullDisplay_.frameSlotReady=true;
            SendMessageW(main,WM_RENDER_PENDING,0,0);
            continue;
        }
        // Wait on DXGI only while there is a frame or an update to present.
        // Message dispatch remains live while the display is busy.
        const DWORD count=work && latency ? 1 : 0;
        const DWORD result=MsgWaitForMultipleObjectsEx(count,count ? &latency : nullptr,INFINITE,
            QS_ALLINPUT,MWMO_INPUTAVAILABLE);
        if (result==WAIT_FAILED) throw_last_error();
        if (count && result==WAIT_OBJECT_0) fullDisplay_.frameSlotReady=true;
    }
}
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int) {
    init_apartment(apartment_type::single_threaded);
    int count{}; wchar_t** args=CommandLineToArgvW(GetCommandLineW(),&count);
    bool selfTest=false,replace=false; App app; app.instance=instance;
    std::wstring report=L"self-test.json";
    for (int i=1;i<count;++i) {
        std::wstring arg=args[i];
        if (arg==L"--self-test") selfTest=true;
        else if (arg==L"--replace") replace=true;
        else if (arg==L"--smoke-test") app.smoke=true;
        else if (arg==L"--monitor-smoke-test") { app.smoke=true; app.monitorSmoke=true; }
        else if (arg==L"--source-display" && i+1<count) app.startupSourceDisplay=args[++i];
        else if (arg==L"--output-display" && i+1<count) app.startupOutputDisplay=args[++i];
        else if (arg==L"--report" && i+1<count) report=args[++i];
        else if (arg==L"--screenshot" && i+1<count) app.smokeImage=args[++i];
        else if (arg==L"--session-report" && i+1<count) app.sessionReport=args[++i];
    }
    LocalFree(args);
    if (selfTest) return RunSelfTests(report);
    app.smokeReport=report;
    if (replace && !ClosePreviousInstances()) {
        if (app.smoke) SmokeReport(report,false,0,0,L"Previous Upscalp instance did not close gracefully.");
        else MessageBoxW(nullptr,L"Önceki Upscalp kapanamadı. Önce Durdur düğmesiyle yakalamayı sonlandırıp uygulamayı kapatın.",L"Upscalp",MB_OK|MB_ICONINFORMATION);
        return 1;
    }
    // Include older builds that did not create the mutex, including hidden ones.
    if (HWND existing=FindWindowW(MainClass,nullptr)) {
        if (app.smoke) { SmokeReport(report,false,0,0,L"Close the existing Upscalp instance before the smoke test."); return 1; }
        ShowWindowAsync(existing,SW_RESTORE); SetForegroundWindow(existing); return 0;
    }
    handle singleInstance{CreateMutexW(nullptr,TRUE,L"Local\\Upscalp.SingleInstance")};
    if (!singleInstance) return 1;
    if (GetLastError()==ERROR_ALREADY_EXISTS) return 0;
    try {
        app.Initialize();
        return app.Run();
    } catch (...) {
        auto error=ErrorText();
        if (app.smoke) SmokeReport(report,false,0,0,error);
        else MessageBoxW(nullptr,error.c_str(),L"Upscalp açılamadı",MB_OK|MB_ICONERROR);
        return 1;
    }
}
