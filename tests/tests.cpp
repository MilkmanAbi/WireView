// WireView tests. Plain asserts, no framework.
//   wireview_tests            unit tests + GPU apply check (WARP) + loopback session
//   wireview_tests --bench    raw TCP loopback throughput and capture speed
// No test ever injects real input: the host side uses a recording sink.
#include <winsock2.h>
#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "capture.h"
#include "cursor.h"
#include "discovery.h"
#include "host.h"
#include "inject.h"
#include "net.h"
#include "protocol.h"
#include "render.h"
#include "viewer.h"

using namespace wv;

static int g_failures = 0, g_checks = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        ++g_checks;                                                       \
        if (!(cond)) {                                                    \
            ++g_failures;                                                 \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        }                                                                 \
    } while (0)

// ---------------------------------------------------------------- frame building

struct FrameBuilder {
    std::vector<MoveRect> moves;
    std::vector<Rect> rects;
    std::vector<uint8_t> pixels;
    std::vector<uint8_t> build(uint32_t seq = 0) const {
        std::vector<uint8_t> out(sizeof(FrameMsg) + moves.size() * sizeof(MoveRect) + rects.size() * sizeof(Rect));
        FrameMsg f{seq, uint16_t(moves.size()), uint16_t(rects.size())};
        std::memcpy(out.data(), &f, sizeof f);
        if (!moves.empty()) std::memcpy(out.data() + sizeof f, moves.data(), moves.size() * sizeof(MoveRect));
        if (!rects.empty())
            std::memcpy(out.data() + sizeof f + moves.size() * sizeof(MoveRect), rects.data(), rects.size() * sizeof(Rect));
        out.insert(out.end(), pixels.begin(), pixels.end());
        return out;
    }
    void add_rect(Rect r, std::mt19937& rng) {
        rects.push_back(r);
        const size_t n = size_t(rect_pixels(r)) * 4;
        for (size_t i = 0; i < n; ++i) pixels.push_back(uint8_t(rng()));
    }
};

static void test_parse_frame() {
    std::printf("parse_frame\n");
    std::mt19937 rng(1);
    const uint32_t W = 64, H = 48;
    FrameBuilder b;
    b.moves.push_back({0, 0, {10, 10, 30, 20}});
    b.add_rect({0, 0, 8, 4}, rng);
    b.add_rect({60, 40, 64, 48}, rng);
    auto buf = b.build(7);
    FrameView f;
    CHECK(parse_frame(buf.data(), buf.size(), W, H, f));
    CHECK(f.seq == 7 && f.moveCount == 1 && f.rectCount == 2 && f.pixelBytes == (32 + 32) * 4);

    CHECK(!parse_frame(buf.data(), buf.size() - 1, W, H, f));             // truncated
    auto longer = buf;
    longer.push_back(0);
    CHECK(!parse_frame(longer.data(), longer.size(), W, H, f));            // trailing bytes
    CHECK(!parse_frame(buf.data(), 4, W, H, f));                           // shorter than header
    CHECK(!parse_frame(buf.data(), buf.size(), 63, H, f));                 // rect outside a smaller mode
    CHECK(!parse_frame(buf.data(), buf.size(), 0, 0, f));                  // no mode yet

    FrameBuilder bad = b;
    bad.moves[0] = {50, 0, {10, 10, 30, 20}};                              // source runs off the right edge
    auto bb = bad.build();
    CHECK(!parse_frame(bb.data(), bb.size(), W, H, f));
    bad = b;
    bad.rects[0] = {5, 5, 5, 9};                                           // empty rect
    bb = bad.build();
    CHECK(!parse_frame(bb.data(), bb.size(), W, H, f));
    bad = b;
    bad.rects[0] = {-1, 0, 7, 4};                                          // negative
    bb = bad.build();
    CHECK(!parse_frame(bb.data(), bb.size(), W, H, f));

    // Huge counts must not read past the buffer.
    std::vector<uint8_t> evil(sizeof(FrameMsg));
    FrameMsg em{0, 0xFFFF, 0xFFFF};
    std::memcpy(evil.data(), &em, sizeof em);
    CHECK(!parse_frame(evil.data(), evil.size(), W, H, f));

    // Rect size overflow: area * 4 must not wrap.
    FrameBuilder big;
    big.rects.push_back({0, 0, int32_t(kMaxDimension), int32_t(kMaxDimension)});
    auto bigBuf = big.build();
    CHECK(!parse_frame(bigBuf.data(), bigBuf.size(), kMaxDimension, kMaxDimension, f));
}

