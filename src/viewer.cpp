#include "viewer.h"

#include <cstring>

namespace wv {

namespace {
constexpr size_t kMaxQueuedFrames = 3;  // then stop reading: TCP pushes back on the host
}

void ViewerConn::start(const std::string& host, uint16_t port, bool wantControl, EventFn fn) {
    stop();
    onEvent_ = std::move(fn);
    stopping_ = false;
    posted_ = false;
    bytes_ = 0;
    {
        std::lock_guard<std::mutex> lock(mu_);
        error_.clear();
        remoteName_.clear();
        remoteAddr_.clear();
        queue_.clear();
        queuedFrames_ = 0;
        input_.clear();
        wantFull_ = false;
    }
    thread_ = std::thread([this, host, port, wantControl] { run(host, port, wantControl); });
}

void ViewerConn::stop(const std::function<void(std::thread&)>& join) {
    if (!thread_.joinable()) return;
    stopping_ = true;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (sock_ != INVALID_SOCKET) shutdown(sock_, SD_BOTH);
    }
    cv_.notify_all();
    if (join)
        join(thread_);
    else
        thread_.join();
}

std::string ViewerConn::error() const {
    std::lock_guard<std::mutex> lock(mu_);
    return error_;
}
std::string ViewerConn::remote_name() const {
    std::lock_guard<std::mutex> lock(mu_);
    return remoteName_;
}
std::string ViewerConn::remote_address() const {
    std::lock_guard<std::mutex> lock(mu_);
    return remoteAddr_;
}

void ViewerConn::fail(const std::string& msg) {
    std::lock_guard<std::mutex> lock(mu_);
    if (error_.empty()) error_ = msg;
}

void ViewerConn::take(std::deque<Packet>& out) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto& p : queue_) out.push_back(std::move(p));
        queue_.clear();
        queuedFrames_ = 0;
        posted_ = false;
    }
    cv_.notify_all();
}

void ViewerConn::recycle(std::vector<uint8_t>&& buf) {
    std::lock_guard<std::mutex> lock(mu_);
    if (pool_.size() < 4 && buf.capacity() > 0) pool_.push_back(std::move(buf));
}

void ViewerConn::send_input(const InputEvent* events, size_t count) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        input_.insert(input_.end(), events, events + count);
    }
    cv_.notify_all();
}

void ViewerConn::request_full() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        wantFull_ = true;
    }
    cv_.notify_all();
}

void ViewerConn::run(std::string host, uint16_t port, bool wantControl) {
    std::string err;
    SOCKET s = tcp_connect(host, port, 5000, err);
    if (s == INVALID_SOCKET) {
        fail(err);
        if (onEvent_) onEvent_(Event::Failed);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        sock_ = s;
        remoteAddr_ = peer_address(s);
    }
    if (stopping_) shutdown(s, SD_BOTH);

    Hello hello{};
    std::memcpy(hello.magic, kMagic, 4);
    hello.version = kProtocolVersion;
    hello.flags = wantControl ? kHelloWantControl : 0;
    fill_name(hello.name, computer_name_utf8().c_str());
    Welcome w{};
    // The host may be waiting for its user to click Yes: give them time.
    set_recv_timeout(s, 60000);
    bool ok = send_all(s, &hello, sizeof hello) && recv_all(s, &w, sizeof w);
    if (!ok || std::memcmp(w.magic, kMagic, 4) != 0) {
        fail(stopping_ ? "Cancelled" : "The other PC did not answer (is WireView sharing there, and allowed through the firewall?)");
    } else if (w.status == kWelcomeRejected) {
        fail("The other PC declined the connection");
        ok = false;
    } else if (w.status == kWelcomeBusy) {
        fail("The other PC is already being viewed");
        ok = false;
    } else if (w.status == kWelcomeBadVersion || w.version != kProtocolVersion) {
        fail("The other PC runs a different WireView version");
        ok = false;
    } else if (w.status != kWelcomeOk) {
        fail("Unexpected answer from the other PC");
        ok = false;
    }
    if (!ok) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            sock_ = INVALID_SOCKET;
        }
        closesocket(s);
        if (onEvent_) onEvent_(Event::Failed);
        return;
    }
    set_recv_timeout(s, 0);
    {
        std::lock_guard<std::mutex> lock(mu_);
        remoteName_.assign(w.name, strnlen(w.name, sizeof w.name));
    }
    grantedAtStart_ = (w.flags & kWelcomeControlGranted) != 0;
    if (onEvent_) onEvent_(Event::Connected);

    sender_ = std::thread([this] { send_loop(); });
    recv_loop();

    stopping_ = true;
    {
        std::lock_guard<std::mutex> lock(mu_);
        shutdown(sock_, SD_BOTH);
    }
    cv_.notify_all();
    sender_.join();
    {
        std::lock_guard<std::mutex> lock(mu_);
        sock_ = INVALID_SOCKET;
    }
    closesocket(s);
    if (onEvent_) onEvent_(Event::Closed);
}

