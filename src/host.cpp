#include "host.h"

#include <cstring>

namespace wv {

void join_plain(std::thread& t) {
    if (t.joinable()) t.join();
}

std::vector<uint8_t> Host::make_msg(uint16_t type, const void* payload, uint32_t size) {
    std::vector<uint8_t> v(sizeof(MsgHeader) + size);
    MsgHeader h{type, 0, size};
    std::memcpy(v.data(), &h, sizeof h);
    if (size) std::memcpy(v.data() + sizeof h, payload, size);
    return v;
}

bool Host::start(uint16_t port, std::string& err) {
    if (running()) return true;
    stopping_ = false;
    listen_ = tcp_listen(port, err);
    if (listen_ == INVALID_SOCKET) return false;
    listener_ = std::thread([this] { listen_loop(); });
    return true;
}

void Host::stop(const std::function<void(std::thread&)>& join) {
    if (!running()) return;
    stopping_ = true;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (listen_ != INVALID_SOCKET) {
            closesocket(listen_);  // unblocks accept()
            listen_ = INVALID_SOCKET;
        }
    }
    kill_session();
    if (join)
        join(listener_);
    else
        listener_.join();
}

void Host::disconnect_viewer() { kill_session(); }

void Host::kill_session() {
    dead_ = true;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (session_ != INVALID_SOCKET) shutdown(session_, SD_BOTH);  // unblocks send/recv
    }
    cv_.notify_all();
}

std::string Host::viewer_text() const {
    std::lock_guard<std::mutex> lock(mu_);
    return viewerText_;
}

void Host::listen_loop() {
    while (!stopping_) {
        SOCKET s = accept(listen_, nullptr, nullptr);
        if (s == INVALID_SOCKET) {
            if (stopping_) break;
            Sleep(50);
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(mu_);
            session_ = s;
            dead_ = false;
        }
        if (stopping_) dead_ = true;
        run_session(s);
        {
            std::lock_guard<std::mutex> lock(mu_);
            session_ = INVALID_SOCKET;
        }
        closesocket(s);
    }
}

void Host::push(OutItem&& item) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        // Only the newest pointer position matters; don't let them pile up behind a big frame.
        auto is_pos = [](const OutItem& i) {
            return !i.frame && i.bytes.size() == sizeof(MsgHeader) + sizeof(PointerPosMsg) &&
                   reinterpret_cast<const MsgHeader*>(i.bytes.data())->type == kMsgPointerPos;
        };
        if (!queue_.empty() && is_pos(item) && is_pos(queue_.back()))
            queue_.back() = std::move(item);
        else
            queue_.push_back(std::move(item));
    }
    cv_.notify_all();
}