static void test_cpu_apply() {
    std::printf("apply_frame_cpu\n");
    const uint32_t W = 8, H = 4;
    std::vector<uint8_t> img(W * H * 4);
    for (size_t i = 0; i < img.size(); i += 4) img[i] = uint8_t(i / 4);  // blue channel = pixel index
    // Overlapping move: shift row 0 columns 0..5 right by 2.
    FrameBuilder b;
    b.moves.push_back({0, 0, {2, 0, 8, 1}});
    auto buf = b.build();
    FrameView f;
    CHECK(parse_frame(buf.data(), buf.size(), W, H, f));
    apply_frame_cpu(img.data(), W, H, f);
    const uint8_t expect[8] = {0, 1, 0, 1, 2, 3, 4, 5};
    bool ok = true;
    for (int x = 0; x < 8; ++x) ok &= img[x * 4] == expect[x];
    CHECK(ok);
}

static void test_input_parse() {
    std::printf("parse_input\n");
    InputEvent ev[3] = {{kInMove, 0, 0, 10, 20}, {kInKey, kKeyUp | kKeyExtended, 0x1D, 0, 0}, {kInButton, 4, 1, 0, 0}};
    const InputEvent* out = nullptr;
    size_t n = 0;
    CHECK(parse_input(reinterpret_cast<uint8_t*>(ev), sizeof ev, out, n) && n == 3);
    CHECK(!parse_input(reinterpret_cast<uint8_t*>(ev), sizeof ev - 1, out, n));
    CHECK(!parse_input(reinterpret_cast<uint8_t*>(ev), 0, out, n));
    InputEvent bad = {9, 0, 0, 0, 0};
    CHECK(!parse_input(reinterpret_cast<uint8_t*>(&bad), sizeof bad, out, n));
    bad = {kInButton, 5, 1, 0, 0};
    CHECK(!parse_input(reinterpret_cast<uint8_t*>(&bad), sizeof bad, out, n));
    bad = {kInKey, 0, 0, 0, 0};  // scancode 0
    CHECK(!parse_input(reinterpret_cast<uint8_t*>(&bad), sizeof bad, out, n));
    bad = {kInKey, 0x80, 0x1E, 0, 0};  // unknown flag
    CHECK(!parse_input(reinterpret_cast<uint8_t*>(&bad), sizeof bad, out, n));
    std::vector<InputEvent> many(kMaxInputEvents + 1, InputEvent{kInMove, 0, 0, 0, 0});
    CHECK(!parse_input(reinterpret_cast<uint8_t*>(many.data()), many.size() * sizeof(InputEvent), out, n));
}

static void test_normalize() {
    std::printf("normalize_coord\n");
    bool ok = true;
    for (LONG size : {1024L, 1920L, 2496L, 3840L, 5760L, 7680L}) {
        for (LONG p = 0; p < size; ++p) {
            const LONG n = normalize_coord(p, size);
            ok &= n >= 0 && n <= 65535;
            ok &= LONG((int64_t(n) * size) / 65536) == p;  // Windows' mapping back to pixels
        }
    }
    CHECK(ok);
    CHECK(normalize_coord(-5, 1920) == 0);
    CHECK(normalize_coord(5000, 1920) == normalize_coord(1919, 1920));
}

struct RecordingSink : InputSink {
    std::mutex mu;
    std::vector<INPUT> got;
    void send(const INPUT* in, UINT n) override {
        std::lock_guard<std::mutex> lock(mu);
        got.insert(got.end(), in, in + n);
    }
    size_t count() {
        std::lock_guard<std::mutex> lock(mu);
        return got.size();
    }
};

