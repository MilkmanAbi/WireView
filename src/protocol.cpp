#include "protocol.h"

#include <cstring>

namespace wv {

uint64_t max_frame_payload(uint32_t width, uint32_t height) {
    return sizeof(FrameMsg) + uint64_t(kMaxRects) * (sizeof(MoveRect) + sizeof(Rect)) +
           uint64_t(width) * height * 4;
}

bool rect_valid(const Rect& r, uint32_t width, uint32_t height) {
    return r.left >= 0 && r.top >= 0 && r.left < r.right && r.top < r.bottom &&
           uint32_t(r.right) <= width && uint32_t(r.bottom) <= height;
}

bool parse_frame(const uint8_t* p, size_t n, uint32_t width, uint32_t height, FrameView& out) {
    if (width == 0 || height == 0 || width > kMaxDimension || height > kMaxDimension) return false;
    if (n < sizeof(FrameMsg) || n > max_frame_payload(width, height)) return false;
    FrameMsg h;
    std::memcpy(&h, p, sizeof h);
    if (h.moveCount > kMaxRects || h.rectCount > kMaxRects) return false;
    const uint64_t meta = sizeof(FrameMsg) + uint64_t(h.moveCount) * sizeof(MoveRect) +
                          uint64_t(h.rectCount) * sizeof(Rect);
    if (meta > n) return false;

    const auto* moves = reinterpret_cast<const MoveRect*>(p + sizeof(FrameMsg));
    for (uint32_t i = 0; i < h.moveCount; ++i) {
        const MoveRect& m = moves[i];
        if (!rect_valid(m.dst, width, height)) return false;
        const Rect src{m.srcX, m.srcY, m.srcX + (m.dst.right - m.dst.left),
                       m.srcY + (m.dst.bottom - m.dst.top)};
        if (!rect_valid(src, width, height)) return false;
    }
    const auto* rects = reinterpret_cast<const Rect*>(p + sizeof(FrameMsg) + h.moveCount * sizeof(MoveRect));
    uint64_t pixelBytes = 0;
    for (uint32_t i = 0; i < h.rectCount; ++i) {
        if (!rect_valid(rects[i], width, height)) return false;
        pixelBytes += rect_pixels(rects[i]) * 4;
    }
    if (meta + pixelBytes != n) return false;

    out.seq = h.seq;
    out.moves = moves;
    out.moveCount = h.moveCount;
    out.rects = rects;
    out.rectCount = h.rectCount;
    out.pixels = p + meta;
    out.pixelBytes = pixelBytes;
    return true;
}

uint64_t pointer_shape_bytes(const PointerShapeMsg& h) {
    if (h.width == 0 || h.height == 0 || h.width > 256 || h.height > 512) return 0;
    uint64_t rows = h.height;
    uint64_t minPitch = 0;
    switch (h.type) {
        case kPointerMonochrome:
            // AND mask then XOR mask: height covers both halves.
            if (h.height % 2) return 0;
            minPitch = (h.width + 7) / 8;
            break;
        case kPointerColor:
        case kPointerMaskedColor:
            minPitch = uint64_t(h.width) * 4;
            break;
        default:
            return 0;
    }
    if (h.pitch < minPitch || h.pitch > 4096) return 0;
    const uint64_t bytes = rows * h.pitch;
    return bytes <= kMaxPointerBytes ? bytes : 0;
}

bool parse_pointer_shape(const uint8_t* p, size_t n, PointerShapeMsg& hdr, const uint8_t*& data) {
    if (n < sizeof(PointerShapeMsg)) return false;
    std::memcpy(&hdr, p, sizeof hdr);
    const uint64_t bytes = pointer_shape_bytes(hdr);
    if (bytes == 0 || sizeof(PointerShapeMsg) + bytes != n) return false;
    data = p + sizeof(PointerShapeMsg);
    return true;
}

bool input_event_valid(const InputEvent& e) {
    switch (e.kind) {
        case kInMove:
            return true;
        case kInButton:
            return e.a <= 4 && e.b <= 1;
        case kInWheel:
            return e.a <= 1 && e.x >= -120 * 100 && e.x <= 120 * 100;
        case kInKey:
            return (e.a & ~(kKeyUp | kKeyExtended)) == 0 && e.b != 0 && e.b < 0x100;
        case kInReleaseAll:
            return true;
        default:
            return false;
    }
}

bool parse_input(const uint8_t* p, size_t n, const InputEvent*& events, size_t& count) {
    if (n == 0 || n % sizeof(InputEvent) || n / sizeof(InputEvent) > kMaxInputEvents) return false;
    events = reinterpret_cast<const InputEvent*>(p);
    count = n / sizeof(InputEvent);
    for (size_t i = 0; i < count; ++i)
        if (!input_event_valid(events[i])) return false;
    return true;
}

void apply_frame_cpu(uint8_t* image, uint32_t width, uint32_t height, const FrameView& f) {
    const size_t stride = size_t(width) * 4;
    std::vector<uint8_t> tmp;
    for (uint32_t i = 0; i < f.moveCount; ++i) {
        const MoveRect& m = f.moves[i];
        const uint32_t w = m.dst.right - m.dst.left, h = m.dst.bottom - m.dst.top;
        tmp.resize(size_t(w) * h * 4);
        for (uint32_t y = 0; y < h; ++y)
            std::memcpy(&tmp[size_t(y) * w * 4], image + (m.srcY + y) * stride + size_t(m.srcX) * 4, size_t(w) * 4);
        for (uint32_t y = 0; y < h; ++y)
            std::memcpy(image + (m.dst.top + y) * stride + size_t(m.dst.left) * 4, &tmp[size_t(y) * w * 4], size_t(w) * 4);
    }
    const uint8_t* src = f.pixels;
    for (uint32_t i = 0; i < f.rectCount; ++i) {
        const Rect& r = f.rects[i];
        const size_t rowBytes = size_t(r.right - r.left) * 4;
        for (int32_t y = r.top; y < r.bottom; ++y) {
            std::memcpy(image + size_t(y) * stride + size_t(r.left) * 4, src, rowBytes);
            src += rowBytes;
        }
    }
    (void)height;
}

void fill_name(char (&dst)[64], const char* utf8) {
    std::memset(dst, 0, sizeof dst);
    if (!utf8) return;
    size_t n = std::strlen(utf8);
    if (n > sizeof dst - 1) n = sizeof dst - 1;
    std::memcpy(dst, utf8, n);
}

}  // namespace wv
