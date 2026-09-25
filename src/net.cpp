// Realtime networking (NetServer, NetClient, NetObject), shared by the full
// engine and the headless server library.
#include "thistle_core.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <unordered_map>

#include <nlohmann/json.hpp>

#if defined(_WIN32)
// Before windows.h: it defines min/max as macros unless told not to (see
// thistle.cpp).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#endif

namespace thistle {

// --- realtime networking -------------------------------------------------
// Non-blocking TCP sockets, length-prefixed JSON messages. See the big comment
// on this API in thistle.hpp before touching this — the scope is deliberate,
// not an oversight.
//
// A connection starts pending: the client sends "hello" (with the password,
// if the server has one), and only then does the server let it join ("welcome",
// then every object) or turn it away ("refused", with the reason). Leaving is
// "bye", with why: kicked (and the reason) or the server stopping.

#if defined(_WIN32)
    using SocketFd = SOCKET;
    constexpr SocketFd kInvalidSocket = INVALID_SOCKET;
    using NetSockLen = int;
    constexpr int kShutdownSend = SD_SEND;
    static void close_socket(SocketFd s) { closesocket(s); }
    static bool would_block() { return WSAGetLastError() == WSAEWOULDBLOCK; }
    static void set_nonblocking(SocketFd s) { u_long mode = 1; ioctlsocket(s, FIONBIO, &mode); }
    static void ensure_sockets_ready() {
        static bool started = false;
        if (!started) { WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa); started = true; }
    }
#else
    using SocketFd = int;
    constexpr SocketFd kInvalidSocket = -1;
    using NetSockLen = socklen_t;
    constexpr int kShutdownSend = SHUT_WR;
    static void close_socket(SocketFd s) { ::close(s); }
    static bool would_block() { return errno == EWOULDBLOCK || errno == EAGAIN; }
    static void set_nonblocking(SocketFd s) { int fl = fcntl(s, F_GETFL, 0); fcntl(s, F_SETFL, fl | O_NONBLOCK); }
    static void ensure_sockets_ready() {}
#endif

// Sending to a connection the other side has closed raises SIGPIPE on Linux
// and macOS, and SIGPIPE ends the process: one player's game crashing
// mid-send would take the whole server down with it.
#if defined(MSG_NOSIGNAL)
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

namespace {

constexpr int kProtocol = 1;
constexpr uint32_t kMaxMessage = 16u << 20;  // far bigger than anything sent; a length past it is garbage
constexpr size_t kMaxQueued = 32u << 20;     // a connection with this much unsent is dropped
constexpr double kJoinTimeout = 10.0;        // seconds a new connection has to say hello
constexpr double kCloseTimeout = 2.0;        // seconds to finish sending a bye before closing anyway
constexpr double kConnectTimeout = 5.0;      // seconds NetClient::connect() waits for the server's answer

double now_seconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void prepare_socket(SocketFd s) {
    set_nonblocking(s);
    int yes = 1;
    // Small JSON messages + Nagle's algorithm = needless latency.
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&yes), sizeof(yes));
#if defined(SO_NOSIGPIPE) // macOS has no MSG_NOSIGNAL
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
#endif
}

std::string address_of(const sockaddr_in& addr) {
    const auto* b = reinterpret_cast<const unsigned char*>(&addr.sin_addr);
    char out[32];
    std::snprintf(out, sizeof(out), "%u.%u.%u.%u:%u", b[0], b[1], b[2], b[3], static_cast<unsigned>(ntohs(addr.sin_port)));
    return out;
}

// One end of a connection. Sending never waits: what the socket won't take
// now waits in tx and goes out on later updates. A peer that stops reading
// (frozen, or doing it on purpose) can't stall the loop that way; once too
// much piles up for it, it's marked broken and dropped.
struct Link {
    SocketFd fd = kInvalidSocket;
    std::string rx;
    std::string tx;
    size_t tx_sent = 0; // how much of tx is already out
    bool broken = false;
};

