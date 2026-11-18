#include "codec.h"

#include <cstring>

#include "protocol.h"

namespace wv {

using Microsoft::WRL::ComPtr;

RectCodec::~RectCodec() {
    if (compressor_) CloseCompressor(compressor_);
    if (decompressor_) CloseDecompressor(decompressor_);
}

bool RectCodec::init() {
    if (!wic_ && FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic_))))
        return false;
    if (!compressor_ && !CreateCompressor(COMPRESS_ALGORITHM_XPRESS_HUFF, nullptr, &compressor_)) return false;
    if (!decompressor_ && !CreateDecompressor(COMPRESS_ALGORITHM_XPRESS_HUFF, nullptr, &decompressor_)) return false;
    return true;
}

uint8_t RectCodec::encode(const uint8_t* bgrx, uint32_t pitch, uint32_t width, uint32_t height, uint32_t quality,
                          std::vector<uint8_t>& out) {
    const uint64_t raw = uint64_t(width) * height * 4;
    if (uint64_t(width) * height < 64) return kRectRaw;  // a caret: headers would cost more than the pixels
    if (encode_palette(bgrx, pitch, width, height, out) && out.size() < raw) return kRectPalette;
    if (encode_jpeg(bgrx, pitch, width, height, quality, out) && out.size() < raw) return kRectJpeg;
    return kRectRaw;
}

bool RectCodec::decode(uint8_t encoding, const uint8_t* data, size_t size, uint32_t width, uint32_t height,
                       uint8_t* dst, uint32_t dstPitch) {
    if (encoding == kRectPalette) return decode_palette(data, size, width, height, dst, dstPitch);
    if (encoding == kRectJpeg) return decode_jpeg(data, size, width, height, dst, dstPitch);
    return false;
}

// ---------------------------------------------------------------- palette
// Payload: u16 colour count (1..256), u16 0, count x u32 colour (B,G,R,0),
// then the w*h one-byte indices compressed with XPRESS-Huffman (block mode).

bool RectCodec::encode_palette(const uint8_t* bgrx, uint32_t pitch, uint32_t width, uint32_t height,
                               std::vector<uint8_t>& out) {
    if (!compressor_) return false;
    constexpr uint32_t kEmpty = 0xFFFFFFFF;  // never a masked colour
    uint32_t keys[1024];
    uint8_t vals[1024];
    std::memset(keys, 0xFF, sizeof keys);
    uint32_t colors[256];
    uint32_t count = 0;
    const size_t n = size_t(width) * height;
    indices_.resize(n);
    uint8_t* idx = indices_.data();
    uint32_t lastKey = kEmpty;
    uint8_t lastIdx = 0;
    for (uint32_t y = 0; y < height; ++y) {
        const uint32_t* row = reinterpret_cast<const uint32_t*>(bgrx + size_t(y) * pitch);
        for (uint32_t x = 0; x < width; ++x) {
            const uint32_t k = row[x] & 0x00FFFFFF;
            if (k != lastKey) {
                uint32_t h = (k * 2654435761u) >> 22;
                while (keys[h] != k && keys[h] != kEmpty) h = (h + 1) & 1023;
                if (keys[h] == kEmpty) {
                    if (count == 256) return false;  // too many colours: not text/UI, use JPEG
                    keys[h] = k;
                    vals[h] = uint8_t(count);
                    colors[count++] = k;
                }
                lastKey = k;
                lastIdx = vals[h];
            }
            *idx++ = lastIdx;
        }
    }
    const size_t head = 4 + size_t(count) * 4;
    out.resize(head + n + 4096);
    const uint16_t c16 = uint16_t(count), zero = 0;
    std::memcpy(out.data(), &c16, 2);
    std::memcpy(out.data() + 2, &zero, 2);
    std::memcpy(out.data() + 4, colors, size_t(count) * 4);
    SIZE_T packed = 0;
    if (!Compress(compressor_, indices_.data(), n, out.data() + head, out.size() - head, &packed)) return false;
    out.resize(head + packed);
    return true;
}

