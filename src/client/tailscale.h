// Backrooms Iris - Tailscale support (PC). Tailscale puts your and your friends' PCs on one
// private network (a "tailnet") with 100.x.y.z addresses, so a game hosted at home can be
// joined from anywhere without port forwarding. We ask the local `tailscale` command for this
// machine's address and for the other machines on the tailnet, then look for games on them.
// Everything runs on a background thread; nothing here blocks a frame.
#pragma once
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace iris {

struct TailPeer {
    std::string name;     // host name ("my-pc")
    std::string ip;       // first IPv4 tailnet address
    bool online = false;
    std::string os;
};

struct TailStatus {
    bool available = false;   // the tailscale command exists and answered
    bool running = false;     // this machine is logged in and connected
    std::string self_ip, self_name, tailnet;
    std::vector<TailPeer> peers;
};

class Tailscale {
public:
    ~Tailscale();
    void refresh();                 // start a background query (no-op if one is running)
    TailStatus status();            // the latest answer
    bool busy();
private:
    std::mutex mu_;
    TailStatus st_;
    std::thread th_;
    bool running_ = false;
};

// Parses `tailscale status --json`. Exposed for tests.
bool parse_tailscale_status(const std::string& json, TailStatus& out);
// 100.64.0.0/10 is Tailscale's address range
bool is_tailscale_ip(const std::string& ip);

} // namespace iris