void flush(Link& l) {
    while (!l.broken && l.tx_sent < l.tx.size()) {
        const size_t n = std::min<size_t>(l.tx.size() - l.tx_sent, 1u << 20);
        const int r = ::send(l.fd, l.tx.data() + l.tx_sent, static_cast<int>(n), kSendFlags);
        if (r > 0) { l.tx_sent += static_cast<size_t>(r); continue; }
        if (r < 0 && would_block()) break;
        l.broken = true;
    }
    if (l.tx_sent == l.tx.size()) {
        l.tx.clear();
        l.tx_sent = 0;
    } else if (l.tx_sent > (1u << 20) && l.tx_sent > l.tx.size() / 2) {
        l.tx.erase(0, l.tx_sent);
        l.tx_sent = 0;
    }
}

size_t queued(const Link& l) { return l.tx.size() - l.tx_sent; }

void send_json(Link& l, const nlohmann::json& j) {
    if (l.broken) return;
    const std::string body = j.dump();
    const uint32_t len = static_cast<uint32_t>(body.size());
    const char hdr[4] = {
        static_cast<char>(len & 0xFF), static_cast<char>((len >> 8) & 0xFF),
        static_cast<char>((len >> 16) & 0xFF), static_cast<char>((len >> 24) & 0xFF)
    };
    l.tx.append(hdr, 4);
    l.tx += body;
    if (queued(l) > kMaxQueued) {
        l.broken = true;
        return;
    }
    flush(l);
}

// Drains everything currently available into rx. False if the connection is
// over (orderly EOF or a real error); what arrived before that is still in rx.
bool receive(Link& l) {
    char tmp[4096];
    for (;;) {
        const int r = ::recv(l.fd, tmp, sizeof(tmp), 0);
        if (r > 0) { l.rx.append(tmp, static_cast<size_t>(r)); continue; }
        if (r == 0) return false;
        return would_block();
    }
}

enum class Extract { None, Message, Bad };

// Pulls one complete length-prefixed message out of rx, if there is one. Bad:
// a length no real message has, so the stream is garbage from here on.
Extract extract(Link& l, nlohmann::json& out) {
    std::string& buf = l.rx;
    if (buf.size() < 4) return Extract::None;
    const uint32_t len = static_cast<unsigned char>(buf[0]) | (static_cast<unsigned char>(buf[1]) << 8) |
                         (static_cast<unsigned char>(buf[2]) << 16) | (static_cast<uint32_t>(static_cast<unsigned char>(buf[3])) << 24);
    if (len > kMaxMessage) return Extract::Bad;
    if (buf.size() < 4 + static_cast<size_t>(len)) return Extract::None;
    bool ok = true;
    try { out = nlohmann::json::parse(buf.begin() + 4, buf.begin() + 4 + len); }
    catch (...) { ok = false; }
    buf.erase(0, 4 + static_cast<size_t>(len));
    return ok ? Extract::Message : Extract::Bad;
}

// Stops sending (after what's queued), reads whatever the other side still
// sends, then closes. Closing with unread data waiting makes the system reset
// the connection instead, and a reset can throw away the message we just
// sent (the reason for a kick, say) before the other side reads it.
void finish(Link& l) {
    flush(l);
    ::shutdown(l.fd, kShutdownSend);
    receive(l);
    l.rx.clear();
    close_socket(l.fd);
    l.fd = kInvalidSocket;
}

} // namespace

// struct NetServer::Impl / NetClient::Impl must be complete (defined) before
// anything below dereferences a NetServer*/NetClient*'s impl_.
struct NetServer::Impl {
    SocketFd listen_fd = kInvalidSocket;
    struct Conn { Link link; int id = 0; std::string address; double since = 0.0; };
    std::vector<Conn> conns;   // joined
    struct Pending { Link link; std::string address; double since = 0.0; };
    std::vector<Pending> pending; // connected, hasn't said hello yet
    struct Closing { Link link; double since = 0.0; bool shut = false; };
    std::vector<Closing> closing; // told why they're going; finishing sending it
    int next_conn_id = 1;
    std::map<uint32_t, std::unique_ptr<NetObject>> objects;
    uint32_t next_net_id = 1;

