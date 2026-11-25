#include "tailscale.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace iris {
namespace {

// ---------------------------------------------------------------- a small JSON reader
struct JVal {
    enum Kind { Null, Bool, Num, Str, Arr, Obj } kind = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<JVal> arr;
    std::vector<std::pair<std::string, JVal>> obj;
    const JVal* get(const char* key) const {
        for (const auto& kv : obj)
            if (kv.first == key) return &kv.second;
        return nullptr;
    }
};

struct JParser {
    const char* p;
    const char* end;
    int depth = 0;
    void ws() { while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++; }
    bool str(std::string& out) {
        if (p >= end || *p != '"') return false;
        p++;
        while (p < end && *p != '"') {
            if (*p == '\\' && p + 1 < end) {
                p++;
                char c = *p;
                if (c == 'n') out += '\n';
                else if (c == 't') out += '\t';
                else if (c == 'u') { out += '?'; p += 4; if (p >= end) return false; }
                else out += c;
                p++;
            } else out += *p++;
        }
        if (p >= end) return false;
        p++;
        return true;
    }
    bool val(JVal& v) {
        if (++depth > 64) return false;
        ws();
        if (p >= end) return false;
        bool ok = true;
        if (*p == '{') {
            v.kind = JVal::Obj;
            p++;
            ws();
            if (p < end && *p == '}') { p++; }
            else
                for (;;) {
                    ws();
                    std::string k;
                    if (!str(k)) { ok = false; break; }
                    ws();
                    if (p >= end || *p != ':') { ok = false; break; }
                    p++;
                    JVal child;
                    if (!val(child)) { ok = false; break; }
                    v.obj.emplace_back(std::move(k), std::move(child));
                    ws();
                    if (p < end && *p == ',') { p++; continue; }
                    if (p < end && *p == '}') { p++; break; }
                    ok = false;
                    break;
                }
        } else if (*p == '[') {
            v.kind = JVal::Arr;
            p++;
            ws();
            if (p < end && *p == ']') { p++; }
            else
                for (;;) {
                    JVal child;
                    if (!val(child)) { ok = false; break; }
                    v.arr.push_back(std::move(child));
                    ws();
                    if (p < end && *p == ',') { p++; continue; }
                    if (p < end && *p == ']') { p++; break; }
                    ok = false;
                    break;
                }
        } else if (*p == '"') {
            v.kind = JVal::Str;
            ok = str(v.s);
        } else if (!std::strncmp(p, "true", 4)) { v.kind = JVal::Bool; v.b = true; p += 4; }
        else if (!std::strncmp(p, "false", 5)) { v.kind = JVal::Bool; p += 5; }
        else if (!std::strncmp(p, "null", 4)) { p += 4; }
        else {
            v.kind = JVal::Num;
            char* e = nullptr;
            v.n = std::strtod(p, &e);
            if (e == p) ok = false;
            p = e;
        }
        depth--;
        return ok;
    }
};

std::string first_ipv4(const JVal* ips) {
    if (!ips || ips->kind != JVal::Arr) return "";
    for (const JVal& ip : ips->arr)
        if (ip.kind == JVal::Str && ip.s.find('.') != std::string::npos && ip.s.find(':') == std::string::npos) return ip.s;
    return "";
}

std::string str_of(const JVal* v) { return v && v->kind == JVal::Str ? v->s : ""; }

// ---------------------------------------------------------------- run a command, read its output
bool run_command(const std::string& cmd, std::string& out) {
#if defined(_WIN32)
    // CreateProcess with no console window (a GUI game must not flash a cmd.exe box)
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return false;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    PROCESS_INFORMATION pi{};
    std::vector<char> line(cmd.begin(), cmd.end());
    line.push_back(0);
    BOOL ok = CreateProcessA(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (!ok) { CloseHandle(rd); return false; }
    char buf[4096];
    DWORD n = 0;
    while (ReadFile(rd, buf, sizeof buf, &n, nullptr) && n > 0) out.append(buf, n);
    WaitForSingleObject(pi.hProcess, 5000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(rd);
    return code == 0;
#elif defined(__vita__)
    (void)cmd; (void)out;
    return false;
#else
    FILE* f = popen((cmd + " 2>/dev/null").c_str(), "r");
    if (!f) return false;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    return pclose(f) == 0;
#endif
}

bool query_status(TailStatus& st) {
    static const char* CANDIDATES[] = {
#if defined(_WIN32)
        "tailscale status --json",
        "\"C:\\Program Files\\Tailscale\\tailscale.exe\" status --json",
#elif defined(__APPLE__)
        "tailscale status --json",
        "/Applications/Tailscale.app/Contents/MacOS/Tailscale status --json",
#else
        "tailscale status --json",
        "/usr/bin/tailscale status --json",
        "/usr/local/bin/tailscale status --json",
#endif
    };
    for (const char* c : CANDIDATES) {
        std::string out;
        run_command(c, out); // exits non-zero when logged out, but still prints JSON
        if (!out.empty() && parse_tailscale_status(out, st)) return true;
    }
    return false;
}

} // namespace

bool is_tailscale_ip(const std::string& ip) {
    unsigned a = 0, b = 0;
    if (std::sscanf(ip.c_str(), "%u.%u.", &a, &b) != 2) return false;
    return a == 100 && b >= 64 && b <= 127;
}

bool parse_tailscale_status(const std::string& json, TailStatus& out) {
    size_t start = json.find('{');
    if (start == std::string::npos) return false;
    JParser jp{json.c_str() + start, json.c_str() + json.size()};
    JVal root;
    if (!jp.val(root) || root.kind != JVal::Obj) return false;
    out = TailStatus{};
    out.available = true;
    std::string state = str_of(root.get("BackendState"));
    out.running = state == "Running";
    if (const JVal* self = root.get("Self")) {
        out.self_ip = first_ipv4(self->get("TailscaleIPs"));
        out.self_name = str_of(self->get("HostName"));
    }
    if (out.self_ip.empty()) out.self_ip = first_ipv4(root.get("TailscaleIPs"));
    if (const JVal* tn = root.get("CurrentTailnet")) out.tailnet = str_of(tn->get("Name"));
    if (const JVal* peers = root.get("Peer"))
        for (const auto& kv : peers->obj) {
            const JVal& p = kv.second;
            TailPeer tp;
            tp.ip = first_ipv4(p.get("TailscaleIPs"));
            if (tp.ip.empty()) continue;
            tp.name = str_of(p.get("HostName"));
            if (tp.name.empty()) tp.name = str_of(p.get("DNSName"));
            const JVal* on = p.get("Online");
            tp.online = on && on->kind == JVal::Bool && on->b;
            tp.os = str_of(p.get("OS"));
            out.peers.push_back(tp);
        }
    return true;
}

Tailscale::~Tailscale() {
    if (th_.joinable()) th_.join();
}

bool Tailscale::busy() {
    std::lock_guard<std::mutex> lk(mu_);
    return running_;
}

void Tailscale::refresh() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (running_) return;
        running_ = true;
    }
    if (th_.joinable()) th_.join();
    th_ = std::thread([this] {
        TailStatus st;
        if (!query_status(st)) st = TailStatus{};
        std::lock_guard<std::mutex> lk(mu_);
        st_ = st;
        running_ = false;
    });
}

TailStatus Tailscale::status() {
    std::lock_guard<std::mutex> lk(mu_);
    return st_;
}

} // namespace iris
