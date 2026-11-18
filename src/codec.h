// Rectangle encodings for slow links, all built into Windows (no libraries):
//   palette: areas with at most 256 colours (text, UI, flat graphics) are sent
//            lossless as a colour table + XPRESS-Huffman compressed indices.
//            Text stays sharp and usually shrinks 20-100x.
//   JPEG:    everything else (photos, video, gradients), via WIC.
// One RectCodec per thread; the thread must have COM initialised (ComScope).
#pragma once
#include <windows.h>
#include <compressapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdint>
#include <vector>

namespace wv {

struct ComScope {
    ComScope() : hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComScope() {
        if (SUCCEEDED(hr)) CoUninitialize();
    }
    HRESULT hr;
};

class RectCodec {
public:
    ~RectCodec();
    bool init();

    // Picks the smallest sensible encoding for a rect of 32-bit pixels (4th byte ignored).
    // Returns kRectPalette / kRectJpeg with `out` filled, or kRectRaw if neither is smaller than raw.
    uint8_t encode(const uint8_t* bgrx, uint32_t pitch, uint32_t width, uint32_t height, uint32_t quality,
                   std::vector<uint8_t>& out);
    // Decodes an encoded rect into dst (32-bit rows of dstPitch). False on any inconsistency.
    bool decode(uint8_t encoding, const uint8_t* data, size_t size, uint32_t width, uint32_t height, uint8_t* dst,
                uint32_t dstPitch);

    // The two encoders on their own (used by encode() and by tests).
    bool encode_palette(const uint8_t* bgrx, uint32_t pitch, uint32_t width, uint32_t height, std::vector<uint8_t>& out);
    bool encode_jpeg(const uint8_t* bgrx, uint32_t pitch, uint32_t width, uint32_t height, uint32_t quality,
                     std::vector<uint8_t>& out);
    bool decode_palette(const uint8_t* data, size_t size, uint32_t width, uint32_t height, uint8_t* dst, uint32_t dstPitch);
    bool decode_jpeg(const uint8_t* data, size_t size, uint32_t width, uint32_t height, uint8_t* dst, uint32_t dstPitch);

private:
    Microsoft::WRL::ComPtr<IWICImagingFactory> wic_;
    COMPRESSOR_HANDLE compressor_ = nullptr;
    DECOMPRESSOR_HANDLE decompressor_ = nullptr;
    std::vector<uint8_t> indices_;
};

}  // namespace wv
