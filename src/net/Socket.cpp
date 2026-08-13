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
#  include <sys/types.h>
#  include <unistd.h>
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

Socket::Socket() noexcept : handle_(kInvalidSocket) {}

Socket::Socket(socket_t handle) noexcept : handle_(handle) {}

Socket::~Socket() { close(); }

Socket::Socket(Socket&& other) noexcept : handle_(other.handle_.exchange(kInvalidSocket)) {}

Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        close();
        handle_.store(other.handle_.exchange(kInvalidSocket));
    }
    return *this;
}

bool Socket::valid() const noexcept { return handle_.load() != kInvalidSocket; }

void Socket::close() noexcept {
    // Atomic exchange: only one caller (or the destructor) ever takes the real
    // handle, so close() is both data-race-free against concurrent readers and
    // safe against a double close from two threads.
    socket_t h = handle_.exchange(kInvalidSocket);
    if (h != kInvalidSocket) {
        closeNative(h);
    }
}

void Socket::shutdownBoth() noexcept {
    // Read the handle rather than exchanging it: this must NOT take ownership, because close()
    // still has to run afterwards to free the descriptor. A concurrent close() that wins the race
    // leaves h invalid here and the shutdown is simply skipped, which is correct — the socket is
    // already going away.
    socket_t h = handle_.load();
    if (h == kInvalidSocket) return;
#ifdef _WIN32
    ::shutdown(h, SD_BOTH);
#else
    ::shutdown(h, SHUT_RDWR);
#endif
    // Return value deliberately unread: ENOTCONN is the expected answer for a listener or an
    // unconnected UDP socket, and every other failure still leaves close() to do the real work.
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
    socket_t h = handle_.load();
    if (h == kInvalidSocket) {
        setErr(err, "acceptTcp on invalid socket");
        return IoStatus::Error;
    }
    if (timeoutMs > 0) {
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(h, &rf);
        timeval tv = msToTimeval(timeoutMs);
        int sel = ::select(static_cast<int>(h) + 1, &rf, nullptr, nullptr, &tv);
        if (sel == 0) return IoStatus::TimedOut;
        if (sel < 0) {
            if (isInterrupted(lastErr())) return IoStatus::TimedOut;
            setErr(err, "select() failed in acceptTcp");
            return IoStatus::Error;
        }
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
        fd_set wf, ef;
        FD_ZERO(&wf);
        FD_ZERO(&ef);
        FD_SET(fd, &wf);
        FD_SET(fd, &ef);
        timeval tv = msToTimeval(timeoutMs);
        int sel = ::select(static_cast<int>(fd) + 1, nullptr, &wf, &ef, &tv);
        if (sel <= 0) {
            setErr(err, sel == 0 ? "connect timed out" : "select() failed");
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

bool Socket::setSendBufferAtLeast(int bytes) {
    return raiseSocketBuffer(handle_.load(), SO_SNDBUF, bytes);
}

bool Socket::setRecvBufferAtLeast(int bytes) {
    return raiseSocketBuffer(handle_.load(), SO_RCVBUF, bytes);
}

int Socket::setSendBufferSize(int bytes) {
    return resizeSocketBuffer(handle_.load(), SO_SNDBUF, bytes);
}

int Socket::setRecvBufferSize(int bytes) {
    return resizeSocketBuffer(handle_.load(), SO_RCVBUF, bytes);
}

bool Socket::setRecvTimeout(int ms) {
    socket_t h = handle_.load();
    if (h == kInvalidSocket) return false;
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
    socket_t h = handle_.load();
    if (h == kInvalidSocket) return false;
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
    socket_t h = handle_.load();
    if (h == kInvalidSocket) return 0;
    sockaddr_in addr;
    socklen_t len = sizeof(addr);
    if (::getsockname(h, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        return 0;
    }
    return ntohs(addr.sin_port);
}

std::string Socket::remoteAddress() const {
    socket_t h = handle_.load();
    if (h == kInvalidSocket) return "";
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
    socket_t h = handle_.load();
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

bool Socket::sendAll(const void* buf, std::size_t len) {
    socket_t h = handle_.load();
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
            if (budgetMs > 0 && elapsedMsSince(callStart) >= budgetMs) return false;
            continue;
        }
        int e = lastErr();
        if (isInterrupted(e)) continue;
        return false;
    }
    return true;
}

RecvFromResult Socket::recvFrom(void* buf, std::size_t len) {
    socket_t h = handle_.load();
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
        int n = static_cast<int>(::recvfrom(h, static_cast<char*>(buf),
                                            static_cast<int>(len), recvFlags,
                                            reinterpret_cast<sockaddr*>(&src), &sl));
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
    socket_t h = handle_.load();
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
