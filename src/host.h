// The sharing side: listens, captures the screen, streams it, applies input.
#pragma once
#include <winsock2.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "capture.h"
#include "inject.h"
#include "net.h"

namespace wv {

struct HostStats {
    uint64_t frames = 0;
    uint64_t bytes = 0;
};

class Host {
public:
    enum class Event { ViewerConnected, ViewerLeft, Error };
    // Called on worker threads.
    using EventFn = std::function<void(Event, const std::string&)>;
    // Return true to let this viewer in. Called on the listener thread.
    using AcceptFn = std::function<bool(const std::string& address, const std::string& name, bool wantsControl)>;

    Host(InputSink& sink) : injector_(sink) {}
    ~Host() { stop(); }

    bool start(uint16_t port, std::string& err);
    // join is how to wait for a thread (the UI pumps messages while waiting).
    void stop(const std::function<void(std::thread&)>& join = nullptr);
    bool running() const { return listener_.joinable(); }

    void set_events(EventFn fn) { onEvent_ = std::move(fn); }
    void set_accept(AcceptFn fn) { accept_ = std::move(fn); }
    void set_monitor(int index) { monitor_ = index; reopen_ = true; }
    void set_allow_control(bool allow) { allowControl_ = allow; }
    bool allow_control() const { return allowControl_; }
    void disconnect_viewer();

    bool has_viewer() const { return viewerActive_; }
    std::string viewer_text() const;
    HostStats stats() const { return {frames_.load(), bytes_.load()}; }

private:
    struct OutItem {
        std::vector<uint8_t> bytes;  // complete message (header + payload), or frame header + metadata
        bool frame = false;
        int slot = -1;
        CaptureJob job;  // frame only
    };

    void listen_loop();
    void run_session(SOCKET s);
    void send_loop(SOCKET s);
    void recv_loop(SOCKET s);
    void push(OutItem&& item);
    void kill_session();
    static std::vector<uint8_t> make_msg(uint16_t type, const void* payload, uint32_t size);

    Injector injector_;
    SOCKET listen_ = INVALID_SOCKET;
    std::thread listener_;
    std::atomic<bool> stopping_{false};
    std::atomic<int> monitor_{0};
    std::atomic<bool> reopen_{false};
    std::atomic<bool> allowControl_{false};
    EventFn onEvent_;
    AcceptFn accept_;

    // session state
    mutable std::mutex mu_;
    std::condition_variable cv_;
    SOCKET session_ = INVALID_SOCKET;
    std::string viewerText_;
    std::atomic<bool> viewerActive_{false};
    std::atomic<bool> dead_{false};
    std::atomic<bool> wantsControl_{false};
    std::atomic<bool> fullRequested_{false};
    std::deque<OutItem> queue_;
    bool slotBusy_[Capture::kSlots] = {};
    std::atomic<uint64_t> frames_{0}, bytes_{0};
};

void join_plain(std::thread& t);

}  // namespace wv
