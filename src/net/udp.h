// Backrooms Iris - tiny UDP socket wrapper: Linux, the BSDs and Windows (winsock).
#pragma once
#include <cstdint>
#include <string>

namespace iris {

struct NetAddr {
    uint32_t ip = 0;   // host byte order
    uint16_t port = 0;
    bool operator==(const NetAddr& o) const { return ip == o.ip && port == o.port; }
};

bool net_init();       // WSAStartup / Vita net module; safe to call more than once
void net_shutdown();
bool net_resolve(const std::string& host, uint16_t port, NetAddr& out); // "192.168.1.5" or a hostname
std::string net_addr_str(const NetAddr& a);
NetAddr net_broadcast(uint16_t port);

class UdpSocket {
public:
    ~UdpSocket() { close(); }
    bool open(uint16_t port = 0, bool broadcast = false, const char* bind_ip = nullptr);
    void close();
    bool is_open() const { return fd_ >= 0; }
    bool send(const NetAddr& to, const void* data, int len);
    int recv(NetAddr& from, void* buf, int cap);  // -1 when nothing is waiting (non-blocking)
    bool wait(int ms);                             // block until readable or timeout
    uint16_t local_port() const;
private:
    long long fd_ = -1;
};

} // namespace iris
