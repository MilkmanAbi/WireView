// Remote pointer shapes (DXGI format) -> something we can draw.
#pragma once
#include <windows.h>

#include <cstdint>
#include <vector>

#include "protocol.h"

namespace wv {

// Straight-alpha BGRA, width x imageHeight (monochrome shapes are half as tall as h.height).
// XOR/invert pixels can't be reproduced without the screen behind them; they become mid grey.
std::vector<uint8_t> pointer_to_bgra(const PointerShapeMsg& h, const uint8_t* data, uint32_t& width, uint32_t& height);

// A real cursor for the viewer window while controlling. Caller DestroyCursor()s it.
HCURSOR pointer_to_hcursor(const PointerShapeMsg& h, const uint8_t* data);

}  // namespace wv
