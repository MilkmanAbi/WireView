#include "discovery.h"

#include <ws2tcpip.h>

#include <algorithm>
#include <cstring>

#include "net.h"

namespace wv {

static const char kProbeMagic[4] = {'W', 'V', 'W', '?'};
static const char kReplyMagic[4] = {'W', 'V', 'W', '!'};

std::vector<Peer> discover_peers(int waitMs) {
    std::vector<Peer> peers;
    const std::vector<Adapter> adapters = list_adapters();
    std::vector<SOCKET> socks;
    DiscoveryProbe probe{};
    std::memcpy(probe.magic, kProbeMagic, 4);
    probe.version = kProtocolVersion;

    auto send_from = [&](uint32_t bindAddr, uint32_t dest) {
        SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s == INVALID_SOCKET) return;
        BOOL yes = TRUE;
        setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&yes), sizeof yes);
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = bindAddr;
        if (bind(s, reinterpret_cast<sockaddr*>(&local), sizeof local) != 0) {
            closesocket(s);
            return;
        }
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_port = htons(kUdpPort);
        to.sin_addr.s_addr = dest;
        sendto(s, reinterpret_cast<const char*>(&probe), sizeof probe, 0, reinterpret_cast<sockaddr*>(&to), sizeof to);
        socks.push_back(s);
    };
    // Directed broadcast per adapter: Windows sends 255.255.255.255 out of one interface only.
    for (const Adapter& a : adapters) send_from(a.ipv4, a.ipv4 | ~a.mask);
    send_from(htonl(INADDR_ANY), htonl(INADDR_BROADCAST));

    const ULONGLONG end = GetTickCount64() + ULONGLONG(waitMs);
    for (;;) {
        const ULONGLONG now = GetTickCount64();
        if (now >= end || socks.empty()) break;
        fd_set rs;
        FD_ZERO(&rs);
        for (SOCKET s : socks) FD_SET(s, &rs);
        const long left = long(end - now);
        timeval tv{left / 1000, (left % 1000) * 1000};
        if (select(0, &rs, nullptr, nullptr, &tv) <= 0) break;
        for (SOCKET s : socks) {
            if (!FD_ISSET(s, &rs)) continue;
            DiscoveryReply r{};
            sockaddr_in from{};
            int fromLen = sizeof from;
            const int n = recvfrom(s, reinterpret_cast<char*>(&r), sizeof r, 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
            if (n != int(sizeof r) || std::memcmp(r.magic, kReplyMagic, 4) != 0 || r.version != kProtocolVersion) continue;
            Peer p;
            p.name.assign(r.name, strnlen(r.name, sizeof r.name));
            p.address = ipv4_text(from.sin_addr.s_addr);
            p.port = r.tcpPort;
            p.controlAllowed = (r.flags & kDiscoveryControlAllowed) != 0;
            if (const Adapter* a = adapter_for(adapters, from.sin_addr.s_addr)) {
                p.adapter = a->name;
                p.directLink = a->directLink;
                p.linkBps = a->linkBps;
            }
            const bool dup = std::any_of(peers.begin(), peers.end(), [&](const Peer& q) { return q.address == p.address; });
            if (!dup) peers.push_back(std::move(p));
        }
    }
    for (SOCKET s : socks) closesocket(s);
    std::stable_sort(peers.begin(), peers.end(), [](const Peer& a, const Peer& b) { return a.directLink > b.directLink; });
    // A PC answers once per shared network; list it once, on its best link (direct links sort first).
    std::vector<Peer> unique;
    for (Peer& p : peers) {
        const bool seen = std::any_of(unique.begin(), unique.end(),
                                      [&](const Peer& q) { return q.name == p.name && q.port == p.port; });
        if (!seen) unique.push_back(std::move(p));
    }
    return unique;
}

bool DiscoveryResponder::start(InfoFn info) {
    stop();
    info_ = std::move(info);
    sock_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock_ == INVALID_SOCKET) return false;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(kUdpPort);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock_, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
        closesocket(sock_);
        sock_ = INVALID_SOCKET;
        return false;
    }
    stopping_ = false;
    thread_ = std::thread([this] { loop(); });
    return true;
}

void DiscoveryResponder::stop() {
    if (!thread_.joinable()) return;
    stopping_ = true;
    closesocket(sock_);  // unblocks recvfrom
    thread_.join();
    sock_ = INVALID_SOCKET;
}

void DiscoveryResponder::loop() {
    while (!stopping_) {
        DiscoveryProbe p{};
        sockaddr_in from{};
        int fromLen = sizeof from;
        const int n = recvfrom(sock_, reinterpret_cast<char*>(&p), sizeof p, 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (n == SOCKET_ERROR) {
            if (stopping_) break;
            if (WSAGetLastError() == WSAECONNRESET) continue;  // ICMP port unreachable from an earlier reply
            Sleep(100);
            continue;
        }
        if (n != int(sizeof p) || std::memcmp(p.magic, kProbeMagic, 4) != 0) continue;
        DiscoveryReply r = info_();
        std::memcpy(r.magic, kReplyMagic, 4);
        r.version = kProtocolVersion;
        sendto(sock_, reinterpret_cast<const char*>(&r), sizeof r, 0, reinterpret_cast<sockaddr*>(&from), fromLen);
    }
}

}  // namespace wv
