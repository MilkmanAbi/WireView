#include "capture.h"

#include <d3dcompiler.h>
#include <dxgi1_2.h>

#include <algorithm>
#include <cstdio>

#include "util.h"

namespace wv {

namespace {

struct FoundOutput {
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput> output;
    DXGI_OUTPUT_DESC desc{};
};

std::vector<FoundOutput> enum_outputs() {
    std::vector<FoundOutput> out;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return out;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        ComPtr<IDXGIOutput> output;
        for (UINT j = 0; adapter->EnumOutputs(j, &output) != DXGI_ERROR_NOT_FOUND; ++j) {
            FoundOutput f;
            output->GetDesc(&f.desc);
            if (f.desc.AttachedToDesktop) {
                f.adapter = adapter;
                f.output = output;
                out.push_back(std::move(f));
            }
            output.Reset();
        }
        adapter.Reset();
    }
    return out;
}

// Clamps r to the screen. Returns false if nothing is left.
bool clip(Rect& r, uint32_t w, uint32_t h) {
    r.left = std::max(r.left, 0);
    r.top = std::max(r.top, 0);
    r.right = std::min(r.right, int32_t(w));
    r.bottom = std::min(r.bottom, int32_t(h));
    return r.left < r.right && r.top < r.bottom;
}

// Box-filtered downscale: 16 bilinear taps spread over each output pixel's footprint.
const char kScaleShader[] = R"(
Texture2D src : register(t0);
SamplerState smp : register(s0);
cbuffer C : register(b0) { float2 footprint; float2 pad; };
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut vs(uint id : SV_VertexID) {
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv;
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float4 ps(VSOut i) : SV_Target {
    float3 acc = 0;
    [unroll] for (int y = 0; y < 4; ++y)
        [unroll] for (int x = 0; x < 4; ++x)
            acc += src.SampleLevel(smp, i.uv + ((float2(x, y) + 0.5) / 4 - 0.5) * footprint, 0).rgb;
    return float4(acc / 16, 1);
}
)";

}  // namespace

std::vector<MonitorInfo> list_monitors() {
    std::vector<MonitorInfo> list;
    for (const FoundOutput& f : enum_outputs()) {
        MonitorInfo m;
        m.desktop = f.desc.DesktopCoordinates;
        char size[48];
        std::snprintf(size, sizeof size, "  %ldx%ld", m.desktop.right - m.desktop.left, m.desktop.bottom - m.desktop.top);
        m.name = to_utf8(f.desc.DeviceName) + size;
        list.push_back(std::move(m));
    }
    return list;
}

bool Capture::open(int monitor, uint32_t maxHeight, std::string& err) {
    close();
    auto outputs = enum_outputs();
    if (outputs.empty()) {
        err = "No monitor found";
        return false;
    }
    if (monitor < 0 || monitor >= int(outputs.size())) monitor = 0;
    FoundOutput& f = outputs[size_t(monitor)];

    // The device must live on the adapter that owns the output (matters on hybrid-GPU laptops).
    HRESULT hr = D3D11CreateDevice(f.adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0,
                                   D3D11_SDK_VERSION, &device_, nullptr, &ctx_);
    if (FAILED(hr)) {
        err = "D3D11CreateDevice failed " + hresult_text(hr);
        return false;
    }
    ComPtr<IDXGIOutput1> out1;
    f.output.As(&out1);
    if (!out1) {
        err = "IDXGIOutput1 not available";
        close();
        return false;
    }
    hr = out1->DuplicateOutput(device_.Get(), &dup_);
    if (FAILED(hr)) {
        if (hr == E_ACCESSDENIED)
            err = "Screen capture is not allowed right now (secure desktop, e.g. a UAC prompt or the lock screen)";
        else if (hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE)
            err = "Too many apps are capturing this screen";
        else if (hr == DXGI_ERROR_UNSUPPORTED)
            err = "Desktop duplication is not supported on this display adapter";
        else
            err = "DuplicateOutput failed " + hresult_text(hr);
        close();
        return false;
    }
    DXGI_OUTDUPL_DESC dd{};
    dup_->GetDesc(&dd);
    if (dd.Rotation != DXGI_MODE_ROTATION_IDENTITY && dd.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED) {
        err = "Rotated monitors are not supported yet";
        close();
        return false;
    }
    nativeW_ = dd.ModeDesc.Width;
    nativeH_ = dd.ModeDesc.Height;
    stream_size(nativeW_, nativeH_, maxHeight, streamW_, streamH_);
    desktop_ = f.desc.DesktopCoordinates;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = nativeW_;
    td.Height = nativeH_;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    hr = device_->CreateTexture2D(&td, nullptr, &mirror_);
    td.Width = streamW_;
    td.Height = streamH_;
    td.BindFlags = 0;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    for (int i = 0; i < kSlots && SUCCEEDED(hr); ++i) hr = device_->CreateTexture2D(&td, nullptr, &staging_[i]);
    if (FAILED(hr)) {
        err = "CreateTexture2D failed " + hresult_text(hr);
        close();
        return false;
    }
    if ((streamW_ != nativeW_ || streamH_ != nativeH_) && !create_scaler(err)) {
        close();
        return false;
    }
    mirrorValid_ = false;
    return true;
}