    Conn* find(int id) {
        for (Conn& c : conns) if (c.id == id) return &c;
        return nullptr;
    }
    void close_with(Link&& link, const nlohmann::json& last) {
        send_json(link, last);
        closing.push_back({std::move(link), now_seconds(), false});
    }
    void update_closing() {
        const double now = now_seconds();
        for (size_t i = 0; i < closing.size();) {
            Closing& c = closing[i];
            flush(c.link);
            if (!c.shut && queued(c.link) == 0 && !c.link.broken) {
                ::shutdown(c.link.fd, kShutdownSend);
                c.shut = true;
            }
            const bool ended = !receive(c.link); // the other side closed too
            c.link.rx.clear();
            if (ended || c.link.broken || now - c.since > kCloseTimeout) {
                close_socket(c.link.fd);
                closing.erase(closing.begin() + static_cast<long>(i));
                continue;
            }
            ++i;
        }
    }
};

struct NetClient::Impl {
    Link link;
    int conn_id = 0; // from the server's welcome
    std::map<uint32_t, std::unique_ptr<NetObject>> objects;
    bool connected = false;
    std::string reason; // why the last connection ended or was refused
};

// A friend "access key": lets the send/dispatch code here reach NetObject's
// private registries without exposing them on the public API.
struct NetObjectAccess {
    static void set_id(NetObject& o, uint32_t id, const std::string& cls) { o.id_ = id; o.class_name_ = cls; }
    static std::vector<std::pair<std::string, NetObject::NetSyncField>>& fields(NetObject& o) { return o.fields_; }
    static std::vector<std::pair<std::string, std::function<void(const NetArgs&)>>>& commands(NetObject& o) { return o.commands_; }
    static std::vector<std::pair<std::string, std::function<void(const NetArgs&)>>>& client_rpcs(NetObject& o) { return o.client_rpcs_; }
    static NetServer::Impl& server_impl(NetServer& s) { return *s.impl_; }
    static NetClient::Impl& client_impl(NetClient& c) { return *c.impl_; }
};

static std::unordered_map<std::string, std::function<std::unique_ptr<NetObject>()>>& net_class_registry() {
    static std::unordered_map<std::string, std::function<std::unique_ptr<NetObject>()>> m;
    return m;
}

static NetServer* g_active_server = nullptr;
static NetClient* g_active_client = nullptr;
static int g_command_sender = 0; // set while an on_command handler runs

static void net_send_spawn(Link& link, NetObject& obj) {
    nlohmann::json vars = nlohmann::json::object();
    for (auto& [name, field] : NetObjectAccess::fields(obj)) vars[name] = field.get();
    send_json(link, { {"t", "spawn"}, {"id", obj.net_id()}, {"class", obj.class_name()}, {"vars", vars} });
}

namespace net {
bool is_server() { return g_active_server != nullptr; }
bool is_client() { return g_active_client != nullptr; }
int command_sender() { return g_command_sender; }
}

void net_register_class(const std::string& class_name, std::function<std::unique_ptr<NetObject>()> make) {
    net_class_registry()[class_name] = std::move(make);
}

// --- NetObject ------------------------------------------------------------

void NetObject::add_sync_field(const std::string& name, NetSyncField field) { fields_.emplace_back(name, std::move(field)); }
void NetObject::on_command(const std::string& name, std::function<void(const NetArgs&)> fn) { commands_.emplace_back(name, std::move(fn)); }
void NetObject::on_client_rpc(const std::string& name, std::function<void(const NetArgs&)> fn) { client_rpcs_.emplace_back(name, std::move(fn)); }

void NetObject::call_command(const std::string& name, NetArgs args) {
    if (!g_active_client) { log_warn("call_command(\"" + name + "\"): not connected as a client"); return; }
    send_json(NetObjectAccess::client_impl(*g_active_client).link, { {"t", "cmd"}, {"id", id_}, {"name", name}, {"args", args} });
}

