// Screen capture with DXGI Desktop Duplication.
//
// The fast path: DXGI already tells us which rectangles changed (dirty rects)
// and which were moved (move rects, e.g. scrolling). We copy only the dirty
// rectangles into a CPU-readable staging texture and hand the mapped memory
// straight to the socket; moves are sent as 24-byte instructions. Nothing is
// compressed and no pixel is copied by the CPU on this side.
//
// For slow links the stream can be smaller than the screen: the GPU scales a
// copy of the screen down (box filter) and only the scaled-down changed areas
// are read back. Moves become ordinary changed areas in that case.
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
    bool full = false;  // rects cover the whole stream
    int slot = -1;
    const uint8_t* data = nullptr;  // mapped staging texture, valid until unmap(slot)
    uint32_t pitch = 0;
    std::vector<MoveRect> moves;  // stream pixels
    std::vector<Rect> rects;      // stream pixels

    bool pointerMoved = false;
    PointerPosMsg pointerPos{};  // native pixels
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
    // monitor indexes list_monitors(); maxHeight as in StreamSettingsMsg.
    // Resets all state (next frame is full).
    bool open(int monitor, uint32_t maxHeight, std::string& err);
    void close();
    bool is_open() const { return dup_ != nullptr; }

    uint32_t width() const { return streamW_; }  // stream size
    uint32_t height() const { return streamH_; }
    uint32_t native_width() const { return nativeW_; }
    uint32_t native_height() const { return nativeH_; }
    bool scaled() const { return scaled_ != nullptr; }
    RECT desktop_rect() const { return desktop_; }

    // Waits up to timeoutMs for an update and fills job, writing pixels into
    // staging slot `slot`, which must not be mapped. With wantFull the job holds
    // the whole stream (from the GPU-side mirror, so it works on a static desktop).
    Status next(int timeoutMs, bool wantFull, int slot, CaptureJob& job, std::string& err);
    void unmap(int slot);

private:
    bool create_scaler(std::string& err);
    void render_scaled();
    Rect to_stream(const Rect& native) const;

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<IDXGIOutputDuplication> dup_;
    ComPtr<ID3D11Texture2D> staging_[kSlots];
    bool mapped_[kSlots] = {};
    ComPtr<ID3D11Texture2D> mirror_;  // GPU copy of the current screen, native size
    bool mirrorValid_ = false;
    // Scaled stream (only when the stream is smaller than the screen).
    ComPtr<ID3D11Texture2D> scaled_;
    ComPtr<ID3D11RenderTargetView> scaledRtv_;
    ComPtr<ID3D11ShaderResourceView> mirrorSrv_;
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_;
    ComPtr<ID3D11SamplerState> sampler_;
    ComPtr<ID3D11Buffer> constants_;

    uint32_t nativeW_ = 0, nativeH_ = 0, streamW_ = 0, streamH_ = 0;
    RECT desktop_{};
    std::vector<uint8_t> meta_;
};

}  // namespace wv