bool Capture::create_scaler(std::string& err) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = streamW_;
    td.Height = streamH_;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    HRESULT hr = device_->CreateTexture2D(&td, nullptr, &scaled_);
    if (SUCCEEDED(hr)) hr = device_->CreateRenderTargetView(scaled_.Get(), nullptr, &scaledRtv_);
    if (SUCCEEDED(hr)) hr = device_->CreateShaderResourceView(mirror_.Get(), nullptr, &mirrorSrv_);
    ComPtr<ID3DBlob> vsCode, psCode, errors;
    if (SUCCEEDED(hr))
        hr = D3DCompile(kScaleShader, sizeof kScaleShader - 1, "scale", nullptr, nullptr, "vs", "vs_4_0",
                        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vsCode, &errors);
    if (SUCCEEDED(hr))
        hr = D3DCompile(kScaleShader, sizeof kScaleShader - 1, "scale", nullptr, nullptr, "ps", "ps_4_0",
                        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psCode, &errors);
    if (SUCCEEDED(hr))
        hr = device_->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs_);
    if (SUCCEEDED(hr))
        hr = device_->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &ps_);
    if (SUCCEEDED(hr)) {
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        hr = device_->CreateSamplerState(&sd, &sampler_);
    }
    if (SUCCEEDED(hr)) {
        const float c[4] = {1.f / float(streamW_), 1.f / float(streamH_), 0, 0};
        D3D11_BUFFER_DESC bd{sizeof c, D3D11_USAGE_IMMUTABLE, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
        D3D11_SUBRESOURCE_DATA init{c, 0, 0};
        hr = device_->CreateBuffer(&bd, &init, &constants_);
    }
    if (FAILED(hr)) {
        err = "Cannot set up GPU scaling " + hresult_text(hr) +
              (errors ? std::string(": ") + static_cast<const char*>(errors->GetBufferPointer()) : std::string());
        return false;
    }
    return true;
}