static void test_injector() {
    std::printf("Injector\n");
    RecordingSink sink;
    Injector inj(sink);
    // Second monitor to the right of a 1920-wide primary.
    inj.set_target(RECT{1920, 0, 1920 + 2496, 1664}, RECT{0, 0, 1920 + 2496, 1664});
    InputEvent ev[] = {
        {kInMove, 0, 0, 100, 50},
        {kInKey, 0, 0x1D, 0, 0},                   // LCtrl down
        {kInKey, kKeyExtended, 0x1D, 0, 0},        // RCtrl down
        {kInButton, 0, 1, 10, 10},                 // left down
        {kInButton, 4, 1, 10, 10},                 // X2 down
        {kInWheel, 0, 0, -120, 0},
        {kInKey, kKeyUp, 0x1D, 0, 0},              // LCtrl up
    };
    inj.apply(ev, std::size(ev));
    CHECK(sink.got.size() == 7);
    if (sink.got.size() == 7) {
        const INPUT& m = sink.got[0];
        CHECK(m.type == INPUT_MOUSE && (m.mi.dwFlags & MOUSEEVENTF_ABSOLUTE) && (m.mi.dwFlags & MOUSEEVENTF_VIRTUALDESK));
        CHECK(LONG((int64_t(m.mi.dx) * (1920 + 2496)) / 65536) == 1920 + 100);
        CHECK(LONG((int64_t(m.mi.dy) * 1664) / 65536) == 50);
        CHECK(sink.got[1].ki.wScan == 0x1D && (sink.got[1].ki.dwFlags & KEYEVENTF_SCANCODE) &&
              !(sink.got[1].ki.dwFlags & KEYEVENTF_EXTENDEDKEY) && !(sink.got[1].ki.dwFlags & KEYEVENTF_KEYUP));
        CHECK(sink.got[2].ki.dwFlags & KEYEVENTF_EXTENDEDKEY);
        CHECK(sink.got[3].mi.dwFlags & MOUSEEVENTF_LEFTDOWN);
        CHECK((sink.got[4].mi.dwFlags & MOUSEEVENTF_XDOWN) && sink.got[4].mi.mouseData == XBUTTON2);
        CHECK((sink.got[5].mi.dwFlags & MOUSEEVENTF_WHEEL) && LONG(sink.got[5].mi.mouseData) == -120);
        CHECK(sink.got[6].ki.dwFlags & KEYEVENTF_KEYUP);
    }
    // Still held: RCtrl, left button, X2. release_all must let go of exactly those.
    sink.got.clear();
    inj.release_all();
    int keyUps = 0, leftUp = 0, xUp = 0;
    for (const INPUT& in : sink.got) {
        if (in.type == INPUT_KEYBOARD && (in.ki.dwFlags & KEYEVENTF_KEYUP) && in.ki.wScan == 0x1D &&
            (in.ki.dwFlags & KEYEVENTF_EXTENDEDKEY))
            ++keyUps;
        if (in.type == INPUT_MOUSE && (in.mi.dwFlags & MOUSEEVENTF_LEFTUP)) ++leftUp;
        if (in.type == INPUT_MOUSE && (in.mi.dwFlags & MOUSEEVENTF_XUP) && in.mi.mouseData == XBUTTON2) ++xUp;
    }
    CHECK(sink.got.size() == 3 && keyUps == 1 && leftUp == 1 && xUp == 1);
    sink.got.clear();
    inj.release_all();
    CHECK(sink.got.empty());
}

static void test_pointer() {
    std::printf("pointer shapes\n");
    // 2x2 monochrome: AND rows then XOR rows.  Pixels: transparent, black / white, invert.
    PointerShapeMsg h{kPointerMonochrome, 2, 4, 1, 0, 0};
    const uint8_t data[4] = {0b10000000, 0b01000000, 0b00000000, 0b11000000};
    // AND row0: 1,0  AND row1: 0,1  XOR row0: 0,0  XOR row1: 1,1
    uint32_t w = 0, hh = 0;
    auto px = pointer_to_bgra(h, data, w, hh);
    CHECK(w == 2 && hh == 2 && px.size() == 16);
    if (px.size() == 16) {
        CHECK(px[3] == 0);                       // AND1 XOR0: transparent
        CHECK(px[4] == 0 && px[7] == 0xFF);      // AND0 XOR0: black
        CHECK(px[8] == 0xFF && px[11] == 0xFF);  // AND0 XOR1: white
        CHECK(px[12] == 0x80 && px[15] == 0xFF); // AND1 XOR1: invert -> grey
    }
    PointerShapeMsg odd{kPointerMonochrome, 2, 3, 1, 0, 0};
    CHECK(pointer_shape_bytes(odd) == 0);
    PointerShapeMsg huge{kPointerColor, 4096, 4096, 16384, 0, 0};
    CHECK(pointer_shape_bytes(huge) == 0);
    PointerShapeMsg shortPitch{kPointerColor, 32, 32, 64, 0, 0};
    CHECK(pointer_shape_bytes(shortPitch) == 0);
    HCURSOR c = pointer_to_hcursor(h, data);
    CHECK(c != nullptr);
    if (c) DestroyCursor(c);
}

