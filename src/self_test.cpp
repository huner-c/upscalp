#include "renderer.h"
#include "displays.h"
#include "input_bridge.h"
#include <filesystem>
#include <fstream>
#include <functional>
#include <cmath>
#include <sstream>

namespace {
std::string Escape(const std::string& value) {
    std::string out;
    for (char c : value) {
        if (c == '\\' || c == '"') { out += '\\'; out += c; }
        else if (c == '\n' || c == '\r') out += ' ';
        else if (static_cast<unsigned char>(c) >= 32) out += c;
    }
    return out;
}
std::vector<unsigned char> Solid(unsigned width,unsigned height,unsigned char value) {
    std::vector<unsigned char> pixels(static_cast<size_t>(width)*height*4,value);
    for (size_t i=3;i<pixels.size();i+=4) pixels[i]=255;
    return pixels;
}
void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void Near(const std::vector<unsigned char>& pixels,int expected,int tolerance=1) {
    for (size_t i=0;i<pixels.size();++i) if (i%4 != 3)
        Require(std::abs(static_cast<int>(pixels[i])-expected)<=tolerance,"Pixel mismatch");
}
}
int RunSelfTests(const std::wstring& reportPath) {
    unsigned passed=0,failed=0;
    std::ostringstream entries;
    auto test = [&](const std::string& name, const std::function<void()>& run) {
        if (passed+failed) entries << ",\n";
        entries << "    {\"name\":\"" << Escape(name) << "\",\"passed\":";
        try { run(); ++passed; entries << "true}"; }
        catch (const winrt::hresult_error& error) { ++failed; entries << "false,\"error\":\"" << Escape(winrt::to_string(error.message())) << "\"}"; }
        catch (const std::exception& error) { ++failed; entries << "false,\"error\":\"" << Escape(error.what()) << "\"}"; }
    };
    Renderer renderer;
    test("Mouse mapping doubles 1080p coordinates into the 4K game client",[&] {
        POINT mapped{};
        Require(MapOutputPoint(POINT{640,360},SIZE{1920,1080},SIZE{3840,2160},mapped),"Visible image rejected");
        Require(mapped.x==1280 && mapped.y==720,"Mouse point was not scaled into the source");
        Require(MapOutputPoint(POINT{1919,1079},SIZE{1920,1080},SIZE{3840,2160},mapped),"Last output pixel rejected");
        Require(mapped.x<3840 && mapped.y<2160,"Mouse point escaped the source");
    });
    test("Mouse mapping rejects letterboxing and handles scaled output",[&] {
        POINT mapped{};
        Require(!MapOutputPoint(POINT{500,20},SIZE{1000,1000},SIZE{3840,2160},mapped),"Letterbox accepted a click");
        Require(MapOutputPoint(POINT{500,500},SIZE{1000,1000},SIZE{3840,2160},mapped),"Image center rejected");
        Require(std::abs(mapped.x-1920)<=4 && std::abs(mapped.y-1080)<=4,"Letterboxed center mapping is wrong");
    });
    test("Large source never intersects side-by-side, stacked, or negative-coordinate displays",[&] {
        const std::vector<std::vector<RECT>> layouts={
            {{-1920,0,0,1080},{0,0,1920,1080},{1920,0,4480,1440}},
            {{0,-1440,2560,0},{0,0,1920,1080},{-1080,0,0,1920}},
            {{-3840,-2160,0,0},{0,-1080,1920,0},{1920,0,3840,1080}}
        };
        for (const auto& layout : layouts) {
            auto parked=ParkOutsideDisplays(layout,3856,2199,layout.front().left);
            Require(!IntersectsAnyDisplay(parked,layout),"Large source spills onto a physical display");
            Require(parked.right-parked.left==3856 && parked.bottom-parked.top==2199,"Parking changed source dimensions");
        }
    });
    test("Source covering a neighbor is detected even when outside the selected TV",[&] {
        std::vector<RECT> layout={{0,0,1920,1080},{1920,0,3840,1080},{0,-1440,2560,0}};
        Require(IntersectsAnyDisplay(RECT{1920,0,5760,2160},layout),"Neighbor overlap was missed");
        Require(IntersectsAnyDisplay(RECT{0,-1440,3840,720},layout),"Upper display overlap was missed");
    });
    bool initialized=false;
    test("Compile all shaders and create D3D11 WARP device",[&] { renderer.Initialize(true); initialized=true; });
    if (initialized) {
        for (unsigned filter=0;filter<=static_cast<unsigned>(Filter::LanczosGuarded);++filter) for (bool linear : {false,true}) {
            test("Constant preservation, filter="+std::to_string(filter)+", linear="+std::to_string(linear),[&] {
                renderer.SetOutputSize(5,3);
                renderer.Upload(12,9,Solid(12,9,127));
                RenderOptions options{static_cast<Filter>(filter),linear,false,0};
                renderer.Process(options); Near(renderer.ReadOutput(options),127);
            });
        }
        test("2x Lanczos pairs match independent linear-light convolution", [&] {
            constexpr unsigned width=24,height=8;
            const auto sinc=[](double x) { return std::abs(x)<1e-8 ? 1.0 : std::sin(3.141592653589793*x)/(3.141592653589793*x); };
            const auto decode=[](double c) { return c<=.04045 ? c/12.92 : std::pow((c+.055)/1.055,2.4); };
            const auto encode=[](double c) {
                c=std::clamp(c,0.0,1.0);
                return c<=.0031308 ? 12.92*c : 1.055*std::pow(c,1/2.4)-.055;
            };
            auto pixels=Solid(width,height,0);
            for (unsigned y=0;y<height;++y) for (unsigned x=0;x<width;++x)
                for (unsigned c=0;c<3;++c) pixels[(y*width+x)*4+c]=static_cast<unsigned char>((x*37+c*43+11)%256);
            renderer.Upload(width,height,pixels);
            renderer.SetOutputSize(width/2,height/2);
            RenderOptions options{Filter::Lanczos,true,false,0};
            renderer.Process(options);
            auto output=renderer.ReadOutput(options);
            for (unsigned x=0;x<width/2;++x) for (unsigned c=0;c<3;++c) {
                const double center=2*x+.5;
                double weighted=0, total=0;
                for (int sample=static_cast<int>(std::ceil(center-4));sample<=center+4;++sample) {
                    const double d=(sample-center)/2;
                    const double weight=sinc(d)*sinc(d/2);
                    const int ix=std::clamp(sample,0,static_cast<int>(width)-1);
                    weighted+=weight*decode(pixels[ix*4+c]/255.0);
                    total+=weight;
                }
                const int expected=static_cast<int>(std::round(255*encode(weighted/total)));
                Require(std::abs(static_cast<int>(output[x*4+c])-expected)<=2,"2:1 sampler reconstruction differs from the reference");
            }
        });
        test("Guarded Lanczos3 keeps flat regions clean next to a high-contrast edge", [&] {
            constexpr unsigned width=32,height=8;
            auto pixels=Solid(width,height,32);
            for (unsigned y=0;y<height;++y) for (unsigned x=15;x<width;++x)
                for (unsigned c=0;c<3;++c) pixels[(y*width+x)*4+c]=220;
            renderer.Upload(width,height,pixels);
            renderer.SetOutputSize(width/2,height/2);
            RenderOptions options{Filter::LanczosGuarded,true,false,0};
            renderer.Process(options);
            auto output=renderer.ReadOutput(options);
            for (unsigned x=0;x<width/2;++x) {
                const auto pixel=output[x*4];
                if (x<7) Require(std::abs(pixel-32)<=1,"Dark side has a ringing halo");
                if (x>7) Require(std::abs(pixel-220)<=1,"Bright side has a ringing halo");
                if (x==7) Require(pixel>32 && pixel<220,"Mixed coverage on the edge was discarded");
            }
        });
        for (bool linear : {false,true}) {
            test(linear ? "2x checker average in linear light is sRGB 188" : "2x checker average in encoded space is 128",[&] {
                auto pixels = Solid(8,8,0);
                for (unsigned y=0;y<8;++y) for (unsigned x=0;x<8;++x)
                    for (unsigned c=0;c<3;++c) pixels[(y*8+x)*4+c] = (x+y)%2 ? 255 : 0;
                renderer.SetOutputSize(4,4); renderer.Upload(8,8,pixels);
                RenderOptions options{Filter::Area,linear,false,0};
                renderer.Process(options); Near(renderer.ReadOutput(options),linear ? 188 : 128);
            });
        }
        test("Fractional area reduction matches independent pixel-overlap reference",[&] {
            constexpr unsigned sw=13,sh=11,dw=5,dh=4;
            auto pixels = Solid(sw,sh,0);
            for (unsigned y=0;y<sh;++y) for (unsigned x=0;x<sw;++x) for (unsigned c=0;c<3;++c)
                pixels[(y*sw+x)*4+c]=static_cast<unsigned char>((x*37+y*53+c*19)%256);
            renderer.Upload(sw,sh,pixels); renderer.SetOutputSize(dw,dh);
            RenderOptions options{Filter::Area,false,false,0};
            renderer.Process(options); auto result=renderer.ReadOutput(options);
            for (unsigned dy=0;dy<dh;++dy) for (unsigned dx=0;dx<dw;++dx) for (unsigned c=0;c<3;++c) {
                const double x0=static_cast<double>(dx)*sw/dw,x1=static_cast<double>(dx+1)*sw/dw;
                const double y0=static_cast<double>(dy)*sh/dh,y1=static_cast<double>(dy+1)*sh/dh;
                double sum=0;
                for (unsigned sy=0;sy<sh;++sy) for (unsigned sx=0;sx<sw;++sx) {
                    double overlapX=std::max(0.0,std::min(x1,static_cast<double>(sx+1))-std::max(x0,static_cast<double>(sx)));
                    double overlapY=std::max(0.0,std::min(y1,static_cast<double>(sy+1))-std::max(y0,static_cast<double>(sy)));
                    sum += pixels[(sy*sw+sx)*4+c]*overlapX*overlapY;
                }
                int expected=static_cast<int>(std::round(sum/((x1-x0)*(y1-y0))));
                Require(std::abs(static_cast<int>(result[(dy*dw+dx)*4+c])-expected)<=1,"Fractional area reference mismatch");
            }
        });
        test("GPU copy respects source crop and BGRA channel order",[&] {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width=8; desc.Height=8; desc.MipLevels=1; desc.ArraySize=1;
            desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count=1;
            desc.Usage=D3D11_USAGE_IMMUTABLE; desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
            auto pixels=Solid(8,8,0);
            for (unsigned y=2;y<6;++y) for (unsigned x=2;x<6;++x) {
                pixels[(y*8+x)*4]=31; pixels[(y*8+x)*4+1]=95; pixels[(y*8+x)*4+2]=191;
            }
            D3D11_SUBRESOURCE_DATA data{pixels.data(),32,0};
            com_ptr<ID3D11Texture2D> source;
            winrt::check_hresult(renderer.Device()->CreateTexture2D(&desc,&data,source.put()));
            renderer.CopyFrame(source.get(),RECT{2,2,6,6}); renderer.SetOutputSize(2,2);
            RenderOptions options{Filter::Area,false,false,0}; renderer.Process(options);
            auto result=renderer.ReadOutput(options);
            Require(result.size()==16,"Output resize failed");
            for (size_t i=0;i<result.size();i+=4)
                Require(result[i]==31 && result[i+1]==95 && result[i+2]==191 && result[i+3]==255,"Cropped color mismatch");
        });
        test("Sharpening preserves a flat field",[&] {
            renderer.Upload(8,8,Solid(8,8,73)); renderer.SetOutputSize(4,4);
            RenderOptions options{Filter::Lanczos,true,false,1}; renderer.Process(options);
            Near(renderer.ReadOutput(options),73);
        });
        test("Reset releases captured source",[&] { renderer.ResetSource(); Require(!renderer.HasSource(),"Source survived reset"); });
    }
    std::ofstream report(std::filesystem::path(reportPath),std::ios::binary);
    report << "{\n  \"suite\":\"D3D11 shader pipeline (WARP)\",\n  \"passed\":" << passed << ",\n  \"failed\":" << failed << ",\n  \"tests\":[\n" << entries.str() << "\n  ]\n}\n";
    return failed || !report ? 1 : 0;
}
