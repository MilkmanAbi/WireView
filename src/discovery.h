// Finding sharing PCs: a UDP broadcast on every IPv4 adapter (a USB4 link
// gets a 169.254.x.x address on both ends, so a directed broadcast reaches
// the other PC), answered by hosts that are sharing.
#pragma once
#include <winsock2.h>

#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "protocol.h"

namespace wv {

#pragma pack(push, 1)
struct DiscoveryProbe {
    char magic[4];  // "WVW?"
    uint16_t version;
};
struct DiscoveryReply {
    char magic[4];  // "WVW!"
    uint16_t version;
    uint16_t tcpPort;
    uint32_t flags;  // kDiscoveryControlAllowed
    char name[64];
};
#pragma pack(pop)
constexpr uint32_t kDiscoveryControlAllowed = 1;

struct Peer {
    std::string name;
    std::string address;
    uint16_t port = kTcpPort;
    bool controlAllowed = false;
    std::string adapter;  // local adapter it was found on
    bool directLink = false;
    uint64_t linkBps = 0;
};

// Blocks for about waitMs.
std::vector<Peer> discover_peers(int waitMs);

class DiscoveryResponder {
public:
    using InfoFn = std::function<DiscoveryReply()>;
    ~DiscoveryResponder() { stop(); }
    bool start(InfoFn info);
    void stop();

private:
    void loop();
    SOCKET sock_ = INVALID_SOCKET;
    std::thread thread_;
    std::atomic<bool> stopping_{false};
    InfoFn info_;
};

}  // namespace wv