static void test_fit() {
    std::printf("fit_rect\n");
    RECT r = fit_rect(1920, 1080, 1920, 1080);
    CHECK(r.left == 0 && r.top == 0 && r.right == 1920 && r.bottom == 1080);
    r = fit_rect(2496, 1664, 1000, 1000);  // wide image in a square: bars top/bottom
    CHECK(r.left == 0 && r.right == 1000 && r.top > 0 && (r.bottom - r.top) == 667);
    r = fit_rect(0, 0, 100, 100);
    CHECK(r.right == 0);
}

// GPU apply (moves through a temp texture, UpdateSubresource rects) must equal the CPU reference.
static void test_gpu_apply() {
    std::printf("GpuImage on WARP vs CPU reference\n");
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr,
                                 &ctx))) {
        std::printf("  skipped: no WARP device\n");
        return;
    }
    const uint32_t W = 200, H = 120;
    GpuImage img;
    img.init(dev.Get(), ctx.Get());
    std::string err;
    CHECK(img.set_mode(W, H, err));
    std::vector<uint8_t> ref(W * H * 4, 0);
    std::mt19937 rng(42);
    auto rnd = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };

    // Full first frame, then 40 random frames with moves and dirty rects.
    FrameBuilder first;
    first.add_rect({0, 0, int32_t(W), int32_t(H)}, rng);
    auto buf = first.build();
    FrameView f;
    CHECK(parse_frame(buf.data(), buf.size(), W, H, f));
    img.apply(f);
    apply_frame_cpu(ref.data(), W, H, f);
    for (int i = 0; i < 40; ++i) {
        FrameBuilder b;
        for (int m = rnd(0, 3); m > 0; --m) {
            const int w = rnd(1, 80), h = rnd(1, 60);
            const int dx = rnd(0, W - w), dy = rnd(0, H - h);
            // Small offsets so source and destination overlap, like scrolling.
            const int sx = std::clamp(dx + rnd(-10, 10), 0, int(W) - w), sy = std::clamp(dy + rnd(-10, 10), 0, int(H) - h);
            b.moves.push_back({sx, sy, {dx, dy, dx + w, dy + h}});
        }
        for (int r = rnd(0, 5); r > 0; --r) {
            const int w = rnd(1, 50), h = rnd(1, 40);
            const int x = rnd(0, W - w), y = rnd(0, H - h);
            b.add_rect({x, y, x + w, y + h}, rng);
        }
        buf = b.build(uint32_t(i));
        if (!parse_frame(buf.data(), buf.size(), W, H, f)) {
            CHECK(false);
            continue;
        }
        img.apply(f);
        apply_frame_cpu(ref.data(), W, H, f);
    }
    std::vector<uint8_t> gpu;
    CHECK(img.read_back(gpu));
    CHECK(gpu == ref);
}

// ---------------------------------------------------------------- loopback session

struct TestViewer {
    ViewerConn conn;
    std::mutex mu;
    std::condition_variable cv;
    std::vector<ViewerConn::Event> events;

    void start(uint16_t port, bool wantControl) {
        conn.start("127.0.0.1", port, wantControl, [this](ViewerConn::Event e) {
            std::lock_guard<std::mutex> lock(mu);
            events.push_back(e);
            cv.notify_all();
        });
    }
    // Waits for an event of the given kind.
    bool wait_for(ViewerConn::Event e, int ms) {
        std::unique_lock<std::mutex> lock(mu);
        return cv.wait_for(lock, std::chrono::milliseconds(ms), [&] {
            for (auto x : events)
                if (x == e) return true;
            return false;
        });
    }
};