void Host::run_session(SOCKET s) {
    // Handshake.
    set_recv_timeout(s, 5000);
    Hello hello{};
    if (!recv_all(s, &hello, sizeof hello) || std::memcmp(hello.magic, kMagic, 4) != 0) return;
    Welcome w{};
    std::memcpy(w.magic, kMagic, 4);
    w.version = kProtocolVersion;
    fill_name(w.name, computer_name_utf8().c_str());
    if (hello.version != kProtocolVersion) {
        w.status = kWelcomeBadVersion;
        send_all(s, &w, sizeof w);
        return;
    }
    const std::string name(hello.name, strnlen(hello.name, sizeof hello.name));
    const std::string address = peer_address(s);
    const bool wants = (hello.flags & kHelloWantControl) != 0;
    if (stopping_ || dead_ || (accept_ && !accept_(address, name, wants)) || stopping_) {
        w.status = kWelcomeRejected;
        send_all(s, &w, sizeof w);
        return;
    }
    bool granted = wants && allowControl_;
    w.status = kWelcomeOk;
    w.flags = granted ? kWelcomeControlGranted : 0;
    if (!send_all(s, &w, sizeof w)) return;
    set_recv_timeout(s, 0);

    {
        std::lock_guard<std::mutex> lock(mu_);
        viewerText_ = (name.empty() ? std::string("viewer") : name) + " (" + address + ")";
        queue_.clear();
        for (bool& b : slotBusy_) b = false;
    }
    wantsControl_ = wants;
    fullRequested_ = false;
    viewerActive_ = true;
    if (onEvent_) onEvent_(Event::ViewerConnected, viewer_text());

    std::thread sender([this, s] { send_loop(s); });
    std::thread receiver([this, s] { recv_loop(s); });

    Capture cap;
    CaptureJob job;
    PointerShapeMsg lastShape{};
    std::vector<uint8_t> lastShapeData;
    int slot = 0;
    bool needFull = true;
    uint32_t sentW = 0, sentH = 0, seq = 0;
    std::string err, lastErr;

    // The sender reads mapped staging memory; never unmap/close under it.
    auto wait_slots_idle = [&] {
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait(lock, [&] {
            if (dead_ || stopping_) return true;
            for (bool b : slotBusy_)
                if (b) return false;
            return true;
        });
    };
    auto close_capture = [&] {
        wait_slots_idle();
        if (dead_ || stopping_) return;  // session end path closes after joining the sender
        cap.close();
    };

    while (!dead_ && !stopping_) {
        if (reopen_.exchange(false)) close_capture();
        if (!cap.is_open()) {
            if (!cap.open(monitor_, err)) {
                if (err != lastErr && onEvent_) onEvent_(Event::Error, err);
                lastErr = err;
                std::unique_lock<std::mutex> lock(mu_);
                cv_.wait_for(lock, std::chrono::milliseconds(500), [this] { return dead_ || stopping_; });
                continue;
            }
            lastErr.clear();
            needFull = true;
            const RECT virt{GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
                            GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN),
                            GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN)};
            injector_.set_target(cap.desktop_rect(), virt);
            if (!lastShapeData.empty()) {
                // DXGI only reports the pointer shape when it changes: replay the last one.
                std::vector<uint8_t> payload(sizeof lastShape + lastShapeData.size());
                std::memcpy(payload.data(), &lastShape, sizeof lastShape);
                std::memcpy(payload.data() + sizeof lastShape, lastShapeData.data(), lastShapeData.size());
                push({make_msg(kMsgPointerShape, payload.data(), uint32_t(payload.size()))});
            }
        }
        if (cap.width() != sentW || cap.height() != sentH) {
            ModeMsg m{cap.width(), cap.height(), kFormatBGRA8};
            push({make_msg(kMsgMode, &m, sizeof m)});
            sentW = cap.width();
            sentH = cap.height();
            needFull = true;
        }
        const bool grant = wantsControl_ && allowControl_;
        if (grant != granted) {
            ControlMsg c{grant ? 1u : 0u};
            push({make_msg(kMsgControl, &c, sizeof c)});
            granted = grant;
            if (!grant) injector_.release_all();
        }

        {
            // A slot is busy while the sender streams it from mapped memory.
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait(lock, [&] { return !slotBusy_[slot] || dead_ || stopping_; });
        }
        if (dead_ || stopping_) break;
        cap.unmap(slot);

        const bool full = needFull || fullRequested_.exchange(false);
        switch (cap.next(100, full, slot, job, err)) {
            case Capture::Status::Timeout:
                needFull = full;
                continue;
            case Capture::Status::AccessLost:
                // Mode change, secure desktop, full-screen switch... reopen and resend everything.
                close_capture();
                needFull = true;
                continue;
            case Capture::Status::Error:
                if (onEvent_) onEvent_(Event::Error, err);
                close_capture();
                needFull = true;
                Sleep(200);
                continue;
            case Capture::Status::Ok:
                break;
        }
        if (job.full) needFull = false;

        if (job.pointerShapeChanged) {
            lastShape = job.pointerShape;
            lastShapeData = job.pointerData;
            std::vector<uint8_t> payload(sizeof lastShape + lastShapeData.size());
            std::memcpy(payload.data(), &lastShape, sizeof lastShape);
            std::memcpy(payload.data() + sizeof lastShape, lastShapeData.data(), lastShapeData.size());
            push({make_msg(kMsgPointerShape, payload.data(), uint32_t(payload.size()))});
        }
        if (job.pointerMoved) push({make_msg(kMsgPointerPos, &job.pointerPos, sizeof job.pointerPos)});

        if (job.hasImage) {
            uint64_t pixelBytes = 0;
            for (const Rect& r : job.rects) pixelBytes += rect_pixels(r) * 4;
            const uint32_t meta = uint32_t(sizeof(FrameMsg) + job.moves.size() * sizeof(MoveRect) +
                                           job.rects.size() * sizeof(Rect));
            OutItem item;
            item.frame = true;
            item.bytes.resize(sizeof(MsgHeader) + meta);
            MsgHeader h{kMsgFrame, 0, uint32_t(meta + pixelBytes)};
            FrameMsg f{seq++, uint16_t(job.moves.size()), uint16_t(job.rects.size())};
            uint8_t* p = item.bytes.data();
            std::memcpy(p, &h, sizeof h);
            p += sizeof h;
            std::memcpy(p, &f, sizeof f);
            p += sizeof f;
            if (!job.moves.empty()) std::memcpy(p, job.moves.data(), job.moves.size() * sizeof(MoveRect));
            p += job.moves.size() * sizeof(MoveRect);
            if (!job.rects.empty()) std::memcpy(p, job.rects.data(), job.rects.size() * sizeof(Rect));
            item.slot = job.slot;
            if (item.slot >= 0) {
                std::lock_guard<std::mutex> lock(mu_);
                slotBusy_[item.slot] = true;
            }
            const int used = item.slot;
            item.job = std::move(job);
            push(std::move(item));
            if (used >= 0) slot ^= 1;
        }
    }

    kill_session();
    sender.join();
    receiver.join();
    cap.close();
    injector_.release_all();
    {
        std::lock_guard<std::mutex> lock(mu_);
        queue_.clear();
        viewerText_.clear();
    }
    viewerActive_ = false;
    if (onEvent_) onEvent_(Event::ViewerLeft, {});
}

