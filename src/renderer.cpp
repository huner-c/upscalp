#include "renderer.h"
#include <d3dcompiler.h>
#include <dxgi1_3.h>
#include <wincodec.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

using winrt::check_hresult;
namespace {
com_ptr<ID3DBlob> Compile(const char* entry, const char* profile) {
    HMODULE module = GetModuleHandleW(nullptr);
    HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(101), RT_RCDATA);
    if (!resource) winrt::throw_last_error();
    const auto data = LockResource(LoadResource(module,resource));
    com_ptr<ID3DBlob> code, errors;
    HRESULT hr = D3DCompile(data, SizeofResource(module,resource), "downscale.hlsl",
        nullptr,nullptr,entry,profile,D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0,code.put(),errors.put());
    if (FAILED(hr)) {
        std::string detail = errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()),errors->GetBufferSize()) : "Shader compilation failed";
        throw winrt::hresult_error(hr,winrt::to_hstring(detail));
    }
    return code;
}
}
void Renderer::Initialize(bool software) {
    D3D_FEATURE_LEVEL level{};
    constexpr D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0};
    check_hresult(D3D11CreateDevice(nullptr,software ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE,
        nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,levels,2,D3D11_SDK_VERSION,device_.put(),&level,context_.put()));
    auto dxgiDevice = device_.as<IDXGIDevice1>();
    check_hresult(dxgiDevice->SetMaximumFrameLatency(1));
    com_ptr<IDXGIAdapter> adapter;
    check_hresult(dxgiDevice->GetAdapter(adapter.put()));
    DXGI_ADAPTER_DESC description{};
    check_hresult(adapter->GetDesc(&description));
    adapterName_ = description.Description;
    check_hresult(adapter->GetParent(__uuidof(IDXGIFactory2),factory_.put_void()));
    auto vs = Compile("VS","vs_5_0");
    auto ps = Compile("FilterPS","ps_5_0");
    auto present = Compile("PresentPS","ps_5_0");
    auto pattern = Compile("PatternPS","ps_5_0");
    check_hresult(device_->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,vertex_.put()));
    check_hresult(device_->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,filter_.put()));
    check_hresult(device_->CreatePixelShader(present->GetBufferPointer(),present->GetBufferSize(),nullptr,present_.put()));
    check_hresult(device_->CreatePixelShader(pattern->GetBufferPointer(),pattern->GetBufferSize(),nullptr,pattern_.put()));
    D3D11_BUFFER_DESC buffer{};
    buffer.ByteWidth = sizeof(Constants);
    buffer.Usage = D3D11_USAGE_DEFAULT;
    buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    check_hresult(device_->CreateBuffer(&buffer,nullptr,constants_.put()));
    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    check_hresult(device_->CreateSamplerState(&sampler,sampler_.put()));
    D3D11_RASTERIZER_DESC rasterizer{};
    rasterizer.FillMode = D3D11_FILL_SOLID;
    rasterizer.CullMode = D3D11_CULL_NONE;
    rasterizer.DepthClipEnable = TRUE;
    check_hresult(device_->CreateRasterizerState(&rasterizer,rasterizer_.put()));
    for (auto& timing : timings_) {
        D3D11_QUERY_DESC query{D3D11_QUERY_TIMESTAMP_DISJOINT,0};
        check_hresult(device_->CreateQuery(&query,timing.disjoint.put()));
        query.Query = D3D11_QUERY_TIMESTAMP;
        check_hresult(device_->CreateQuery(&query,timing.begin.put()));
        check_hresult(device_->CreateQuery(&query,timing.end.put()));
    }
}
Surface Renderer::MakeSurface(unsigned width, unsigned height, DXGI_FORMAT format, bool target) {
    Surface surface;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width; desc.Height = height; desc.MipLevels = 1; desc.ArraySize = 1;
    desc.Format = format; desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | (target ? D3D11_BIND_RENDER_TARGET : 0);
    check_hresult(device_->CreateTexture2D(&desc,nullptr,surface.texture.put()));
    check_hresult(device_->CreateShaderResourceView(surface.texture.get(),nullptr,surface.srv.put()));
    if (target) check_hresult(device_->CreateRenderTargetView(surface.texture.get(),nullptr,surface.rtv.put()));
    return surface;
}
void Renderer::SetOutputSize(unsigned width, unsigned height) {
    if (!width || !height || width > 8192 || height > 8192) throw std::invalid_argument("Invalid output size");
    if (width == outputWidth_ && height == outputHeight_) return;
    outputWidth_ = width; outputHeight_ = height;
    horizontal_ = {}; output_ = {};
}
void Renderer::EnsureSource(unsigned width, unsigned height) {
    if (!width || !height || width > 16384 || height > 16384) throw std::invalid_argument("Invalid source size");
    if (width == sourceWidth_ && height == sourceHeight_ && source_.texture) return;
    Surface source;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width=width; desc.Height=height; desc.MipLevels=1; desc.ArraySize=1;
    desc.Format=DXGI_FORMAT_B8G8R8A8_TYPELESS; desc.SampleDesc.Count=1;
    desc.Usage=D3D11_USAGE_DEFAULT; desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    check_hresult(device_->CreateTexture2D(&desc,nullptr,source.texture.put()));
    D3D11_SHADER_RESOURCE_VIEW_DESC view{};
    view.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;
    view.Texture2D.MipLevels=1;
    view.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
    check_hresult(device_->CreateShaderResourceView(source.texture.get(),&view,source.srv.put()));
    view.Format=DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    com_ptr<ID3D11ShaderResourceView> srgb;
    check_hresult(device_->CreateShaderResourceView(source.texture.get(),&view,srgb.put()));
    source_=std::move(source);
    sourceSrgb_=std::move(srgb);
    sourceWidth_ = width; sourceHeight_ = height;
    horizontal_ = {}; output_ = {};
}
void Renderer::EnsurePipeline() {
    if (!horizontal_.texture) horizontal_ = MakeSurface(outputWidth_,sourceHeight_,DXGI_FORMAT_R16G16B16A16_FLOAT,true);
    if (!output_.texture) output_ = MakeSurface(outputWidth_,outputHeight_,DXGI_FORMAT_R16G16B16A16_FLOAT,true);
}
void Renderer::ResetSource() {
    source_ = {}; sourceSrgb_ = nullptr; horizontal_ = {}; output_ = {};
    sourceWidth_ = sourceHeight_ = 0;
    lastGpuMs_ = -1;
}
void Renderer::Upload(unsigned width, unsigned height, const std::vector<unsigned char>& bgra) {
    if (bgra.size() != static_cast<size_t>(width)*height*4) throw std::invalid_argument("Invalid image bytes");
    EnsureSource(width,height);
    context_->UpdateSubresource(source_.texture.get(),0,nullptr,bgra.data(),width*4,0);
}
void Renderer::CopyFrame(ID3D11Texture2D* frame, const RECT& crop) {
    D3D11_TEXTURE2D_DESC desc{}; frame->GetDesc(&desc);
    if (crop.left < 0 || crop.top < 0 || crop.right <= crop.left || crop.bottom <= crop.top ||
        crop.right > static_cast<LONG>(desc.Width) || crop.bottom > static_cast<LONG>(desc.Height))
        throw std::invalid_argument("Capture crop is outside the texture");
    EnsureSource(static_cast<unsigned>(crop.right-crop.left),static_cast<unsigned>(crop.bottom-crop.top));
    D3D11_BOX box{static_cast<UINT>(crop.left),static_cast<UINT>(crop.top),0,
        static_cast<UINT>(crop.right),static_cast<UINT>(crop.bottom),1};
    // One GPU-to-GPU copy makes the source lifetime independent of WGC's pool.
    context_->CopySubresourceRegion(source_.texture.get(),0,0,0,0,frame,0,&box);
}
void Renderer::Pass(ID3D11RenderTargetView* target, unsigned width, unsigned height,
    ID3D11PixelShader* shader, const Constants& settings, ID3D11ShaderResourceView* input,
    ID3D11ShaderResourceView* original, int left, int top) {
    context_->UpdateSubresource(constants_.get(),0,nullptr,&settings,0,0);
    ID3D11Buffer* buffers[] = {constants_.get()};
    ID3D11ShaderResourceView* images[] = {input,original};
    ID3D11SamplerState* samplers[] = {sampler_.get()};
    D3D11_VIEWPORT viewport{static_cast<float>(left),static_cast<float>(top),static_cast<float>(width),static_cast<float>(height),0,1};
    context_->OMSetRenderTargets(1,&target,nullptr);
    context_->OMSetBlendState(nullptr,nullptr,0xffffffff);
    context_->OMSetDepthStencilState(nullptr,0);
    context_->RSSetState(rasterizer_.get());
    context_->RSSetViewports(1,&viewport);
    context_->IASetInputLayout(nullptr);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vertex_.get(),nullptr,0);
    context_->PSSetShader(shader,nullptr,0);
    context_->PSSetConstantBuffers(0,1,buffers);
    context_->PSSetSamplers(0,1,samplers);
    context_->PSSetShaderResources(0,2,images);
    context_->Draw(3,0);
    ID3D11ShaderResourceView* empty[] = {nullptr,nullptr};
    context_->PSSetShaderResources(0,2,empty);
    context_->OMSetRenderTargets(0,nullptr,nullptr);
}
void Renderer::PollTiming() {
    for (auto& timing : timings_) {
        if (!timing.pending) continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT data{};
        UINT64 begin{},end{};
        if (context_->GetData(timing.disjoint.get(),&data,sizeof(data),D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            context_->GetData(timing.begin.get(),&begin,sizeof(begin),D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            context_->GetData(timing.end.get(),&end,sizeof(end),D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        if (!data.Disjoint && data.Frequency) lastGpuMs_ = static_cast<double>(end-begin)*1000.0/static_cast<double>(data.Frequency);
        timing.pending = false;
    }
}
void Renderer::Process(const RenderOptions& options) {
    if (!HasSource()) return;
    EnsurePipeline();
    PollTiming();
    auto& timing = timings_[timingIndex_++ % timings_.size()];
    const bool measure = !timing.pending;
    if (measure) { context_->Begin(timing.disjoint.get()); context_->End(timing.begin.get()); }
    Constants settings{};
    settings.source[0] = static_cast<float>(sourceWidth_); settings.source[1] = static_cast<float>(sourceHeight_);
    settings.target[0] = static_cast<float>(outputWidth_); settings.target[1] = static_cast<float>(sourceHeight_);
    settings.filter = static_cast<unsigned>(options.filter); settings.linearLight = options.linearLight;
    settings.horizontal = 1;
    const bool sampleSrgb=options.linearLight && sourceWidth_==2*outputWidth_ &&
        (options.filter==Filter::Lanczos || options.filter==Filter::LanczosGuarded);
    if (sampleSrgb) settings.linearLight=0;
    Pass(horizontal_.rtv.get(),outputWidth_,sourceHeight_,filter_.get(),settings,
        sampleSrgb ? sourceSrgb_.get() : source_.srv.get());
    settings.source[0] = static_cast<float>(outputWidth_);
    settings.target[1] = static_cast<float>(outputHeight_);
    settings.horizontal = 0;
    Pass(output_.rtv.get(),outputWidth_,outputHeight_,filter_.get(),settings,horizontal_.srv.get());
    if (measure) { context_->End(timing.end.get()); context_->End(timing.disjoint.get()); timing.pending = true; }
}
Renderer::Constants Renderer::PresentationConstants(unsigned width, unsigned height, const RenderOptions& options) const {
    Constants settings{};
    settings.source[0] = static_cast<float>(outputWidth_); settings.source[1] = static_cast<float>(outputHeight_);
    settings.target[0] = static_cast<float>(width); settings.target[1] = static_cast<float>(height);
    settings.linearLight = options.linearLight; settings.comparison = options.comparison;
    settings.sharpness = options.sharpness;
    return settings;
}
void Renderer::EnsureDisplay(DisplaySurface& display) {
    RECT rect{}; GetClientRect(display.hwnd,&rect);
    unsigned width = static_cast<unsigned>(std::max(1L,rect.right));
    unsigned height = static_cast<unsigned>(std::max(1L,rect.bottom));
    const UINT flags=display.lowLatency ? DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT : 0;
    if (!display.swapchain) {
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = width; desc.Height = height; desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1; desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2; desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.Scaling = DXGI_SCALING_STRETCH; desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        desc.Flags=flags;
        check_hresult(factory_->CreateSwapChainForHwnd(device_.get(),display.hwnd,&desc,nullptr,nullptr,display.swapchain.put()));
        check_hresult(factory_->MakeWindowAssociation(display.hwnd,DXGI_MWA_NO_ALT_ENTER));
        if (display.lowLatency) {
            auto pacing=display.swapchain.as<IDXGISwapChain2>();
            check_hresult(pacing->SetMaximumFrameLatency(1));
            display.frameLatency.attach(pacing->GetFrameLatencyWaitableObject());
            if (!display.frameLatency) throw winrt::hresult_error(E_FAIL,L"DXGI sunum zamanlayıcısı açılamadı.");
        }
    } else if (display.width != width || display.height != height) {
        display.rtv = nullptr;
        check_hresult(display.swapchain->ResizeBuffers(0,width,height,DXGI_FORMAT_UNKNOWN,flags));
    }
    if (!display.rtv) {
        com_ptr<ID3D11Texture2D> buffer;
        check_hresult(display.swapchain->GetBuffer(0,__uuidof(ID3D11Texture2D),buffer.put_void()));
        check_hresult(device_->CreateRenderTargetView(buffer.get(),nullptr,display.rtv.put()));
    }
    display.width = width; display.height = height;
}
void Renderer::PrepareDisplay(DisplaySurface& display) {
    EnsureDisplay(display);
}
bool Renderer::Present(DisplaySurface& display, const RenderOptions& options, bool vsync) {
    if (!output_.texture || !IsWindow(display.hwnd)) return false;
    if (display.lowLatency && !display.frameSlotReady) return false;
    EnsureDisplay(display);
    double scale = std::min(static_cast<double>(display.width)/outputWidth_,static_cast<double>(display.height)/outputHeight_);
    auto width = static_cast<unsigned>(std::max(1.0,std::round(outputWidth_*scale)));
    auto height = static_cast<unsigned>(std::max(1.0,std::round(outputHeight_*scale)));
    if (width!=display.width || height!=display.height) {
        const float black[] = {0,0,0,1};
        context_->ClearRenderTargetView(display.rtv.get(),black);
    }
    auto settings = PresentationConstants(width,height,options);
    Pass(display.rtv.get(),width,height,present_.get(),settings,output_.srv.get(),source_.srv.get(),
        static_cast<int>((display.width-width)/2),static_cast<int>((display.height-height)/2));
    const HRESULT result=display.swapchain->Present(vsync ? 1 : 0,DXGI_PRESENT_DO_NOT_WAIT);
    if (result==DXGI_ERROR_WAS_STILL_DRAWING || result==DXGI_STATUS_OCCLUDED) return false;
    check_hresult(result);
    display.frameSlotReady=false;
    SetPropW(display.hwnd,L"Upscalp.Presented",reinterpret_cast<HANDLE>(1));
    return true;
}
void Renderer::DrawPattern(DisplaySurface& display, float seconds) {
    EnsureDisplay(display);
    Constants settings{};
    settings.target[0] = static_cast<float>(display.width); settings.target[1] = static_cast<float>(display.height);
    settings.time = seconds;
    Pass(display.rtv.get(),display.width,display.height,pattern_.get(),settings,nullptr);
    const HRESULT result=display.swapchain->Present(1,DXGI_PRESENT_DO_NOT_WAIT);
    if (result==DXGI_ERROR_WAS_STILL_DRAWING || result==DXGI_STATUS_OCCLUDED) return;
    check_hresult(result);
    SetPropW(display.hwnd,L"Upscalp.Presented",reinterpret_cast<HANDLE>(1));
}
std::vector<unsigned char> Renderer::ReadOutput(const RenderOptions& options) {
    if (!output_.texture) throw std::runtime_error("No processed frame");
    auto encoded = MakeSurface(outputWidth_,outputHeight_,DXGI_FORMAT_B8G8R8A8_UNORM,true);
    auto settings = PresentationConstants(outputWidth_,outputHeight_,options);
    Pass(encoded.rtv.get(),outputWidth_,outputHeight_,present_.get(),settings,output_.srv.get(),source_.srv.get());
    D3D11_TEXTURE2D_DESC desc{}; encoded.texture->GetDesc(&desc);
    desc.BindFlags = 0; desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    com_ptr<ID3D11Texture2D> staging;
    check_hresult(device_->CreateTexture2D(&desc,nullptr,staging.put()));
    context_->CopyResource(staging.get(),encoded.texture.get());
    D3D11_MAPPED_SUBRESOURCE mapping{};
    check_hresult(context_->Map(staging.get(),0,D3D11_MAP_READ,0,&mapping));
    std::vector<unsigned char> pixels(static_cast<size_t>(outputWidth_)*outputHeight_*4);
    for (unsigned y=0;y<outputHeight_;++y) std::memcpy(pixels.data()+static_cast<size_t>(y)*outputWidth_*4,
        static_cast<const unsigned char*>(mapping.pData)+static_cast<size_t>(y)*mapping.RowPitch,outputWidth_*4);
    context_->Unmap(staging.get(),0);
    return pixels;
}
void Renderer::SaveOutput(const std::wstring& path, const RenderOptions& options) {
    auto pixels = ReadOutput(options);
    com_ptr<IWICImagingFactory> wic;
    check_hresult(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,__uuidof(IWICImagingFactory),wic.put_void()));
    com_ptr<IWICStream> stream;
    check_hresult(wic->CreateStream(stream.put()));
    check_hresult(stream->InitializeFromFilename(path.c_str(),GENERIC_WRITE));
    com_ptr<IWICBitmapEncoder> encoder;
    check_hresult(wic->CreateEncoder(GUID_ContainerFormatPng,nullptr,encoder.put()));
    check_hresult(encoder->Initialize(stream.get(),WICBitmapEncoderNoCache));
    com_ptr<IWICBitmapFrameEncode> frame;
    check_hresult(encoder->CreateNewFrame(frame.put(),nullptr));
    check_hresult(frame->Initialize(nullptr));
    check_hresult(frame->SetSize(outputWidth_,outputHeight_));
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    check_hresult(frame->SetPixelFormat(&format));
    if (!IsEqualGUID(format,GUID_WICPixelFormat32bppBGRA)) throw std::runtime_error("PNG encoder rejected BGRA");
    check_hresult(frame->WritePixels(outputHeight_,outputWidth_*4,static_cast<UINT>(pixels.size()),pixels.data()));
    check_hresult(frame->Commit()); check_hresult(encoder->Commit());
}