void Capture::render_scaled() {
    ID3D11RenderTargetView* rtv = scaledRtv_.Get();
    ctx_->OMSetRenderTargets(1, &rtv, nullptr);
    D3D11_VIEWPORT vp{0, 0, float(streamW_), float(streamH_), 0, 1};
    ctx_->RSSetViewports(1, &vp);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx_->IASetInputLayout(nullptr);
    ctx_->VSSetShader(vs_.Get(), nullptr, 0);
    ctx_->PSSetShader(ps_.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srv = mirrorSrv_.Get();
    ctx_->PSSetShaderResources(0, 1, &srv);
    ID3D11SamplerState* smp = sampler_.Get();
    ctx_->PSSetSamplers(0, 1, &smp);
    ID3D11Buffer* cb = constants_.Get();
    ctx_->PSSetConstantBuffers(0, 1, &cb);
    ctx_->Draw(3, 0);
    // Unbind so the mirror can be written again next frame.
    ID3D11ShaderResourceView* none = nullptr;
    ctx_->PSSetShaderResources(0, 1, &none);
    ctx_->OMSetRenderTargets(0, nullptr, nullptr);
}

Rect Capture::to_stream(const Rect& r) const {
    // Grow by a pixel: the box filter reads a little past each output pixel.
    Rect s{int32_t(int64_t(r.left) * streamW_ / nativeW_) - 1, int32_t(int64_t(r.top) * streamH_ / nativeH_) - 1,
           int32_t((int64_t(r.right) * streamW_ + nativeW_ - 1) / nativeW_) + 1,
           int32_t((int64_t(r.bottom) * streamH_ + nativeH_ - 1) / nativeH_) + 1};
    if (!clip(s, streamW_, streamH_)) return Rect{0, 0, 0, 0};
    return s;
}

void Capture::close() {
    if (ctx_) ctx_->ClearState();
    for (int i = 0; i < kSlots; ++i) {
        if (mapped_[i] && ctx_) ctx_->Unmap(staging_[i].Get(), 0);
        mapped_[i] = false;
        staging_[i].Reset();
    }
    constants_.Reset();
    sampler_.Reset();
    ps_.Reset();
    vs_.Reset();
    mirrorSrv_.Reset();
    scaledRtv_.Reset();
    scaled_.Reset();
    mirror_.Reset();
    mirrorValid_ = false;
    dup_.Reset();
    ctx_.Reset();
    device_.Reset();
    nativeW_ = nativeH_ = streamW_ = streamH_ = 0;
}

void Capture::unmap(int slot) {
    if (slot >= 0 && slot < kSlots && mapped_[slot]) {
        ctx_->Unmap(staging_[slot].Get(), 0);
        mapped_[slot] = false;
    }
}

Capture::Status Capture::next(int timeoutMs, bool wantFull, int slot, CaptureJob& job, std::string& err) {
    job.clear();
    if (!dup_ || slot < 0 || slot >= kSlots || mapped_[slot]) {
        err = "capture not ready";
        return Status::Error;
    }
    ID3D11Texture2D* source = scaled_ ? scaled_.Get() : mirror_.Get();  // what the stream is read from
    auto copy_full = [&] {
        ctx_->CopyResource(staging_[slot].Get(), source);
        job.moves.clear();
        job.rects.assign(1, Rect{0, 0, int32_t(streamW_), int32_t(streamH_)});
        job.full = true;
        job.hasImage = true;
    };

    DXGI_OUTDUPL_FRAME_INFO fi{};
    ComPtr<IDXGIResource> res;
    HRESULT hr = dup_->AcquireNextFrame(UINT(timeoutMs), &fi, &res);

    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        if (!(wantFull && mirrorValid_)) return Status::Timeout;
        copy_full();
    } else if (hr == DXGI_ERROR_ACCESS_LOST) {
        return Status::AccessLost;
    } else if (FAILED(hr)) {
        err = "AcquireNextFrame failed " + hresult_text(hr);
        return Status::Error;
    } else {
        if (fi.LastMouseUpdateTime.QuadPart != 0) {
            job.pointerMoved = true;
            job.pointerPos = {fi.PointerPosition.Position.x, fi.PointerPosition.Position.y,
                              fi.PointerPosition.Visible ? 1u : 0u};
        }
        if (fi.PointerShapeBufferSize) {
            job.pointerData.resize(fi.PointerShapeBufferSize);
            UINT need = 0;
            DXGI_OUTDUPL_POINTER_SHAPE_INFO si{};
            if (SUCCEEDED(dup_->GetFramePointerShape(fi.PointerShapeBufferSize, job.pointerData.data(), &need, &si))) {
                job.pointerShape = {si.Type, si.Width, si.Height, si.Pitch, si.HotSpot.x, si.HotSpot.y};
                job.pointerData.resize(size_t(si.Pitch) * si.Height);
                job.pointerShapeChanged = pointer_shape_bytes(job.pointerShape) == job.pointerData.size();
            }
        }

        ComPtr<ID3D11Texture2D> tex;
        res.As(&tex);
        const bool updated = fi.LastPresentTime.QuadPart != 0;
        std::vector<MoveRect> moves;  // native pixels
        std::vector<Rect> dirtyRects;
        bool changed = false;

        if (!mirrorValid_) {
            // First frame after open: the returned image is the whole desktop.
            ctx_->CopyResource(mirror_.Get(), tex.Get());
            mirrorValid_ = true;
            wantFull = changed = true;
        } else if (updated && fi.TotalMetadataBufferSize) {
            meta_.resize(fi.TotalMetadataBufferSize);
            UINT moveBytes = 0, dirtyBytes = 0;
            auto* mv = reinterpret_cast<DXGI_OUTDUPL_MOVE_RECT*>(meta_.data());
            hr = dup_->GetFrameMoveRects(UINT(meta_.size()), mv, &moveBytes);
            auto* dirty = reinterpret_cast<RECT*>(meta_.data() + moveBytes);
            if (SUCCEEDED(hr)) hr = dup_->GetFrameDirtyRects(UINT(meta_.size() - moveBytes), dirty, &dirtyBytes);
            bool overflow = FAILED(hr);
            if (!overflow) {
                const UINT nMoves = moveBytes / sizeof(DXGI_OUTDUPL_MOVE_RECT);
                const UINT nDirty = dirtyBytes / sizeof(RECT);
                overflow = nMoves > kMaxRects || nDirty > kMaxRects;
                for (UINT i = 0; i < nMoves && !overflow; ++i) {
                    MoveRect m{mv[i].SourcePoint.x, mv[i].SourcePoint.y,
                               {mv[i].DestinationRect.left, mv[i].DestinationRect.top, mv[i].DestinationRect.right,
                                mv[i].DestinationRect.bottom}};
                    const Rect src{m.srcX, m.srcY, m.srcX + (m.dst.right - m.dst.left),
                                   m.srcY + (m.dst.bottom - m.dst.top)};
                    if (!rect_valid(m.dst, nativeW_, nativeH_) || !rect_valid(src, nativeW_, nativeH_)) {
                        overflow = true;
                        break;
                    }
                    moves.push_back(m);
                }
                for (UINT i = 0; i < nDirty && !overflow; ++i) {
                    Rect r{dirty[i].left, dirty[i].top, dirty[i].right, dirty[i].bottom};
                    if (clip(r, nativeW_, nativeH_)) dirtyRects.push_back(r);
                }
            }
            changed = true;
            if (overflow) {
                ctx_->CopyResource(mirror_.Get(), tex.Get());
                wantFull = true;
            } else {
                // Keep the GPU mirror equal to the screen: moved and dirty areas.
                for (const MoveRect& m : moves) {
                    D3D11_BOX b{UINT(m.dst.left), UINT(m.dst.top), 0, UINT(m.dst.right), UINT(m.dst.bottom), 1};
                    ctx_->CopySubresourceRegion(mirror_.Get(), 0, b.left, b.top, 0, tex.Get(), 0, &b);
                }
                for (const Rect& r : dirtyRects) {
                    D3D11_BOX b{UINT(r.left), UINT(r.top), 0, UINT(r.right), UINT(r.bottom), 1};
                    ctx_->CopySubresourceRegion(mirror_.Get(), 0, b.left, b.top, 0, tex.Get(), 0, &b);
                }
            }
        }
        res.Reset();
        tex.Reset();
        dup_->ReleaseFrame();  // everything needed is in the mirror now

        if (changed && scaled_) render_scaled();
        if (!wantFull && (!moves.empty() || !dirtyRects.empty())) {
            if (!scaled_) {
                job.moves = std::move(moves);
                job.rects = std::move(dirtyRects);
            } else {
                // Scaled moves would not land on whole pixels: send those areas as changed instead.
                for (const MoveRect& m : moves) job.rects.push_back(to_stream(m.dst));
                for (const Rect& r : dirtyRects) job.rects.push_back(to_stream(r));
                job.rects.erase(std::remove_if(job.rects.begin(), job.rects.end(),
                                               [](const Rect& r) { return r.left >= r.right || r.top >= r.bottom; }),
                                job.rects.end());
            }
            uint64_t pixels = 0;
            for (const Rect& r : job.rects) pixels += rect_pixels(r);
            if (job.rects.size() > kMaxRects || pixels > uint64_t(streamW_) * streamH_) {
                wantFull = true;
            } else {
                for (const Rect& r : job.rects) {
                    D3D11_BOX b{UINT(r.left), UINT(r.top), 0, UINT(r.right), UINT(r.bottom), 1};
                    ctx_->CopySubresourceRegion(staging_[slot].Get(), 0, b.left, b.top, 0, source, 0, &b);
                }
                job.hasImage = !job.moves.empty() || !job.rects.empty();
            }
        }
        if (wantFull) copy_full();
    }

    if (job.hasImage && !job.rects.empty()) {
        D3D11_MAPPED_SUBRESOURCE m{};
        hr = ctx_->Map(staging_[slot].Get(), 0, D3D11_MAP_READ, 0, &m);
        if (FAILED(hr)) {
            err = "Map failed " + hresult_text(hr);
            return Status::Error;
        }
        mapped_[slot] = true;
        job.slot = slot;
        job.data = static_cast<const uint8_t*>(m.pData);
        job.pitch = m.RowPitch;
    }
    return Status::Ok;
}

}  // namespace wv
