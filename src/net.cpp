#include "net.h"

#include <iphlpapi.h>
#include <mstcpip.h>

#include <algorithm>
#include <cstdio>

#include "util.h"

namespace wv {

WinsockInit::WinsockInit() {
    WSADATA d;
    ok = WSAStartup(MAKEWORD(2, 2), &d) == 0;
}
WinsockInit::~WinsockInit() {
    if (ok) WSACleanup();
}

void tune_socket(SOCKET s) {
    BOOL one = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);
    int buf = 8 * 1024 * 1024;
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&buf), sizeof buf);
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&buf), sizeof buf);
}

void set_recv_timeout(SOCKET s, int ms) {
    DWORD t = DWORD(ms);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&t), sizeof t);
}

bool send_all(SOCKET s, const void* data, size_t len) {
    const char* p = static_cast<const char*>(data);
    while (len) {
        const int chunk = int(std::min<size_t>(len, 1u << 30));
        const int n = send(s, p, chunk, 0);
        if (n <= 0) return false;
        p += n;
        len -= size_t(n);
    }
    return true;
}

bool send_gather(SOCKET s, WSABUF* bufs, size_t count) {
    constexpr size_t kBatch = 512;
    size_t i = 0;
    while (i < count) {
        if (bufs[i].len == 0) {
            ++i;
            continue;
        }
        const DWORD n = DWORD(std::min(count - i, kBatch));
        DWORD sent = 0;
        if (WSASend(s, bufs + i, n, &sent, 0, nullptr, nullptr) != 0) return false;
        // Blocking sockets normally send everything; walk forward for the rare partial send.
        while (sent && i < count) {
            if (sent >= bufs[i].len) {
                sent -= bufs[i].len;
                bufs[i].len = 0;
                ++i;
            } else {
                bufs[i].buf += sent;
                bufs[i].len -= sent;
                sent = 0;
            }
        }
    }
    return true;
}

bool recv_all(SOCKET s, void* data, size_t len) {
    char* p = static_cast<char*>(data);
    while (len) {
        const int chunk = int(std::min<size_t>(len, 1u << 30));
        const int n = recv(s, p, chunk, 0);
        if (n <= 0) return false;
        p += n;
        len -= size_t(n);
    }
    return true;
}

bool send_msg(SOCKET s, uint16_t type, const void* payload, uint32_t size) {
    MsgHeader h{type, 0, size};
    WSABUF b[2] = {{sizeof h, reinterpret_cast<char*>(&h)},
                   {size, const_cast<char*>(static_cast<const char*>(payload))}};
    return send_gather(s, b, size ? 2 : 1);
}

SOCKET tcp_connect(const std::string& host, uint16_t port, int timeoutMs, std::string& err) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* res = nullptr;
    char portText[16];
    std::snprintf(portText, sizeof portText, "%u", port);
    if (int rc = getaddrinfo(host.c_str(), portText, &hints, &res); rc != 0) {
        err = "Cannot resolve \"" + host + "\": " + wsa_error_text(rc);
        return INVALID_SOCKET;
    }
    SOCKET result = INVALID_SOCKET;
    err = "No address to connect to";
    for (addrinfo* a = res; a && result == INVALID_SOCKET; a = a->ai_next) {
        SOCKET s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == INVALID_SOCKET) continue;
        tune_socket(s);  // buffer sizes must be set before connect to affect the window
        u_long nb = 1;
        ioctlsocket(s, FIONBIO, &nb);
        int rc = connect(s, a->ai_addr, int(a->ai_addrlen));
        if (rc == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) {
            fd_set w, e;
            FD_ZERO(&w);
            FD_ZERO(&e);
            FD_SET(s, &w);
            FD_SET(s, &e);
            timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
            rc = select(0, nullptr, &w, &e, &tv);
            if (rc == 1 && FD_ISSET(s, &w)) {
                rc = 0;
            } else {
                int soErr = WSAETIMEDOUT;
                if (rc == 1) {
                    int len = sizeof soErr;
                    getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soErr), &len);
                }
                WSASetLastError(soErr);
                rc = SOCKET_ERROR;
            }
        }
        if (rc == 0) {
            nb = 0;
            ioctlsocket(s, FIONBIO, &nb);
            result = s;
        } else {
            err = "Cannot connect to " + host + ": " + wsa_error_text(WSAGetLastError());
            closesocket(s);
        }
    }
    freeaddrinfo(res);
    return result;
}

