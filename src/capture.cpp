#include "capture.h"

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

bool Capture::open(int monitor, std::string& err) {
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
    width_ = dd.ModeDesc.Width;
    height_ = dd.ModeDesc.Height;
    desktop_ = f.desc.DesktopCoordinates;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = width_;
    td.Height = height_;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    hr = device_->CreateTexture2D(&td, nullptr, &mirror_);
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    for (int i = 0; i < kSlots && SUCCEEDED(hr); ++i) hr = device_->CreateTexture2D(&td, nullptr, &staging_[i]);
    if (FAILED(hr)) {
        err = "CreateTexture2D failed " + hresult_text(hr);
        close();
        return false;
    }
    mirrorValid_ = false;
    return true;
}

void Capture::close() {
    for (int i = 0; i < kSlots; ++i) {
        if (mapped_[i] && ctx_) ctx_->Unmap(staging_[i].Get(), 0);
        mapped_[i] = false;
        staging_[i].Reset();
    }
    mirror_.Reset();
    mirrorValid_ = false;
    dup_.Reset();
    ctx_.Reset();
    device_.Reset();
    width_ = height_ = 0;
}

void Capture::unmap(int slot) {
    if (slot >= 0 && slot < kSlots && mapped_[slot]) {
        ctx_->Unmap(staging_[slot].Get(), 0);
        mapped_[slot] = false;
    }
}

bool Capture::copy_full(int slot, CaptureJob& job) {
    ctx_->CopyResource(staging_[slot].Get(), mirror_.Get());
    job.moves.clear();
    job.rects.assign(1, Rect{0, 0, int32_t(width_), int32_t(height_)});
    job.full = true;
    job.hasImage = true;
    return true;
}

Capture::Status Capture::next(int timeoutMs, bool wantFull, int slot, CaptureJob& job, std::string& err) {
    job.clear();
    if (!dup_ || slot < 0 || slot >= kSlots || mapped_[slot]) {
        err = "capture not ready";
        return Status::Error;
    }

    DXGI_OUTDUPL_FRAME_INFO fi{};
    ComPtr<IDXGIResource> res;
    HRESULT hr = dup_->AcquireNextFrame(UINT(timeoutMs), &fi, &res);
    bool acquired = SUCCEEDED(hr);

    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        if (!(wantFull && mirrorValid_)) return Status::Timeout;
        copy_full(slot, job);
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

        if (!mirrorValid_) {
            // First frame after open: the returned image is the whole desktop.
            ctx_->CopyResource(mirror_.Get(), tex.Get());
            mirrorValid_ = true;
            wantFull = true;
        } else if (updated && fi.TotalMetadataBufferSize) {
            meta_.resize(fi.TotalMetadataBufferSize);
            UINT moveBytes = 0, dirtyBytes = 0;
            auto* mv = reinterpret_cast<DXGI_OUTDUPL_MOVE_RECT*>(meta_.data());
            hr = dup_->GetFrameMoveRects(UINT(meta_.size()), mv, &moveBytes);
            auto* dirty = reinterpret_cast<RECT*>(meta_.data() + moveBytes);
            if (SUCCEEDED(hr)) hr = dup_->GetFrameDirtyRects(UINT(meta_.size() - moveBytes), dirty, &dirtyBytes);
            if (FAILED(hr)) {
                wantFull = true;  // couldn't read what changed: resend everything
                ctx_->CopyResource(mirror_.Get(), tex.Get());
            } else {
                const UINT nMoves = moveBytes / sizeof(DXGI_OUTDUPL_MOVE_RECT);
                const UINT nDirty = dirtyBytes / sizeof(RECT);
                uint64_t pixels = 0;
                bool overflow = nMoves > kMaxRects || nDirty > kMaxRects;
                for (UINT i = 0; i < nMoves && !overflow; ++i) {
                    MoveRect m{mv[i].SourcePoint.x, mv[i].SourcePoint.y,
                               {mv[i].DestinationRect.left, mv[i].DestinationRect.top, mv[i].DestinationRect.right,
                                mv[i].DestinationRect.bottom}};
                    const Rect src{m.srcX, m.srcY, m.srcX + (m.dst.right - m.dst.left),
                                   m.srcY + (m.dst.bottom - m.dst.top)};
                    if (!rect_valid(m.dst, width_, height_) || !rect_valid(src, width_, height_)) {
                        overflow = true;
                        break;
                    }
                    job.moves.push_back(m);
                }
                for (UINT i = 0; i < nDirty && !overflow; ++i) {
                    Rect r{dirty[i].left, dirty[i].top, dirty[i].right, dirty[i].bottom};
                    if (!clip(r, width_, height_)) continue;
                    pixels += rect_pixels(r);
                    job.rects.push_back(r);
                }
                if (pixels > uint64_t(width_) * height_) overflow = true;

                // Keep the GPU mirror equal to the screen: moved and dirty areas.
                if (overflow) {
                    ctx_->CopyResource(mirror_.Get(), tex.Get());
                    wantFull = true;
                } else {
                    for (const MoveRect& m : job.moves) {
                        D3D11_BOX b{UINT(m.dst.left), UINT(m.dst.top), 0, UINT(m.dst.right), UINT(m.dst.bottom), 1};
                        ctx_->CopySubresourceRegion(mirror_.Get(), 0, b.left, b.top, 0, tex.Get(), 0, &b);
                    }
                    for (const Rect& r : job.rects) {
                        D3D11_BOX b{UINT(r.left), UINT(r.top), 0, UINT(r.right), UINT(r.bottom), 1};
                        ctx_->CopySubresourceRegion(mirror_.Get(), 0, b.left, b.top, 0, tex.Get(), 0, &b);
                        ctx_->CopySubresourceRegion(staging_[slot].Get(), 0, b.left, b.top, 0, tex.Get(), 0, &b);
                    }
                    job.hasImage = !job.moves.empty() || !job.rects.empty();
                }
            }
        }
        if (wantFull) copy_full(slot, job);
        res.Reset();
        tex.Reset();
        if (acquired) dup_->ReleaseFrame();
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
