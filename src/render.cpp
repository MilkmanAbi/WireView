#include "render.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>

#include "cursor.h"
#include "util.h"

namespace wv {

namespace {

// One triangle that covers the viewport; no vertex buffer needed.
const char kShader[] = R"(
Texture2D tex : register(t0);
SamplerState smp : register(s0);
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut vs(uint id : SV_VertexID) {
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv;
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float4 ps_opaque(VSOut i) : SV_Target { return float4(tex.Sample(smp, i.uv).rgb, 1); }
float4 ps_alpha(VSOut i) : SV_Target { return tex.Sample(smp, i.uv); }
)";

ComPtr<ID3DBlob> compile(const char* entry, const char* target, std::string& err) {
    ComPtr<ID3DBlob> code, errors;
    HRESULT hr = D3DCompile(kShader, sizeof kShader - 1, "wireview", nullptr, nullptr, entry, target,
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr)) {
        err = std::string("Shader compile failed: ") +
              (errors ? static_cast<const char*>(errors->GetBufferPointer()) : hresult_text(hr));
        return nullptr;
    }
    return code;
}

}  // namespace

RECT fit_rect(uint32_t w, uint32_t h, int cw, int ch) {
    if (!w || !h || cw <= 0 || ch <= 0) return RECT{0, 0, 0, 0};
    const double s = std::min(double(cw) / w, double(ch) / h);
    const int dw = std::max(1, int(std::lround(w * s))), dh = std::max(1, int(std::lround(h * s)));
    const int x = (cw - dw) / 2, y = (ch - dh) / 2;
    return RECT{x, y, x + dw, y + dh};
}

// ---------------------------------------------------------------- GpuImage

bool GpuImage::set_mode(uint32_t width, uint32_t height, std::string& err) {
    if (width == width_ && height == height_ && tex_) return true;
    reset();
    D3D11_TEXTURE2D_DESC td{};
    td.Width = width;
    td.Height = height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    HRESULT hr = device_->CreateTexture2D(&td, nullptr, &tex_);
    if (SUCCEEDED(hr)) {
        td.BindFlags = 0;
        hr = device_->CreateTexture2D(&td, nullptr, &temp_);
    }
    if (SUCCEEDED(hr)) hr = device_->CreateShaderResourceView(tex_.Get(), nullptr, &srv_);
    if (FAILED(hr)) {
        err = "Cannot create a " + std::to_string(width) + "x" + std::to_string(height) + " texture " + hresult_text(hr);
        reset();
        return false;
    }
    width_ = width;
    height_ = height;
    return true;
}

void GpuImage::apply(const FrameView& f) {
    if (!tex_) return;
    // Moves first, in order, each through a temp texture (source and destination may overlap).
    for (uint32_t i = 0; i < f.moveCount; ++i) {
        const MoveRect& m = f.moves[i];
        const UINT w = UINT(m.dst.right - m.dst.left), h = UINT(m.dst.bottom - m.dst.top);
        D3D11_BOX src{UINT(m.srcX), UINT(m.srcY), 0, UINT(m.srcX) + w, UINT(m.srcY) + h, 1};
        D3D11_BOX dst{UINT(m.dst.left), UINT(m.dst.top), 0, UINT(m.dst.right), UINT(m.dst.bottom), 1};
        ctx_->CopySubresourceRegion(temp_.Get(), 0, dst.left, dst.top, 0, tex_.Get(), 0, &src);
        ctx_->CopySubresourceRegion(tex_.Get(), 0, dst.left, dst.top, 0, temp_.Get(), 0, &dst);
    }
    const uint8_t* p = f.pixels;
    for (uint32_t i = 0; i < f.rectCount; ++i) {
        const Rect& r = f.rects[i];
        const UINT pitch = UINT(r.right - r.left) * 4;
        D3D11_BOX b{UINT(r.left), UINT(r.top), 0, UINT(r.right), UINT(r.bottom), 1};
        ctx_->UpdateSubresource(tex_.Get(), 0, &b, p, pitch, 0);
        p += size_t(pitch) * UINT(r.bottom - r.top);
    }
}

