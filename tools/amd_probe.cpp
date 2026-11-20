// Read-only desktop-backend diagnostics. No SetEnabled, custom timing creation,
// EDID override, display mode change, or driver installation is performed.
#include <windows.h>
#include <ADLX.h>
#include <IDisplays.h>
#include <IDisplaySettings.h>
#include "displays.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <cstring>
#include <cstdint>
#include <vector>

namespace {
std::string Quote(const std::string& text) {
    std::ostringstream out; out << '"';
    for (unsigned char c : text) {
        if (c=='"' || c=='\\') out << '\\' << c;
        else if (c=='\n') out << "\\n";
        else if (c=='\r') out << "\\r";
        else if (c=='\t') out << "\\t";
        else if (c>=32) out << c;
    }
    out << '"'; return out.str();
}
std::string Utf8(const std::wstring& value) {
    if (value.empty()) return {};
    int length=WideCharToMultiByte(CP_UTF8,0,value.data(),static_cast<int>(value.size()),nullptr,0,nullptr,nullptr);
    std::string result(length,'\0');
    WideCharToMultiByte(CP_UTF8,0,value.data(),static_cast<int>(value.size()),result.data(),length,nullptr,nullptr);
    return result;
}
void Check(ADLX_RESULT result,const char* operation) {
    if (result!=ADLX_OK && result!=ADLX_ALREADY_INITIALIZED)
        throw std::runtime_error(std::string(operation)+" returned ADLX_RESULT="+std::to_string(result));
}
struct Runtime {
    HMODULE dll{};
    ADLXTerminate_Fn terminate{};
    adlx::IADLXSystem* system{};
    bool initialized{};
    std::string version;
    ~Runtime() {
        if (initialized && terminate) terminate();
        if (dll) FreeLibrary(dll);
    }
    template<class T> T Function(const char* name) {
        auto address=GetProcAddress(dll,name);
        if (!address) throw std::runtime_error(std::string("Missing installed ADLX export: ")+name);
        return reinterpret_cast<T>(address);
    }
    void Open() {
        // Only load AMD's installed system library, never a DLL from cwd.
        dll=LoadLibraryExW(ADLX_DLL_NAMEW,nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!dll) throw std::runtime_error("Could not load installed System32/amdadlx64.dll, Win32="+std::to_string(GetLastError()));
        auto queryVersion=Function<ADLXQueryVersion_Fn>(ADLX_QUERY_VERSION_FUNCTION_NAME);
        auto queryFullVersion=Function<ADLXQueryFullVersion_Fn>(ADLX_QUERY_FULL_VERSION_FUNCTION_NAME);
        auto initialize=Function<ADLXInitialize_Fn>(ADLX_INIT_FUNCTION_NAME);
        terminate=Function<ADLXTerminate_Fn>(ADLX_TERMINATE_FUNCTION_NAME);
        const char* text{}; Check(queryVersion(&text),"ADLXQueryVersion");
        version=text ? text : "";
        adlx_uint64 runtimeVersion{}; Check(queryFullVersion(&runtimeVersion),"ADLXQueryFullVersion");
        // Only base display/VSR interfaces are queried. Initialize against the
        // installed runtime version instead of demanding the latest SDK version.
        Check(initialize(runtimeVersion,&system),"ADLXInitialize");
        initialized=true;
    }
};
using DisplayModes = std::set<std::tuple<DWORD,DWORD,DWORD,bool>>;
DisplayModes EnumerateModes(const std::wstring& device,DWORD flags) {
    DisplayModes modes;
    for (DWORD index=0;index<4096;++index) {
        DEVMODEW mode{};
        mode.dmSize=sizeof(mode);
        if (!EnumDisplaySettingsExW(device.c_str(),index,&mode,flags)) break;
        if (mode.dmBitsPerPel>=24) {
            modes.emplace(mode.dmPelsWidth,mode.dmPelsHeight,mode.dmDisplayFrequency,
                (mode.dmDisplayFlags&DM_INTERLACED)!=0);
        }
    }
    return modes;
}
bool Contains4k(const DisplayModes& modes) {
    for (const auto& mode : modes)
        if (std::get<0>(mode)==3840 && std::get<1>(mode)==2160) return true;
    return false;
}
struct DesktopConfig {
    LONG status{ERROR_GEN_FAILURE};
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
};
DesktopConfig ReadDesktopConfig(UINT32 flags=QDC_ONLY_ACTIVE_PATHS) {
    DesktopConfig result;
    for (int retry=0;retry<3;++retry) {
        UINT32 paths{},modes{};
        result.status=GetDisplayConfigBufferSizes(flags,&paths,&modes);
        if (result.status!=ERROR_SUCCESS) return result;
        result.paths.resize(paths);
        result.modes.resize(modes);
        result.status=QueryDisplayConfig(flags,&paths,result.paths.data(),&modes,result.modes.data(),nullptr);
        if (result.status==ERROR_INSUFFICIENT_BUFFER) continue;
        if (result.status==ERROR_SUCCESS) {
            result.paths.resize(paths);
            result.modes.resize(modes);
        }
        return result;
    }
    return result;
}
constexpr UINT32 ValidateOnly=SDC_USE_SUPPLIED_DISPLAY_CONFIG|SDC_VALIDATE;
std::wstring CcdDeviceName(const DISPLAYCONFIG_PATH_INFO& path) {
    DISPLAYCONFIG_SOURCE_DEVICE_NAME name{};
    name.header={DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME,sizeof(name),path.sourceInfo.adapterId,path.sourceInfo.id};
    if (DisplayConfigGetDeviceInfo(&name.header)!=ERROR_SUCCESS) return {};
    return name.viewGdiDeviceName;
}
void WriteCcdTests(std::ostream& out,const DesktopConfig& config,const std::wstring& device) {
    if (config.status!=ERROR_SUCCESS) {
        out << "{\"queryStatus\":" << config.status << '}';
        return;
    }
    for (auto path : config.paths) {
        if (device!=CcdDeviceName(path)) continue;
        if (path.sourceInfo.modeInfoIdx>=config.modes.size() || path.targetInfo.modeInfoIdx>=config.modes.size())
            continue;
        DISPLAYCONFIG_MODE_INFO modes[]={config.modes[path.sourceInfo.modeInfoIdx],config.modes[path.targetInfo.modeInfoIdx]};
        if (modes[0].infoType!=DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE || modes[1].infoType!=DISPLAYCONFIG_MODE_INFO_TYPE_TARGET)
            continue;
        path.sourceInfo.modeInfoIdx=0;
        path.targetInfo.modeInfoIdx=1;
        auto& source=modes[0].sourceMode;
        const auto& signal=modes[1].targetMode.targetVideoSignalInfo;
        out << "{\"desktopSource\":[" << source.width << ',' << source.height << ']'
            << ",\"activeSignal\":[" << signal.activeSize.cx << ',' << signal.activeSize.cy << ']'
            << ",\"signalRefresh\":[" << signal.vSyncFreq.Numerator << ',' << signal.vSyncFreq.Denominator << ']';
        // A single output at the origin isolates the test from how the other
        // monitor rectangles would need to move. Both calls validate only.
        source.position={0,0};
        const auto baseline=SetDisplayConfig(1,&path,2,modes,ValidateOnly);
        out << ",\"soloCurrentModeValidation\":" << baseline;
        if (signal.activeSize.cx==1920 && signal.activeSize.cy==1080) {
            source.width=3840;
            source.height=2160;
            path.targetInfo.scaling=DISPLAYCONFIG_SCALING_STRETCHED;
            auto stretched=SetDisplayConfig(1,&path,2,modes,ValidateOnly);
            path.targetInfo.scaling=DISPLAYCONFIG_SCALING_ASPECTRATIOCENTEREDMAX;
            auto aspect=SetDisplayConfig(1,&path,2,modes,ValidateOnly);
            out << ",\"solo4kDesktop1080SignalStretched\":" << stretched
                << ",\"solo4kDesktop1080SignalAspect\":" << aspect;
            source.width=3200;
            source.height=1800;
            const auto mode1800=SetDisplayConfig(1,&path,2,modes,ValidateOnly);
            source.width=2560;
            source.height=1440;
            const auto mode1440=SetDisplayConfig(1,&path,2,modes,ValidateOnly);
            out << ",\"solo1800pDesktop1080SignalAspect\":" << mode1800
                << ",\"solo1440pDesktop1080SignalAspect\":" << mode1440;
        }
        out << '}';
        return;
    }
    out << "null";
}
std::string ValidateAcerSamsungClone() {
    const auto config=ReadDesktopConfig(QDC_ONLY_ACTIVE_PATHS|QDC_VIRTUAL_MODE_AWARE);
    if (config.status!=ERROR_SUCCESS) return "{\"queryStatus\":"+std::to_string(config.status)+"}";
    DISPLAYCONFIG_PATH_INFO paths[2]{};
    const wchar_t* names[]={L"Acer X233H",L"SAMSUNG"};
    DISPLAYCONFIG_MODE_INFO modes[4]{};
    for (size_t i=0;i<2;++i) {
        bool found=false;
        for (const auto& path : config.paths) {
            DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
            target.header={DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME,sizeof(target),path.targetInfo.adapterId,path.targetInfo.id};
            if (DisplayConfigGetDeviceInfo(&target.header)!=ERROR_SUCCESS ||
                _wcsicmp(target.monitorFriendlyDeviceName,names[i])!=0)
                continue;
            paths[i]=path;
            found=true;
            break;
        }
        if (!found) return "null";
        if (!(paths[i].flags&DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE)) return "{\"virtualAwarePaths\":false}";
        const auto sourceIndex=paths[i].sourceInfo.sourceModeInfoIdx;
        const auto targetIndex=paths[i].targetInfo.targetModeInfoIdx;
        if (sourceIndex>=config.modes.size() || targetIndex>=config.modes.size()) return "null";
        modes[2*i]=config.modes[sourceIndex];
        modes[2*i+1]=config.modes[targetIndex];
        if (modes[2*i].infoType!=DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE ||
            modes[2*i+1].infoType!=DISPLAYCONFIG_MODE_INFO_TYPE_TARGET) return "null";
        const auto& signal=modes[2*i+1].targetMode.targetVideoSignalInfo;
        if (signal.activeSize.cx!=1920 || signal.activeSize.cy!=1080) return "null";
        paths[i].sourceInfo.sourceModeInfoIdx=static_cast<UINT32>(2*i);
        paths[i].targetInfo.targetModeInfoIdx=static_cast<UINT32>(2*i+1);
        paths[i].targetInfo.desktopModeInfoIdx=DISPLAYCONFIG_PATH_DESKTOP_IMAGE_IDX_INVALID;
    }
    auto validate=[&](UINT32 width,UINT32 height,DISPLAYCONFIG_SCALING scaling) {
        for (size_t i=0;i<2;++i) {
            auto& source=modes[2*i].sourceMode;
            source.width=width;
            source.height=height;
            source.position={0,0}; // Equal positions request a Windows virtual clone.
            paths[i].targetInfo.scaling=scaling;
        }
        return SetDisplayConfig(2,paths,4,modes,ValidateOnly|SDC_VIRTUAL_MODE_AWARE);
    };
    const LONG baseline=validate(1920,1080,DISPLAYCONFIG_SCALING_IDENTITY);
    const LONG scaled=validate(3840,2160,DISPLAYCONFIG_SCALING_ASPECTRATIOCENTEREDMAX);
    return "{\"virtualAwarePaths\":true,\"1080Desktop1080Signals\":"+std::to_string(baseline)+
        ",\"4kDesktop1080Signals\":"+std::to_string(scaled)+"}";
}
std::string ValidateVirtualSamsungClone(const std::wstring& virtualDevice) {
    auto config=ReadDesktopConfig(QDC_ONLY_ACTIVE_PATHS|QDC_VIRTUAL_MODE_AWARE);
    if (config.status!=ERROR_SUCCESS) return "{\"queryStatus\":"+std::to_string(config.status)+"}";
    const auto virtualPath=std::find_if(config.paths.begin(),config.paths.end(),[&](const auto& path) {
        return CcdDeviceName(path)==virtualDevice;
    });
    const auto samsungPath=std::find_if(config.paths.begin(),config.paths.end(),[](const auto& path) {
        DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
        target.header={DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME,sizeof(target),path.targetInfo.adapterId,path.targetInfo.id};
        return DisplayConfigGetDeviceInfo(&target.header)==ERROR_SUCCESS &&
               _wcsicmp(target.monitorFriendlyDeviceName,L"SAMSUNG")==0;
    });
    if (virtualPath==config.paths.end() || samsungPath==config.paths.end() || virtualPath==samsungPath) return "null";
    if (!(virtualPath->flags&DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE) ||
        !(samsungPath->flags&DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE)) return "{\"virtualAwarePaths\":false}";
    const UINT32 sourceIndex=virtualPath->sourceInfo.sourceModeInfoIdx;
    const UINT32 samsungIndex=samsungPath->sourceInfo.sourceModeInfoIdx;
    const UINT32 signalIndex=samsungPath->targetInfo.targetModeInfoIdx;
    if (sourceIndex>=config.modes.size() || samsungIndex>=config.modes.size() || signalIndex>=config.modes.size() ||
        config.modes[sourceIndex].infoType!=DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE ||
        config.modes[samsungIndex].infoType!=DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE ||
        config.modes[signalIndex].infoType!=DISPLAYCONFIG_MODE_INFO_TYPE_TARGET) return "null";
    const auto source=config.modes[sourceIndex].sourceMode;
    const auto signal=config.modes[signalIndex].targetMode.targetVideoSignalInfo;
    if (source.width!=3840 || source.height!=2160 || signal.activeSize.cx!=1920 || signal.activeSize.cy!=1080)
        return "null";

    auto validate=[&](UINT32 flags) {
        return SetDisplayConfig(static_cast<UINT32>(config.paths.size()),config.paths.data(),
            static_cast<UINT32>(config.modes.size()),config.modes.data(),ValidateOnly|SDC_VIRTUAL_MODE_AWARE|flags);
    };
    const LONG current=validate(0);
    // Equal source modes request a clone. Keep Samsung's existing HDMI signal fixed.
    config.modes[samsungIndex].sourceMode=source;
    samsungPath->targetInfo.desktopModeInfoIdx=DISPLAYCONFIG_PATH_DESKTOP_IMAGE_IDX_INVALID;
    samsungPath->targetInfo.scaling=DISPLAYCONFIG_SCALING_ASPECTRATIOCENTEREDMAX;
    const LONG fixedClone=validate(0);
    const LONG adjustable=validate(SDC_ALLOW_CHANGES);
    return "{\"virtualAwarePaths\":true,\"currentTopology\":"+std::to_string(current)+
        ",\"exact4kClone1080SamsungSignal\":"+std::to_string(fixedClone)+
        ",\"allowWindowsToChangeModes\":"+std::to_string(adjustable)+"}";
}
std::string WindowsDisplays() {
    std::ostringstream out; out << '[';
    auto displays=EnumerateDisplays();
    const auto ccd=ReadDesktopConfig();
    for (size_t i=0;i<displays.size();++i) {
        if (i) out << ',';
        const auto& display=displays[i];
        DEVMODEW current{}; current.dmSize=sizeof(current);
        BOOL currentOk=EnumDisplaySettingsExW(display.device.c_str(),ENUM_CURRENT_SETTINGS,&current,0);
        // CDS_TEST asks the driver to validate a mode; it does not apply it.
        DEVMODEW candidate=current;
        candidate.dmSize=sizeof(candidate);
        candidate.dmPelsWidth=3840; candidate.dmPelsHeight=2160; candidate.dmDisplayFrequency=60;
        candidate.dmBitsPerPel=32; candidate.dmDisplayFlags=0;
        candidate.dmFields=DM_PELSWIDTH|DM_PELSHEIGHT|DM_DISPLAYFREQUENCY|DM_BITSPERPEL|DM_DISPLAYFLAGS;
        LONG test4k=ChangeDisplaySettingsExW(display.device.c_str(),&candidate,nullptr,CDS_TEST,nullptr);
        const auto modes=EnumerateModes(display.device,0);
        const auto rawModes=EnumerateModes(display.device,EDS_RAWMODE);
        size_t rawInterlaced=0;
        for (const auto& mode : rawModes)
            if (std::get<3>(mode)) ++rawInterlaced;
        out << "{\"name\":" << Quote(Utf8(display.name)) << ",\"device\":" << Quote(Utf8(display.device))
            << ",\"desktopPosition\":[" << display.bounds.left << ',' << display.bounds.top << ']'
            << ",\"desktopCurrent\":";
        if (currentOk) out << '[' << current.dmPelsWidth << ',' << current.dmPelsHeight << ',' << current.dmDisplayFrequency << ']';
        else out << "null";
        out << ",\"desktopInterlaced\":";
        if (currentOk) out << ((current.dmDisplayFlags&DM_INTERLACED) ? "true" : "false"); else out << "null";
        out << ",\"test3840x2160At60Result\":" << test4k
            << ",\"has3840x2160Mode\":" << (Contains4k(modes) ? "true" : "false")
            << ",\"rawHas3840x2160Mode\":" << (rawModes.empty() ? "null" : Contains4k(rawModes) ? "true" : "false")
            << ",\"rawModeCount\":" << rawModes.size() << ",\"rawInterlacedModeCount\":" << rawInterlaced
            << ",\"displayConfig\":";
        WriteCcdTests(out,ccd,display.device);
        out << ",\"modes\":[";
        std::set<std::tuple<DWORD,DWORD,DWORD>> modeSizes;
        for (const auto& mode : modes)
            modeSizes.emplace(std::get<0>(mode),std::get<1>(mode),std::get<2>(mode));
        bool first=true;
        for (auto [width,height,hz] : modeSizes) {
            if (!first) out << ','; first=false;
            out << '[' << width << ',' << height << ',' << hz << ']';
        }
        out << "]}";
    }
    out << ']'; return out.str();
}
// Observe the public MSVC x64 interface's dispatch table without invoking its
// setters. Module-relative locations connect runtime interface identity to the
// exact installed binary for subsequent static analysis (not an ASLR address).
std::string VsrMethodLocations(adlx::IADLXDisplayVSR* vsr) {
    const void* const* table{};
    std::memcpy(&table,vsr,sizeof(table));
    const char* names[]={"Acquire","Release","QueryInterface","IsSupported","IsEnabled","SetEnabled"};
    std::ostringstream out; out << '[';
    for (size_t i=0;i<6;++i) {
        if (i) out << ',';
        const void* method{}; std::memcpy(&method,table+i,sizeof(method));
        HMODULE module{};
        out << "{\"method\":" << Quote(names[i]);
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(method),&module)) {
            wchar_t path[32768]{};
            DWORD length=GetModuleFileNameW(module,path,static_cast<DWORD>(std::size(path)));
            auto rva=reinterpret_cast<std::uintptr_t>(method)-reinterpret_cast<std::uintptr_t>(module);
            std::ostringstream hex; hex << "0x" << std::hex << rva;
            out << ",\"modulePath\":" << Quote(Utf8(std::wstring(path,length)))
                << ",\"relativeVirtualAddress\":" << Quote(hex.str());
        } else out << ",\"modulePath\":null,\"relativeVirtualAddress\":null";
        out << '}';
    }
    out << ']'; return out.str();
}
std::string AmdDisplays(Runtime& runtime) {
    adlx::IADLXDisplayServicesPtr services;
    Check(runtime.system->GetDisplaysServices(&services),"GetDisplaysServices");
    adlx::IADLXDisplayListPtr displays;
    Check(services->GetDisplays(&displays),"GetDisplays");
    std::ostringstream out; out << '[';
    for (adlx_uint i=displays->Begin();i<displays->End();++i) {
        if (i!=displays->Begin()) out << ',';
        adlx::IADLXDisplayPtr display;
        Check(displays->At(i,&display),"Display.At");
        const char* name{}; display->Name(&name);
        adlx_int nativeWidth{},nativeHeight{};
        auto nativeStatus=display->NativeResolution(&nativeWidth,&nativeHeight);
        out << "{\"name\":" << Quote(name ? name : "") << ",\"native\":";
        if (nativeStatus==ADLX_OK) out << '[' << nativeWidth << ',' << nativeHeight << ']'; else out << "null";
        ADLX_DISPLAY_TYPE type{};
        ADLX_DISPLAY_CONNECTOR_TYPE connector{};
        auto typeStatus=display->DisplayType(&type);
        auto connectorStatus=display->ConnectorType(&connector);
        out << ",\"displayType\":";
        if (typeStatus==ADLX_OK) out << static_cast<int>(type); else out << "null";
        out << ",\"connectorType\":";
        if (connectorStatus==ADLX_OK) out << static_cast<int>(connector); else out << "null";
        adlx::IADLXDisplayGPUScalingPtr scaling;
        auto scalingStatus=services->GetGPUScaling(display,&scaling);
        adlx_bool scalingEnabled=false;
        if (scalingStatus==ADLX_OK && scaling) scalingStatus=scaling->IsEnabled(&scalingEnabled);
        out << ",\"gpuScalingEnabled\":";
        if (scalingStatus==ADLX_OK && scaling) out << (scalingEnabled ? "true" : "false");
        else out << "null";
        adlx::IADLXDisplayVSRPtr vsr;
        ADLX_RESULT getStatus=services->GetVirtualSuperResolution(display,&vsr);
        adlx_bool supported=false,enabled=false;
        ADLX_RESULT supportedStatus=ADLX_NOT_SUPPORTED,enabledStatus=ADLX_NOT_SUPPORTED;
        if (getStatus==ADLX_OK && vsr) {
            supportedStatus=vsr->IsSupported(&supported);
            enabledStatus=vsr->IsEnabled(&enabled);
        }
        out << ",\"vsr\":{\"interfaceStatus\":" << getStatus << ",\"supportedStatus\":" << supportedStatus << ",\"supported\":";
        if (supportedStatus==ADLX_OK) out << (supported ? "true" : "false"); else out << "null";
        out << ",\"enabledStatus\":" << enabledStatus << ",\"enabled\":";
        if (enabledStatus==ADLX_OK) out << (enabled ? "true" : "false"); else out << "null";
        out << "},\"vsrMethodLocations\":";
        if (getStatus==ADLX_OK && vsr) out << VsrMethodLocations(vsr.GetPtr());
        else out << "null";
        out << '}';
    }
    out << ']'; return out.str();
}
}
int wmain(int argc,wchar_t** argv) {
    std::filesystem::path reportPath=argc>1 ? argv[1] : L"amd-desktop-probe.json";
    std::string amd="[]",version,error;
    bool available=false;
    try {
        Runtime runtime; runtime.Open(); version=runtime.version;
        amd=AmdDisplays(runtime); available=true;
    } catch (const std::exception& exception) { error=exception.what(); }
    std::ofstream report(reportPath,std::ios::binary);
    report << "{\n\"readOnly\":true,\n\"settingsChanged\":false,\n\"adlxAvailable\":" << (available ? "true" : "false")
        << ",\n\"adlxVersion\":" << Quote(version) << ",\n\"error\":" << (error.empty() ? "null" : Quote(error))
        << ",\n\"amdDisplays\":" << amd << ",\n\"windowsDisplays\":" << WindowsDisplays()
        << ",\n\"acerSamsungCloneValidation\":" << ValidateAcerSamsungClone();
    if (argc>2) report << ",\n\"virtualSamsungCloneValidation\":" << ValidateVirtualSamsungClone(argv[2]);
    report << "\n}\n";
    std::cout << "Read-only AMD desktop report: " << reportPath.string() << '\n';
    if (!error.empty()) std::cout << error << '\n';
    return !report ? 1 : (available ? 0 : 2);
}
