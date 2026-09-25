// Realtime networking (NetServer, NetClient, NetObject), shared by the full
// engine and the headless server library.
#include "thistle_core.h"

#include <cerrno>
#include <cstring>
#include <map>
#include <string>
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
// Plain blocking-connect / non-blocking-poll TCP sockets, length-prefixed
// JSON messages. See the big comment on this API in thistle.hpp before
// touching this — the scope is deliberate, not an oversight.

#if defined(_WIN32)
    using SocketFd = SOCKET;
    constexpr SocketFd kInvalidSocket = INVALID_SOCKET;
    using NetSockLen = int;
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
    static void close_socket(SocketFd s) { ::close(s); }
    static bool would_block() { return errno == EWOULDBLOCK || errno == EAGAIN; }
    static void set_nonblocking(SocketFd s) { int fl = fcntl(s, F_GETFL, 0); fcntl(s, F_SETFL, fl | O_NONBLOCK); }
    static void ensure_sockets_ready() {}
#endif

static void set_nodelay(SocketFd s) {   // small JSON messages + Nagle's algorithm = needless latency
    int yes = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&yes, sizeof(yes));
}

// Blocking send loop (busy-retries on EWOULDBLOCK) — fine for the tiny
// messages this protocol actually carries; not fine if you start sending
// megabytes over this, which you shouldn't.
static bool net_send_all(SocketFd s, const char* data, size_t n) {
    size_t sent = 0;
    while (sent < n) {
        int r = ::send(s, data + sent, static_cast<int>(n - sent), 0);
        if (r > 0) { sent += static_cast<size_t>(r); continue; }
        if (r < 0 && would_block()) continue;
        return false;
    }
    return true;
}

static bool net_send_json(SocketFd s, const nlohmann::json& j) {
    std::string body = j.dump();
    uint32_t len = static_cast<uint32_t>(body.size());
    unsigned char hdr[4] = {
        static_cast<unsigned char>(len & 0xFF), static_cast<unsigned char>((len >> 8) & 0xFF),
        static_cast<unsigned char>((len >> 16) & 0xFF), static_cast<unsigned char>((len >> 24) & 0xFF)
    };
    return net_send_all(s, reinterpret_cast<const char*>(hdr), 4) && net_send_all(s, body.data(), body.size());
}

// Drains everything currently available into buf. Returns false if the
// connection should be closed (orderly EOF or a real error).
static bool net_recv_available(SocketFd s, std::string& buf) {
    char tmp[4096];
    for (;;) {
        int r = ::recv(s, tmp, sizeof(tmp), 0);
        if (r > 0) { buf.append(tmp, static_cast<size_t>(r)); continue; }
        if (r == 0) return false;
        if (would_block()) return true;
        return false;
    }
}

// Pulls one complete length-prefixed message out of buf, if there is one.
static bool net_extract_message(std::string& buf, nlohmann::json& out) {
    if (buf.size() < 4) return false;
    uint32_t len = static_cast<unsigned char>(buf[0]) | (static_cast<unsigned char>(buf[1]) << 8) |
                   (static_cast<unsigned char>(buf[2]) << 16) | (static_cast<unsigned char>(buf[3]) << 24);
    if (buf.size() < 4 + len) return false;
    bool ok = true;
    try { out = nlohmann::json::parse(buf.begin() + 4, buf.begin() + 4 + len); }
    catch (...) { ok = false; }
    buf.erase(0, 4 + len);
    return ok;
}

// A friend "access key": lets the send/dispatch code above reach NetObject's
// private registries without exposing them on the public API.
// struct NetServer::Impl / NetClient::Impl must be complete (defined) before
// anything below dereferences a NetServer*/NetClient*'s impl_ — hence up here,
// right after the access key that lets non-member code reach impl_ at all.
struct NetServer::Impl {
    SocketFd listen_fd = kInvalidSocket;
    struct Conn { SocketFd fd; std::string rx; int id; };
    std::vector<Conn> conns;
    int next_conn_id = 1;
    std::map<uint32_t, std::unique_ptr<NetObject>> objects;
    uint32_t next_net_id = 1;
};

