#include "event_loop.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <mutex>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>

#ifdef __linux__
#include <sys/epoll.h>
#else
#include <sys/event.h>
#endif

#include "protocol.h"
#include "server.h"  // MAX_LINE

namespace {

// Stop running commands for a client once this many reply bytes are waiting to
// be sent. Otherwise a client that pipelines thousands of GETs for big values
// and never reads the answers could make us buffer without limit.
const size_t OUT_LIMIT = 1024 * 1024;

struct Event {
    int fd;
    bool readable;
    bool writable;
};

// ---- Poller: the only OS-specific part ------------------------------------
// Both versions are level-triggered: "tell me again if there is still data".
// That is the forgiving mode, we can stop reading early and not lose a wakeup.
#ifdef __linux__

class Poller {
public:
    Poller() : fd_(epoll_create1(0)) {}
    ~Poller() {
        if (fd_ >= 0) close(fd_);
    }
    bool ok() const { return fd_ >= 0; }

    void add(int fd) { ctl(EPOLL_CTL_ADD, fd, true, false); }
    void set_interest(int fd, bool read, bool write) { ctl(EPOLL_CTL_MOD, fd, read, write); }
    void remove(int fd) { epoll_ctl(fd_, EPOLL_CTL_DEL, fd, nullptr); }

    // Blocks until something is ready. Returns how many events were stored.
    int wait(Event* out, int max) {
        epoll_event evs[64];
        int n = epoll_wait(fd_, evs, max < 64 ? max : 64, -1);
        if (n < 0) return errno == EINTR ? 0 : -1;
        for (int i = 0; i < n; i++) {
            uint32_t e = evs[i].events;
            // On error/hang-up, report both: the next recv()/send() will say what happened.
            bool broken = (e & (EPOLLERR | EPOLLHUP)) != 0;
            out[i] = {evs[i].data.fd, (e & EPOLLIN) != 0 || broken, (e & EPOLLOUT) != 0 || broken};
        }
        return n;
    }

private:
    void ctl(int op, int fd, bool read, bool write) {
        epoll_event ev{};
        ev.events = (read ? EPOLLIN : 0) | (write ? EPOLLOUT : 0);
        ev.data.fd = fd;
        epoll_ctl(fd_, op, fd, &ev);
    }
    int fd_;
};

#else  // macOS / BSD: kqueue

class Poller {
public:
    Poller() : fd_(kqueue()) {}
    ~Poller() {
        if (fd_ >= 0) close(fd_);
    }
    bool ok() const { return fd_ >= 0; }

    void add(int fd) {
        change(fd, EVFILT_READ, EV_ADD | EV_ENABLE);
        change(fd, EVFILT_WRITE, EV_ADD | EV_DISABLE);
    }
    void set_interest(int fd, bool read, bool write) {
        change(fd, EVFILT_READ, read ? EV_ENABLE : EV_DISABLE);
        change(fd, EVFILT_WRITE, write ? EV_ENABLE : EV_DISABLE);
    }
    void remove(int) {}  // kqueue forgets a socket automatically when it is closed

    int wait(Event* out, int max) {
        struct kevent evs[64];
        int n = kevent(fd_, nullptr, 0, evs, max < 64 ? max : 64, nullptr);
        if (n < 0) return errno == EINTR ? 0 : -1;
        for (int i = 0; i < n; i++) {
            bool broken = (evs[i].flags & EV_ERROR) != 0;
            out[i] = {static_cast<int>(evs[i].ident), evs[i].filter == EVFILT_READ || broken,
                      evs[i].filter == EVFILT_WRITE || broken};
        }
        return n;
    }

private:
    void change(int fd, short filter, unsigned short flags) {
        struct kevent kev;
        EV_SET(&kev, fd, filter, flags, 0, 0, nullptr);
        kevent(fd_, &kev, 1, nullptr, 0, nullptr);
    }
    int fd_;
};

#endif

void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// Everything we remember about one client between events. The blocking
// servers keep this on a thread's stack; here it has to live in a table.
struct Conn {
    explicit Conn(int f) : fd(f) {}
    int fd;
    std::string in;        // received bytes that are not a finished command yet
    std::string out;       // replies not fully sent yet
    size_t out_off = 0;    // how much of `out` has been sent
    bool reading = true;   // do we currently ask the OS about incoming data?
    bool writing = false;  // ...and about room to send?
    bool closing = false;  // close once `out` is flushed (used after an error reply)
};

}  // namespace

// ---- One event loop: one thread, one poller, many clients ------------------
struct EventLoops::Loop {
    explicit Loop(Storage& s) : storage(s) {
        if (!poller.ok() || pipe(wake) < 0) {
            perror("event loop setup");
            std::exit(1);
        }
        set_nonblocking(wake[0]);
        set_nonblocking(wake[1]);
    }

    // Called from the accept thread: queue the socket, then poke the loop.
    // Without the poke, the loop thread would stay asleep in wait().
    void hand_over(int fd) {
        {
            std::lock_guard<std::mutex> lock(mu);
            incoming.push_back(fd);
        }
        char b = 1;
        (void)!write(wake[1], &b, 1);  // if the pipe is full the loop is already about to wake
    }

