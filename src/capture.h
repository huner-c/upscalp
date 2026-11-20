#pragma once
#include "renderer.h"
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <memory>
#include <mutex>

constexpr UINT WM_CAPTURE_FRAME = WM_APP+1;
constexpr UINT WM_CAPTURE_CLOSED = WM_APP+2;
struct CapturedFrame {
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame frame{nullptr};
    long long arrivalQpc{};
};
class Capture {
public:
    ~Capture() { Stop(); }
    void Start(HWND source, HWND notification, ID3D11Device* device);
    void StartMonitor(HMONITOR monitor, HWND notification, ID3D11Device* device);
    void Stop();
    CapturedFrame TakeLatest();
    bool ResizePoolIfNeeded(CapturedFrame& frame, const winrt::Windows::Graphics::SizeInt32& size);
    bool Running() const { return pool_ != nullptr; }
    unsigned long long Dropped() const;
    HWND Source() const { return source_; }
    HMONITOR SourceMonitor() const { return monitor_; }
    unsigned long long Generation() const { return generation_; }
    static RECT ClientCrop(HWND source, int width, int height);
private:
    void StartItem(HWND source, HMONITOR monitor, HWND notification, ID3D11Device* device);
    struct Mailbox {
        std::mutex mutex;
        winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame latest{nullptr};
        long long arrivalQpc{};
        bool active{true}, posted{};
        unsigned long long dropped{};
        HWND notification{};
    };
    HWND source_{};
    HMONITOR monitor_{};
    unsigned long long generation_{};
    std::shared_ptr<Mailbox> mailbox_;
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem item_{nullptr};
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool pool_{nullptr};
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession session_{nullptr};
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice winrtDevice_{nullptr};
    winrt::Windows::Graphics::SizeInt32 size_{};
    winrt::event_token frameToken_{}, closedToken_{};
};
