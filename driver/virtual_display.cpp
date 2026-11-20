#include <ntstatus.h>
#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS

#include <wdf.h>
#include <IddCx.h>
#include <d3d11.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <array>
#include <cstring>
#include <new>

namespace {

using Microsoft::WRL::ComPtr;

struct Timing {
    UINT width, height, hBlank, vBlank, hFront, hSync, vFront, vSync, pixelRate;
};

constexpr std::array<Timing, 2> timings{{
    {3840, 2160, 560, 90, 176, 88, 8, 10, 594000000},
    {1920, 1080, 280, 45, 88, 44, 4, 5, 148500000},
}};

// EDID 1.4 with the same detailed timings advertised to IddCx below.
constexpr std::array<BYTE, 128> MakeEdid() {
    std::array<BYTE, 128> edid{
        0, 255, 255, 255, 255, 255, 255, 0, 0x56, 0x63, 1, 0, 1, 0, 0, 0,
        1, 35, 1, 4, 0xa0, 0, 0, 120, 0x06,
        0xee, 0x91, 0xa3, 0x54, 0x4c, 0x99, 0x26, 0x0f, 0x50, 0x54,
    };
    for (size_t i = 38; i < 54; ++i) edid[i] = 1;
    for (size_t i = 0; i < timings.size(); ++i) {
        const auto& mode = timings[i];
        auto* dtd = edid.data() + 54 + 18 * i;
        const UINT clock = mode.pixelRate / 10000;
        dtd[0] = static_cast<BYTE>(clock);
        dtd[1] = static_cast<BYTE>(clock >> 8);
        dtd[2] = static_cast<BYTE>(mode.width);
        dtd[3] = static_cast<BYTE>(mode.hBlank);
        dtd[4] = static_cast<BYTE>(((mode.width >> 8) << 4) | (mode.hBlank >> 8));
        dtd[5] = static_cast<BYTE>(mode.height);
        dtd[6] = static_cast<BYTE>(mode.vBlank);
        dtd[7] = static_cast<BYTE>(((mode.height >> 8) << 4) | (mode.vBlank >> 8));
        dtd[8] = static_cast<BYTE>(mode.hFront);
        dtd[9] = static_cast<BYTE>(mode.hSync);
        dtd[10] = static_cast<BYTE>((mode.vFront << 4) | (mode.vSync & 15));
        dtd[11] = static_cast<BYTE>(((mode.hFront >> 8) << 6) | ((mode.hSync >> 8) << 4) |
                                    ((mode.vFront >> 4) << 2) | (mode.vSync >> 4));
        dtd[17] = 0x1e;
    }
    edid[93] = 0xfc;
    constexpr char name[] = "Upscalp 4K\n  ";
    for (size_t i = 0; i < sizeof(name) - 1; ++i) edid[95 + i] = name[i];
    edid[111] = 0x10; // Unused descriptor.
    unsigned sum = 0;
    for (auto byte : edid) sum += byte;
    edid[127] = static_cast<BYTE>(0u - sum);
    return edid;
}

constexpr auto edid = MakeEdid();
constexpr GUID monitorContainer{0x92dc31f2, 0x66af, 0x47e2, {0x95, 0xa9, 0x8e, 0xdc, 0x93, 0x87, 0xf4, 0x2a}};

DISPLAYCONFIG_VIDEO_SIGNAL_INFO Signal(const Timing& mode, UINT divider) {
    DISPLAYCONFIG_VIDEO_SIGNAL_INFO signal{};
    signal.activeSize = {mode.width, mode.height};
    signal.totalSize = {mode.width + mode.hBlank, mode.height + mode.vBlank};
    signal.pixelRate = mode.pixelRate;
    signal.hSyncFreq = {mode.pixelRate / signal.totalSize.cx, 1};
    signal.vSyncFreq = {60, 1};
    signal.AdditionalSignalInfo.videoStandard = 255;
    signal.AdditionalSignalInfo.vSyncFreqDivider = divider;
    signal.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;
    return signal;
}

struct SwapChainWorker {
    IDDCX_SWAPCHAIN chain;
    HANDLE available;
    LUID renderAdapter;
    HANDLE stop{};
    HANDLE thread{};

    static DWORD CALLBACK Run(void* context) {
        auto& worker = *static_cast<SwapChainWorker*>(context);
        worker.ProcessFrames();
        WdfObjectDelete(worker.chain);
        return 0;
    }