bool GpuImage::read_back(std::vector<uint8_t>& bgra) {
    if (!tex_) return false;
    D3D11_TEXTURE2D_DESC td{};
    tex_->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> st;
    if (FAILED(device_->CreateTexture2D(&td, nullptr, &st))) return false;
    ctx_->CopyResource(st.Get(), tex_.Get());
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx_->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
    bgra.resize(size_t(width_) * height_ * 4);
    for (uint32_t y = 0; y < height_; ++y)
        memcpy(&bgra[size_t(y) * width_ * 4], static_cast<uint8_t*>(m.pData) + size_t(y) * m.RowPitch, size_t(width_) * 4);
    ctx_->Unmap(st.Get(), 0);
    return true;
}

// ---------------------------------------------------------------- Renderer

bool Renderer::init(HWND hwnd, std::string& err) {
    shutdown();
    hwnd_ = hwnd;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                   &device_, nullptr, &ctx_);
    if (FAILED(hr))
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device_,
                               nullptr, &ctx_);
    if (FAILED(hr)) {
        err = "D3D11CreateDevice failed " + hresult_text(hr);
        return false;
    }
    ComPtr<IDXGIDevice1> dxgiDevice;
    device_.As(&dxgiDevice);
    dxgiDevice->SetMaximumFrameLatency(1);  // never queue frames: show the newest one
    ComPtr<IDXGIAdapter> adapter;
    dxgiDevice->GetAdapter(&adapter);
    ComPtr<IDXGIFactory2> factory;
    adapter->GetParent(IID_PPV_ARGS(&factory));

    RECT rc;
    GetClientRect(hwnd, &rc);
    bufW_ = UINT(std::max<LONG>(rc.right, 8));
    bufH_ = UINT(std::max<LONG>(rc.bottom, 8));
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = bufW_;
    sd.Height = bufH_;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.Scaling = DXGI_SCALING_NONE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    hr = factory->CreateSwapChainForHwnd(device_.Get(), hwnd, &sd, nullptr, nullptr, &swap_);
    if (FAILED(hr)) {
        sd.Scaling = DXGI_SCALING_STRETCH;
        hr = factory->CreateSwapChainForHwnd(device_.Get(), hwnd, &sd, nullptr, nullptr, &swap_);
    }
    if (FAILED(hr)) {
        err = "CreateSwapChainForHwnd failed " + hresult_text(hr);
        shutdown();
        return false;
    }
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

    auto vsCode = compile("vs", "vs_4_0", err);
    auto ps1 = vsCode ? compile("ps_opaque", "ps_4_0", err) : nullptr;
    auto ps2 = ps1 ? compile("ps_alpha", "ps_4_0", err) : nullptr;
    if (!ps2) {
        shutdown();
        return false;
    }
    device_->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs_);
    device_->CreatePixelShader(ps1->GetBufferPointer(), ps1->GetBufferSize(), nullptr, &psOpaque_);
    device_->CreatePixelShader(ps2->GetBufferPointer(), ps2->GetBufferSize(), nullptr, &psAlpha_);

    D3D11_SAMPLER_DESC smp{};
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    smp.MaxLOD = D3D11_FLOAT32_MAX;
    device_->CreateSamplerState(&smp, &linear_);
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    device_->CreateSamplerState(&smp, &point_);

    D3D11_BLEND_DESC bd{};
    auto& rt = bd.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D11_BLEND_SRC_ALPHA;
    rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ONE;
    rt.DestBlendAlpha = D3D11_BLEND_ZERO;
    rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    device_->CreateBlendState(&bd, &blend_);

    image_.init(device_.Get(), ctx_.Get());
    return create_targets();
}

void Renderer::shutdown() {
    if (ctx_) ctx_->ClearState();
    image_.reset();
    ptrSrv_.Reset();
    ptrTex_.Reset();
    blend_.Reset();
    point_.Reset();
    linear_.Reset();
    psAlpha_.Reset();
    psOpaque_.Reset();
    vs_.Reset();
    rtv_.Reset();
    swap_.Reset();
    ctx_.Reset();
    device_.Reset();
}