void NetObject::call_client_rpc(const std::string& name, NetArgs args) {
    if (!g_active_server) { log_warn("call_client_rpc(\"" + name + "\"): not running as a server"); return; }
    nlohmann::json msg = { {"t", "rpc"}, {"id", id_}, {"name", name}, {"args", args} };
    for (auto& c : NetObjectAccess::server_impl(*g_active_server).conns) send_json(c.link, msg);
}

bool NetObject::call_target_rpc(int conn_id, const std::string& name, NetArgs args) {
    if (!g_active_server) { log_warn("call_target_rpc(\"" + name + "\"): not running as a server"); return false; }
    auto* c = NetObjectAccess::server_impl(*g_active_server).find(conn_id);
    if (!c) return false;
    // Same message as a ClientRpc: the client can't tell (or need to) that only it got it.
    send_json(c->link, { {"t", "rpc"}, {"id", id_}, {"name", name}, {"args", args} });
    return true;
}

// --- spawn / despawn (server-only) -----------------------------------------

NetObject* net_spawn(const std::string& class_name) {
    if (!g_active_server) { log_warn("net_spawn(\"" + class_name + "\"): no active server"); return nullptr; }
    auto it = net_class_registry().find(class_name);
    if (it == net_class_registry().end()) { log_error("net_spawn: unregistered class \"" + class_name + "\""); return nullptr; }
    std::unique_ptr<NetObject> obj = it->second();
    uint32_t id = g_active_server->impl_->next_net_id++;
    NetObjectAccess::set_id(*obj, id, class_name);
    NetObject* raw = obj.get();
    g_active_server->impl_->objects[id] = std::move(obj);
    for (auto& c : g_active_server->impl_->conns) net_send_spawn(c.link, *raw);
    return raw;
}

void net_despawn(NetObject* obj) {
    if (!g_active_server || !obj) return;
    uint32_t id = obj->net_id();
    nlohmann::json msg = { {"t", "despawn"}, {"id", id} };
    for (auto& c : g_active_server->impl_->conns) send_json(c.link, msg);
    g_active_server->impl_->objects.erase(id);
}

NetObject* net_find(uint32_t id) {
    if (g_active_server) {
        auto& objs = NetObjectAccess::server_impl(*g_active_server).objects;
        auto it = objs.find(id);
        return it != objs.end() ? it->second.get() : nullptr;
    }
    if (g_active_client) {
        auto& objs = NetObjectAccess::client_impl(*g_active_client).objects;
        auto it = objs.find(id);
        return it != objs.end() ? it->second.get() : nullptr;
    }
    return nullptr;
}

void net_each_object(const std::function<void(NetObject&)>& fn) {
    if (g_active_server) { for (auto& [id, obj] : NetObjectAccess::server_impl(*g_active_server).objects) fn(*obj); return; }
    if (g_active_client) { for (auto& [id, obj] : NetObjectAccess::client_impl(*g_active_client).objects) fn(*obj); return; }
}

// --- NetServer --------------------------------------------------------------

NetServer::NetServer() : impl_(std::make_unique<Impl>()) {}
NetServer::~NetServer() { stop(); }

bool NetServer::listen(int port) {
    ensure_sockets_ready();
    impl_->listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (impl_->listen_fd == kInvalidSocket) return false;
    int yes = 1;
#if defined(_WIN32)
    // Not SO_REUSEADDR: on Windows that lets a second program bind a port
    // that's already listening, so two servers would quietly share one.
    setsockopt(impl_->listen_fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&yes), sizeof(yes));
