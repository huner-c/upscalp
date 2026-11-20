#include "input_bridge.h"
#include <windowsx.h>
#include <algorithm>
#include <chrono>

bool MapOutputPoint(POINT point,SIZE output,SIZE source,POINT& mapped) {
    if (output.cx<=0 || output.cy<=0 || source.cx<=0 || source.cy<=0) return false;
    double scale=std::min(output.cx/1920.0,output.cy/1080.0);
    LONG width=static_cast<LONG>(1920*scale+.5),height=static_cast<LONG>(1080*scale+.5);
    LONG left=(output.cx-width)/2,top=(output.cy-height)/2;
    if (width<=0 || height<=0 || point.x<left || point.y<top || point.x>=left+width || point.y>=top+height) return false;
    mapped.x=std::clamp(static_cast<LONG>((static_cast<long long>(point.x-left)*source.cx)/width),0L,source.cx-1);
    mapped.y=std::clamp(static_cast<LONG>((static_cast<long long>(point.y-top)*source.cy)/height),0L,source.cy-1);
    return true;
}
void InputBridge::Start(HWND source,HWND output) {
    Stop();
    if (!IsWindow(source) || !IsWindow(output)) return;
    source_=source; output_=output; GetWindowThreadProcessId(source_,&sourcePid_);
    GetWindowRect(output_,&outputBounds_);
    forwarded_=0;
    worker_=std::jthread([this](std::stop_token stop) {
        while (!stop.stop_requested()) {
            DWORD foregroundPid{};
            HWND foreground=GetForegroundWindow(); GetWindowThreadProcessId(foreground,&foregroundPid);
            bool shouldConfine=armed_.load() && IsWindow(source_) && (foregroundPid==sourcePid_ || foreground==output_);
            if (shouldConfine) {
                RECT current{};
                if (GetClipCursor(&current) && !EqualRect(&current,&outputBounds_)) ClipCursor(&outputBounds_);
                confined_=true;
            } else if (confined_.exchange(false)) {
                // Do not clear a new foreground application's own cursor clip.
                RECT current{};
                if (GetClipCursor(&current) && EqualRect(&current,&outputBounds_)) ClipCursor(nullptr);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
        }
    });
    Acquire();
}
void InputBridge::Release() {
    armed_=false; confined_=false;
    ClipCursor(nullptr);
}
void InputBridge::Stop() {
    armed_=false;
    if (worker_.joinable()) { worker_.request_stop(); worker_.join(); }
    if (source_ || output_) ClipCursor(nullptr);
    confined_=false; source_=nullptr; output_=nullptr; sourcePid_=0;
}
bool InputBridge::Acquire() {
    if (!IsWindow(source_) || !IsWindow(output_)) return false;
    ClipCursor(nullptr);
    SetForegroundWindow(source_);
    DWORD foregroundPid{}; GetWindowThreadProcessId(GetForegroundWindow(),&foregroundPid);
    if (foregroundPid!=sourcePid_) return false;
    armed_=true;
    ClipCursor(&outputBounds_);
    SetCursorPos((outputBounds_.left+outputBounds_.right)/2,(outputBounds_.top+outputBounds_.bottom)/2);
    confined_=true;
    return true;
}
bool InputBridge::Forward(UINT message,WPARAM wp,LPARAM lp) {
    if (!IsWindow(source_) || !IsWindow(output_)) return false;
    bool button=message==WM_LBUTTONDOWN || message==WM_RBUTTONDOWN || message==WM_MBUTTONDOWN || message==WM_XBUTTONDOWN;
    if (button) {
        DWORD foregroundPid{}; GetWindowThreadProcessId(GetForegroundWindow(),&foregroundPid);
        if (!armed_.load() || foregroundPid!=sourcePid_) Acquire();
    }
    if (!armed_.load()) return false;
    RECT output{},source{};
    if (!GetClientRect(output_,&output) || !GetClientRect(source_,&source)) return false;
    POINT point{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
    bool wheel=message==WM_MOUSEWHEEL || message==WM_MOUSEHWHEEL;
    if (wheel) ScreenToClient(output_,&point);
    POINT mapped{};
    if (!MapOutputPoint(point,SIZE{output.right,output.bottom},SIZE{source.right,source.bottom},mapped)) return false;
    if (wheel) ClientToScreen(source_,&mapped);
    if (PostMessageW(source_,message,wp,MAKELPARAM(static_cast<SHORT>(mapped.x),static_cast<SHORT>(mapped.y)))) {
        ++forwarded_; return true;
    }
    return false;
}
