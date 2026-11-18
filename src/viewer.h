// The viewing side's network half: connects, receives the screen, sends input.
// The UI thread pulls received messages with take() and draws them.
#pragma once
#include <winsock2.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "net.h"

namespace wv {

class ViewerConn {
public:
    enum class Event { Connected, Failed, Closed, Packets };
    // Called on worker threads; the UI turns it into a PostMessage.
    using EventFn = std::function<void(Event)>;

    struct Packet {
        uint16_t type = 0;
        std::vector<uint8_t> data;
    };

    ~ViewerConn() { stop(); }

    void start(const std::string& host, uint16_t port, bool wantControl, EventFn fn);
    void stop(const std::function<void(std::thread&)>& join = nullptr);
    bool active() const { return thread_.joinable(); }

    // UI thread: grab everything received so far, in order.
    void take(std::deque<Packet>& out);
    void recycle(std::vector<uint8_t>&& buf);

    void send_input(const InputEvent* events, size_t count);
    void request_full();

    std::string error() const;
    std::string remote_name() const;
    std::string remote_address() const;
    bool control_granted_at_start() const { return grantedAtStart_; }
    uint64_t bytes_received() const { return bytes_; }

private:
    void run(std::string host, uint16_t port, bool wantControl);
    void recv_loop();
    void send_loop();
    void fail(const std::string& msg);

    EventFn onEvent_;
    std::thread thread_, sender_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> grantedAtStart_{false};
    std::atomic<uint64_t> bytes_{0};

    mutable std::mutex mu_;
    std::condition_variable cv_;
    SOCKET sock_ = INVALID_SOCKET;
    std::string error_, remoteName_, remoteAddr_;
    std::deque<Packet> queue_;
    size_t queuedFrames_ = 0;
    std::vector<std::vector<uint8_t>> pool_;
    std::atomic<bool> posted_{false};

    std::vector<InputEvent> input_;
    bool wantFull_ = false;
};

}  // namespace wv