bool Renderer::create_targets() {
    ComPtr<ID3D11Texture2D> back;
    if (FAILED(swap_->GetBuffer(0, IID_PPV_ARGS(&back)))) return false;
    return SUCCEEDED(device_->CreateRenderTargetView(back.Get(), nullptr, &rtv_));
}

void Renderer::resize() {
    if (!swap_) return;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    const UINT w = UINT(std::max<LONG>(rc.right, 8)), h = UINT(std::max<LONG>(rc.bottom, 8));
    if (w == bufW_ && h == bufH_) return;
    ctx_->OMSetRenderTargets(0, nullptr, nullptr);
    rtv_.Reset();
    if (SUCCEEDED(swap_->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0))) {
        bufW_ = w;
        bufH_ = h;
    }
    create_targets();
}

void Renderer::set_pointer_shape(const PointerShapeMsg& h, const uint8_t* data) {
    ptrSrv_.Reset();
    ptrTex_.Reset();
    ptrW_ = ptrH_ = 0;
    if (!device_) return;
    uint32_t w = 0, hh = 0;
    std::vector<uint8_t> bgra = pointer_to_bgra(h, data, w, hh);
    if (bgra.empty()) return;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = hh;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{bgra.data(), w * 4, 0};
    if (FAILED(device_->CreateTexture2D(&td, &sd, &ptrTex_))) return;
    if (FAILED(device_->CreateShaderResourceView(ptrTex_.Get(), nullptr, &ptrSrv_))) return;
    ptrW_ = w;
    ptrH_ = hh;
}

bool Renderer::draw() {
    if (!swap_ || !rtv_) return true;
    const float bg[4] = {GetRValue(background_) / 255.f, GetGValue(background_) / 255.f,
                         GetBValue(background_) / 255.f, 1.f};
    ctx_->ClearRenderTargetView(rtv_.Get(), bg);
    imageRect_ = fit_rect(image_.width(), image_.height(), int(bufW_), int(bufH_));

    if (image_.srv() && imageRect_.right > imageRect_.left) {
        ID3D11RenderTargetView* rtv = rtv_.Get();
        ctx_->OMSetRenderTargets(1, &rtv, nullptr);
        ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx_->IASetInputLayout(nullptr);
        ctx_->VSSetShader(vs_.Get(), nullptr, 0);

        const float scale = float(imageRect_.right - imageRect_.left) / float(image_.width());
        const bool exact = (imageRect_.right - imageRect_.left) == LONG(image_.width());
        D3D11_VIEWPORT vp{float(imageRect_.left), float(imageRect_.top), float(imageRect_.right - imageRect_.left),
                          float(imageRect_.bottom - imageRect_.top), 0, 1};
        ctx_->RSSetViewports(1, &vp);
        ID3D11SamplerState* smp = exact ? point_.Get() : linear_.Get();
        ctx_->PSSetSamplers(0, 1, &smp);
        ID3D11ShaderResourceView* srv = image_.srv();
        ctx_->PSSetShaderResources(0, 1, &srv);
        ctx_->PSSetShader(psOpaque_.Get(), nullptr, 0);
        ctx_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
        ctx_->Draw(3, 0);

        if (drawPointer_ && ptrVisible_ && ptrSrv_) {
            const float ps = scale * ptrScale_;
            D3D11_VIEWPORT pv{imageRect_.left + ptrX_ * ps, imageRect_.top + ptrY_ * ps, ptrW_ * ps, ptrH_ * ps, 0, 1};
            ctx_->RSSetViewports(1, &pv);
            srv = ptrSrv_.Get();
            ctx_->PSSetShaderResources(0, 1, &srv);
            ctx_->PSSetShader(psAlpha_.Get(), nullptr, 0);
            ctx_->OMSetBlendState(blend_.Get(), nullptr, 0xFFFFFFFF);
            ctx_->Draw(3, 0);
        }
    }
    const HRESULT hr = swap_->Present(0, 0);
    return hr != DXGI_ERROR_DEVICE_REMOVED && hr != DXGI_ERROR_DEVICE_RESET;
}

}  // namespace wv
