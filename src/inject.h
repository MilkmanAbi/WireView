// Turns viewer input events into SendInput calls on the host.
#pragma once
#include <windows.h>

#include <bitset>
#include <mutex>
#include <vector>

#include "protocol.h"

namespace wv {

class InputSink {
public:
    virtual ~InputSink() = default;
    virtual void send(const INPUT* inputs, UINT count) = 0;
};

class SendInputSink : public InputSink {
public:
    void send(const INPUT* inputs, UINT count) override;
};

// Maps a remote pixel (relative to the captured monitor) to SendInput's
// 0..65535 absolute range over the virtual desktop.
LONG normalize_coord(LONG pixelInVirtualDesk, LONG virtualSize);

class Injector {
public:
    explicit Injector(InputSink& sink) : sink_(sink) {}
    // monitor: captured monitor in virtual-desktop pixels; virt: the whole virtual desktop.
    void set_target(const RECT& monitor, const RECT& virt);
    void apply(const InputEvent* events, size_t count);
    void release_all();

private:
    void add_mouse(std::vector<INPUT>& out, int32_t x, int32_t y, DWORD flags, DWORD data);

    InputSink& sink_;
    std::mutex mu_;
    RECT monitor_{0, 0, 1, 1}, virt_{0, 0, 1, 1};
    std::bitset<512> keys_;  // scancode | (extended << 8)
    unsigned buttons_ = 0;   // bit per button index
    int32_t lastX_ = 0, lastY_ = 0;
};

}  // namespace wv