    void ProcessFrames() const {
        ComPtr<IDXGIFactory4> factory;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<ID3D11Device> device;
        ComPtr<IDXGIDevice> dxgiDevice;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
            FAILED(factory->EnumAdapterByLuid(renderAdapter, IID_PPV_ARGS(&adapter))) ||
            FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                     D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                     D3D11_SDK_VERSION, &device, nullptr, nullptr)) ||
            FAILED(device.As(&dxgiDevice))) return;

        IDARG_IN_SWAPCHAINSETDEVICE binding{dxgiDevice.Get()};
        if (FAILED(IddCxSwapChainSetDevice(chain, &binding))) return;

        const HANDLE events[]{stop, available};
        while (WaitForSingleObject(stop, 0) == WAIT_TIMEOUT) {
            IDARG_OUT_RELEASEANDACQUIREBUFFER frame{};
            const HRESULT result = IddCxSwapChainReleaseAndAcquireBuffer(chain, &frame);
            if (result == E_PENDING) {
                if (WaitForMultipleObjects(ARRAYSIZE(events), events, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) break;
                continue;
            }
            if (FAILED(result)) break;
            // The Upscalp application captures the composed display through WGC.
            if (frame.MetaData.pSurface) frame.MetaData.pSurface->Release();
            if (FAILED(IddCxSwapChainFinishedProcessingFrame(chain))) break;
        }
    }

    ~SwapChainWorker() {
        if (thread) {
            SetEvent(stop);
            WaitForSingleObject(thread, INFINITE);
            CloseHandle(thread);
        }
        if (stop) CloseHandle(stop);
    }
};

struct DeviceContext { bool adapterStarted; };
struct MonitorContext { SwapChainWorker* worker; };
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DeviceContext, GetDeviceContext)
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(MonitorContext, GetMonitorContext)

void StopWorker(WDFOBJECT monitor) {
    auto* context = GetMonitorContext(monitor);
    auto* worker = context->worker;
    context->worker = nullptr;
    delete worker;
}

NTSTATUS AssignSwapChain(IDDCX_MONITOR monitor, const IDARG_IN_SETSWAPCHAIN* chain) {
    StopWorker(monitor);
    auto* worker = new (std::nothrow) SwapChainWorker{chain->hSwapChain, chain->hNextSurfaceAvailable,
                                                   chain->RenderAdapterLuid};
    if (!worker) return STATUS_GRAPHICS_INDIRECT_DISPLAY_ABANDON_SWAPCHAIN;
    worker->stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (worker->stop) worker->thread = CreateThread(nullptr, 0, SwapChainWorker::Run, worker, 0, nullptr);
    if (!worker->thread) {
        delete worker;
        return STATUS_GRAPHICS_INDIRECT_DISPLAY_ABANDON_SWAPCHAIN;
    }
    GetMonitorContext(monitor)->worker = worker;
    return STATUS_SUCCESS;
}

NTSTATUS UnassignSwapChain(IDDCX_MONITOR monitor) {
    StopWorker(monitor);
    return STATUS_SUCCESS;
}

NTSTATUS ParseMonitor(const IDARG_IN_PARSEMONITORDESCRIPTION* input, IDARG_OUT_PARSEMONITORDESCRIPTION* output) {
    if (input->MonitorDescription.DataSize != edid.size() ||
        !input->MonitorDescription.pData ||
        std::memcmp(input->MonitorDescription.pData, edid.data(), edid.size()) != 0) return STATUS_INVALID_PARAMETER;

    output->MonitorModeBufferOutputCount = static_cast<UINT>(timings.size());
    output->PreferredMonitorModeIdx = 0;
    if (!input->MonitorModeBufferInputCount) return STATUS_SUCCESS;
    if (input->MonitorModeBufferInputCount < timings.size()) return STATUS_BUFFER_TOO_SMALL;
    for (size_t i = 0; i < timings.size(); ++i) {
        auto& mode = input->pMonitorModes[i];
        mode = {};
        mode.Size = sizeof(mode);
        mode.Origin = IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR;
        mode.MonitorVideoSignalInfo = Signal(timings[i], 0);
    }
    return STATUS_SUCCESS;
}

NTSTATUS QueryTargetModes(IDDCX_MONITOR, const IDARG_IN_QUERYTARGETMODES* input, IDARG_OUT_QUERYTARGETMODES* output) {
    output->TargetModeBufferOutputCount = static_cast<UINT>(timings.size());
    if (input->TargetModeBufferInputCount < timings.size()) return STATUS_SUCCESS;
    for (size_t i = 0; i < timings.size(); ++i) {
        auto& mode = input->pTargetModes[i];
        mode = {};
        mode.Size = sizeof(mode);
        mode.TargetVideoSignalInfo.targetVideoSignalInfo = Signal(timings[i], 1);
    }
    return STATUS_SUCCESS;
}

