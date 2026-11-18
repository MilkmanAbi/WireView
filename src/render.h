// Viewer-side drawing with Direct3D 11.
#pragma once
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <vector>

#include "protocol.h"

namespace wv {

using Microsoft::WRL::ComPtr;

// The remote screen as a GPU texture. Frames are applied in place: moved
// areas are copied GPU-side, changed rectangles are uploaded as-is.
class GpuImage {
public:
    void init(ID3D11Device* device, ID3D11DeviceContext* ctx) {
        device_ = device;
        ctx_ = ctx;
    }
    bool set_mode(uint32_t width, uint32_t height, std::string& err);
    void apply(const FrameView& f);
    bool read_back(std::vector<uint8_t>& bgra);  // for tests
    ID3D11ShaderResourceView* srv() const { return srv_.Get(); }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    void reset() {
        tex_.Reset();
        temp_.Reset();
        srv_.Reset();
        width_ = height_ = 0;
    }

private:
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* ctx_ = nullptr;
    ComPtr<ID3D11Texture2D> tex_, temp_;
    ComPtr<ID3D11ShaderResourceView> srv_;
    uint32_t width_ = 0, height_ = 0;
};

class Renderer {
public:
    ~Renderer() { shutdown(); }
    bool init(HWND hwnd, std::string& err);
    void shutdown();
    bool ready() const { return swap_ != nullptr; }
    void resize();  // after the window changed size

    GpuImage& image() { return image_; }
    void set_pointer_shape(const PointerShapeMsg& h, const uint8_t* data);
    void set_pointer_pos(int32_t x, int32_t y, bool visible) {
        ptrX_ = x;
        ptrY_ = y;
        ptrVisible_ = visible;
    }
    void set_draw_pointer(bool draw) { drawPointer_ = draw; }
    void set_background(COLORREF c) { background_ = c; }

    // Draws and presents. Returns false if the GPU device was lost (call init again).
    bool draw();
    // Where the remote image sits in the window (client pixels), and its scale.
    RECT image_rect() const { return imageRect_; }

private:
    bool create_targets();

    HWND hwnd_ = nullptr;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<IDXGISwapChain1> swap_;
    ComPtr<ID3D11RenderTargetView> rtv_;
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> psOpaque_, psAlpha_;
    ComPtr<ID3D11SamplerState> linear_, point_;
    ComPtr<ID3D11BlendState> blend_;
    ComPtr<ID3D11Texture2D> ptrTex_;
    ComPtr<ID3D11ShaderResourceView> ptrSrv_;
    uint32_t ptrW_ = 0, ptrH_ = 0;
    int32_t ptrX_ = 0, ptrY_ = 0;
    bool ptrVisible_ = false, drawPointer_ = true;
    GpuImage image_;
    RECT imageRect_{};
    UINT bufW_ = 0, bufH_ = 0;
    COLORREF background_ = RGB(128, 128, 128);
};

// Letterboxed placement of a w x h image in a cw x ch area.
RECT fit_rect(uint32_t w, uint32_t h, int cw, int ch);

}  // namespace wv
