/* SPDX-License-Identifier: BSD-2-Clause */
#include "runtime_data_socket.hpp"
#include "runtime_data_store.hpp"
#include "core/logging.hpp"
#include <cerrno>
#include <charconv>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static std::string_view trim(std::string_view s)
{
    while (!s.empty() && std::isspace((unsigned char)s.front())) s.remove_prefix(1);
    while (!s.empty() && std::isspace((unsigned char)s.back())) s.remove_suffix(1);
    return s;
}

static RuntimeDataStore::Value parse_value(std::string_view s)
{
    s = trim(s);
    if (s == "true" || s == "on") return true;
    if (s == "false" || s == "off") return false;
    int64_t i = 0;
    auto ir = std::from_chars(s.data(), s.data() + s.size(), i);
    if (!s.empty() && ir.ec == std::errc() && ir.ptr == s.data() + s.size()) return i;
    std::string v(s);
    char *end = nullptr;
    errno = 0;
    const double d = std::strtod(v.c_str(), &end);
    if (errno == 0 && end == v.c_str() + v.size()) return d;
    return v;
}

RuntimeDataSocket::RuntimeDataSocket(RuntimeDataStore &store, const std::string &path)
    : socket_path_(path), store_(store)
{
    if (path.empty() || path.size() >= sizeof(sockaddr_un::sun_path)) return;
    server_fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (server_fd_ < 0) { LOG_ERROR("RuntimeDataSocket: socket() failed: " << strerror(errno)); return; }
    ::unlink(path.c_str());
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    if (::bind(server_fd_, (sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("RuntimeDataSocket: bind() failed: " << strerror(errno));
        ::close(server_fd_); server_fd_ = -1; return;
    }
    if (::listen(server_fd_, 4) < 0) {
        LOG_ERROR("RuntimeDataSocket: listen() failed: " << strerror(errno));
        ::close(server_fd_); server_fd_ = -1; ::unlink(path.c_str()); return;
    }
    LOG(1, "RuntimeDataSocket: listening on " << path);
}

RuntimeDataSocket::~RuntimeDataSocket()
{
    CloseClient();
    if (server_fd_ >= 0) ::close(server_fd_);
    if (!socket_path_.empty()) ::unlink(socket_path_.c_str());
}

bool RuntimeDataSocket::IsValid() const { return server_fd_ >= 0; }

void RuntimeDataSocket::CloseClient()
{
    if (client_fd_ >= 0) ::close(client_fd_);
    client_fd_ = -1;
    recv_buf_.clear();
}

void RuntimeDataSocket::AcceptConnections()
{
    if (server_fd_ < 0) return;
    const int fd = ::accept4(server_fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0) return;
    CloseClient();
    client_fd_ = fd;
}

void RuntimeDataSocket::Process()
{
    if (client_fd_ < 0) return;
    char buf[4096];
    for (;;) {
        const ssize_t n = ::recv(client_fd_, buf, sizeof(buf), 0);
        if (n > 0) {
            recv_buf_.append(buf, (size_t)n);
            if (recv_buf_.size() > MAX_RECEIVE_BUFFER) { CloseClient(); return; }
            continue;
        }
        if (n == 0) { CloseClient(); return; }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        CloseClient(); return;
    }

    size_t pos;
    while ((pos = recv_buf_.find('\n')) != std::string::npos) {
        std::string line = recv_buf_.substr(0, pos);
        recv_buf_.erase(0, pos + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() <= MAX_COMMAND_LENGTH) ProcessLine(line);
    }
    if (recv_buf_.size() > MAX_COMMAND_LENGTH) CloseClient();
}

void RuntimeDataSocket::ProcessLine(const std::string &line)
{
    std::istringstream in(line);
    std::string op, key;
    in >> op;
    if (op == "SET") {
        in >> key;
        std::string value;
        std::getline(in, value);
        value = std::string(trim(value));
        if (!key.empty() && !value.empty()) store_.Set(key, parse_value(value));
    } else if (op == "DEL") {
        in >> key;
        if (!key.empty()) store_.Delete(key);
    } else if (op == "CLEAR") {
        store_.Clear();
    } else if (!trim(line).empty()) {
        LOG_ERROR("RuntimeDataSocket: unknown command: " << line);
    }
}
