// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio — cross-platform socket foundation (impl).
//
// Copyright (C) 2025-2026 Terrell Deppe
//
//
// This is the ONLY translation unit that includes platform socket headers. The
// POSIX path (<sys/socket.h> et al.) and the Winsock path (<winsock2.h>) are
// both here; everything else in naudio_net sees only the integer handle typedef
// from Socket.hpp.

#include "naudio/net/Socket.hpp"

#include <chrono>
#include <cstring>
#include <mutex>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <mstcpip.h>
#  pragma comment(lib, "ws2_32.lib")
// Not all SDK headers expose SIO_UDP_CONNRESET (mstcpip.h omits it in some
// versions); the canonical value is _WSAIOW(IOC_VENDOR, 12) — libuv defines it
// the same way.
#  ifndef SIO_UDP_CONNRESET
#    define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#  endif
#else
#  include <arpa/inet.h>
#  include <errno.h>
#  include <fcntl.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <poll.h>
#  include <sys/types.h>
#  include <unistd.h>
#endif

// Per-socket receive-buffer overflow reporting (issue #29). Linux only: SO_RXQ_OVFL makes the
// kernel attach its own discard count to each received datagram as SCM ancillary data.
// macOS/BSD report UDP overflow system-wide (netstat -s) and never per socket; Winsock has no
// equivalent. Feature-tested rather than assumed from __linux__ alone, so a libc that does not
// declare the option degrades to "not measured" instead of failing to build.
#if defined(__linux__) && defined(SO_RXQ_OVFL)
#  define NAUDIO_HAVE_RXQ_OVFL 1
#endif

