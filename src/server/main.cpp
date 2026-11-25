// Backrooms Iris - headless dedicated server.
//
// Milestone 1 status: this is the skeleton the FreeBSD installer deploys. It loads the
// config, generates the same Level 0 as the clients (shared core), runs a fixed tick
// loop and answers status queries over UDP. Player connections + authority + lag
// compensation are milestone 4.
//
// Builds on FreeBSD, Linux and macOS (POSIX sockets). No SDL, no GL.
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdarg>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <string>
#include <thread>

#include "../core/config.h"
#include "../core/level0.h"

using namespace iris;

namespace {

volatile sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

struct ServerConfig {
    std::string name = "Backrooms Iris server";
    std::string bind = "0.0.0.0";
    int port = IRIS_DEFAULT_PORT;
    int max_players = IRIS_MAX_PLAYERS;
    uint32_t seed = 0;     // 0 = random each start
    int tick_rate = 30;
};

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

void log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void log(const char* fmt, ...) {
    char ts[32];
    std::time_t t = std::time(nullptr);
    std::strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", std::localtime(&t));
    std::printf("%s ", ts);
    va_list ap;
    va_start(ap, fmt);
    std::vprintf(fmt, ap);
    va_end(ap);
    std::printf("\n");
    std::fflush(stdout);
}

// key = value, '#' comments
bool load_config(const std::string& path, ServerConfig& c) {
    std::ifstream f(path);
    if (!f) return false;
    std::string line;
    int n = 0;
    while (std::getline(f, line)) {
        n++;
        size_t hash = line.find('#');
        if (hash != std::string::npos) line = line.substr(0, hash);
        line = trim(line);
        if (line.empty()) continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) { log("config %s:%d: expected key = value", path.c_str(), n); continue; }
        std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
        if (k == "name") c.name = v;
        else if (k == "bind") c.bind = v;
        else if (k == "port") c.port = std::atoi(v.c_str());
        else if (k == "max_players") c.max_players = std::atoi(v.c_str());
        else if (k == "seed") c.seed = (uint32_t)std::strtoul(v.c_str(), nullptr, 10);
        else if (k == "tick_rate") c.tick_rate = std::atoi(v.c_str());
        else log("config %s:%d: unknown key '%s'", path.c_str(), n, k.c_str());
    }
    return true;
}

bool resolve(const std::string& host, int port, sockaddr_in& out) {
    std::memset(&out, 0, sizeof out);
    out.sin_family = AF_INET;
    out.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host.c_str(), &out.sin_addr) == 1) return true;
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) return false;
    out.sin_addr = ((sockaddr_in*)res->ai_addr)->sin_addr;
    freeaddrinfo(res);
    return true;
}

// `iris_server --query host[:port]`: ask a running server for its status.
int query(const std::string& target) {
    std::string host = target;
    int port = IRIS_DEFAULT_PORT;
    size_t colon = target.rfind(':');
    if (colon != std::string::npos) { host = target.substr(0, colon); port = std::atoi(target.c_str() + colon + 1); }
    sockaddr_in addr;
    if (!resolve(host, port, addr)) { std::fprintf(stderr, "can't resolve %s\n", host.c_str()); return 2; }
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) { std::perror("socket"); return 2; }
    timeval tv{2, 0};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    const char q[] = "IRIS?";
    sendto(s, q, sizeof q - 1, 0, (sockaddr*)&addr, sizeof addr);
    char buf[1024];
    ssize_t n = recv(s, buf, sizeof buf - 1, 0);
    close(s);
    if (n <= 0) { std::fprintf(stderr, "no answer from %s:%d\n", host.c_str(), port); return 1; }
    buf[n] = 0;
    std::printf("%s\n", buf);
    return 0;
}

void usage() {
    std::printf(
        "iris_server %s - Backrooms Iris dedicated server\n\n"
        "  --config FILE     config file (default: none, built-in defaults)\n"
        "  --port N          UDP port (default %d)\n"
        "  --seed N          world seed (0 = random)\n"
        "  --name TEXT       server name\n"
        "  --query HOST[:P]  ask a running server for its status and exit\n"
        "  --version         print the version\n",
        IRIS_VERSION, IRIS_DEFAULT_PORT);
}

} // namespace

