// docs/08#transport-layers: the agent drops a socket whose peer stopped answering pings, and reconnects.
// The server here completes the WebSocket handshake and then never sends a byte — a peer gone
// without a FIN, as a dropped Wi-Fi uplink looks from the robot.
#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <glib.h>

#include "core/loop.hpp"
#include "core/signaling_client.hpp"

namespace {

/// Accepts WebSocket upgrades on 127.0.0.1 and then goes silent: no pong, no close, ever.
class SilentServer {
  public:
    SilentServer() {
        listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
        listen(listen_fd_, 4);
        socklen_t len = sizeof addr;
        getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);
        thread_ = std::thread([this] { serve(); });
    }
    ~SilentServer() {
        stop_ = true;
        thread_.join();
        for (int fd : clients_) close(fd);
        close(listen_fd_);
    }
    int port() const { return port_; }
    int accepted() const { return accepted_; }

  private:
    void serve() {
        while (!stop_) {
            pollfd p{listen_fd_, POLLIN, 0};
            if (poll(&p, 1, 50) <= 0) continue;
            const int fd = accept(listen_fd_, nullptr, nullptr);
            if (fd < 0) continue;
            std::string request;
            char buf[1024];
            while (request.find("\r\n\r\n") == std::string::npos) {
                const ssize_t n = read(fd, buf, sizeof buf);
                if (n <= 0) break;
                request.append(buf, static_cast<size_t>(n));
            }
            const auto at = request.find("Sec-WebSocket-Key: ");
            if (at == std::string::npos) {
                close(fd);
                continue;
            }
            const auto start = at + 19;
            const std::string key = request.substr(start, request.find("\r\n", start) - start);
            const std::string reply = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                                      "Sec-WebSocket-Accept: " + accept_key(key) + "\r\n\r\n";
            if (write(fd, reply.data(), reply.size()) != static_cast<ssize_t>(reply.size())) {
                close(fd);
                continue;
            }
            clients_.push_back(fd); // held open, never read or written again
            ++accepted_;
        }
    }
    static std::string accept_key(const std::string& key) {
        const std::string in = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        GChecksum* sha1 = g_checksum_new(G_CHECKSUM_SHA1);
        g_checksum_update(sha1, reinterpret_cast<const guchar*>(in.data()), static_cast<gssize>(in.size()));
        guint8 digest[20];
        gsize len = sizeof digest;
        g_checksum_get_digest(sha1, digest, &len);
        g_checksum_free(sha1);
        gchar* b64 = g_base64_encode(digest, len);
        std::string out(b64);
        g_free(b64);
        return out;
    }

    int listen_fd_ = -1;
    int port_ = 0;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<int> accepted_{0};
    std::vector<int> clients_;
};

bool wait_for(const std::function<bool()>& done, std::chrono::milliseconds limit) {
    const auto until = std::chrono::steady_clock::now() + limit;
    while (!done()) {
        if (std::chrono::steady_clock::now() > until) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return true;
}

} // namespace

TEST(SignalingClient, dropsASilentSocketAndReconnects) {
    SilentServer server;
    fjarr::CoreLoop loop;
    loop.start();
    std::unique_ptr<fjarr::core::SignalingClient> client;
    fjarr::core::SignalingConfig config;
    config.url = "ws://127.0.0.1:" + std::to_string(server.port()) + "/ws";
    config.robot_id = "test-robot";
    config.dev_token = "t";
    config.keepalive_interval_s = 1;
    config.keepalive_pong_timeout_s = 1;
    loop.call_sync([&] {
        client = std::make_unique<fjarr::core::SignalingClient>(loop, config, fjarr::core::SignalingHooks{});
        client->start();
    });
    ASSERT_TRUE(wait_for([&] { return server.accepted() >= 1; }, std::chrono::seconds(5)));
    // A ping after ≤ 1 s, unanswered for 1 s, libsoup's 5 s wait for the dead peer's close frame,
    // then the first backoff step (≤ 600 ms): a second dial after ~7.6 s. Without the pong timeout
    // the socket stays "open" until TCP gives up, minutes later.
    EXPECT_TRUE(wait_for([&] { return server.accepted() >= 2; }, std::chrono::seconds(12)))
        << "the client kept a socket whose peer never answered a ping";
    loop.call_sync([&] { client.reset(); });
    loop.stop();
}