namespace naudio::net {

#ifdef _WIN32
const socket_t kInvalidSocket = static_cast<socket_t>(INVALID_SOCKET);
#else
const socket_t kInvalidSocket = static_cast<socket_t>(-1);
#endif

namespace {

// Linux flags MSG_NOSIGNAL on send to suppress SIGPIPE; macOS uses the
// SO_NOSIGPIPE socket option (set in configureNew); Windows has no SIGPIPE.
#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

int lastErr() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

bool isWouldBlock(int e) {
#ifdef _WIN32
    return e == WSAEWOULDBLOCK;
#else
    return e == EAGAIN || e == EWOULDBLOCK;
#endif
}

// A recv under SO_RCVTIMEO reports the deadline as EAGAIN/EWOULDBLOCK on POSIX
// but as WSAETIMEDOUT on Windows.
bool isRecvTimeout(int e) {
#ifdef _WIN32
    return e == WSAEWOULDBLOCK || e == WSAETIMEDOUT;
#else
    return e == EAGAIN || e == EWOULDBLOCK;
#endif
}

bool isInProgress(int e) {
#ifdef _WIN32
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS;
#else
    return e == EINPROGRESS;
#endif
}

bool isInterrupted(int e) {
#ifdef _WIN32
    return e == WSAEINTR;
#else
    return e == EINTR;
#endif
}

void closeNative(socket_t h) {
#ifdef _WIN32
    ::closesocket(h);
#else
    ::close(h);
#endif
}

// Half-closes both directions so a thread parked in the kernel on this descriptor returns now.
// The return value is deliberately unread — ENOTCONN is the expected answer for a listener or an
// unconnected UDP socket, and no caller could act on any other failure either.
void shutdownNative(socket_t h) {
#ifdef _WIN32
    ::shutdown(h, SD_BOTH);
#else
    ::shutdown(h, SHUT_RDWR);
#endif
}

// How long close() waits for in-flight syscalls to leave before freeing the descriptor anyway.
//
// SIZED AGAINST THE MEASURED CALLERS, not picked for looking round. What has to fit inside it is
// the longest a syscall can stay in flight after close() has issued its ::shutdown:
//
//   * a TCP recv or send — promptly, because ::shutdown does wake those (MEASURED at 11 ms on
//     Linux, see setSendTimeout's note; 206 ms in the macOS probe below).
//   * an accept — at most kAcceptPollSliceMs, because that call polls in slices and checks for a
//     pending close between them. It is NOT the caller's 1000 ms deadline; that is the whole
//     reason the slice exists.
//   * a UDP recvFrom — its SO_RCVTIMEO, and nothing shorter: ::shutdown answers ENOTCONN on an
//     unconnected datagram socket and wakes nothing. The largest in the tree is 2000 ms
//     (tests/net/test_dual_transport.cpp:67); the demux loop's 500 ms never reaches the drain at
//     all, since UdpServerTransport::close joins that thread before closing the socket.
//
// So 2000 ms is the real worst case and this is 2.5x it. Every one of them is a kernel wait rather
// than compute, so a sanitizer build does not stretch it.
//
// Reaching this deadline is not an error path to be tuned away — it means a descriptor was freed
// with a syscall still on it, i.e. exactly the defect. Socket::drainTimeouts() counts it so that
// stays visible instead of becoming a silent fallback.
constexpr int kDrainTimeoutMs = 5000;

std::atomic<std::uint64_t> g_drainTimeouts{0};

// The longest acceptTcp will sit in one ::select before looking up to see whether a close() has
// begun. It is NOT a change to the caller's deadline — the slices are summed back up to it.
//
// MEASURED, and it is the difference between a 20% slower test suite and a 0.4% one. ::shutdown
// does not wake a thread parked in select() on a LISTENER: measured on macOS 2026-08-13, a bare
// close() ends that wait in 204 ms while a shutdown() leaves it parked for the full 5000 ms
// (/tmp/naudio-fdprobe/wake.c). So once close() stopped freeing the descriptor immediately, every
// server teardown paid out the accept loop's whole 1000 ms poll: the full suite went 138.58 s ->
// 165.85 s, +28.0 s spread over 29 tests in flat ~1.00 s steps — the accept poll's signature.
//
// Linux gains from this too rather than merely being unharmed: close() does not wake a blocked
// select() there at ALL (the reason self-pipes exist), so before this the accept thread already
// sat out its remaining poll on every stop — it was just charged to the join instead of to close.
constexpr int kAcceptPollSliceMs = 50;

void setErr(std::string* err, const char* what) {
    if (err) *err = std::string(what) + " (errno=" + std::to_string(lastErr()) + ")";
}

// Per-socket setup applied right after creation: macOS SIGPIPE suppression.
void configureNew(socket_t fd) {
#ifdef __APPLE__
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#else
    (void)fd;
#endif
}

bool setNonBlocking(socket_t fd, bool nonBlocking) {
#ifdef _WIN32
    u_long mode = nonBlocking ? 1 : 0;
    return ::ioctlsocket(fd, FIONBIO, &mode) == 0;
#else
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    flags = nonBlocking ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return ::fcntl(fd, F_SETFL, flags) == 0;
#endif
}

// Resolves host:port into an IPv4 sockaddr. An empty host binds the wildcard
// (INADDR_ANY). Numeric strings go through inet_pton; names ("localhost") fall
// back to getaddrinfo (AF_INET).
bool resolveV4(const std::string& host, std::uint16_t port, sockaddr_in& out,
               std::string* err) {
    std::memset(&out, 0, sizeof(out));
    out.sin_family = AF_INET;
    out.sin_port = htons(port);

    if (host.empty() || host == "0.0.0.0") {
        out.sin_addr.s_addr = htonl(INADDR_ANY);
        return true;
    }
    if (::inet_pton(AF_INET, host.c_str(), &out.sin_addr) == 1) {
        return true;
    }

    addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (::getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || res == nullptr) {
        if (err) *err = "getaddrinfo failed for host '" + host + "'";
        return false;
    }
    out.sin_addr = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr;
    ::freeaddrinfo(res);
    return true;
}

timeval msToTimeval(int ms) {
    timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    return tv;
}

// Waits for ONE descriptor to become ready. Returns >0 ready, 0 deadline reached, <0 error with
// lastErr() carrying the reason — the same three-way answer ::select gives, so callers are
// unchanged in shape.
//
// POSIX USES poll(), NOT select(), AND THAT IS THE WHOLE POINT OF THIS FUNCTION. select()'s
// fd_set is a fixed bitmap of FD_SETSIZE (1024) bits indexed BY DESCRIPTOR NUMBER, so a
// descriptor >= FD_SETSIZE cannot be represented at all. Measured on 2026-08-31, with a low-fd
// control beside each arm:
//
//   * Linux, as this project ships it (glibc, _FORTIFY_SOURCE active — the released
//     libnaudio.so.1.0.0 carries an undefined reference to __fdelt_chk): FD_SET on such a
//     descriptor ABORTS the process. "*** bit out of range 0 - FD_SETSIZE on fd_set ***",
//     SIGABRT. A server does not fail the connection; the host process dies.
//   * Linux built without fortification: a silent 4-byte write PAST the end of the fd_set, after
//     which the accept loop carries on and appears to work. Confirmed with a guard word.
//   * macOS: FD_SET is bounds-guarded so nothing is corrupted, but ::select then rejects
//     nfds > FD_SETSIZE with EINVAL, so acceptTcp returns Error forever and the listener is dead.
//
// naudio is a LIBRARY: the descriptor number is set by the HOST process, not by us. An
// application that already holds a thousand descriptors — a server, a GUI with many files open,
// anything with a raised RLIMIT_NOFILE — hands us a high one on the first listen. poll() takes
// the descriptor as a plain int in a struct and has no such ceiling.
//
// WINDOWS DELIBERATELY KEEPS select(), and this is not the lazy branch. Winsock's fd_set is not
// a bitmap: it is { u_int fd_count; SOCKET fd_array[FD_SETSIZE]; }, indexed by insertion order,
// and select()'s first argument is ignored entirely — so a SOCKET of any value is fine here,
// where only one is ever added. WSAPoll would be the analogue but is documented not to report a
// FAILED connection in revents, which is precisely what connectTcp below relies on. Swapping it
// in would trade a bug Windows does not have for one it does.
enum class WaitFor { Readable, Writable };

int waitOne(socket_t h, WaitFor what, int timeoutMs) {
#ifdef _WIN32
    // Both call shapes are preserved exactly as they were before poll() arrived: the readable
    // wait passes no exception set, the writable wait does — a refused connect signals the
    // exception set on Winsock and the write set on POSIX.
    fd_set primary;
    FD_ZERO(&primary);
    FD_SET(h, &primary);
    timeval tv = msToTimeval(timeoutMs);
    if (what == WaitFor::Readable) {
        return ::select(static_cast<int>(h) + 1, &primary, nullptr, nullptr, &tv);
    }
    fd_set except;
    FD_ZERO(&except);
    FD_SET(h, &except);
    return ::select(static_cast<int>(h) + 1, nullptr, &primary, &except, &tv);
#else
    // POLLERR / POLLHUP / POLLNVAL are output-only: they arrive in revents whether or not they
    // were requested, so a failed connect wakes this wait exactly as the Winsock exception set
    // does. Callers confirm with SO_ERROR either way.
    pollfd pfd{};
    pfd.fd = static_cast<int>(h);
    pfd.events = (what == WaitFor::Readable) ? POLLIN : POLLOUT;
    return ::poll(&pfd, 1, timeoutMs);
#endif
}

// Reads SO_SNDTIMEO back from the kernel, in milliseconds. 0 means "no deadline" —
// setSendTimeout(0)'s documented meaning, and also what a socket nobody configured
// reports — so a 0 here leaves sendAll behaving exactly as it did before the budget
// existed.
//
// READ BACK RATHER THAN CACHED IN A Socket MEMBER, on purpose. A member would have to be
// carried by the move constructor and move-assignment, which today copy only handle_
// (:224-232) — and TcpClientTransport::connect moves the socket into the connection, so a
// missed member would silently drop the budget on the one path that matters. This project
// passes no warning flags, so nothing would report it. The kernel is the single owner of
// the value instead, and it cannot drift from what was actually set (issue #70).
int sendTimeoutMs(socket_t h) {
    if (h == kInvalidSocket) return 0;
#ifdef _WIN32
    DWORD t = 0;
    int len = static_cast<int>(sizeof(t));
    if (::getsockopt(h, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<char*>(&t), &len) != 0) {
        return 0;
    }
    return static_cast<int>(t);
#else
    timeval tv{};
    socklen_t len = sizeof(tv);
    if (::getsockopt(h, SOL_SOCKET, SO_SNDTIMEO, &tv, &len) != 0) return 0;
    const long long ms =
        static_cast<long long>(tv.tv_sec) * 1000 + static_cast<long long>(tv.tv_usec) / 1000;
    if (ms <= 0) return 0;
    if (ms > 0x7fffffffLL) return 0x7fffffff;  // a deadline that large is "no deadline"
    return static_cast<int>(ms);
#endif
}

std::int64_t elapsedMsSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - t0)
        .count();
}

}  // namespace

