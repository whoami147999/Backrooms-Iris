#include "udp.h"

#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
typedef int socklen_t;
#define IRIS_CLOSE closesocket
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#define IRIS_CLOSE ::close
#endif

#if defined(__vita__)
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>
#include <cstdlib>
#endif

namespace iris {

static bool g_net_ready = false;

bool net_init() {
    if (g_net_ready) return true;
#if defined(_WIN32)
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
#elif defined(__vita__)
    sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
    static void* pool = nullptr;
    if (!pool) pool = std::malloc(1024 * 1024);
    SceNetInitParam p;
    p.memory = pool;
    p.size = 1024 * 1024;
    p.flags = 0;
    sceNetInit(&p); // returns an error if already initialised, which is fine
    sceNetCtlInit();
#endif
    g_net_ready = true;
    return true;
}

void net_shutdown() {
    if (!g_net_ready) return;
#if defined(_WIN32)
    WSACleanup();
#elif defined(__vita__)
    sceNetCtlTerm();
    sceNetTerm();
#endif
    g_net_ready = false;
}

// "a.b.c.d" -> host-order IPv4 without relying on inet_pton (not on every platform's libc)
static bool parse_ipv4(const std::string& s, uint32_t& out) {
    uint32_t parts[4];
    int n = 0;
    size_t i = 0;
    while (n < 4) {
        if (i >= s.size() || s[i] < '0' || s[i] > '9') return false;
        uint32_t v = 0;
        int digits = 0;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9' && digits < 4) { v = v * 10 + (uint32_t)(s[i] - '0'); i++; digits++; }
        if (v > 255) return false;
        parts[n++] = v;
        if (n < 4) { if (i >= s.size() || s[i] != '.') return false; i++; }
    }
    if (i != s.size()) return false;
    out = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
    return true;
}

bool net_resolve(const std::string& host, uint16_t port, NetAddr& out) {
    net_init();
    out.port = port;
    if (parse_ipv4(host, out.ip)) return true;
#if defined(__vita__)
    return false; // Vita: numeric addresses only (LAN browser fills them in)
#else
    addrinfo hints, *res = nullptr;
    std::memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) return false;
    out.ip = ntohl(((sockaddr_in*)res->ai_addr)->sin_addr.s_addr);
    freeaddrinfo(res);
    return true;
#endif
}

std::string net_addr_str(const NetAddr& a) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%u.%u.%u.%u:%u", (a.ip >> 24) & 255, (a.ip >> 16) & 255, (a.ip >> 8) & 255, a.ip & 255, a.port);
    return buf;
}

NetAddr net_broadcast(uint16_t port) { return {0xFFFFFFFFu, port}; }

bool UdpSocket::open(uint16_t port, bool broadcast, const char* bind_ip) {
    net_init();
    close();
    long long s = (long long)socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#if defined(_WIN32)
    if ((SOCKET)s == INVALID_SOCKET) return false;
#else
    if (s < 0) return false;
#endif
    int yes = 1;
    setsockopt((int)s, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof yes);
    if (broadcast) setsockopt((int)s, SOL_SOCKET, SO_BROADCAST, (const char*)&yes, sizeof yes);
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    uint32_t bip;
    if (bind_ip && *bind_ip && parse_ipv4(bind_ip, bip)) addr.sin_addr.s_addr = htonl(bip);
    if (bind((int)s, (sockaddr*)&addr, sizeof addr) != 0) {
        IRIS_CLOSE((int)s);
        return false;
    }
#if defined(_WIN32)
    u_long nb = 1;
    ioctlsocket((SOCKET)s, FIONBIO, &nb);
#elif defined(__vita__)
    int nb = 1;
    setsockopt((int)s, SOL_SOCKET, SO_NONBLOCK, &nb, sizeof nb);
#else
    fcntl((int)s, F_SETFL, fcntl((int)s, F_GETFL, 0) | O_NONBLOCK);
#endif
    fd_ = s;
    return true;
}

void UdpSocket::close() {
    if (fd_ >= 0) IRIS_CLOSE((int)fd_);
    fd_ = -1;
}

bool UdpSocket::send(const NetAddr& to, const void* data, int len) {
    if (fd_ < 0) return false;
    sockaddr_in a;
    std::memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(to.port);
    a.sin_addr.s_addr = htonl(to.ip);
    return sendto((int)fd_, (const char*)data, len, 0, (sockaddr*)&a, sizeof a) == len;
}

int UdpSocket::recv(NetAddr& from, void* buf, int cap) {
    if (fd_ < 0) return -1;
    sockaddr_in a;
    socklen_t al = sizeof a;
    int n = (int)recvfrom((int)fd_, (char*)buf, cap, 0, (sockaddr*)&a, &al);
    if (n < 0) return -1;
    from.ip = ntohl(a.sin_addr.s_addr);
    from.port = ntohs(a.sin_port);
    return n;
}

bool UdpSocket::wait(int ms) {
    if (fd_ < 0) return false;
#if defined(__vita__)
    // no select() on Vita sockets: just sleep a little
    (void)ms;
    return true;
#else
    fd_set set;
    FD_ZERO(&set);
    FD_SET((int)fd_, &set);
    timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    return select((int)fd_ + 1, &set, nullptr, nullptr, &tv) > 0;
#endif
}

uint16_t UdpSocket::local_port() const {
    if (fd_ < 0) return 0;
    sockaddr_in a;
    socklen_t al = sizeof a;
    if (getsockname((int)fd_, (sockaddr*)&a, &al) != 0) return 0;
    return ntohs(a.sin_port);
}

} // namespace iris