SOCKET tcp_listen(uint16_t port, std::string& err) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        err = "socket: " + wsa_error_text(WSAGetLastError());
        return s;
    }
    BOOL excl = TRUE;  // nobody else may bind the same port while we listen
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&excl), sizeof excl);
    tune_socket(s);  // accepted sockets inherit the buffer sizes
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || listen(s, 4) != 0) {
        err = "Cannot listen on port " + std::to_string(port) + ": " + wsa_error_text(WSAGetLastError());
        closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

static std::string sockaddr_text(const sockaddr* sa, int len) {
    char host[NI_MAXHOST] = {};
    if (getnameinfo(sa, len, host, sizeof host, nullptr, 0, NI_NUMERICHOST) != 0) return "?";
    return host;
}

std::string peer_address(SOCKET s) {
    sockaddr_storage ss{};
    int len = sizeof ss;
    if (getpeername(s, reinterpret_cast<sockaddr*>(&ss), &len) != 0) return "?";
    return sockaddr_text(reinterpret_cast<sockaddr*>(&ss), len);
}

std::string local_address(SOCKET s) {
    sockaddr_storage ss{};
    int len = sizeof ss;
    if (getsockname(s, reinterpret_cast<sockaddr*>(&ss), &len) != 0) return "?";
    return sockaddr_text(reinterpret_cast<sockaddr*>(&ss), len);
}

std::string wsa_error_text(int code) {
    wchar_t* msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, DWORD(code), 0, reinterpret_cast<wchar_t*>(&msg), 0, nullptr);
    std::string text = msg ? to_utf8(msg) : "error " + std::to_string(code);
    if (msg) LocalFree(msg);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == '.')) text.pop_back();
    return text;
}

std::string computer_name_utf8() {
    wchar_t buf[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD n = MAX_COMPUTERNAME_LENGTH + 1;
    if (!GetComputerNameW(buf, &n)) return "PC";
    return to_utf8(std::wstring(buf, n));
}

static bool is_direct_link(const std::string& description) {
    std::string d = description;
    std::transform(d.begin(), d.end(), d.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return d.find("usb4") != std::string::npos || d.find("thunderbolt") != std::string::npos ||
           d.find("p2p network") != std::string::npos;
}

std::vector<Adapter> list_adapters() {
    std::vector<Adapter> out;
    ULONG size = 32 * 1024;
    std::vector<uint8_t> buf;
    ULONG rc = ERROR_BUFFER_OVERFLOW;
    for (int tries = 0; tries < 3 && rc == ERROR_BUFFER_OVERFLOW; ++tries) {
        buf.resize(size);
        rc = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                  nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
    }
    if (rc != NO_ERROR) return out;
    for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()); a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            if (u->Address.lpSockaddr->sa_family != AF_INET) continue;
            Adapter ad;
            ad.name = to_utf8(a->FriendlyName ? a->FriendlyName : L"");
            ad.description = to_utf8(a->Description ? a->Description : L"");
            ad.ipv4 = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr)->sin_addr.s_addr;
            ULONG mask = 0;
            ConvertLengthToIpv4Mask(u->OnLinkPrefixLength, &mask);
            ad.mask = mask;
            ad.linkBps = a->TransmitLinkSpeed == ~0ULL ? 0 : a->TransmitLinkSpeed;
            ad.directLink = is_direct_link(ad.description);
            out.push_back(std::move(ad));
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const Adapter& x, const Adapter& y) { return x.directLink > y.directLink; });
    return out;
}

const Adapter* adapter_for(const std::vector<Adapter>& list, uint32_t addr) {
    for (const Adapter& a : list)
        if ((a.ipv4 & a.mask) == (addr & a.mask)) return &a;
    return nullptr;
}

std::string format_bps(double bps) {
    char buf[32];
    if (bps >= 1e9)
        std::snprintf(buf, sizeof buf, "%.2f Gbit/s", bps / 1e9);
    else if (bps >= 1e6)
        std::snprintf(buf, sizeof buf, "%.1f Mbit/s", bps / 1e6);
    else
        std::snprintf(buf, sizeof buf, "%.0f kbit/s", bps / 1e3);
    return buf;
}

std::string ipv4_text(uint32_t addr) {
    in_addr a;
    a.s_addr = addr;
    char buf[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &a, buf, sizeof buf);
    return buf;
}

}  // namespace wv