// ---------------------------------------------------------------------------

Socket::IoScope::IoScope(const Socket& s) noexcept : s_(s), h_(kInvalidSocket), entered_(false) {
    std::lock_guard<std::mutex> lock(s_.ioMutex_);
    // Read the handle under the SAME lock close() takes to remove it. That is the whole ordering
    // argument: either we see a live descriptor and are counted before close() can start waiting,
    // or close() has already taken it and we see the sentinel and never touch it.
    h_ = s_.handle_.load();
    if (h_ == kInvalidSocket) return;
    ++s_.inFlight_;
    entered_ = true;
}

Socket::IoScope::~IoScope() {
    if (!entered_) return;
    std::lock_guard<std::mutex> lock(s_.ioMutex_);
    if (--s_.inFlight_ == 0) s_.ioIdle_.notify_all();
}

std::uint64_t Socket::drainTimeouts() noexcept { return g_drainTimeouts.load(); }

Socket::Socket() noexcept : handle_(kInvalidSocket) {}

Socket::Socket(socket_t handle) noexcept : handle_(handle) {}

Socket::~Socket() { close(); }

Socket::Socket(Socket&& other) noexcept : handle_(kInvalidSocket) {
    // Under the SOURCE's lock, so the steal cannot land between an IoScope's handle read and its
    // count increment. Moving a socket that has live I/O on it was never meaningful — the moved-to
    // object has its own mutex and would not be waited on — but taking the lock costs nothing and
    // keeps the handle transfer ordered against the same mutex everything else here uses.
    std::lock_guard<std::mutex> lock(other.ioMutex_);
    handle_.store(other.handle_.exchange(kInvalidSocket));
    adoptDropCounter(other);
}

Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        close();  // drains this socket's own in-flight I/O before the descriptor goes
        std::lock_guard<std::mutex> lock(other.ioMutex_);
        handle_.store(other.handle_.exchange(kInvalidSocket));
        adoptDropCounter(other);
    }
    return *this;
}

// The receive-drop counter is a property of the DESCRIPTOR (the kernel option is set on it), so
// it has to travel with the handle above or a socket enabled before being moved into a connection
// would silently report -1 forever. Called under the source's ioMutex_, like the handle steal.
void Socket::adoptDropCounter(Socket& other) noexcept {
    lastRawDrops_.store(other.lastRawDrops_.exchange(0, std::memory_order_relaxed),
                        std::memory_order_relaxed);
    dropsAccum_.store(other.dropsAccum_.exchange(0, std::memory_order_relaxed),
                      std::memory_order_relaxed);
    dropCounterOn_.store(other.dropCounterOn_.exchange(false, std::memory_order_relaxed),
                         std::memory_order_relaxed);
}

bool Socket::valid() const noexcept { return handle_.load() != kInvalidSocket; }

