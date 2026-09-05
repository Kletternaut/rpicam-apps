/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once

#include <string>

class RuntimeDataStore;

static constexpr const char *RUNTIME_DATA_SOCKET_DEFAULT_PATH = "/tmp/rpicam-vid-data.sock";

class RuntimeDataSocket
{
public:
    explicit RuntimeDataSocket(RuntimeDataStore &store,
                                const std::string &path = RUNTIME_DATA_SOCKET_DEFAULT_PATH);
    ~RuntimeDataSocket();

    RuntimeDataSocket(const RuntimeDataSocket &) = delete;
    RuntimeDataSocket &operator=(const RuntimeDataSocket &) = delete;

    bool IsValid() const;
    void AcceptConnections();

    // Drain all currently available socket data and apply complete commands
    // to the RuntimeDataStore. Non-blocking; safe to call once per main-loop
    // iteration.
    void Process();

    const std::string &Path() const { return socket_path_; }

private:
    static constexpr std::size_t MAX_RECEIVE_BUFFER = 64 * 1024;
    static constexpr std::size_t MAX_COMMAND_LENGTH = 4096;

    void CloseClient();
    void ProcessLine(const std::string &line);

    int server_fd_ = -1;
    int client_fd_ = -1;
    std::string socket_path_;
    std::string recv_buf_;
    RuntimeDataStore &store_;
};