void Host::send_loop(SOCKET s) {
    std::vector<WSABUF> bufs;
    for (;;) {
        OutItem item;
        {
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait(lock, [this] { return !queue_.empty() || dead_; });
            if (dead_) break;
            item = std::move(queue_.front());
            queue_.pop_front();
        }
        bool ok;
        uint64_t total = item.bytes.size();
        if (!item.frame || item.slot < 0) {
            ok = send_all(s, item.bytes.data(), item.bytes.size());
        } else {
            // Zero copy: every row goes straight from the mapped staging texture to the socket.
            const CaptureJob& j = item.job;
            bufs.clear();
            bufs.push_back({ULONG(item.bytes.size()), reinterpret_cast<char*>(item.bytes.data())});
            for (const Rect& r : j.rects) {
                const ULONG rowBytes = ULONG(r.right - r.left) * 4;
                const uint8_t* base = j.data + size_t(r.top) * j.pitch + size_t(r.left) * 4;
                const uint32_t rows = uint32_t(r.bottom - r.top);
                if (rowBytes == j.pitch) {
                    bufs.push_back({rowBytes * rows, reinterpret_cast<char*>(const_cast<uint8_t*>(base))});
                } else {
                    for (uint32_t y = 0; y < rows; ++y)
                        bufs.push_back({rowBytes, reinterpret_cast<char*>(const_cast<uint8_t*>(base + size_t(y) * j.pitch))});
                }
                total += uint64_t(rowBytes) * rows;
            }
            ok = send_gather(s, bufs.data(), bufs.size());
        }
        if (ok) {
            bytes_ += total;
            if (item.frame) ++frames_;
        }
        if (item.slot >= 0) {
            std::lock_guard<std::mutex> lock(mu_);
            slotBusy_[item.slot] = false;
        }
        cv_.notify_all();
        if (!ok) {
            kill_session();
            break;
        }
    }
}

void Host::recv_loop(SOCKET s) {
    std::vector<uint8_t> buf;
    for (;;) {
        MsgHeader h;
        if (!recv_all(s, &h, sizeof h)) break;
        if (h.type == kMsgInput) {
            if (h.size == 0 || h.size > kMaxInputEvents * sizeof(InputEvent)) break;
            buf.resize(h.size);
            if (!recv_all(s, buf.data(), h.size)) break;
            const InputEvent* ev = nullptr;
            size_t n = 0;
            if (!parse_input(buf.data(), buf.size(), ev, n)) break;  // malformed: drop the viewer
            // Policy is checked per message, so unticking "Allow control" takes effect at once.
            if (wantsControl_ && allowControl_) injector_.apply(ev, n);
        } else if (h.type == kMsgRequestFull) {
            if (h.size != 0) break;
            fullRequested_ = true;
        } else {
            break;  // unknown message: drop the viewer
        }
    }
    kill_session();
}

}  // namespace wv