void Socket::close() noexcept {
    // 1. Take the descriptor. Under ioMutex_ so it is ordered against IoScope's read: after this,
    //    every new scope sees the sentinel and no further syscall can enter. The exchange also
    //    keeps close() single-shot — two threads racing here, or a destructor following an explicit
    //    close(), and only one gets a real handle.
    socket_t h;
    {
        std::lock_guard<std::mutex> lock(ioMutex_);
        h = handle_.exchange(kInvalidSocket);
    }
    if (h == kInvalidSocket) return;

    // 2. Wake whoever is already inside, so the wait below is microseconds rather than the
    //    caller's own receive deadline. It does NOT reach every waiter — a select on a listener
    //    and a recvfrom on an unconnected UDP socket both ignore it (measured; see
    //    kAcceptPollSliceMs) — which is why acceptTcp polls in slices and why step 3 is bounded.
    shutdownNative(h);

    // 3. Wait for them to leave. This is the step that makes the descriptor safe to free.
    {
        std::unique_lock<std::mutex> lock(ioMutex_);
        if (!ioIdle_.wait_for(lock, std::chrono::milliseconds(kDrainTimeoutMs),
                              [this] { return inFlight_ == 0; })) {
            // Deadline reached with a syscall still on the descriptor. Freeing it here is the
            // pre-#90 behaviour and the pre-#90 hazard; the alternative is leaking the descriptor
            // or blocking forever, both worse. Counted so it cannot pass unnoticed.
            g_drainTimeouts.fetch_add(1);
        }
    }

    // 4. Now the number is ours alone to give back.
    closeNative(h);
}

void Socket::shutdownBoth() noexcept {
    // Scoped like every other syscall here: shutdown() takes a descriptor, so it is exposed to the
    // same reuse hazard as recv() if it runs after some other thread's close() freed the number.
    // A concurrent close() that wins leaves this un-entered and the shutdown is simply skipped,
    // which is correct — close() issues its own shutdown at step 2 anyway.
    IoScope io(*this);
    if (!io.entered()) return;
    shutdownNative(io.handle());
}

void Socket::ensureStartup() {
#ifdef _WIN32
    static std::once_flag once;
    std::call_once(once, [] {
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
    });
#endif
}

std::string Socket::resolveHostV4(const std::string& host) {
    ensureStartup();
    sockaddr_in addr;
    if (!resolveV4(host, 0, addr, nullptr)) return "";
    char ip[INET_ADDRSTRLEN] = {0};
    ::inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
    return std::string(ip);
}

Socket Socket::listenTcp(const std::string& bindHost, std::uint16_t port,
                         bool ownsPort, std::string* err) {
    ensureStartup();
    sockaddr_in addr;
    if (!resolveV4(bindHost, port, addr, err)) return Socket();

    socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd == kInvalidSocket) {
        setErr(err, "socket() failed");
        return Socket();
    }
    configureNew(fd);
    if (ownsPort) {
        // PORT OWNERSHIP IS NOT THE SAME SYSCALL ON EVERY PLATFORM (issue #85). Both branches
        // below request one policy — "no other live listener shares this port, and a restart
        // may reclaim it" — but SO_REUSEADDR only delivers that policy on POSIX.
        int one = 1;
#ifdef _WIN32
        // Winsock's SO_REUSEADDR is not the POSIX flag under another name. It permits binding a
        // port another socket is ACTIVELY LISTENING on — the behaviour SO_EXCLUSIVEADDRUSE
        // exists to prevent — so while this passed SO_REUSEADDR here, a second server bound a
        // port the first was serving and na_server_start() returned NA_OK for a port it did not
        // have. Neither process was told. CI run 31643720529 caught it from the #58 arm.
        //
        // SO_EXCLUSIVEADDRUSE is strictly stronger than passing nothing: it also stops a foreign
        // process from taking this port by setting SO_REUSEADDR itself, which is the hijack the
        // bare default still permits.
        //
        // Return deliberately unread, matching the POSIX branch: the option requires no
        // privilege on any supported Windows version, and if it were ever refused the socket
        // falls back to Winsock's default — which still refuses the plain double bind this
        // issue is about. Failing the listen over it would be the worse trade.
        ::setsockopt(fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<char*>(&one),
                     sizeof(one));
#else
        // The restart-after-TIME_WAIT accommodation, and it is load-bearing rather than
        // decorative: a listener that has accepted and closed a connection leaves that 5-tuple
        // in TIME_WAIT holding this port, and without this flag the rebind is refused with
        // EADDRINUSE. Measured on macOS 2026-08-13 — see the permit-control in
        // tests/net/test_socket.cpp, which reddens if this line is removed. It never permits
        // two live listeners on any POSIX platform.
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<char*>(&one),
                     sizeof(one));
#endif
    }
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        setErr(err, "bind() failed");
        closeNative(fd);
        return Socket();
    }
    if (::listen(fd, SOMAXCONN) != 0) {
        setErr(err, "listen() failed");
        closeNative(fd);
        return Socket();
    }
    return Socket(fd);
}