void ViewerConn::recv_loop() {
    SOCKET s = sock_;
    uint32_t width = 0, height = 0;
    for (;;) {
        MsgHeader h;
        if (!recv_all(s, &h, sizeof h)) {
            if (!stopping_) fail("The connection was closed");
            return;
        }
        uint64_t limit = 0;
        switch (h.type) {
            case kMsgMode: limit = sizeof(ModeMsg); break;
            case kMsgFrame: limit = width ? max_frame_payload(width, height) : 0; break;
            case kMsgPointerPos: limit = sizeof(PointerPosMsg); break;
            case kMsgPointerShape: limit = sizeof(PointerShapeMsg) + kMaxPointerBytes; break;
            case kMsgControl: limit = sizeof(ControlMsg); break;
            default: break;
        }
        if (limit == 0 || h.size > limit) {
            fail("Protocol error (message " + std::to_string(h.type) + ", " + std::to_string(h.size) + " bytes)");
            return;
        }

        Packet p;
        p.type = h.type;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (!pool_.empty()) {
                p.data = std::move(pool_.back());
                pool_.pop_back();
            }
        }
        p.data.resize(h.size);
        if (h.size && !recv_all(s, p.data.data(), h.size)) {
            if (!stopping_) fail("The connection was closed");
            return;
        }
        bytes_ += sizeof h + h.size;

        bool valid = true;
        if (h.type == kMsgMode) {
            ModeMsg m{};
            valid = h.size == sizeof m;
            if (valid) std::memcpy(&m, p.data.data(), sizeof m);
            valid = valid && m.format == kFormatBGRA8 && m.width && m.height && m.width <= kMaxDimension &&
                    m.height <= kMaxDimension;
            if (valid) {
                width = m.width;
                height = m.height;
            }
        } else if (h.type == kMsgFrame) {
            FrameView f;
            valid = parse_frame(p.data.data(), p.data.size(), width, height, f);
        } else if (h.type == kMsgPointerShape) {
            PointerShapeMsg ps;
            const uint8_t* d;
            valid = parse_pointer_shape(p.data.data(), p.data.size(), ps, d);
        } else {
            valid = h.size == limit;
        }
        if (!valid) {
            fail("Protocol error (bad message " + std::to_string(h.type) + ")");
            return;
        }

        {
            std::unique_lock<std::mutex> lock(mu_);
            if (h.type == kMsgFrame) {
                cv_.wait(lock, [this] { return queuedFrames_ < kMaxQueuedFrames || stopping_; });
                if (stopping_) return;
                ++queuedFrames_;
            }
            queue_.push_back(std::move(p));
        }
        if (!posted_.exchange(true) && onEvent_) onEvent_(Event::Packets);
    }
}

void ViewerConn::send_loop() {
    std::vector<InputEvent> batch;
    for (;;) {
        bool full = false;
        {
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait(lock, [this] { return stopping_ || !input_.empty() || wantFull_; });
            if (stopping_) return;
            batch.swap(input_);
            input_.clear();
            full = wantFull_;
            wantFull_ = false;
        }
        SOCKET s = sock_;
        for (size_t i = 0; i < batch.size(); i += kMaxInputEvents) {
            const size_t n = std::min<size_t>(kMaxInputEvents, batch.size() - i);
            if (!send_msg(s, kMsgInput, &batch[i], uint32_t(n * sizeof(InputEvent)))) return;
        }
        batch.clear();
        if (full && !send_msg(s, kMsgRequestFull, nullptr, 0)) return;
    }
}

}  // namespace wv
