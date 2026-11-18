// Screen capture with DXGI Desktop Duplication.
//
// The fast path: DXGI already tells us which rectangles changed (dirty rects)
// and which were moved (move rects, e.g. scrolling). We copy only the dirty
// rectangles into a CPU-readable staging texture and hand the mapped memory
// straight to the socket; moves are sent as 24-byte instructions. Nothing is
// compressed and no pixel is copied by the CPU on this side.
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

struct MonitorInfo {
    std::string name;  // e.g. "\\.\DISPLAY1  2496x1664"
    RECT desktop{};    // in virtual-desktop pixels
};
std::vector<MonitorInfo> list_monitors();

struct CaptureJob {
    bool hasImage = false;
    bool full = false;  // rects cover the whole screen
    int slot = -1;
    const uint8_t* data = nullptr;  // mapped staging texture, valid until unmap(slot)
    uint32_t pitch = 0;
    std::vector<MoveRect> moves;
    std::vector<Rect> rects;

    bool pointerMoved = false;
    PointerPosMsg pointerPos{};
    bool pointerShapeChanged = false;
    PointerShapeMsg pointerShape{};
    std::vector<uint8_t> pointerData;

    void clear() {
        hasImage = full = false;
        slot = -1;
        data = nullptr;
        pitch = 0;
        moves.clear();
        rects.clear();
        pointerMoved = pointerShapeChanged = false;
    }
};

class Capture {
public:
    static constexpr int kSlots = 2;

    enum class Status { Timeout, Ok, AccessLost, Error };

    ~Capture() { close(); }
    // monitor indexes list_monitors(). Resets all state (next frame is full).
    bool open(int monitor, std::string& err);
    void close();
    bool is_open() const { return dup_ != nullptr; }

    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    RECT desktop_rect() const { return desktop_; }

    // Waits up to timeoutMs for an update and fills job, writing pixels into
    // staging slot `slot`, which must not be mapped. With wantFull the job holds
    // the whole screen (from the GPU-side mirror, so it works on a static desktop).
    Status next(int timeoutMs, bool wantFull, int slot, CaptureJob& job, std::string& err);
    void unmap(int slot);

private:
    bool copy_full(int slot, CaptureJob& job);

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<IDXGIOutputDuplication> dup_;
    ComPtr<ID3D11Texture2D> staging_[kSlots];
    bool mapped_[kSlots] = {};
    ComPtr<ID3D11Texture2D> mirror_;  // GPU copy of the current screen
    bool mirrorValid_ = false;
    uint32_t width_ = 0, height_ = 0;
    RECT desktop_{};
    std::vector<uint8_t> meta_;
};

}  // namespace wv