IoStatus Socket::acceptTcp(int timeoutMs, Socket& out, std::string* err) {
    // Held across the select AND the accept: both take the listener's descriptor, so neither may
    // run on a number close() has already handed back.
    IoScope io(*this);
    if (!io.entered()) {
        setErr(err, "acceptTcp on invalid socket");
        return IoStatus::Error;
    }
    const socket_t h = io.handle();

    // POLLED IN SLICES rather than in one wait of the caller's whole deadline, and the slices are
    // summed back up to it — so this is not a change to the contract, only to how often the wait
    // looks up. What it looks up for is a close() that began while we were parked: close() takes
    // the handle before it waits for us, so an invalid handle_ here means the descriptor is on its
    // way out and there is nothing left to accept. Without this the drain in close() would sit out
    // the caller's full poll on every teardown, because ::shutdown does not wake a select on a
    // listener (see kAcceptPollSliceMs for the measurement).
    //
    // timeoutMs <= 0 keeps its documented "blocks until a client arrives" meaning — it just loops
    // forever instead of parking in one indefinite ::accept, which is what makes even that case
    // answer a close instead of hanging until the drain deadline expires.
    for (int remaining = timeoutMs;;) {
        const int slice = (timeoutMs > 0 && remaining < kAcceptPollSliceMs) ? remaining
                                                                           : kAcceptPollSliceMs;
        int sel = waitOne(h, WaitFor::Readable, slice);
        if (sel > 0) break;  // a connection is pending — go take it
        if (sel < 0) {
            // EINTR reports TimedOut exactly as it always has, rather than being retried here:
            // every caller already loops on TimedOut, and preserving the old answer keeps this
            // change to the polling cadence alone.
            if (isInterrupted(lastErr())) return IoStatus::TimedOut;
            setErr(err, "readiness wait failed in acceptTcp");
            return IoStatus::Error;
        }
        if (handle_.load() == kInvalidSocket) {
            setErr(err, "acceptTcp: socket closed");
            return IoStatus::Error;
        }
        if (timeoutMs > 0 && (remaining -= slice) <= 0) return IoStatus::TimedOut;
    }

    socket_t c = ::accept(h, nullptr, nullptr);
    if (c == kInvalidSocket) {
        int e = lastErr();
        if (isWouldBlock(e) || isInterrupted(e)) return IoStatus::TimedOut;
        setErr(err, "accept() failed");
        return IoStatus::Error;
    }
    configureNew(c);
    out = Socket(c);
    return IoStatus::Ok;
}

Socket Socket::connectTcp(const std::string& host, std::uint16_t port,
                          int timeoutMs, std::string* err) {
    ensureStartup();
    sockaddr_in addr;
    if (!resolveV4(host, port, addr, err)) return Socket();

    socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd == kInvalidSocket) {
        setErr(err, "socket() failed");
        return Socket();
    }
    configureNew(fd);

    if (timeoutMs <= 0) {
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            setErr(err, "connect() failed");
            closeNative(fd);
            return Socket();
        }
        return Socket(fd);
    }

    // Timed connect: non-blocking connect, then select for writability.
    if (!setNonBlocking(fd, true)) {
        setErr(err, "setNonBlocking failed");
        closeNative(fd);
        return Socket();
    }
    int rc = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (rc != 0 && !isInProgress(lastErr())) {
        setErr(err, "connect() failed");
        closeNative(fd);
        return Socket();
    }
    if (rc != 0) {
        // A completed non-blocking connect signals writability; a FAILED one signals the
        // write set on POSIX but the EXCEPTION set on Winsock. Watch both so a refused
        // connect is reported immediately (via SO_ERROR) instead of waiting out the timeout.
        int sel = waitOne(fd, WaitFor::Writable, timeoutMs);
        if (sel <= 0) {
            setErr(err, sel == 0 ? "connect timed out" : "readiness wait failed");
            closeNative(fd);
            return Socket();
        }
        int soErr = 0;
        socklen_t len = sizeof(soErr);
        ::getsockopt(fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soErr), &len);
        if (soErr != 0) {
            if (err) *err = "connect failed (SO_ERROR=" + std::to_string(soErr) + ")";
            closeNative(fd);
            return Socket();
        }
    }
    setNonBlocking(fd, false);
    return Socket(fd);
}

Socket Socket::bindUdp(const std::string& bindHost, std::uint16_t port,
                       bool reuseAddr, std::string* err) {
    ensureStartup();
    sockaddr_in addr;
    if (!resolveV4(bindHost, port, addr, err)) return Socket();

    socket_t fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd == kInvalidSocket) {
        setErr(err, "socket() failed");
        return Socket();
    }
    configureNew(fd);
#ifdef _WIN32
    {
        // Winsock quirk: a datagram sent to a port with no listener comes back as an
        // ICMP port-unreachable that makes SUBSEQUENT recvfrom()/sendto() on this
        // socket fail with WSAECONNRESET — which the framing layer treats as a fatal
        // socket error. POSIX UDP sockets don't do this; opt out to match.
        BOOL noReset = FALSE;
        DWORD bytes = 0;
        ::WSAIoctl(fd, SIO_UDP_CONNRESET, &noReset, sizeof(noReset),
                   nullptr, 0, &bytes, nullptr, nullptr);
    }
#endif
    if (reuseAddr) {
        int one = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<char*>(&one),
                     sizeof(one));
    }
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        setErr(err, "bind() failed");
        closeNative(fd);
        return Socket();
    }
    return Socket(fd);
}