bool RectCodec::decode_palette(const uint8_t* data, size_t size, uint32_t width, uint32_t height, uint8_t* dst,
                               uint32_t dstPitch) {
    if (!decompressor_ || size < 4) return false;
    uint16_t count;
    std::memcpy(&count, data, 2);
    if (count == 0 || count > 256) return false;
    const size_t head = 4 + size_t(count) * 4;
    if (size <= head) return false;
    uint32_t colors[256];
    std::memcpy(colors, data + 4, size_t(count) * 4);
    const size_t n = size_t(width) * height;
    indices_.resize(n);
    SIZE_T got = 0;
    if (!Decompress(decompressor_, data + head, size - head, indices_.data(), n, &got) || got != n) return false;
    const uint8_t* idx = indices_.data();
    for (uint32_t y = 0; y < height; ++y) {
        uint32_t* row = reinterpret_cast<uint32_t*>(dst + size_t(y) * dstPitch);
        for (uint32_t x = 0; x < width; ++x) {
            const uint8_t i = *idx++;
            if (i >= count) return false;
            row[x] = colors[i] | 0xFF000000;
        }
    }
    return true;
}

// ---------------------------------------------------------------- JPEG

bool RectCodec::encode_jpeg(const uint8_t* bgrx, uint32_t pitch, uint32_t width, uint32_t height, uint32_t quality,
                            std::vector<uint8_t>& out) {
    out.clear();
    if (!wic_ || !width || !height) return false;
    const UINT bufferSize = pitch * (height - 1) + width * 4;
    ComPtr<IWICBitmap> bitmap;
    // 32bppBGR: DXGI's alpha byte is undefined, so tell WIC to ignore it.
    if (FAILED(wic_->CreateBitmapFromMemory(width, height, GUID_WICPixelFormat32bppBGR, pitch, bufferSize,
                                            const_cast<BYTE*>(bgrx), &bitmap)))
        return false;
    ComPtr<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) return false;
    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(wic_->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder))) return false;
    if (FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) return false;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    if (FAILED(encoder->CreateNewFrame(&frame, &props))) return false;

    PROPBAG2 opt[2] = {};
    VARIANT val[2];
    opt[0].pstrName = const_cast<LPOLESTR>(L"ImageQuality");
    VariantInit(&val[0]);
    val[0].vt = VT_R4;
    val[0].fltVal = float(quality > 100 ? 100 : quality) / 100.f;
    opt[1].pstrName = const_cast<LPOLESTR>(L"JpegYCrCbSubsampling");
    VariantInit(&val[1]);
    val[1].vt = VT_UI1;
    // Full colour resolution at high quality; 4:2:0 otherwise (text mostly goes the palette way anyway).
    val[1].bVal = BYTE(quality >= 90 ? WICJpegYCrCbSubsampling444 : WICJpegYCrCbSubsampling420);
    props->Write(2, opt, val);

    if (FAILED(frame->Initialize(props.Get()))) return false;
    if (FAILED(frame->SetSize(width, height))) return false;
    WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
    if (FAILED(frame->SetPixelFormat(&fmt))) return false;
    if (FAILED(frame->WriteSource(bitmap.Get(), nullptr))) return false;  // converts 32 -> 24 bit
    if (FAILED(frame->Commit()) || FAILED(encoder->Commit())) return false;

    STATSTG st{};
    if (FAILED(stream->Stat(&st, STATFLAG_NONAME))) return false;
    HGLOBAL mem = nullptr;
    if (FAILED(GetHGlobalFromStream(stream.Get(), &mem))) return false;
    const void* p = GlobalLock(mem);
    if (!p) return false;
    out.assign(static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + size_t(st.cbSize.QuadPart));
    GlobalUnlock(mem);
    return !out.empty();
}

bool RectCodec::decode_jpeg(const uint8_t* jpeg, size_t size, uint32_t width, uint32_t height, uint8_t* dst,
                            uint32_t dstPitch) {
    if (!wic_ || size == 0 || size > 0x7FFFFFFF) return false;
    ComPtr<IWICStream> stream;
    if (FAILED(wic_->CreateStream(&stream))) return false;
    if (FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(jpeg), DWORD(size)))) return false;
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(wic_->CreateDecoder(GUID_ContainerFormatJpeg, nullptr, &decoder))) return false;
    if (FAILED(decoder->Initialize(stream.Get(), WICDecodeMetadataCacheOnDemand))) return false;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) return false;
    UINT w = 0, h = 0;
    if (FAILED(frame->GetSize(&w, &h)) || w != width || h != height) return false;
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(wic_->CreateFormatConverter(&conv))) return false;
    if (FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGR, WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeCustom)))
        return false;
    return SUCCEEDED(conv->CopyPixels(nullptr, dstPitch, dstPitch * height, dst));
}

}  // namespace wv