static void test_loopback() {
    std::printf("loopback session (real screen capture, recorded input)\n");
    const uint16_t port = 47190;
    RecordingSink sink;
    Host host(sink);
    std::atomic<bool> acceptNext{false};
    host.set_accept([&](const std::string&, const std::string&, bool) { return acceptNext.load(); });
    std::string err;
    if (!host.start(port, err)) {
        std::printf("  cannot listen: %s\n", err.c_str());
        CHECK(false);
        return;
    }

    // 1. A rejected viewer gets a clear error.
    {
        TestViewer v;
        v.start(port, false);
        CHECK(v.wait_for(ViewerConn::Event::Failed, 5000));
        CHECK(v.conn.error().find("declined") != std::string::npos);
        v.conn.stop();
    }

    // 2. Accepted, control not allowed: screen arrives, input is ignored.
    acceptNext = true;
    host.set_allow_control(false);
    TestViewer v;
    v.start(port, true);
    CHECK(v.wait_for(ViewerConn::Event::Connected, 5000));
    CHECK(!v.conn.control_granted_at_start());

    uint32_t w = 0, h = 0;
    std::vector<uint8_t> image;
    int fullFrames = 0, frames = 0, bad = 0;
    bool gotGrant = false, grantValue = false;
    uint64_t pixelBytes = 0;
    auto pump = [&](int ms) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < end) {
            std::deque<ViewerConn::Packet> q;
            v.conn.take(q);
            for (auto& p : q) {
                if (p.type == kMsgMode) {
                    ModeMsg m;
                    std::memcpy(&m, p.data.data(), sizeof m);
                    w = m.width;
                    h = m.height;
                    image.assign(size_t(w) * h * 4, 0);
                } else if (p.type == kMsgFrame) {
                    FrameView f;
                    if (!parse_frame(p.data.data(), p.data.size(), w, h, f)) {
                        ++bad;
                        continue;
                    }
                    ++frames;
                    pixelBytes += f.pixelBytes;
                    if (f.rectCount == 1 && f.moveCount == 0 && rect_pixels(f.rects[0]) == uint64_t(w) * h) ++fullFrames;
                    apply_frame_cpu(image.data(), w, h, f);
                } else if (p.type == kMsgControl) {
                    gotGrant = true;
                    ControlMsg c;
                    std::memcpy(&c, p.data.data(), sizeof c);
                    grantValue = c.granted != 0;
                }
                v.conn.recycle(std::move(p.data));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    };
    pump(1500);
    std::printf("  mode %ux%u, %d frames (%d full), %d bad\n", w, h, frames, fullFrames, bad);
    CHECK(w > 0 && h > 0);
    CHECK(fullFrames >= 1);
    CHECK(bad == 0);
    // Some pixel must be non-zero: we really captured something.
    bool nonZero = false;
    for (size_t i = 0; i < image.size() && !nonZero; i += 4) nonZero = image[i] | image[i + 1] | image[i + 2];
    CHECK(nonZero);

    // A full refresh works even on a static desktop (served from the GPU mirror).
    const int fullBefore = fullFrames;
    v.conn.request_full();
    pump(1000);
    CHECK(fullFrames > fullBefore);

    InputEvent move{kInMove, 0, 0, 5, 5};
    v.conn.send_input(&move, 1);
    pump(300);
    CHECK(sink.count() == 0);  // not allowed: nothing injected

    // 3. Allow control mid-session: the viewer is told, input flows to the sink.
    host.set_allow_control(true);
    v.conn.request_full();  // nudges the capture loop so it notices immediately
    pump(800);
    CHECK(gotGrant && grantValue);
    InputEvent key{kInKey, 0, 0x1E, 0, 0};
    v.conn.send_input(&key, 1);
    pump(300);
    CHECK(sink.count() == 1);

    // 4. A malformed message drops the viewer, and held keys are released on the host.
    {
        RecordingSink& s = sink;
        const size_t before = s.count();
        // Send garbage straight on a second connection is not possible (one session); instead
        // disconnect cleanly and check the host released the held 'A'.
        v.conn.stop();
        for (int i = 0; i < 100 && host.has_viewer(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        CHECK(!host.has_viewer());
        bool released = false;
        {
            std::lock_guard<std::mutex> lock(s.mu);
            for (size_t i = before; i < s.got.size(); ++i)
                released |= s.got[i].type == INPUT_KEYBOARD && s.got[i].ki.wScan == 0x1E &&
                            (s.got[i].ki.dwFlags & KEYEVENTF_KEYUP);
        }
        CHECK(released);
    }

    // 5. Raw protocol abuse: unknown message type after the handshake -> host drops us.
    {
        SOCKET s = tcp_connect("127.0.0.1", port, 3000, err);
        CHECK(s != INVALID_SOCKET);
        if (s != INVALID_SOCKET) {
            Hello hello{};
            std::memcpy(hello.magic, kMagic, 4);
            hello.version = kProtocolVersion;
            Welcome wel{};
            CHECK(send_all(s, &hello, sizeof hello) && recv_all(s, &wel, sizeof wel) && wel.status == kWelcomeOk);
            MsgHeader junk{999, 0, 0};
            send_all(s, &junk, sizeof junk);
            // Drain until the host closes the connection.
            set_recv_timeout(s, 5000);
            std::vector<char> sinkBuf(1 << 20);
            int n;
            while ((n = recv(s, sinkBuf.data(), int(sinkBuf.size()), 0)) > 0) {
            }
            CHECK(n == 0);  // orderly close by the host, not a timeout
            closesocket(s);
        }
    }
    host.stop();
}

static void test_discovery() {
    std::printf("discovery (informational)\n");
    DiscoveryResponder r;
    if (!r.start([] {
            DiscoveryReply x{};
            fill_name(x.name, "selftest");
            x.tcpPort = 1234;
            return x;
        })) {
        std::printf("  UDP %u busy (WireView running?), skipped\n", kUdpPort);
        return;
    }
    auto peers = discover_peers(500);
    bool self = false;
    for (auto& p : peers) {
        std::printf("  found %s at %s:%u adapter '%s'%s\n", p.name.c_str(), p.address.c_str(), p.port, p.adapter.c_str(),
                    p.directLink ? " [direct]" : "");
        self |= p.name == "selftest" && p.port == 1234;
    }
    if (!self) std::printf("  (did not hear itself; normal with no active network adapter)\n");
    r.stop();
}

// ---------------------------------------------------------------- bench

static void bench() {
    std::printf("bench: raw TCP loopback, gather send of full frames row by row\n");
    std::string err;
    SOCKET ls = tcp_listen(47191, err);
    if (ls == INVALID_SOCKET) {
        std::printf("  %s\n", err.c_str());
        return;
    }
    const uint32_t W = 2496, H = 1664, pitch = 2560 * 4;  // padded pitch, like a mapped texture
    std::vector<uint8_t> tex(size_t(pitch) * H, 0x5A);
    const int kFrames = 120;
    std::thread rx([&] {
        SOCKET s = accept(ls, nullptr, nullptr);
        std::vector<uint8_t> buf(size_t(W) * H * 4);
        for (int i = 0; i < kFrames; ++i)
            if (!recv_all(s, buf.data(), buf.size())) break;
        closesocket(s);
    });
    SOCKET s = tcp_connect("127.0.0.1", 47191, 3000, err);
    std::vector<WSABUF> bufs(H);
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kFrames; ++i) {
        for (uint32_t y = 0; y < H; ++y) bufs[y] = {W * 4, reinterpret_cast<char*>(tex.data() + size_t(y) * pitch)};
        if (!send_gather(s, bufs.data(), bufs.size())) break;
    }
    rx.join();
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const double bytes = double(W) * H * 4 * kFrames;
    std::printf("  %d frames of %ux%u BGRA in %.2f s = %.0f fps, %s\n", kFrames, W, H, secs, kFrames / secs,
                format_bps(bytes * 8 / secs).c_str());
    closesocket(s);
    closesocket(ls);

    std::printf("bench: capture full frames (copy to staging + map)\n");
    Capture cap;
    if (!cap.open(0, err)) {
        std::printf("  %s\n", err.c_str());
        return;
    }
    CaptureJob job;
    int got = 0, slot = 0;
    // The first call waits for DXGI's initial frame; after that a zero timeout serves full
    // frames from the GPU mirror, so this times copy + map only (not waiting for screen updates).
    cap.next(1000, true, slot, job, err);
    cap.unmap(slot);
    uint64_t sum = 0;
    const auto c0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 60; ++i) {
        cap.unmap(slot);
        if (cap.next(0, true, slot, job, err) == Capture::Status::Ok && job.full && job.data) {
            ++got;
            // Touch every row like the socket would.
            for (uint32_t y = 0; y < cap.height(); y += 1) sum += job.data[size_t(y) * job.pitch];
        }
        slot ^= 1;
    }
    if (sum == 1) std::printf(" ");
    const double cs = std::chrono::duration<double>(std::chrono::steady_clock::now() - c0).count();
    std::printf("  %d full %ux%u frames in %.2f s = %.1f ms each\n", got, cap.width(), cap.height(), cs,
                got ? cs * 1000 / got : 0.0);
}

int main(int argc, char** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WinsockInit ws;
    if (argc > 1 && std::strcmp(argv[1], "--bench") == 0) {
        bench();
        return 0;
    }
    test_parse_frame();
    test_cpu_apply();
    test_input_parse();
    test_normalize();
    test_injector();
    test_pointer();
    test_fit();
    test_gpu_apply();
    test_loopback();
    test_discovery();
    std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