namespace {
// Raise one SO_*BUF option to at least `bytes`. Kernels commonly return a doubled value
// from getsockopt (Linux) or clamp to kern.ipc.maxsockbuf (macOS), so the request is
// verified by reading the option back rather than trusting setsockopt's return.
bool raiseSocketBuffer(socket_t h, int optname, int bytes) {
    if (h == kInvalidSocket || bytes <= 0) return false;
    int current = 0;
    socklen_t len = sizeof(current);
    if (::getsockopt(h, SOL_SOCKET, optname, reinterpret_cast<char*>(&current), &len) == 0 &&
        current >= bytes) {
        return true;  // already large enough — never shrink it
    }
    int want = bytes;
    ::setsockopt(h, SOL_SOCKET, optname, reinterpret_cast<char*>(&want), sizeof(want));
    current = 0;
    len = sizeof(current);
    if (::getsockopt(h, SOL_SOCKET, optname, reinterpret_cast<char*>(&current), &len) != 0) {
        return false;
    }
    return current >= bytes;
}

// Set one SO_*BUF option to `bytes`, allowing a SHRINK, and report what the kernel settled on.
//
// Deliberately a SEPARATE function rather than a flag on raiseSocketBuffer above. That one's
// "never shrink" clause is load-bearing for UDP — macOS refuses a sendto() larger than SO_SNDBUF
// — so a caller wanting a smaller buffer must not be able to reach it by passing a smaller
// number to the primitive whose whole contract is that it cannot lower one.
int resizeSocketBuffer(socket_t h, int optname, int bytes) {
    // bytes == 0 is passed through rather than rejected: on Winsock it is the documented way
    // to disable buffering for that direction outright, which is not otherwise reachable.
    if (h == kInvalidSocket || bytes < 0) return 0;
    int want = bytes;
    ::setsockopt(h, SOL_SOCKET, optname, reinterpret_cast<char*>(&want), sizeof(want));
    int current = 0;
    socklen_t len = sizeof(current);
    if (::getsockopt(h, SOL_SOCKET, optname, reinterpret_cast<char*>(&current), &len) != 0) {
        return 0;
    }
    return current;
}
}  // namespace

// The option setters are scoped too, and not out of symmetry. A setsockopt that lands on a REUSED
// descriptor silently reconfigures whatever socket now owns that number — a buffer size or a
// timeout applied to an unrelated connection, with no error anywhere. That is a quieter failure
// than a stray recv, not a smaller one.
bool Socket::setSendBufferAtLeast(int bytes) {
    IoScope io(*this);
    return io.entered() && raiseSocketBuffer(io.handle(), SO_SNDBUF, bytes);
}

bool Socket::setRecvBufferAtLeast(int bytes) {
    IoScope io(*this);
    return io.entered() && raiseSocketBuffer(io.handle(), SO_RCVBUF, bytes);
}

bool Socket::enableReceiveDropCounter() noexcept {
#if defined(NAUDIO_HAVE_RXQ_OVFL)
    IoScope io(*this);
    if (!io.entered()) return false;
    const int on = 1;
    if (::setsockopt(io.handle(), SOL_SOCKET, SO_RXQ_OVFL, &on, sizeof(on)) != 0) return false;
    // Zero the accumulator here rather than at construction: the kernel's counter is cumulative
    // since the socket was created, and the first reading after this call is the baseline we
    // difference against. Enabling twice restarts from the next datagram, which is the only
    // meaning that does not double-count.
    lastRawDrops_.store(0, std::memory_order_relaxed);
    dropsAccum_.store(0, std::memory_order_relaxed);
    dropCounterOn_.store(true, std::memory_order_relaxed);
    return true;
#else
    return false;
#endif
}

std::int64_t Socket::receiveDrops() const noexcept {
    // The flag, not the accumulator, is what distinguishes "measured, and nothing was dropped"
    // from "no mechanism on this platform". Collapsing the two into 0 is precisely the reading
    // error this counter exists to prevent.
    if (!dropCounterOn_.load(std::memory_order_relaxed)) return -1;
    return static_cast<std::int64_t>(dropsAccum_.load(std::memory_order_relaxed));
}

int Socket::setSendBufferSize(int bytes) {
    IoScope io(*this);
    return io.entered() ? resizeSocketBuffer(io.handle(), SO_SNDBUF, bytes) : 0;
}

int Socket::setRecvBufferSize(int bytes) {
    IoScope io(*this);
    return io.entered() ? resizeSocketBuffer(io.handle(), SO_RCVBUF, bytes) : 0;
}

