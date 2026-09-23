/**
 * docs/15 "whole-process hang (SIGSTOP)" and the docs/23 supervision contract (ADR-0019).
 *
 * systemd is the only defence against a deadlock in our own code, and it only works if two things
 * hold: the watchdog ping is driven by the core loop (so a wedged process stops pinging), and
 * READY is not claimed before the agent can actually serve. Neither was exercised by anything.
 *
 * This test IS the supervisor: a unix datagram socket named by NOTIFY_SOCKET, exactly what systemd
 * gives the unit, so the real `sd_notify` path in the daemon runs. Nothing here needs systemd
 * itself — the real `WatchdogSec` kill on a systemd host is an M2.5 gate.
 */
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <gtest/gtest.h>

extern char** environ;

namespace {

/// A stand-in for systemd's notify socket: bind it, hand its path to the child as NOTIFY_SOCKET.
class NotifySocket {
  public:
    NotifySocket() {
        char tmpl[] = "/tmp/fjarr-sup-XXXXXX";
        dir_ = mkdtemp(tmpl);
        path_ = dir_ + "/notify";
        fd_ = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, path_.c_str(), sizeof(addr.sun_path) - 1);
        bind_ok_ = ::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
        timeval tv{0, 250 * 1000};
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }
    ~NotifySocket() {
        if (fd_ >= 0) ::close(fd_);
        ::unlink(path_.c_str());
        ::rmdir(dir_.c_str());
    }
    bool ok() const { return bind_ok_ && fd_ >= 0; }
    const std::string& path() const { return path_; }

    /// Count the notifications containing `needle` over `ms`, draining as they arrive.
    int count(const char* needle, int ms) {
        int n = 0;
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < until) {
            char buf[512];
            const ssize_t r = ::recv(fd_, buf, sizeof(buf) - 1, 0);
            if (r <= 0) continue; // timeout: keep waiting until `ms` is spent
            buf[r] = '\0';
            if (std::strstr(buf, needle)) n++;
        }
        return n;
    }
    void drain(int ms) { count("", ms); }

  private:
    std::string dir_, path_;
    int fd_ = -1;
    bool bind_ok_ = false;
};

/// Spawn the real `fjarr-agent` with our environment; it never reaches a server on purpose.
pid_t spawn_agent(const std::string& notify_path) {
    std::vector<std::string> extra = {
        // The child is the daemon, and under the ASan preset its startup VA-API probe walks into
        // Intel's closed driver, which mallocs and `operator delete`s the same pointer
        // (iHD_drv_video.so, via vaDestroyContext). That is a third-party defect, not ours, and it
        // would abort the child before it ever pings. Leak detection goes too: this child is killed
        // abruptly on purpose, and the daemon's leaks are the memory ladder's job (docs/23), not
        // this probe's.
        "ASAN_OPTIONS=alloc_dealloc_mismatch=0:detect_leaks=0",
        "NOTIFY_SOCKET=" + notify_path,
        "WATCHDOG_USEC=1500000", // the daemon pings at WatchdogSec/3 = 500 ms
        "FJARR_AGENT_ROBOT_ID=supervision-probe",
        "FJARR_AGENT_SERVER_URL=ws://127.0.0.1:9", // discard port: the connection never succeeds
        "FJARR_MEDIA_ENCODER=software",
        "FJARR_INTROSPECT_ENABLED=0",
        "FJARR_AGENT_LOG_LEVEL=error",
    };
    std::vector<char*> env;
    for (char** e = environ; *e; e++) {
        const std::string s = *e;
        bool overridden = false;
        for (const auto& x : extra)
            if (s.rfind(x.substr(0, x.find('=') + 1), 0) == 0) overridden = true;
        if (!overridden) env.push_back(*e);
    }
    for (auto& x : extra) env.push_back(x.data());
    env.push_back(nullptr);

    const pid_t pid = ::fork();
    if (pid != 0) return pid;
    char* argv[] = {const_cast<char*>(FJARR_AGENT_BINARY), nullptr};
    ::execve(FJARR_AGENT_BINARY, argv, env.data());
    _exit(127);
}

void reap(pid_t pid) {
    ::kill(pid, SIGCONT); // never leave it stopped, whatever the assertions did
    ::kill(pid, SIGTERM);
    int status = 0;
    for (int i = 0; i < 100; i++) {
        if (::waitpid(pid, &status, WNOHANG) == pid) return;
        ::usleep(50 * 1000);
    }
    ::kill(pid, SIGKILL);
    ::waitpid(pid, &status, 0);
}

} // namespace

TEST(Supervision, theWatchdogPingsWhileHealthyAndStopsWhileTheProcessIsStopped) {
    NotifySocket notify;
    ASSERT_TRUE(notify.ok()) << "could not bind the fake notify socket";
    const pid_t pid = spawn_agent(notify.path());
    ASSERT_GT(pid, 0);

    // Healthy: pings at WatchdogSec/3 (500 ms), so ~4 in 2.5 s.
    const int healthy = notify.count("WATCHDOG=1", 2500);
    EXPECT_GE(healthy, 3) << "the agent is not feeding the watchdog (got " << healthy << " in 2.5 s)";

    // Stopped: the process cannot run, so it cannot ping — which is the whole point. A watchdog
    // fed from a thread that survives a wedged core loop would keep the agent "healthy" forever.
    ASSERT_EQ(::kill(pid, SIGSTOP), 0);
    notify.drain(300); // whatever was already in flight
    const int stopped = notify.count("WATCHDOG=1", 1500);
    EXPECT_EQ(stopped, 0) << "a stopped process still fed the watchdog " << stopped << " times";

    ASSERT_EQ(::kill(pid, SIGCONT), 0);
    const int resumed = notify.count("WATCHDOG=1", 2500);
    EXPECT_GE(resumed, 1) << "the agent did not resume feeding the watchdog after SIGCONT";
    reap(pid);
}

TEST(Supervision, readyIsNotClaimedBeforeTheAgentCanServe) {
    // docs/23: READY follows the first hello-ack, not process start. An agent that announces
    // itself ready while it has never reached the server would make systemd — and any operator
    // reading `systemctl status` — believe a robot is fine when it is unreachable.
    NotifySocket notify;
    ASSERT_TRUE(notify.ok());
    const pid_t pid = spawn_agent(notify.path());
    ASSERT_GT(pid, 0);
    const int ready = notify.count("READY=1", 3000);
    EXPECT_EQ(ready, 0) << "READY=1 was sent although the agent never reached a server";
    reap(pid);
}