struct NetClient::Impl {
    SocketFd fd = kInvalidSocket;
    int conn_id = 0; // from the server's welcome
    std::string rx;
    std::map<uint32_t, std::unique_ptr<NetObject>> objects;
    bool connected = false;
};

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

static void net_send_spawn(SocketFd fd, NetObject& obj) {
    nlohmann::json vars = nlohmann::json::object();
    for (auto& [name, field] : NetObjectAccess::fields(obj)) vars[name] = field.get();
    net_send_json(fd, { {"t", "spawn"}, {"id", obj.net_id()}, {"class", obj.class_name()}, {"vars", vars} });
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
    net_send_json(NetObjectAccess::client_impl(*g_active_client).fd, { {"t", "cmd"}, {"id", id_}, {"name", name}, {"args", args} });
}

void NetObject::call_client_rpc(const std::string& name, NetArgs args) {
    if (!g_active_server) { log_warn("call_client_rpc(\"" + name + "\"): not running as a server"); return; }
    nlohmann::json msg = { {"t", "rpc"}, {"id", id_}, {"name", name}, {"args", args} };
    for (auto& c : NetObjectAccess::server_impl(*g_active_server).conns) net_send_json(c.fd, msg);
}

bool NetObject::call_target_rpc(int conn_id, const std::string& name, NetArgs args) {
    if (!g_active_server) { log_warn("call_target_rpc(\"" + name + "\"): not running as a server"); return false; }
    for (auto& c : NetObjectAccess::server_impl(*g_active_server).conns) {
        if (c.id != conn_id) continue;
        // Same message as a ClientRpc: the client can't tell (or need to) that only it got it.
        net_send_json(c.fd, { {"t", "rpc"}, {"id", id_}, {"name", name}, {"args", args} });
        return true;
    }
    return false;
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
    for (auto& c : g_active_server->impl_->conns) net_send_spawn(c.fd, *raw);
    return raw;
}

void net_despawn(NetObject* obj) {
    if (!g_active_server || !obj) return;
    uint32_t id = obj->net_id();
    nlohmann::json msg = { {"t", "despawn"}, {"id", id} };
    for (auto& c : g_active_server->impl_->conns) net_send_json(c.fd, msg);
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
    setsockopt(impl_->listen_fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
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

    for (;;) {   // drain pending connections
        sockaddr_in cliaddr{};
        NetSockLen len = sizeof(cliaddr);
        SocketFd fd = ::accept(impl_->listen_fd, reinterpret_cast<sockaddr*>(&cliaddr), &len);
        if (fd == kInvalidSocket) break;
        set_nonblocking(fd);
        set_nodelay(fd);
        int cid = impl_->next_conn_id++;
        impl_->conns.push_back({fd, std::string(), cid});
        net_send_json(fd, { {"t", "welcome"}, {"conn", cid} });            // older clients ignore it
        for (auto& [id, obj] : impl_->objects) net_send_spawn(fd, *obj);   // full snapshot for the newcomer
        if (on_connect) on_connect(cid);
    }

    for (size_t i = 0; i < impl_->conns.size(); ) {
        Impl::Conn& c = impl_->conns[i];
        if (!net_recv_available(c.fd, c.rx)) {
            close_socket(c.fd);
            int cid = c.id;
            impl_->conns.erase(impl_->conns.begin() + static_cast<long>(i));
            if (on_disconnect) on_disconnect(cid);
            continue;
        }
        nlohmann::json msg;
        while (net_extract_message(c.rx, msg)) {
            if (msg.value("t", std::string()) != "cmd") continue;   // the server only ever receives Commands
            uint32_t id = msg.value("id", 0u);
            std::string name = msg.value("name", std::string());
            auto it = impl_->objects.find(id);
            if (it == impl_->objects.end()) continue;
            for (auto& [n, fn] : NetObjectAccess::commands(*it->second)) {
                if (n != name) continue;
                g_command_sender = c.id;
                fn(msg.value("args", NetArgs::object()));
                g_command_sender = 0;
                break;
            }
        }
        ++i;
    }

    for (auto& [id, obj] : impl_->objects) {   // flush dirty NetVars, one batched sync message per object
        nlohmann::json vars = nlohmann::json::object();
        for (auto& [name, field] : NetObjectAccess::fields(*obj))
            if (field.is_dirty()) { vars[name] = field.get(); field.clear_dirty(); }
        if (vars.empty()) continue;
        nlohmann::json msg = { {"t", "sync"}, {"id", id}, {"vars", vars} };
        for (auto& c : impl_->conns) net_send_json(c.fd, msg);
    }
}

void NetServer::stop() {
    if (impl_->listen_fd != kInvalidSocket) { close_socket(impl_->listen_fd); impl_->listen_fd = kInvalidSocket; }
    for (auto& c : impl_->conns) close_socket(c.fd);
    impl_->conns.clear();
    impl_->objects.clear();
    if (g_active_server == this) g_active_server = nullptr;
}

int NetServer::connection_count() const { return static_cast<int>(impl_->conns.size()); }

NetObject* NetServer::find(uint32_t id) const {
    auto it = impl_->objects.find(id);
    return it != impl_->objects.end() ? it->second.get() : nullptr;
}

// --- NetClient --------------------------------------------------------------

NetClient::NetClient() : impl_(std::make_unique<Impl>()) {}
NetClient::~NetClient() { disconnect(); }

bool NetClient::connect(const std::string& host, int port) {
    ensure_sockets_ready();
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0) return false;
    SocketFd fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    bool ok = fd != kInvalidSocket && ::connect(fd, res->ai_addr, static_cast<int>(res->ai_addrlen)) == 0;
    freeaddrinfo(res);
    if (!ok) { if (fd != kInvalidSocket) close_socket(fd); return false; }
    set_nonblocking(fd);
    set_nodelay(fd);
    impl_->fd = fd;
    impl_->connected = true;
    g_active_client = this;
    if (on_connect) on_connect();
    return true;
}