NTSTATUS AdapterReady(IDDCX_ADAPTER adapter, const IDARG_IN_ADAPTER_INIT_FINISHED* result) {
    if (!NT_SUCCESS(result->AdapterInitStatus)) return result->AdapterInitStatus;

    IDDCX_MONITOR_INFO info{};
    info.Size = sizeof(info);
    info.MonitorType = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INDIRECT_VIRTUAL;
    info.MonitorContainerId = monitorContainer;
    info.MonitorDescription.Size = sizeof(info.MonitorDescription);
    info.MonitorDescription.Type = IDDCX_MONITOR_DESCRIPTION_TYPE_EDID;
    info.MonitorDescription.DataSize = static_cast<UINT>(edid.size());
    info.MonitorDescription.pData = const_cast<BYTE*>(edid.data());

    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, MonitorContext);
    attributes.EvtCleanupCallback = [](WDFOBJECT object) { StopWorker(object); };
    IDARG_IN_MONITORCREATE create{&attributes, &info};
    IDARG_OUT_MONITORCREATE monitor{};
    const NTSTATUS status = IddCxMonitorCreate(adapter, &create, &monitor);
    if (!NT_SUCCESS(status)) return status;

    IDARG_OUT_MONITORARRIVAL arrival{};
    return IddCxMonitorArrival(monitor.MonitorObject, &arrival);
}

NTSTATUS DeviceD0Entry(WDFDEVICE device, WDF_POWER_DEVICE_STATE) {
    auto& started = GetDeviceContext(device)->adapterStarted;
    if (started) return STATUS_SUCCESS;

    IDDCX_ENDPOINT_VERSION version{};
    version.Size = sizeof(version);
    version.MajorVer = 1;
    IDDCX_ADAPTER_CAPS capabilities{};
    capabilities.Size = sizeof(capabilities);
    capabilities.MaxMonitorsSupported = 1;
    auto& diagnostics = capabilities.EndPointDiagnostics;
    diagnostics.Size = sizeof(diagnostics);
    diagnostics.GammaSupport = IDDCX_FEATURE_IMPLEMENTATION_NONE;
    diagnostics.TransmissionType = IDDCX_TRANSMISSION_TYPE_WIRED_OTHER;
    diagnostics.pEndPointFriendlyName = L"Upscalp Virtual Display";
    diagnostics.pEndPointManufacturerName = L"Upscalp";
    diagnostics.pEndPointModelName = L"Upscalp 4K";
    diagnostics.pFirmwareVersion = &version;
    diagnostics.pHardwareVersion = &version;

    IDARG_IN_ADAPTER_INIT init{};
    init.WdfDevice = device;
    init.pCaps = &capabilities;
    IDARG_OUT_ADAPTER_INIT adapter{};
    const NTSTATUS status = IddCxAdapterInitAsync(&init, &adapter);
    started = NT_SUCCESS(status);
    return status;
}

NTSTATUS AddDevice(WDFDRIVER, PWDFDEVICE_INIT init) {
    WDF_PNPPOWER_EVENT_CALLBACKS power;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&power);
    power.EvtDeviceD0Entry = DeviceD0Entry;
    WdfDeviceInitSetPnpPowerEventCallbacks(init, &power);

    IDD_CX_CLIENT_CONFIG config;
    IDD_CX_CLIENT_CONFIG_INIT(&config);
    config.EvtIddCxAdapterInitFinished = AdapterReady;
    config.EvtIddCxAdapterCommitModes = [](IDDCX_ADAPTER, const IDARG_IN_COMMITMODES*) -> NTSTATUS { return STATUS_SUCCESS; };
    config.EvtIddCxParseMonitorDescription = ParseMonitor;
    config.EvtIddCxMonitorQueryTargetModes = QueryTargetModes;
    config.EvtIddCxMonitorAssignSwapChain = AssignSwapChain;
    config.EvtIddCxMonitorUnassignSwapChain = UnassignSwapChain;
    const NTSTATUS configured = IddCxDeviceInitConfig(init, &config);
    if (!NT_SUCCESS(configured)) return configured;

    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, DeviceContext);
    WDFDEVICE device{};
    const NTSTATUS created = WdfDeviceCreate(&init, &attributes, &device);
    return NT_SUCCESS(created) ? IddCxDeviceInitialize(device) : created;
}

} // namespace

extern "C" DRIVER_INITIALIZE DriverEntry;

extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING registry) {
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, AddDevice);
    return WdfDriverCreate(driver, registry, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
}
