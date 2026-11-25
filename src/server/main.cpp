// Backrooms Iris - headless dedicated server (FreeBSD, Linux).
// Runs the authoritative game for up to 4 players. No SDL, no GL.
#include <signal.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <string>
#include <thread>

#include "../core/config.h"
#include "../net/server.h"

using namespace iris;

namespace {

volatile sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

void log_line(const char* line) {
    char ts[32];
    std::time_t t = std::time(nullptr);
    std::strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", std::localtime(&t));
    std::printf("%s %s\n", ts, line);
    std::fflush(stdout);
}

bool parse_level(const std::string& v, LevelId& out) { return level_from_number(v.c_str(), out); }

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
        if (eq == std::string::npos) continue;
        std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
        char msg[160];
        if (k == "name") c.name = v;
        else if (k == "bind") c.bind = v;
        else if (k == "port") c.port = (uint16_t)std::atoi(v.c_str());
        else if (k == "max_players") c.max_players = std::atoi(v.c_str());
        else if (k == "seed") c.seed = (uint32_t)std::strtoul(v.c_str(), nullptr, 10);
        else if (k == "start_level") {
            if (!parse_level(v, c.level)) { std::snprintf(msg, sizeof msg, "config %s:%d: unknown level '%s'", path.c_str(), n, v.c_str()); log_line(msg); }
        } else if (k == "difficulty") {
            if (!difficulty_from_name(v.c_str(), c.difficulty)) { std::snprintf(msg, sizeof msg, "config %s:%d: unknown difficulty '%s'", path.c_str(), n, v.c_str()); log_line(msg); }
        } else if (k == "tick_rate" || k == "snapshot_rate") {
            int r = std::atoi(v.c_str());
            if (r >= 10 && r <= 60) c.snapshot_rate = r;
        } else { std::snprintf(msg, sizeof msg, "config %s:%d: unknown key '%s'", path.c_str(), n, k.c_str()); log_line(msg); }
    }
    return true;
}

// `iris_server --query host[:port]`: ask a running server for its status.
int query(const std::string& target) {
    std::string host = target;
    uint16_t port = IRIS_DEFAULT_PORT;
    size_t colon = target.rfind(':');
    if (colon != std::string::npos) { host = target.substr(0, colon); port = (uint16_t)std::atoi(target.c_str() + colon + 1); }
    NetAddr addr;
    if (!net_resolve(host, port, addr)) { std::fprintf(stderr, "can't resolve %s\n", host.c_str()); return 2; }
    UdpSocket s;
    if (!s.open(0)) { std::fprintf(stderr, "can't open a socket\n"); return 2; }
    s.send(addr, "IRIS?", 5);
    char buf[512];
    NetAddr from;
    for (int i = 0; i < 20; i++) {
        s.wait(100);
        int n = s.recv(from, buf, sizeof buf - 1);
        if (n > 0) { buf[n] = 0; std::printf("%s\n", buf); return 0; }
    }
    std::fprintf(stderr, "no answer from %s\n", net_addr_str(addr).c_str());
    return 1;
}

void usage() {
    std::printf(
        "iris_server %s - Backrooms Iris dedicated server\n\n"
        "  --config FILE     config file\n"
        "  --port N          UDP port (default %d)\n"
        "  --seed N          world seed (0 = random)\n"
        "  --level N         starting level: 0 1 2 4 5 6 8 9 11 37 fun\n"
        "  --difficulty D    easy, medium, hard, nightmare\n"
        "  --name TEXT       server name\n"
        "  --query HOST[:P]  ask a running server for its status and exit\n"
        "  --version         print the version\n",
        IRIS_VERSION, IRIS_DEFAULT_PORT);
}

} // namespace

int main(int argc, char** argv) {
    ServerConfig cfg;
    std::string cfg_path;
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
        else if (a == "--port") cfg.port = (uint16_t)std::atoi(next().c_str());
        else if (a == "--seed") cfg.seed = (uint32_t)std::strtoul(next().c_str(), nullptr, 10);
        else if (a == "--name") cfg.name = next();
        else if (a == "--level") {
            if (!parse_level(next(), cfg.level)) { std::fprintf(stderr, "levels: 0 1 2 4 5 6 8 9 11 37 fun\n"); return 2; }
        } else if (a == "--difficulty") {
            if (!difficulty_from_name(next().c_str(), cfg.difficulty)) { std::fprintf(stderr, "difficulty: easy medium hard nightmare\n"); return 2; }
        } else { std::fprintf(stderr, "unknown option %s (try --help)\n", a.c_str()); return 2; }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    Server server;
    server.log = log_line;
    std::string err;
    char msg[256];
    std::snprintf(msg, sizeof msg, "Backrooms Iris server %s (protocol %d) \"%s\", max %d players", IRIS_VERSION,
                  IRIS_PROTOCOL_VERSION, cfg.name.c_str(), cfg.max_players);
    log_line(msg);
    if (!server.start(cfg, &err)) {
        log_line(err.c_str());
        return 1;
    }
    auto last_status = std::chrono::steady_clock::now();
    while (!g_stop) {
        server.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        if (std::chrono::steady_clock::now() - last_status > std::chrono::hours(1)) {
            last_status = std::chrono::steady_clock::now();
            std::snprintf(msg, sizeof msg, "alive: %d players, %s", server.player_count(), level_label(server.sim.world.id()));
            log_line(msg);
        }
    }
    server.stop();
    log_line("shut down");
    return 0;
}
