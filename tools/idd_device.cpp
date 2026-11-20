#include <windows.h>
#include <cfgmgr32.h>
#include <swdevice.h>

#include <cstdio>
#include <cwchar>
#include <vector>

namespace {

constexpr wchar_t hardwareIds[] = L"UpscalpVirtualDisplay\0";
constexpr wchar_t monitorPathPrefix[] = L"\\\\?\\DISPLAY#USC0001#";
HANDLE stopEvent{};

struct CreateResult {
    HANDLE event;
    HRESULT status{E_PENDING};
    wchar_t deviceId[MAX_DEVICE_ID_LEN]{};
};

struct SoftwareDevice {
    HSWDEVICE handle{};
    ~SoftwareDevice() { if (handle) SwDeviceClose(handle); }
};

void CALLBACK Created(HSWDEVICE, HRESULT status, void* context, PCWSTR id) {
    auto& result = *static_cast<CreateResult*>(context);
    result.status = status;
    if (id) wcscpy_s(result.deviceId, id);
    SetEvent(result.event);
}

BOOL WINAPI ConsoleControl(DWORD type) {
    if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT) return FALSE;
    SetEvent(stopEvent);
    return TRUE;
}

bool ReportMonitor() {
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    LONG query = ERROR_INSUFFICIENT_BUFFER;
    for (int attempt = 0; attempt < 3 && query == ERROR_INSUFFICIENT_BUFFER; ++attempt) {
        UINT32 pathCount{}, modeCount{};
        query = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount);
        if (query != ERROR_SUCCESS) break;
        paths.resize(pathCount);
        modes.resize(modeCount);
        query = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(),
                                   &modeCount, modes.data(), nullptr);
        paths.resize(pathCount);
        modes.resize(modeCount);
    }
    if (query != ERROR_SUCCESS) return false;

    for (const auto& path : paths) {
        DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
        target.header = {DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME, sizeof(target),
                         path.targetInfo.adapterId, path.targetInfo.id};
        if (DisplayConfigGetDeviceInfo(&target.header) != ERROR_SUCCESS ||
            _wcsnicmp(target.monitorDevicePath, monitorPathPrefix, ARRAYSIZE(monitorPathPrefix) - 1) != 0) continue;

        const auto index = path.sourceInfo.modeInfoIdx;
        if (index >= modes.size() || modes[index].infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) continue;
        const auto& source = modes[index].sourceMode;
        const auto refresh = path.targetInfo.refreshRate;
        std::wprintf(L"%ls: %u x %u at %.3f Hz, desktop position (%ld, %ld)\n",
                     target.monitorFriendlyDeviceName, source.width, source.height,
                     refresh.Denominator ? static_cast<double>(refresh.Numerator) / refresh.Denominator : 0.0,
                     source.position.x, source.position.y);
        return true;
    }
    return false;
}

int StartMonitor() {
    CreateResult result{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    if (!result.event) return 1;

    SW_DEVICE_CREATE_INFO info{};
    info.cbSize = sizeof(info);
    info.pszInstanceId = L"VirtualDisplay";
    info.pszzHardwareIds = hardwareIds;
    info.pszDeviceDescription = L"Upscalp Indirect Display Adapter";
    info.CapabilityFlags = SWDeviceCapabilitiesSilentInstall | SWDeviceCapabilitiesRemovable |
                           SWDeviceCapabilitiesDriverRequired;

    SoftwareDevice device;
    HRESULT status = SwDeviceCreate(L"Upscalp", L"HTREE\\ROOT\\0", &info, 0, nullptr,
                                    Created, &result, &device.handle);
    if (SUCCEEDED(status)) {
        const HANDLE events[]{result.event, stopEvent};
        const DWORD wait = WaitForMultipleObjects(ARRAYSIZE(events), events, FALSE, 15000);
        if (wait != WAIT_OBJECT_0) {
            // Closing synchronizes with an in-flight creation callback before its context is discarded.
            SwDeviceClose(device.handle);
            device.handle = nullptr;
            status = wait == WAIT_OBJECT_0 + 1 ? HRESULT_FROM_WIN32(ERROR_CANCELLED)
                                              : HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        } else {
            status = result.status;
        }
    }
    CloseHandle(result.event);
    if (FAILED(status)) {
        std::fwprintf(stderr, L"Software device creation failed (0x%08lX). Stage the signed INF and run as administrator.\n",
                      static_cast<unsigned long>(status));
        return 1;
    }

    std::wprintf(L"Software device: %ls\n", result.deviceId);
    for (int i = 0; i < 60; ++i) {
        if (ReportMonitor()) {
            std::wprintf(L"Virtual monitor active. Press Ctrl+C to disconnect it.\n");
            WaitForSingleObject(stopEvent, INFINITE);
            return 0;
        }
        if (WaitForSingleObject(stopEvent, 250) != WAIT_TIMEOUT) return 0;
    }

    DEVINST instance{};
    ULONG flags{}, problem{};
    if (CM_Locate_DevNodeW(&instance, result.deviceId, CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS &&
        CM_Get_DevNode_Status(&flags, &problem, instance, 0) == CR_SUCCESS) {
        std::fwprintf(stderr, L"PnP status 0x%lX, problem code %lu.\n", flags, problem);
    }
    // Windows can remember a disconnected display topology from an earlier session.
    std::wprintf(L"Virtual monitor has no active desktop. Check Windows Display settings and select Extend desktop.\n"
                 L"Press Ctrl+C to remove the software device.\n");
    for (;;) {
        if (WaitForSingleObject(stopEvent, 1000) != WAIT_TIMEOUT) return 1;
        if (ReportMonitor()) {
            WaitForSingleObject(stopEvent, INFINITE);
            return 0;
        }
    }
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && wcscmp(argv[1], L"--status") == 0) {
        if (ReportMonitor()) return 0;
        std::wprintf(L"No active Upscalp virtual display.\n");
        return 1;
    }
    if (argc != 2 || wcscmp(argv[1], L"--start") != 0) {
        std::wprintf(L"Usage: UpscalpIddDevice.exe --status | --start\n");
        return 2;
    }

    stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stopEvent) return 1;
    if (!SetConsoleCtrlHandler(ConsoleControl, TRUE)) {
        CloseHandle(stopEvent);
        return 1;
    }
    const int result = StartMonitor();
    SetConsoleCtrlHandler(ConsoleControl, FALSE);
    CloseHandle(stopEvent);
    return result;
}
