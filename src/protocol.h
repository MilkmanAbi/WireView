// WireView wire protocol. See docs/protocol.md.
// Everything is little-endian and packed. One TCP connection per session:
// host -> viewer carries the screen, viewer -> host carries input.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace wv {

constexpr uint16_t kTcpPort = 47110;
constexpr uint16_t kUdpPort = 47111;
constexpr uint16_t kProtocolVersion = 1;
constexpr char kMagic[4] = {'W', 'V', 'W', '1'};

#pragma pack(push, 1)

struct Rect {
    int32_t left, top, right, bottom;
};

// First bytes the viewer sends after connecting.
struct Hello {
    char magic[4];
    uint16_t version;
    uint16_t flags;  // kHelloWantControl
    char name[64];   // UTF-8 computer name, NUL padded
};
constexpr uint16_t kHelloWantControl = 1;

// The host's answer.
struct Welcome {
    char magic[4];
    uint16_t version;
    uint16_t status;  // WelcomeStatus
    uint16_t flags;   // kWelcomeControlGranted
    uint16_t reserved;
    char name[64];
};
enum WelcomeStatus : uint16_t { kWelcomeOk = 0, kWelcomeRejected = 1, kWelcomeBusy = 2, kWelcomeBadVersion = 3 };
constexpr uint16_t kWelcomeControlGranted = 1;

struct MsgHeader {
    uint16_t type;
    uint16_t flags;
    uint32_t size;  // payload bytes following this header
};

enum MsgType : uint16_t {
    // host -> viewer
    kMsgMode = 1,          // ModeMsg. Always followed by a full frame.
    kMsgFrame = 2,         // FrameMsg + MoveRect[] + Rect[] + pixels
    kMsgPointerPos = 3,    // PointerPosMsg
    kMsgPointerShape = 4,  // PointerShapeMsg + shape bytes
    kMsgControl = 5,       // ControlMsg
    // viewer -> host
    kMsgInput = 16,        // InputEvent[] (size is a multiple of 12)
    kMsgRequestFull = 17,  // empty: please send a full frame
};

constexpr uint32_t kFormatBGRA8 = 1;

struct ModeMsg {
    uint32_t width, height, format;
};

struct FrameMsg {
    uint32_t seq;
    uint16_t moveCount;
    uint16_t rectCount;
};

struct MoveRect {
    int32_t srcX, srcY;
    Rect dst;
};

struct PointerPosMsg {
    int32_t x, y;  // top-left of the pointer image, in remote pixels
    uint32_t visible;
};

// type uses DXGI_OUTDUPL_POINTER_SHAPE_TYPE values.
constexpr uint32_t kPointerMonochrome = 1, kPointerColor = 2, kPointerMaskedColor = 4;
struct PointerShapeMsg {
    uint32_t type, width, height, pitch;
    int32_t hotX, hotY;
};

struct ControlMsg {
    uint32_t granted;
};

enum InputKind : uint8_t {
    kInMove = 1,        // x,y
    kInButton = 2,      // a = button (0 L, 1 R, 2 M, 3 X1, 4 X2), b = 1 down / 0 up, x,y
    kInWheel = 3,       // a = 1 horizontal, x = delta
    kInKey = 4,         // b = scancode, a = kKeyUp | kKeyExtended
    kInReleaseAll = 5,  // release everything held
};
constexpr uint8_t kKeyUp = 1, kKeyExtended = 2;

struct InputEvent {
    uint8_t kind;
    uint8_t a;
    uint16_t b;
    int32_t x, y;
};

#pragma pack(pop)

static_assert(sizeof(Hello) == 72);
static_assert(sizeof(Welcome) == 76);
static_assert(sizeof(MsgHeader) == 8);
static_assert(sizeof(FrameMsg) == 8);
static_assert(sizeof(MoveRect) == 24);
static_assert(sizeof(InputEvent) == 12);

// Limits. Anything above them drops the connection.
constexpr uint32_t kMaxDimension = 16384;
constexpr uint32_t kMaxRects = 4096;
constexpr uint32_t kMaxPointerBytes = 256 * 256 * 4 * 2;
constexpr uint32_t kMaxInputEvents = 1024;

inline uint64_t rect_pixels(const Rect& r) {
    return uint64_t(uint32_t(r.right - r.left)) * uint32_t(r.bottom - r.top);
}

// Largest frame payload we accept for a given mode.
uint64_t max_frame_payload(uint32_t width, uint32_t height);

// A parsed frame. Pointers point into the payload buffer.
struct FrameView {
    uint32_t seq = 0;
    const MoveRect* moves = nullptr;
    uint32_t moveCount = 0;
    const Rect* rects = nullptr;
    uint32_t rectCount = 0;
    const uint8_t* pixels = nullptr;  // rect 0 rows, then rect 1 rows... (w*4 bytes per row)
    uint64_t pixelBytes = 0;
};

bool rect_valid(const Rect& r, uint32_t width, uint32_t height);
bool parse_frame(const uint8_t* p, size_t n, uint32_t width, uint32_t height, FrameView& out);
bool parse_pointer_shape(const uint8_t* p, size_t n, PointerShapeMsg& hdr, const uint8_t*& data);
// Bytes of shape data a pointer header implies, 0 if the header is invalid.
uint64_t pointer_shape_bytes(const PointerShapeMsg& h);
bool parse_input(const uint8_t* p, size_t n, const InputEvent*& events, size_t& count);
bool input_event_valid(const InputEvent& e);

// Reference (CPU) application of a frame to a BGRA image of width*height.
// Used by tests to check the GPU path.
void apply_frame_cpu(uint8_t* image, uint32_t width, uint32_t height, const FrameView& f);

void fill_name(char (&dst)[64], const char* utf8);

}  // namespace wv