void NetClient::update() {
    if (!impl_->connected) return;
    if (!net_recv_available(impl_->fd, impl_->rx)) { disconnect(); return; }

    nlohmann::json msg;
    while (net_extract_message(impl_->rx, msg)) {
        std::string t = msg.value("t", std::string());
        uint32_t id = msg.value("id", 0u);
        if (t == "welcome") {
            impl_->conn_id = msg.value("conn", 0);
        } else if (t == "spawn") {
            std::string cls = msg.value("class", std::string());
            auto it = net_class_registry().find(cls);
            if (it == net_class_registry().end()) { log_error("net: unknown class \"" + cls + "\" (not registered on this machine)"); continue; }
            std::unique_ptr<NetObject> obj = it->second();
            NetObjectAccess::set_id(*obj, id, cls);
            nlohmann::json vars = msg.value("vars", NetArgs::object());
            for (auto& [name, field] : NetObjectAccess::fields(*obj))
                if (vars.contains(name)) field.set(vars[name]);
            impl_->objects[id] = std::move(obj);
        } else if (t == "sync") {
            auto it = impl_->objects.find(id);
            if (it == impl_->objects.end()) continue;
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
            auto it = impl_->objects.find(id);
            if (it == impl_->objects.end()) continue;
            std::string name = msg.value("name", std::string());
            for (auto& [n, fn] : NetObjectAccess::client_rpcs(*it->second))
                if (n == name) { fn(msg.value("args", NetArgs::object())); break; }
        } else if (t == "despawn") {
            impl_->objects.erase(id);
        }
    }
}

void NetClient::disconnect() {
    bool was = impl_->connected;
    if (impl_->fd != kInvalidSocket) { close_socket(impl_->fd); impl_->fd = kInvalidSocket; }
    impl_->connected = false;
    impl_->conn_id = 0;
    impl_->objects.clear();
    if (g_active_client == this) g_active_client = nullptr;
    if (was && on_disconnect) on_disconnect();
}

bool NetClient::connected() const { return impl_->connected; }
int NetClient::connection_id() const { return impl_->conn_id; }

NetObject* NetClient::find(uint32_t id) const {
    auto it = impl_->objects.find(id);
    return it != impl_->objects.end() ? it->second.get() : nullptr;
}

} // namespace thistle
