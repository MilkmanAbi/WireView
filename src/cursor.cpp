#include "cursor.h"

#include <cstring>

namespace wv {

static bool bit(const uint8_t* row, uint32_t x) { return (row[x / 8] >> (7 - x % 8)) & 1; }

std::vector<uint8_t> pointer_to_bgra(const PointerShapeMsg& h, const uint8_t* data, uint32_t& width, uint32_t& height) {
    std::vector<uint8_t> out;
    width = height = 0;
    if (pointer_shape_bytes(h) == 0) return out;
    width = h.width;
    height = h.type == kPointerMonochrome ? h.height / 2 : h.height;
    out.assign(size_t(width) * height * 4, 0);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            uint8_t* px = &out[(size_t(y) * width + x) * 4];
            if (h.type == kPointerMonochrome) {
                const bool andBit = bit(data + size_t(y) * h.pitch, x);
                const bool xorBit = bit(data + size_t(y + height) * h.pitch, x);
                if (andBit && !xorBit) continue;  // transparent
                const uint8_t v = andBit ? 0x80 : (xorBit ? 0xFF : 0x00);
                px[0] = px[1] = px[2] = v;
                px[3] = 0xFF;
            } else {
                const uint8_t* s = data + size_t(y) * h.pitch + size_t(x) * 4;
                if (h.type == kPointerColor) {
                    std::memcpy(px, s, 4);
                } else {  // masked colour: mask 0 = opaque colour, 0xFF = XOR with screen
                    if (s[3] == 0) {
                        std::memcpy(px, s, 3);
                        px[3] = 0xFF;
                    } else if (s[0] | s[1] | s[2]) {
                        px[0] = px[1] = px[2] = 0x80;
                        px[3] = 0xFF;
                    }
                }
            }
        }
    }
    return out;
}

HCURSOR pointer_to_hcursor(const PointerShapeMsg& h, const uint8_t* data) {
    if (pointer_shape_bytes(h) == 0) return nullptr;
    ICONINFO ii{};
    ii.fIcon = FALSE;
    ii.xHotspot = DWORD(h.hotX < 0 ? 0 : h.hotX);
    ii.yHotspot = DWORD(h.hotY < 0 ? 0 : h.hotY);

    if (h.type == kPointerMonochrome) {
        // Same AND/XOR layout Windows uses; CreateBitmap wants WORD-aligned rows.
        const uint32_t stride = ((h.width + 15) / 16) * 2;
        std::vector<uint8_t> bits(size_t(stride) * h.height, 0);
        const uint32_t copy = (h.width + 7) / 8;
        for (uint32_t y = 0; y < h.height; ++y) std::memcpy(&bits[size_t(y) * stride], data + size_t(y) * h.pitch, copy);
        ii.hbmMask = CreateBitmap(int(h.width), int(h.height), 1, 1, bits.data());
        HCURSOR c = ii.hbmMask ? HCURSOR(CreateIconIndirect(&ii)) : nullptr;
        if (ii.hbmMask) DeleteObject(ii.hbmMask);
        return c;
    }

    uint32_t w = 0, hh = 0;
    std::vector<uint8_t> bgra = pointer_to_bgra(h, data, w, hh);
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof bi;
    bi.bV5Width = LONG(w);
    bi.bV5Height = -LONG(hh);  // top-down
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    ii.hbmColor = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!ii.hbmColor) return nullptr;
    std::memcpy(bits, bgra.data(), bgra.size());
    std::vector<uint8_t> mask(size_t((w + 15) / 16) * 2 * hh, 0);
    ii.hbmMask = CreateBitmap(int(w), int(hh), 1, 1, mask.data());
    HCURSOR c = ii.hbmMask ? HCURSOR(CreateIconIndirect(&ii)) : nullptr;
    DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    return c;
}

}  // namespace wv
