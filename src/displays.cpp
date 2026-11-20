#include "displays.h"
#include <algorithm>
#include <map>
#include <stdexcept>

std::vector<DisplayTarget> EnumerateDisplays() {
    struct Names { std::wstring friendly,identity; };
    std::map<std::wstring,Names> names;
    for (int retry=0;retry<3;++retry) {
        UINT32 pathCount{},modeCount{};
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS,&pathCount,&modeCount)!=ERROR_SUCCESS) break;
        std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
        LONG result=QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS,&pathCount,paths.data(),&modeCount,modes.data(),nullptr);
        if (result==ERROR_INSUFFICIENT_BUFFER) continue;
        if (result!=ERROR_SUCCESS) break;
        paths.resize(pathCount);
        for (const auto& path : paths) {
            DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
            source.header={DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME,sizeof(source),path.sourceInfo.adapterId,path.sourceInfo.id};
            DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
            target.header={DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME,sizeof(target),path.targetInfo.adapterId,path.targetInfo.id};
            if (DisplayConfigGetDeviceInfo(&source.header)==ERROR_SUCCESS && DisplayConfigGetDeviceInfo(&target.header)==ERROR_SUCCESS)
                names[source.viewGdiDeviceName]={target.monitorFriendlyDeviceName,target.monitorDevicePath};
        }
        break;
    }
    std::vector<DisplayTarget> displays;
    EnumDisplayMonitors(nullptr,nullptr,[](HMONITOR monitor,HDC,LPRECT,LPARAM data)->BOOL {
        auto& result=*reinterpret_cast<std::vector<DisplayTarget>*>(data);
        MONITORINFOEXW info{}; info.cbSize=sizeof(info);
        if (GetMonitorInfoW(monitor,&info)) {
            DisplayTarget target;
            target.monitor=monitor; target.bounds=info.rcMonitor; target.device=info.szDevice;
            target.primary=(info.dwFlags&MONITORINFOF_PRIMARY)!=0;
            result.push_back(std::move(target));
        }
        return TRUE;
    },reinterpret_cast<LPARAM>(&displays));
    std::sort(displays.begin(),displays.end(),[](const auto& a,const auto& b) { return a.device<b.device; });
    for (auto& display : displays) {
        auto found=names.find(display.device);
        if (found!=names.end()) { display.name=found->second.friendly; display.identity=found->second.identity; }
        if (display.name.empty()) {
            DISPLAY_DEVICEW device{}; device.cb=sizeof(device);
            if (EnumDisplayDevicesW(display.device.c_str(),0,&device,0)) display.name=device.DeviceString;
        }
        if (display.name.empty()) display.name=L"Ekran";
        if (display.identity.empty()) display.identity=display.device;
        std::wstring shortDevice=display.device;
        if (shortDevice.starts_with(L"\\\\.\\")) shortDevice.erase(0,4);
        display.label=display.name+L"  ·  "+shortDevice+L"  ·  "+
            std::to_wstring(display.bounds.right-display.bounds.left)+L"×"+
            std::to_wstring(display.bounds.bottom-display.bounds.top)+(display.primary ? L"  (Ana)" : L"");
    }
    return displays;
}
bool IntersectsAnyDisplay(const RECT& rectangle,const std::vector<RECT>& displays) {
    for (const auto& display : displays) {
        RECT intersection{};
        if (IntersectRect(&intersection,&rectangle,&display)) return true;
    }
    return false;
}
RECT ParkOutsideDisplays(const std::vector<RECT>& displays,LONG width,LONG height,LONG preferredX) {
    if (displays.empty() || width<=0 || height<=0 || width>16384 || height>16384)
        throw std::invalid_argument("Cannot place source outside displays");
    RECT all=displays.front();
    for (const auto& r : displays) {
        all.left=std::min(all.left,r.left); all.top=std::min(all.top,r.top);
        all.right=std::max(all.right,r.right); all.bottom=std::max(all.bottom,r.bottom);
    }
    // Leave an actual gap; moving to a hole in a multi-monitor layout is insufficient.
    const POINT candidates[]={{preferredX,all.bottom+64},{all.right+64,all.top},
        {all.left-width-64,all.top},{preferredX,all.top-height-64}};
    for (const auto& p : candidates) {
        RECT result{p.x,p.y,p.x+width,p.y+height};
        if (result.left>=-32768 && result.top>=-32768 && result.right<32767 && result.bottom<32767 &&
            !IntersectsAnyDisplay(result,displays)) return result;
    }
    throw std::runtime_error("No safe offscreen position in this desktop layout");
}