bool Socket::setRecvTimeout(int ms) {
    IoScope io(*this);
    if (!io.entered()) return false;
    const socket_t h = io.handle();
#ifdef _WIN32
    DWORD t = static_cast<DWORD>(ms < 0 ? 0 : ms);
    return ::setsockopt(h, SOL_SOCKET, SO_RCVTIMEO,
                        reinterpret_cast<char*>(&t), sizeof(t)) == 0;
#else
    timeval tv = msToTimeval(ms < 0 ? 0 : ms);
    return ::setsockopt(h, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0;
#endif
}

bool Socket::setSendTimeout(int ms) {
    IoScope io(*this);
    if (!io.entered()) return false;
    const socket_t h = io.handle();
#ifdef _WIN32
    DWORD t = static_cast<DWORD>(ms < 0 ? 0 : ms);
    return ::setsockopt(h, SOL_SOCKET, SO_SNDTIMEO,
                        reinterpret_cast<char*>(&t), sizeof(t)) == 0;
#else
    timeval tv = msToTimeval(ms < 0 ? 0 : ms);
    return ::setsockopt(h, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) == 0;
#endif
}

std::uint16_t Socket::localPort() const {
    IoScope io(*this);
    if (!io.entered()) return 0;
    const socket_t h = io.handle();
    sockaddr_in addr;
    socklen_t len = sizeof(addr);
    if (::getsockname(h, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        return 0;
    }
    return ntohs(addr.sin_port);
}

std::string Socket::remoteAddress() const {
    IoScope io(*this);
    if (!io.entered()) return "";
    const socket_t h = io.handle();
    sockaddr_in addr;
    socklen_t len = sizeof(addr);
    if (::getpeername(h, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        return "";
    }
    char ip[INET_ADDRSTRLEN] = {0};
    ::inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
    return std::string(ip) + ":" + std::to_string(ntohs(addr.sin_port));
}

RecvResult Socket::recv(void* buf, std::size_t len) {
    // THE CALL THE 17 TSan ARMS NAMED. The scope is what stops close() freeing this descriptor
    // while the ::recv below is parked in the kernel on it.
    IoScope io(*this);
    if (!io.entered()) return {IoStatus::Error, 0};
    const socket_t h = io.handle();
    for (;;) {
        int n = static_cast<int>(
            ::recv(h, static_cast<char*>(buf), static_cast<int>(len), 0));
        if (n > 0) return {IoStatus::Ok, static_cast<std::size_t>(n)};
        if (n == 0) return {IoStatus::Closed, 0};
        int e = lastErr();
        if (isInterrupted(e)) continue;
        if (isRecvTimeout(e)) return {IoStatus::TimedOut, 0};
        return {IoStatus::Error, 0};
    }
}

// Why the last sendAll() on this thread stopped — see Socket::lastSendStop() in the header for
// why the reason has to be observable at all. thread_local rather than a member so that threads
// sharing one Socket do not overwrite each other's answer, and so sizeof(Socket) is unchanged.
namespace {
thread_local Socket::SendStop g_lastSendStop = Socket::SendStop::Ok;
}  // namespace

Socket::SendStop Socket::lastSendStop() noexcept { return g_lastSendStop; }

bool Socket::sendAll(const void* buf, std::size_t len) {
    // Held across the whole retry loop, including the sendTimeoutMs() getsockopt inside it — a
    // partial write that resumes on a reused descriptor would put this frame's tail on someone
    // else's connection.
    IoScope io(*this);
    if (!io.entered()) {
        g_lastSendStop = SendStop::NotEntered;
        return false;
    }
    const socket_t h = io.handle();
    const char* p = static_cast<const char*>(buf);
    std::size_t left = len;

    // Started unconditionally because the budget below has to cover the FIRST ::send too;
    // starting the clock at the first partial write would exclude it. A steady_clock read is
    // a vDSO load, not a syscall, so this costs nothing on the overwhelmingly common path
    // where one ::send takes everything.
    const auto callStart = std::chrono::steady_clock::now();
    int budgetMs = -1;  // -1 = not looked up yet; 0 = no deadline configured

    while (left > 0) {
        int n = static_cast<int>(::send(h, p, static_cast<int>(left), kSendFlags));
        if (n > 0) {
            p += n;
            left -= static_cast<std::size_t>(n);
            if (left == 0) break;

            // A PARTIAL WRITE, so we are about to loop — and the next ::send arms a FRESH
            // SO_SNDTIMEO. Without a whole-call budget a peer making repeated slow forward
            // progress therefore extends this call in proportion to how much it drip-feeds,
            // not in proportion to the deadline: MEASURED on macOS/arm64 at 9 partial
            // writes and 3263 ms against a 200 ms deadline (16.3x) for a peer draining
            // 32 KB every 40 ms, versus 201 ms (1.0x) with this check in place (issue #70).
            //
            // Looked up here rather than at function entry so a send that completes in one
            // call pays no getsockopt at all; once fetched it is reused for the rest of the
            // call.
            if (budgetMs < 0) budgetMs = sendTimeoutMs(h);
            if (budgetMs > 0 && elapsedMsSince(callStart) >= budgetMs) {
                // The ONE exit that means issue #70's guarantee fired. A build with the
                // condition above removed can reach every other exit here but never this one,
                // which is what makes the test assertion on it a real detector on platforms
                // where the clock cannot separate the two (issue #88 item 6).
                //
                // REACHED ON POSIX ONLY, in practice. This whole branch sits inside `n > 0`,
                // because a partial write is the event that re-arms a fresh SO_SNDTIMEO and so
                // is what #70 exists to bound. Winsock reports no byte count on a timed-out
                // blocking send, so a wedged Windows call never presents a partial write and
                // exits via SendFailed below instead — the call is still bounded by one
                // deadline (budgetMs IS sendTimeoutMs, so the two limits are the same number),
                // but this line does not run and the test arm cannot detect a mutant there.
                // Measured on windows-latest 2026-08-15; see tests/net/test_socket.cpp.
                g_lastSendStop = SendStop::Budget;
                return false;
            }
            continue;
        }
        int e = lastErr();
        if (isInterrupted(e)) continue;
        g_lastSendStop = SendStop::SendFailed;
        return false;
    }
    g_lastSendStop = SendStop::Ok;
    return true;
}

RecvFromResult Socket::recvFrom(void* buf, std::size_t len) {
    // The UDP counterpart of recv's scope, and the one case where close()'s ::shutdown does NOT
    // shorten the wait: an unconnected datagram socket answers ENOTCONN, so a thread parked here
    // leaves on its SO_RCVTIMEO (500 ms on the demux loop) and nothing sooner. That is precisely
    // why the drain has a deadline rather than waiting forever.
    IoScope io(*this);
    if (!io.entered()) return {IoStatus::Error, 0, "", 0, false};
    const socket_t h = io.handle();
    sockaddr_in src;
    socklen_t sl = sizeof(src);
    // Linux MSG_TRUNC makes recvfrom return the datagram's TRUE length even when
    // the tail was discarded, so a too-large datagram is detectable. The
    // copied bytes are still capped at len. Other platforms lack a portable
    // input MSG_TRUNC, so truncation there is best-effort (the caller's margin
    // buffer + CRC check absorbs an oversized datagram).
#if defined(__linux__)
    const int recvFlags = MSG_TRUNC;
#else
    const int recvFlags = 0;
#endif
    for (;;) {
        int n = -1;
        bool taken = false;
#if defined(NAUDIO_HAVE_RXQ_OVFL)
        // The kernel's discard count rides on ancillary data, and only recvmsg can carry it.
        // This branch is entered ONLY on a socket that asked for the counter, so every other
        // receive path — the server's demux loop included — keeps the plain recvfrom below,
        // byte for byte. That is deliberate: this file is the one place naudio touches platform
        // socket headers, and the narrower the Linux-only path, the less a mistake in it can
        // reach. MSG_TRUNC means the same thing here as it does to recvfrom: n is the
        // datagram's TRUE length even when the tail was dropped.
        if (dropCounterOn_.load(std::memory_order_relaxed)) {
            taken = true;
            iovec iov;
            iov.iov_base = buf;
            iov.iov_len = len;
            // CMSG_SPACE, not CMSG_LEN — the kernel needs the alignment padding as well as the
            // payload. The union is the portable way to get cmsghdr's alignment on the buffer.
            union {
                char bytes[CMSG_SPACE(sizeof(std::uint32_t))];
                struct cmsghdr align;
            } cmsgBuf;
            msghdr msg{};
            msg.msg_name = &src;
            msg.msg_namelen = sizeof(src);
            msg.msg_iov = &iov;
            msg.msg_iovlen = 1;
            msg.msg_control = cmsgBuf.bytes;
            msg.msg_controllen = sizeof(cmsgBuf.bytes);
            n = static_cast<int>(::recvmsg(h, &msg, recvFlags));
            if (n >= 0) {
                sl = msg.msg_namelen;
                for (cmsghdr* c = CMSG_FIRSTHDR(&msg); c != nullptr; c = CMSG_NXTHDR(&msg, c)) {
                    if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SO_RXQ_OVFL) continue;
                    std::uint32_t raw = 0;
                    std::memcpy(&raw, CMSG_DATA(c), sizeof(raw));
                    // Cumulative since socket creation, and 32-bit. Accumulate the DELTA so the
                    // exported total does not wrap where the raw counter does — unsigned
                    // subtraction across the wrap is well-defined and yields the true delta.
                    const std::uint32_t prev =
                        lastRawDrops_.exchange(raw, std::memory_order_relaxed);
                    const std::uint32_t delta = raw - prev;
                    if (delta != 0) dropsAccum_.fetch_add(delta, std::memory_order_relaxed);
                }
            }
        }
#endif
        if (!taken) {
            n = static_cast<int>(::recvfrom(h, static_cast<char*>(buf),
                                            static_cast<int>(len), recvFlags,
                                            reinterpret_cast<sockaddr*>(&src), &sl));
        }
        if (n >= 0) {
            char ip[INET_ADDRSTRLEN] = {0};
            ::inet_ntop(AF_INET, &src.sin_addr, ip, sizeof(ip));
            const std::size_t got = static_cast<std::size_t>(n);
            const bool truncated = got > len;
            return {IoStatus::Ok, truncated ? len : got, std::string(ip),
                    ntohs(src.sin_port), truncated};
        }
        int e = lastErr();
        if (isInterrupted(e)) continue;
#ifdef _WIN32
        // Winsock reports an oversized datagram as WSAEMSGSIZE — but the buffer
        // HAS been filled with the clamped head and src is populated. That is the
        // moral equivalent of the Linux MSG_TRUNC path (clamp + flag), not an error.
        if (e == WSAEMSGSIZE) {
            char ip[INET_ADDRSTRLEN] = {0};
            ::inet_ntop(AF_INET, &src.sin_addr, ip, sizeof(ip));
            return {IoStatus::Ok, len, std::string(ip), ntohs(src.sin_port), true};
        }
#endif
        if (isRecvTimeout(e)) return {IoStatus::TimedOut, 0, "", 0, false};
        return {IoStatus::Error, 0, "", 0, false};
    }
}

bool Socket::sendTo(const void* buf, std::size_t len, const std::string& host,
                    std::uint16_t port) {
    IoScope io(*this);
    if (!io.entered()) return false;
    const socket_t h = io.handle();
    sockaddr_in dst;
    if (!resolveV4(host, port, dst, nullptr)) return false;
    for (;;) {
        int n = static_cast<int>(::sendto(h, static_cast<const char*>(buf),
                                          static_cast<int>(len), kSendFlags,
                                          reinterpret_cast<sockaddr*>(&dst), sizeof(dst)));
        if (n >= 0) return n == static_cast<int>(len);
        if (isInterrupted(lastErr())) continue;  // EINTR — retry the whole datagram
        return false;
    }
}

}  // namespace naudio::net