#else
    // Restarting a server right away: the old one's connections linger in
    // TIME_WAIT, and without this the port stays taken for a minute or two.
    setsockopt(impl_->listen_fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
#endif
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (::bind(impl_->listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(impl_->listen_fd, 16) != 0) {
        close_socket(impl_->listen_fd);
        impl_->listen_fd = kInvalidSocket;
        return false;
    }
    set_nonblocking(impl_->listen_fd);
    g_active_server = this;
    return true;
}

void NetServer::update() {
    if (impl_->listen_fd == kInvalidSocket) return;
    Impl& im = *impl_;
    const double now = now_seconds();

    for (;;) {   // drain pending connections
        sockaddr_in cliaddr{};
        NetSockLen len = sizeof(cliaddr);
        SocketFd fd = ::accept(im.listen_fd, reinterpret_cast<sockaddr*>(&cliaddr), &len);
        if (fd == kInvalidSocket) break;
        prepare_socket(fd);
        Impl::Pending p;
        p.link.fd = fd;
        p.address = address_of(cliaddr);
        p.since = now;
        im.pending.push_back(std::move(p));
    }

    // New connections: nothing reaches them (and they reach nothing) until
    // their hello says they may join.
    for (size_t i = 0; i < im.pending.size();) {
        Impl::Pending& p = im.pending[i];
        const bool open = receive(p.link);
        nlohmann::json msg;
        const Extract e = extract(p.link, msg);
        if (e == Extract::None) {
            if (!open || now - p.since > kJoinTimeout) {
                close_socket(p.link.fd);
                im.pending.erase(im.pending.begin() + static_cast<long>(i));
                continue;
            }
            ++i;
            continue;
        }
        Impl::Pending taken = std::move(p);
        im.pending.erase(im.pending.begin() + static_cast<long>(i));
        std::string refusal;
        if (e == Extract::Bad || msg.value("t", std::string()) != "hello") {
            close_socket(taken.link.fd); // not one of ours
            continue;
        }
        if (msg.value("v", 0) != kProtocol) refusal = "The server runs a different version of Thistle";
        else if (!password.empty() && msg.value("password", std::string()) != password) refusal = "Wrong password";
        else if (max_players > 0 && static_cast<int>(im.conns.size()) >= max_players) refusal = "The server is full";
        if (!refusal.empty()) {
            im.close_with(std::move(taken.link), { {"t", "refused"}, {"reason", refusal} });
            continue;
        }
        const int cid = im.next_conn_id++;
        im.conns.push_back({std::move(taken.link), cid, taken.address, now});
        Link& link = im.conns.back().link; // (rx may already hold its first commands)
        send_json(link, { {"t", "welcome"}, {"conn", cid} });
        for (auto& [id, obj] : im.objects) net_send_spawn(link, *obj); // full snapshot for the newcomer
        if (on_connect) on_connect(cid);
    }

    // Joined connections. By id, not by reference: a handler can kick
    // someone (its own sender included), which removes them from conns.
    std::vector<int> ids;
    for (const Impl::Conn& c : im.conns) ids.push_back(c.id);
    for (const int cid : ids) {
        Impl::Conn* c = im.find(cid);
        if (!c) continue;
        flush(c->link);
        bool open = receive(c->link);
        nlohmann::json msg;
        Extract e;
        while (open && (e = extract(c->link, msg)) != Extract::None) {
            if (e == Extract::Bad) { open = false; break; }
            if (msg.value("t", std::string()) != "cmd") continue; // a joined client only ever sends Commands
            const uint32_t id = msg.value("id", 0u);
            const std::string name = msg.value("name", std::string());
            auto it = im.objects.find(id);
            if (it == im.objects.end()) continue;
            for (auto& [n, fn] : NetObjectAccess::commands(*it->second)) {
                if (n != name) continue;
                g_command_sender = cid;
                fn(msg.value("args", NetArgs::object()));
                g_command_sender = 0;
                break;
            }
            c = im.find(cid); // the handler may have kicked them
            if (!c) break;
        }
        if (!c) continue;
        if (!open || c->link.broken) {
            if (c->link.broken && queued(c->link) > 0) {
                log_warn("net: dropped connection " + std::to_string(cid) + " (" + c->address + "): it stopped receiving");
            }
            close_socket(c->link.fd);
            im.conns.erase(im.conns.begin() + (c - im.conns.data()));
            if (on_disconnect) on_disconnect(cid);
        }
    }

    for (auto& [id, obj] : im.objects) {   // flush dirty NetVars, one batched sync message per object
        nlohmann::json vars = nlohmann::json::object();
        for (auto& [name, field] : NetObjectAccess::fields(*obj))
            if (field.is_dirty()) { vars[name] = field.get(); field.clear_dirty(); }
        if (vars.empty()) continue;
        nlohmann::json msg = { {"t", "sync"}, {"id", id}, {"vars", vars} };
        for (auto& c : im.conns) send_json(c.link, msg);
    }

    im.update_closing();
}

bool NetServer::kick(int conn_id, const std::string& reason) {
    Impl::Conn* c = impl_->find(conn_id);
    if (!c) return false;
    Link link = std::move(c->link);
    impl_->conns.erase(impl_->conns.begin() + (c - impl_->conns.data()));
    impl_->close_with(std::move(link), { {"t", "bye"}, {"why", "kicked"}, {"reason", reason} });
    if (on_disconnect) on_disconnect(conn_id);
    return true;
}

void NetServer::stop() {
    Impl& im = *impl_;
    if (im.listen_fd != kInvalidSocket) { close_socket(im.listen_fd); im.listen_fd = kInvalidSocket; }
    for (auto& c : im.conns) {
        send_json(c.link, { {"t", "bye"}, {"why", "stopped"} });
        finish(c.link);
    }
    im.conns.clear();
    for (auto& p : im.pending) close_socket(p.link.fd);
    im.pending.clear();
    for (auto& c : im.closing) finish(c.link);
    im.closing.clear();
    im.objects.clear();
    if (g_active_server == this) g_active_server = nullptr;
}

int NetServer::connection_count() const { return static_cast<int>(impl_->conns.size()); }

std::vector<NetConnection> NetServer::connections() const {
    std::vector<NetConnection> out;
    const double now = now_seconds();
    for (const Impl::Conn& c : impl_->conns) out.push_back({c.id, c.address, now - c.since});
    return out;
}

NetObject* NetServer::find(uint32_t id) const {
    auto it = impl_->objects.find(id);
    return it != impl_->objects.end() ? it->second.get() : nullptr;
}

// --- NetClient --------------------------------------------------------------

NetClient::NetClient() : impl_(std::make_unique<Impl>()) {}
NetClient::~NetClient() { disconnect(); }

namespace {
// What a player should read about why the server said bye.
std::string bye_reason(const nlohmann::json& msg) {
    if (msg.value("why", std::string()) == "stopped") return "The server stopped";
    const std::string reason = msg.value("reason", std::string());
    return reason.empty() ? "Kicked by the server" : "Kicked: " + reason;
}
} // namespace

bool NetClient::connect(const std::string& host, int port, const std::string& password) {
    if (impl_->connected) disconnect();
    Impl& im = *impl_;
    im.reason.clear();
    const std::string where = host + ":" + std::to_string(port);
    ensure_sockets_ready();
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0) {
        im.reason = "Couldn't find " + host;
        return false;
    }
    SocketFd fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    bool ok = fd != kInvalidSocket && ::connect(fd, res->ai_addr, static_cast<int>(res->ai_addrlen)) == 0;
    freeaddrinfo(res);
    if (!ok) {
        if (fd != kInvalidSocket) close_socket(fd);
        im.reason = "Couldn't reach " + where;
        return false;
    }
    prepare_socket(fd);
    im.link = Link{};
    im.link.fd = fd;
    send_json(im.link, { {"t", "hello"}, {"v", kProtocol}, {"password", password} });

    // Wait for the server's answer: in, or turned away and why. A server in
    // this same process (a test, a game hosting its own) has to be kept
    // running meanwhile, or it could never answer.
    const double give_up = now_seconds() + kConnectTimeout;
    for (;;) {
        if (g_active_server) g_active_server->update();
        flush(im.link);
        const bool open = receive(im.link);
        nlohmann::json msg;
        Extract e = Extract::None;
        while ((e = extract(im.link, msg)) == Extract::Message) {
            const std::string t = msg.value("t", std::string());
            if (t == "welcome") {
                im.conn_id = msg.value("conn", 0);
                im.connected = true;
                g_active_client = this;
                if (on_connect) on_connect();
                return true; // anything after it (the objects) is for update()
            }
            if (t == "refused" || t == "bye") {
                im.reason = t == "bye" ? bye_reason(msg) : msg.value("reason", std::string("Refused by the server"));
                break;
            }
        }
        if (!im.reason.empty() || e == Extract::Bad || !open || im.link.broken || now_seconds() > give_up) {
            if (im.reason.empty()) im.reason = (open && !im.link.broken && e != Extract::Bad) ? "No answer from " + where : "The server closed the connection";
            close_socket(im.link.fd);
            im.link = Link{};
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

void NetClient::update() {
    Impl& im = *impl_;
    if (!im.connected) return;
    flush(im.link);
    const bool open = receive(im.link);

    nlohmann::json msg;
    Extract e;
    while ((e = extract(im.link, msg)) == Extract::Message) {
        std::string t = msg.value("t", std::string());
        uint32_t id = msg.value("id", 0u);
        if (t == "welcome") {
            im.conn_id = msg.value("conn", 0);
        } else if (t == "spawn") {
            std::string cls = msg.value("class", std::string());
            auto it = net_class_registry().find(cls);
            if (it == net_class_registry().end()) { log_error("net: unknown class \"" + cls + "\" (not registered on this machine)"); continue; }
            std::unique_ptr<NetObject> obj = it->second();
            NetObjectAccess::set_id(*obj, id, cls);
            nlohmann::json vars = msg.value("vars", NetArgs::object());
            for (auto& [name, field] : NetObjectAccess::fields(*obj))
                if (vars.contains(name)) field.set(vars[name]);
            im.objects[id] = std::move(obj);
        } else if (t == "sync") {
            auto it = im.objects.find(id);
            if (it == im.objects.end()) continue;
            // Bind to a named variable before .items() — nlohmann's items()
            // returns a proxy that references the json it was called on, and
            // calling it directly on a temporary (msg.value(...).items()) left
            // that proxy pointing at an already-destroyed temporary. Real bug,
            // found by actually running this, not by reading the code.
            nlohmann::json vars = msg.value("vars", NetArgs::object());
            for (auto& [name, val] : vars.items())
                for (auto& [n, field] : NetObjectAccess::fields(*it->second))
                    if (n == name) { field.set(val); break; }
        } else if (t == "rpc") {
            auto it = im.objects.find(id);
            if (it == im.objects.end()) continue;
            std::string name = msg.value("name", std::string());
            for (auto& [n, fn] : NetObjectAccess::client_rpcs(*it->second))
                if (n == name) { fn(msg.value("args", NetArgs::object())); break; }
        } else if (t == "despawn") {
            im.objects.erase(id);
        } else if (t == "bye") {
            end(bye_reason(msg));
            return;
        }
        if (!im.connected) return; // a handler disconnected
    }
    if (!open || e == Extract::Bad || im.link.broken) end("Lost the connection to the server");
}

void NetClient::disconnect() { end(""); }

void NetClient::end(const std::string& reason) {
    Impl& im = *impl_;
    bool was = im.connected;
    if (im.link.fd != kInvalidSocket) finish(im.link);
    im.link = Link{};
    im.connected = false;
    im.conn_id = 0;
    im.objects.clear();
    im.reason = reason; // before on_disconnect, which may want to show it
    if (g_active_client == this) g_active_client = nullptr;
    if (was && on_disconnect) on_disconnect();
}

bool NetClient::connected() const { return impl_->connected; }
int NetClient::connection_id() const { return impl_->conn_id; }
const std::string& NetClient::disconnect_reason() const { return impl_->reason; }

NetObject* NetClient::find(uint32_t id) const {
    auto it = impl_->objects.find(id);
    return it != impl_->objects.end() ? it->second.get() : nullptr;
}

} // namespace thistle
