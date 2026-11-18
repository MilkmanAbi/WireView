// Thin Winsock helpers. Blocking sockets everywhere: each direction of a
// session has its own thread, so blocking calls are the cheapest option.
#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstdint>
#include <string>
#include <vector>

#include "protocol.h"

namespace wv {

struct WinsockInit {
    WinsockInit();
    ~WinsockInit();
    bool ok = false;
};

// Large buffers + no Nagle. Raw frames are megabytes, input events are 12 bytes;
// both want to leave immediately.
void tune_socket(SOCKET s);
void set_recv_timeout(SOCKET s, int ms);  // 0 = none

bool send_all(SOCKET s, const void* data, size_t len);
// Sends every buffer in order. Handles partial sends. Modifies bufs.
bool send_gather(SOCKET s, WSABUF* bufs, size_t count);
bool recv_all(SOCKET s, void* data, size_t len);
bool send_msg(SOCKET s, uint16_t type, const void* payload, uint32_t size);

// Resolves host (name, IPv4 or IPv6 literal) and connects with a timeout.
SOCKET tcp_connect(const std::string& host, uint16_t port, int timeoutMs, std::string& err);
// Listens on all IPv4 interfaces.
SOCKET tcp_listen(uint16_t port, std::string& err);

std::string peer_address(SOCKET s);
std::string local_address(SOCKET s);
std::string wsa_error_text(int code);
std::string computer_name_utf8();

struct Adapter {
    std::string name;         // friendly name, UTF-8
    std::string description;  // driver description, UTF-8
    uint32_t ipv4 = 0;        // network byte order
    uint32_t mask = 0;        // network byte order
    uint64_t linkBps = 0;
    bool directLink = false;  // USB4 / Thunderbolt host-to-host adapter
};
// IPv4 adapters that are up, direct links first.
std::vector<Adapter> list_adapters();
// Adapter whose subnet contains addr (network byte order), or nullptr.
const Adapter* adapter_for(const std::vector<Adapter>& list, uint32_t addr);
std::string format_bps(double bitsPerSecond);
std::string ipv4_text(uint32_t addrNetworkOrder);

}  // namespace wv
