#pragma once
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <winrt/base.h>
#include <array>
#include <string>
#include <vector>

using winrt::com_ptr;
enum class Filter : unsigned { Area, Mitchell, Lanczos, Bilinear, LanczosGuarded };
struct RenderOptions {
    Filter filter = Filter::LanczosGuarded;
    bool linearLight = true;
    bool comparison = false;
    float sharpness = .15f;
};
struct Surface {
    com_ptr<ID3D11Texture2D> texture;
    com_ptr<ID3D11ShaderResourceView> srv;
    com_ptr<ID3D11RenderTargetView> rtv;
};
struct DisplaySurface {
    HWND hwnd{};
    unsigned width{}, height{};
    com_ptr<IDXGISwapChain1> swapchain;
    com_ptr<ID3D11RenderTargetView> rtv;
    bool lowLatency{};
    winrt::handle frameLatency;
    bool frameSlotReady{};
};
class Renderer {
public:
    void Initialize(bool software = false);
    ID3D11Device* Device() const { return device_.get(); }
    std::wstring AdapterName() const { return adapterName_; }
    void SetOutputSize(unsigned width, unsigned height);
    void Upload(unsigned width, unsigned height, const std::vector<unsigned char>& bgra);
    void CopyFrame(ID3D11Texture2D* frame, const RECT& crop);
    void Process(const RenderOptions& options);
    void PrepareDisplay(DisplaySurface& display);
    bool Present(DisplaySurface& display, const RenderOptions& options, bool vsync);
    void DrawPattern(DisplaySurface& display, float seconds);
    std::vector<unsigned char> ReadOutput(const RenderOptions& options);
    void SaveOutput(const std::wstring& path, const RenderOptions& options);
    void ResetSource();
    bool HasSource() const { return source_.texture != nullptr; }
    unsigned SourceWidth() const { return sourceWidth_; }
    unsigned SourceHeight() const { return sourceHeight_; }
    unsigned OutputWidth() const { return outputWidth_; }
    unsigned OutputHeight() const { return outputHeight_; }
    double LastGpuMs() const { return lastGpuMs_; }
private:
    struct alignas(16) Constants {
        float source[2]{}, target[2]{};
        unsigned horizontal{}, filter{}, linearLight{}, comparison{};
        float sharpness{}, time{}, padding[2]{};
    };
    struct Timing {
        com_ptr<ID3D11Query> disjoint, begin, end;
        bool pending{};
    };
    Surface MakeSurface(unsigned width, unsigned height, DXGI_FORMAT format, bool target);
    void EnsureSource(unsigned width, unsigned height);
    void EnsurePipeline();
    void EnsureDisplay(DisplaySurface& display);
    void Pass(ID3D11RenderTargetView* target, unsigned width, unsigned height,
              ID3D11PixelShader* shader, const Constants& constants,
              ID3D11ShaderResourceView* image, ID3D11ShaderResourceView* original = nullptr,
              int left = 0, int top = 0);
    Constants PresentationConstants(unsigned width, unsigned height, const RenderOptions& options) const;
    void PollTiming();
    com_ptr<ID3D11Device> device_;
    com_ptr<ID3D11DeviceContext> context_;
    com_ptr<IDXGIFactory2> factory_;
    com_ptr<ID3D11VertexShader> vertex_;
    com_ptr<ID3D11PixelShader> filter_, present_, pattern_;
    com_ptr<ID3D11Buffer> constants_;
    com_ptr<ID3D11SamplerState> sampler_;
    com_ptr<ID3D11RasterizerState> rasterizer_;
    Surface source_, horizontal_, output_;
    com_ptr<ID3D11ShaderResourceView> sourceSrgb_;
    unsigned sourceWidth_{}, sourceHeight_{}, outputWidth_{1920}, outputHeight_{1080};
    std::wstring adapterName_;
    std::array<Timing,4> timings_;
    unsigned timingIndex_{};
    double lastGpuMs_{-1};
};
int RunSelfTests(const std::wstring& reportPath);
