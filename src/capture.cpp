#include "capture.h"
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <dwmapi.h>
#include <algorithm>
#include <atomic>
#include <chrono>

using namespace winrt;
using winrt::Windows::Graphics::SizeInt32;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;

void Capture::Start(HWND source, HWND notification, ID3D11Device* device) {
    if (!IsWindow(source) || IsIconic(source)) throw hresult_error(E_INVALIDARG,L"Kaynak pencere acik olmali; simge durumuna kucultmeyin.");
    StartItem(source,nullptr,notification,device);
}
void Capture::StartMonitor(HMONITOR monitor, HWND notification, ID3D11Device* device) {
    MONITORINFO info{sizeof(info)};
    if (!GetMonitorInfoW(monitor,&info)) throw hresult_error(E_INVALIDARG,L"Kaynak ekran artık etkin değil.");
    StartItem(nullptr,monitor,notification,device);
}
void Capture::StartItem(HWND source, HMONITOR monitor, HWND notification, ID3D11Device* device) {
    Stop();
    if (!GraphicsCaptureSession::IsSupported()) throw hresult_error(E_NOTIMPL,L"Windows Graphics Capture bu sistemde kullanilamiyor.");
    source_ = source;
    monitor_ = monitor;
    static std::atomic<unsigned long long> nextGeneration{0};
    generation_ = ++nextGeneration;
    try {
        auto interop = get_activation_factory<GraphicsCaptureItem,IGraphicsCaptureItemInterop>();
        if (monitor) check_hresult(interop->CreateForMonitor(monitor,guid_of<GraphicsCaptureItem>(),put_abi(item_)));
        else check_hresult(interop->CreateForWindow(source,guid_of<GraphicsCaptureItem>(),put_abi(item_)));
        com_ptr<IDXGIDevice> dxgiDevice;
        check_hresult(device->QueryInterface(__uuidof(IDXGIDevice),dxgiDevice.put_void()));
        // Use the same D3D11 device for capture and filtering.
        com_ptr<IInspectable> inspectable;
        check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.get(),inspectable.put()));
        winrtDevice_ = inspectable.as<IDirect3DDevice>();
        size_ = item_.Size();
        pool_ = Direct3D11CaptureFramePool::CreateFreeThreaded(winrtDevice_,DirectXPixelFormat::B8G8R8A8UIntNormalized,2,size_);
        session_ = pool_.CreateCaptureSession(item_);
        if (auto timing=session_.try_as<IGraphicsCaptureSession5>()) {
            // Let source updates through across independently clocked displays.
            // The TV swap chain still paces presentation at its own refresh rate.
            try { timing.MinUpdateInterval(std::chrono::milliseconds(1)); }
            catch (const hresult_error&) { /* The default interval remains usable. */ }
        }
        // Capture the game's own cursor for the first prototype; no input injection.
        session_.IsCursorCaptureEnabled(true);
        mailbox_ = std::make_shared<Mailbox>();
        mailbox_->notification = notification;
        auto state = mailbox_;
        auto generation = generation_;
        frameToken_ = pool_.FrameArrived([state,generation](auto const& sender, auto const&) {
            try {
                auto frame = sender.TryGetNextFrame();
                if (!frame) return;
                LARGE_INTEGER arrival{};
                QueryPerformanceCounter(&arrival);
                std::lock_guard guard(state->mutex);
                if (!state->active) { frame.Close(); return; }
                if (state->latest) { state->latest.Close(); ++state->dropped; }
                state->latest = frame;
                state->arrivalQpc = arrival.QuadPart;
                if (!state->posted) {
                    state->posted = PostMessageW(state->notification,WM_CAPTURE_FRAME,static_cast<WPARAM>(generation),0)!=FALSE;
                }
            } catch (...) { /* Closing the pool can cancel an in-flight callback. */ }
        });
        closedToken_ = item_.Closed([state,generation](auto const&,auto const&) {
            std::lock_guard guard(state->mutex);
            if (state->active) PostMessageW(state->notification,WM_CAPTURE_CLOSED,static_cast<WPARAM>(generation),0);
        });
        session_.StartCapture();
    } catch (...) { Stop(); throw; }
}
void Capture::Stop() {
    if (mailbox_) {
        std::lock_guard guard(mailbox_->mutex);
        mailbox_->active = false;
        if (mailbox_->latest) { mailbox_->latest.Close(); mailbox_->latest = nullptr; }
    }
    // Never hold the mailbox mutex while closing WinRT event sources.
    if (item_) { try { item_.Closed(closedToken_); } catch (...) {} }
    if (pool_) { try { pool_.FrameArrived(frameToken_); } catch (...) {} }
    if (session_) { try { session_.Close(); } catch (...) {} }
    if (pool_) { try { pool_.Close(); } catch (...) {} }
    session_ = nullptr; pool_ = nullptr; item_ = nullptr; winrtDevice_ = nullptr;
    mailbox_.reset(); source_ = nullptr; monitor_ = nullptr; size_ = {};
}
CapturedFrame Capture::TakeLatest() {
    CapturedFrame result;
    if (!mailbox_) return result;
    std::lock_guard guard(mailbox_->mutex);
    result.frame = std::move(mailbox_->latest);
    mailbox_->latest = nullptr;
    mailbox_->posted = false;
    if (result.frame) result.arrivalQpc = mailbox_->arrivalQpc;
    return result;
}
bool Capture::ResizePoolIfNeeded(CapturedFrame& frame, const SizeInt32& size) {
    if (size.Width < 1 || size.Height < 1) {
        frame.frame.Close();
        return true;
    }
    if (size.Width == size_.Width && size.Height == size_.Height) return false;
    frame.frame.Close();
    {
        std::lock_guard guard(mailbox_->mutex);
        if (mailbox_->latest) {
            mailbox_->latest.Close();
            mailbox_->latest = nullptr;
        }
    }
    pool_.Recreate(winrtDevice_,DirectXPixelFormat::B8G8R8A8UIntNormalized,2,size);
    size_ = size;
    return true;
}
unsigned long long Capture::Dropped() const {
    if (!mailbox_) return 0;
    std::lock_guard guard(mailbox_->mutex);
    return mailbox_->dropped;
}
RECT Capture::ClientCrop(HWND source, int width, int height) {
    RECT full{0,0,width,height},client{},bounds{};
    if (!GetClientRect(source,&client)) return full;
    if (client.right == width && client.bottom == height) return full;
    // WGC's window extent normally matches DWM's visible frame bounds.
    if (FAILED(DwmGetWindowAttribute(source,DWMWA_EXTENDED_FRAME_BOUNDS,&bounds,sizeof(bounds)))) return full;
    if (std::abs((bounds.right-bounds.left)-width)>2 || std::abs((bounds.bottom-bounds.top)-height)>2) return full;
    POINT origin{0,0}; if (!ClientToScreen(source,&origin)) return full;
    LONG left = origin.x-bounds.left,top=origin.y-bounds.top;
    if (left < 0 || top < 0 || left+client.right > width || top+client.bottom > height || client.right <= 0 || client.bottom <= 0) return full;
    return {left,top,left+client.right,top+client.bottom};
}