    void run() {
        poller.add(wake[0]);
        Event evs[64];
        char buf[16 * 1024];
        while (true) {
            int n = poller.wait(evs, 64);
            if (n < 0) {
                perror("event wait");
                return;
            }
            for (int i = 0; i < n; i++) {
                if (evs[i].fd == wake[0]) {
                    char sink[64];
                    while (read(wake[0], sink, sizeof(sink)) > 0) {
                    }
                    adopt_new_clients();
                    continue;
                }
                // A client closed earlier in this same batch has no entry any
                // more, so a leftover event for it is simply skipped.
                auto it = conns.find(evs[i].fd);
                if (it == conns.end()) continue;
                Conn& c = it->second;

                bool keep = true;
                if (evs[i].writable && !c.out.empty()) keep = drive(c);
                if (keep && evs[i].readable && c.reading) keep = on_readable(c, buf, sizeof(buf));
                if (!keep) {
                    poller.remove(c.fd);
                    close(c.fd);
                    conns.erase(it);
                }
            }
        }
    }

private:
    void adopt_new_clients() {
        std::vector<int> fresh;
        {
            std::lock_guard<std::mutex> lock(mu);
            fresh.swap(incoming);
        }
        for (int fd : fresh) {
            conns.emplace(fd, Conn(fd));
            poller.add(fd);
        }
    }

    // Returns false if the connection should be closed.
    bool on_readable(Conn& c, char* buf, size_t size) {
        ssize_t n = recv(c.fd, buf, size, 0);
        if (n == 0) return false;  // client closed the connection normally
        if (n < 0) {
            // Spurious wakeup or signal: not an error, we'll be told again.
            return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
        }
        c.in.append(buf, static_cast<size_t>(n));
        return drive(c);
    }

    // Run the commands we have, send what we can, and set up what to wait for
    // next. Returns false if the connection should be closed.
    bool drive(Conn& c) {
        while (true) {
            run_commands(c);
            if (!flush(c)) return false;
            // If everything went out and complete lines are still waiting
            // (we paused at OUT_LIMIT), keep going.
            if (c.out.empty() && !c.closing && c.in.find('\n') != std::string::npos) continue;
            break;
        }
        if (c.closing && c.out.empty()) return false;

        // While replies are waiting, stop reading from this client (it must
        // catch up first) and ask to be told when sending is possible.
        bool want_read = c.out.empty() && !c.closing;
        bool want_write = !c.out.empty();
        if (want_read != c.reading || want_write != c.writing) {
            poller.set_interest(c.fd, want_read, want_write);
            c.reading = want_read;
            c.writing = want_write;
        }
        return true;
    }

    // Same framing rules as serve_client: only run a command once its '\n' is here.
    void run_commands(Conn& c) {
        if (c.closing) return;
        size_t start = 0;
        size_t newline;
        while (c.out.size() - c.out_off < OUT_LIMIT &&
               (newline = c.in.find('\n', start)) != std::string::npos) {
            if (newline - start > MAX_LINE) {
                too_long(c);
                return;
            }
            c.out += handle_command(storage, c.in.substr(start, newline - start));
            start = newline + 1;
        }
        c.in.erase(0, start);  // keep only the unfinished tail
        if (c.in.find('\n') == std::string::npos && c.in.size() > MAX_LINE) {
            too_long(c);
        }
    }

    void too_long(Conn& c) {
        c.out += "ERROR line too long\n";
        c.in.clear();
        c.closing = true;
    }

    // Send as much as the socket accepts right now. Unlike the blocking
    // send_all, running out of room is normal: we keep the rest and come back
    // when the OS says the socket is writable. Returns false if the connection broke.
    bool flush(Conn& c) {
        while (c.out_off < c.out.size()) {
            ssize_t n = send(c.fd, c.out.data() + c.out_off, c.out.size() - c.out_off, 0);
            if (n < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) return true;  // socket full, wait
                return false;  // e.g. EPIPE: client is gone
            }
            c.out_off += static_cast<size_t>(n);
        }
        c.out.clear();
        c.out_off = 0;
        return true;
    }

    Storage& storage;
    Poller poller;
    int wake[2];  // self-pipe: writing a byte here wakes this loop up
    std::mutex mu;
    std::vector<int> incoming;               // guarded by mu
    std::unordered_map<int, Conn> conns;     // touched only by this loop's thread
};

EventLoops::EventLoops(size_t loops, Storage& storage) {
    // loops == 0 means "not used" (other server modes): start nothing.
    for (size_t i = 0; i < loops; i++) {
        Loop* loop = new Loop(storage);
        loops_.push_back(loop);
        std::thread([loop] { loop->run(); }).detach();
    }
}

void EventLoops::add_client(int fd) {
    // Replies are small and clients wait for each one, so don't let the kernel
    // delay them to batch with later data.
    int yes = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
    // Never let recv()/send() block: one stuck client must not freeze a loop
    // that is also serving hundreds of others.
    set_nonblocking(fd);
    loops_[next_++ % loops_.size()]->hand_over(fd);
}

const char* EventLoops::backend() {
#ifdef __linux__
    return "epoll";
#else
    return "kqueue";
#endif
}
