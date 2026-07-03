// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio — cross-platform socket foundation.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

// The single containment point for platform socket differences. NOTHING outside
// Socket.cpp includes a platform socket header — POSIX (<sys/socket.h> et al.)
// and Winsock (<winsock2.h>) both live in the .cpp. This header keeps only a
// plain integer typedef for the native handle, so it is safe to #include widely
// (the transport interfaces, the protocol handler, the TCP transports).
//
// Cross-platform from the start (POSIX + a Winsock shim); IPv4 (AF_INET) only for
// v1 — the wire is IPv4 loopback/wildcard; IPv6 is a future extension.

namespace naudio::net {

// Native socket handle, kept platform-neutral here. On POSIX it is an int fd; on
// Windows a SOCKET is an unsigned pointer-width handle — uintptr_t holds both.
#ifdef _WIN32
using socket_t = std::uintptr_t;
#else
using socket_t = int;
#endif

// An invalid/unset handle sentinel (POSIX -1; Windows INVALID_SOCKET == ~0).
// Defined in the .cpp where the platform value is known.
extern const socket_t kInvalidSocket;

// Outcome of a non-instant socket operation — three states: bytes/connection (Ok),
// timeout (TimedOut), and EOF/error (Closed/Error). The framing FSM treats both
// Closed and Error as fatal (tear the connection down).
enum class IoStatus {
    Ok,        // Operation succeeded (bytes transferred / client accepted).
    TimedOut,  // The recv/accept deadline elapsed with no progress (retryable).
    Closed,    // Peer closed the connection (recv returned 0 / EOF).
    Error,     // A non-recoverable socket error.
};

// Result of a stream recv: status plus the byte count (valid only on Ok).
struct RecvResult {
    IoStatus status = IoStatus::Error;
    std::size_t bytes = 0;
};

// Result of a datagram recvFrom: status, byte count, and the sender endpoint
// (numeric host:port) for UDP address-based demultiplexing.
struct RecvFromResult {
    IoStatus status = IoStatus::Error;
    std::size_t bytes = 0;
    std::string senderHost;        // numeric IPv4 string, e.g. "127.0.0.1"
    std::uint16_t senderPort = 0;
    // True when the datagram was larger than the supplied buffer and the tail
    // was discarded. `bytes` is then clamped to the buffer size. Reliably
    // set on Linux (MSG_TRUNC reports the true length); best-effort elsewhere.
    bool truncated = false;
};

// A thin RAII wrapper around one OS socket. Move-only; the destructor closes the
// handle. Factory functions return an invalid Socket (check valid()) on failure
// and, when given a non-null err, fill it with a diagnostic string.
class Socket {
public:
    Socket() noexcept;                    // an empty, invalid socket
    explicit Socket(socket_t handle) noexcept;  // adopt an existing handle
    ~Socket();

    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    bool valid() const noexcept;
    socket_t handle() const noexcept { return handle_.load(); }
    void close() noexcept;

    // Initializes the platform socket library (Winsock WSAStartup). Idempotent
    // and process-wide; a no-op on POSIX. Called implicitly by every factory,
    // exposed for tests/drivers that want to front-load it.
    static void ensureStartup();

    // Resolves a host ("localhost", "example.com", or a numeric IP) to its
    // numeric IPv4 dotted-quad ("127.0.0.1"), or "" on failure. Used to compare a
    // datagram's numeric sender against a configured remote host (UDP source
    // validation, C3).
    static std::string resolveHostV4(const std::string& host);

    // --- TCP server ---

    // Creates a bound + listening TCP socket. An empty bindHost binds the
    // wildcard address "0.0.0.0" (the default — LAN-reachable); pass
    // "127.0.0.1" for loopback-only. port 0 selects an ephemeral port.
    static Socket listenTcp(const std::string& bindHost, std::uint16_t port,
                            bool reuseAddr, std::string* err);

    // Accepts one pending connection. timeoutMs == 0 blocks; > 0 waits up to the
    // deadline (TimedOut if none arrives). On Ok, `out` receives the new socket.
    IoStatus acceptTcp(int timeoutMs, Socket& out, std::string* err);

    // --- TCP client ---

    // Connects to host:port with a connect deadline (timeoutMs == 0 blocks).
    static Socket connectTcp(const std::string& host, std::uint16_t port,
                             int timeoutMs, std::string* err);

    // --- UDP ---

    // Creates a bound UDP socket (wildcard bindHost => "0.0.0.0").
    static Socket bindUdp(const std::string& bindHost, std::uint16_t port,
                          bool reuseAddr, std::string* err);

    // --- options / introspection ---

    // Sets the receive timeout (SO_RCVTIMEO). ms == 0 means block indefinitely.
    bool setRecvTimeout(int ms);

    // The bound local port, or 0 if unbound/unknown.
    std::uint16_t localPort() const;

    // The connected peer as "ip:port", or "" if unavailable.
    std::string remoteAddress() const;

    // --- stream I/O (TCP) ---

    // Reads up to len bytes. Honors the receive timeout: TimedOut on deadline,
    // Closed on peer EOF (recv == 0), Error otherwise. Retries EINTR internally.
    RecvResult recv(void* buf, std::size_t len);

    // Writes all len bytes, looping over partial sends. Returns false on error.
    bool sendAll(const void* buf, std::size_t len);

    // --- datagram I/O (UDP) ---

    // Receives one datagram, recording the sender endpoint. Honors the recv
    // timeout (TimedOut on deadline).
    RecvFromResult recvFrom(void* buf, std::size_t len);

    // Sends one datagram to host:port. Returns false on error.
    bool sendTo(const void* buf, std::size_t len, const std::string& host,
                std::uint16_t port);

private:
    // Atomic so close() (which stores kInvalidSocket) cannot data-race the
    // receive worker / send path reading the handle for a syscall (C4 — the
    // TSan-flagged Socket::close vs AudioProtocolHandler recv race). Each I/O
    // method loads the handle once into a local before the syscall; a close()
    // landing after the load makes the syscall fail cleanly (EBADF) with no UB.
    std::atomic<socket_t> handle_;
};

}  // namespace naudio::net