int main(int argc, char** argv) {
    ServerConfig cfg;
    std::string cfg_path;
    // first pass: config file, so command-line flags can override it
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--config" && i + 1 < argc) cfg_path = argv[++i];
        else if (a == "--query" && i + 1 < argc) return query(argv[++i]);
        else if (a == "--version") { std::printf("iris_server %s (protocol %d)\n", IRIS_VERSION, IRIS_PROTOCOL_VERSION); return 0; }
        else if (a == "--help" || a == "-h") { usage(); return 0; }
    }
    if (!cfg_path.empty() && !load_config(cfg_path, cfg)) {
        std::fprintf(stderr, "can't read config %s\n", cfg_path.c_str());
        return 1;
    }
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--config") next();
        else if (a == "--port") cfg.port = std::atoi(next().c_str());
        else if (a == "--seed") cfg.seed = (uint32_t)std::strtoul(next().c_str(), nullptr, 10);
        else if (a == "--name") cfg.name = next();
        else { std::fprintf(stderr, "unknown option %s (try --help)\n", a.c_str()); return 2; }
    }
    if (cfg.port <= 0 || cfg.port > 65535) { std::fprintf(stderr, "bad port %d\n", cfg.port); return 1; }
    if (cfg.max_players < 1 || cfg.max_players > IRIS_MAX_PLAYERS) cfg.max_players = IRIS_MAX_PLAYERS;
    if (cfg.tick_rate < 10 || cfg.tick_rate > 120) cfg.tick_rate = 30;
    if (cfg.seed == 0) cfg.seed = (uint32_t)std::time(nullptr) ^ ((uint32_t)getpid() << 16);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { std::perror("socket"); return 1; }
    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    sockaddr_in addr;
    if (!resolve(cfg.bind, cfg.port, addr)) { std::fprintf(stderr, "bad bind address %s\n", cfg.bind.c_str()); return 1; }
    if (bind(sock, (sockaddr*)&addr, sizeof addr) != 0) {
        std::fprintf(stderr, "can't bind %s:%d: %s\n", cfg.bind.c_str(), cfg.port, std::strerror(errno));
        return 1;
    }
    timeval tv{0, 1000}; // recv waits at most 1 ms so the tick loop keeps its rhythm
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    Level0 level(cfg.seed);
    log("Backrooms Iris server %s (protocol %d)", IRIS_VERSION, IRIS_PROTOCOL_VERSION);
    log("name \"%s\" | %s:%d/udp | seed %u | %d Hz | max %d players", cfg.name.c_str(), cfg.bind.c_str(), cfg.port,
        cfg.seed, cfg.tick_rate, cfg.max_players);
    log("spawn district %d, spawn light %d (world generated from the shared core)",
        (int)level.district(0, 0), (int)level.light(0, 0));

    using clk = std::chrono::steady_clock;
    const auto tick = std::chrono::microseconds(1000000 / cfg.tick_rate);
    auto start = clk::now(), next_tick = start;
    unsigned long long ticks = 0;
    char buf[1500];

    while (!g_stop) {
        // network: answer status queries
        sockaddr_in from;
        socklen_t fl = sizeof from;
        ssize_t n = recvfrom(sock, buf, sizeof buf, 0, (sockaddr*)&from, &fl);
        if (n == 5 && std::memcmp(buf, "IRIS?", 5) == 0) {
            long up = (long)std::chrono::duration_cast<std::chrono::seconds>(clk::now() - start).count();
            char reply[512];
            int len = std::snprintf(reply, sizeof reply,
                                    "IRIS! name=\"%s\" version=%s protocol=%d players=0/%d seed=%u tick=%d uptime=%lds",
                                    cfg.name.c_str(), IRIS_VERSION, IRIS_PROTOCOL_VERSION, cfg.max_players, cfg.seed,
                                    cfg.tick_rate, up);
            sendto(sock, reply, (size_t)len, 0, (sockaddr*)&from, fl);
        }

        // simulation
        auto now = clk::now();
        while (now >= next_tick) {
            ticks++;
            next_tick += tick;
            if (ticks % (unsigned long long)(cfg.tick_rate * 3600) == 0)
                log("alive: %llu ticks, 0 players", ticks);
        }
        if (now - next_tick > std::chrono::seconds(1)) next_tick = now; // don't spiral after a stall
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
    log("shutting down after %llu ticks", ticks);
    close(sock);
    return 0;
}
