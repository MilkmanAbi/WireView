#include "inject.h"

#include <algorithm>

namespace wv {

void SendInputSink::send(const INPUT* inputs, UINT count) {
    if (count) SendInput(count, const_cast<INPUT*>(inputs), sizeof(INPUT));
}

LONG normalize_coord(LONG p, LONG size) {
    if (size <= 1) return 0;
    p = std::clamp<LONG>(p, 0, size - 1);
    // Round up so Windows' floor(n * size / 65536) lands exactly on p.
    const LONG n = LONG((int64_t(p) * 65536 + size - 1) / size);
    return std::min<LONG>(n, 65535);
}

void Injector::set_target(const RECT& monitor, const RECT& virt) {
    std::lock_guard<std::mutex> lock(mu_);
    monitor_ = monitor;
    virt_ = virt;
}

void Injector::add_mouse(std::vector<INPUT>& out, int32_t x, int32_t y, DWORD flags, DWORD data) {
    const LONG mw = monitor_.right - monitor_.left, mh = monitor_.bottom - monitor_.top;
    x = std::clamp<int32_t>(x, 0, std::max<LONG>(mw - 1, 0));
    y = std::clamp<int32_t>(y, 0, std::max<LONG>(mh - 1, 0));
    lastX_ = x;
    lastY_ = y;
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dx = normalize_coord(monitor_.left + x - virt_.left, virt_.right - virt_.left);
    in.mi.dy = normalize_coord(monitor_.top + y - virt_.top, virt_.bottom - virt_.top);
    in.mi.dwFlags = flags | MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    in.mi.mouseData = data;
    out.push_back(in);
}

void Injector::apply(const InputEvent* events, size_t count) {
    static constexpr DWORD kDown[5] = {MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_MIDDLEDOWN,
                                       MOUSEEVENTF_XDOWN, MOUSEEVENTF_XDOWN};
    static constexpr DWORD kUp[5] = {MOUSEEVENTF_LEFTUP, MOUSEEVENTF_RIGHTUP, MOUSEEVENTF_MIDDLEUP, MOUSEEVENTF_XUP,
                                     MOUSEEVENTF_XUP};
    std::vector<INPUT> out;
    bool release = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        out.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            const InputEvent& e = events[i];
            if (!input_event_valid(e)) continue;  // already checked by the parser; never trust twice-removed data
            switch (e.kind) {
                case kInMove:
                    add_mouse(out, e.x, e.y, 0, 0);
                    break;
                case kInButton: {
                    const bool down = e.b != 0;
                    const DWORD data = e.a == 3 ? XBUTTON1 : e.a == 4 ? XBUTTON2 : 0;
                    add_mouse(out, e.x, e.y, down ? kDown[e.a] : kUp[e.a], data);
                    if (down)
                        buttons_ |= 1u << e.a;
                    else
                        buttons_ &= ~(1u << e.a);
                    break;
                }
                case kInWheel: {
                    INPUT in{};
                    in.type = INPUT_MOUSE;
                    in.mi.dwFlags = e.a ? MOUSEEVENTF_HWHEEL : MOUSEEVENTF_WHEEL;
                    in.mi.mouseData = DWORD(e.x);
                    out.push_back(in);
                    break;
                }
                case kInKey: {
                    const bool up = e.a & kKeyUp, ext = e.a & kKeyExtended;
                    INPUT in{};
                    in.type = INPUT_KEYBOARD;
                    in.ki.wScan = e.b;
                    in.ki.dwFlags = KEYEVENTF_SCANCODE | (up ? KEYEVENTF_KEYUP : 0) | (ext ? KEYEVENTF_EXTENDEDKEY : 0);
                    out.push_back(in);
                    keys_[e.b | (ext ? 0x100 : 0)] = !up;
                    break;
                }
                case kInReleaseAll:
                    release = true;
                    break;
            }
        }
    }
    sink_.send(out.data(), UINT(out.size()));
    if (release) release_all();
}

void Injector::release_all() {
    static constexpr DWORD kUp[5] = {MOUSEEVENTF_LEFTUP, MOUSEEVENTF_RIGHTUP, MOUSEEVENTF_MIDDLEUP, MOUSEEVENTF_XUP,
                                     MOUSEEVENTF_XUP};
    std::vector<INPUT> out;
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (size_t k = 0; k < keys_.size(); ++k) {
            if (!keys_[k]) continue;
            INPUT in{};
            in.type = INPUT_KEYBOARD;
            in.ki.wScan = WORD(k & 0xFF);
            in.ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP | ((k & 0x100) ? KEYEVENTF_EXTENDEDKEY : 0);
            out.push_back(in);
        }
        keys_.reset();
        for (int b = 0; b < 5; ++b) {
            if (!(buttons_ & (1u << b))) continue;
            add_mouse(out, lastX_, lastY_, kUp[b], b == 3 ? XBUTTON1 : b == 4 ? XBUTTON2 : 0);
        }
        buttons_ = 0;
    }
    sink_.send(out.data(), UINT(out.size()));
}

}  // namespace wv
