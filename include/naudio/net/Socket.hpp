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

    // Half-closes BOTH directions with ::shutdown, which wakes any thread currently parked in
    // send() or recv() on this socket. Errors are ignored on purpose: ENOTCONN on a socket that
    // was never connected (a listener, an unconnected UDP socket) is the expected answer, not a
    // fault, and there is nothing a caller could do about it.
    //
    // WHY THIS IS NOT THE SAME AS close() (issue #56, item 1). close() drops a DESCRIPTOR; it does
    // not by itself guarantee that a thread already blocked in the kernel on that socket returns.
    // A blocked send holds its own reference to the open file description, so on Linux the
    // descriptor going away need not disturb it — the classic symptom being a writer thread that
    // stays parked after the connection was torn down. macOS/BSD do generally wake the sleeper,
    // which is exactly why this went unnoticed here for so long: the tree had no ::shutdown call
    // anywhere and the one platform it was developed on papers over the difference.
    //
    // shutdown() is the portable instrument for that: it changes the SOCKET's state rather than
    // the descriptor table, so a parked send/recv returns promptly on every platform. Call it
    // BEFORE close(), never instead of it — it frees no resources.
    void shutdownBoth() noexcept;

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

    // Sets the send timeout (SO_SNDTIMEO). ms == 0 means block indefinitely.
    //
    // ONE KNOB, TWO MECHANISMS. This arms the kernel's own send deadline AND is the
    // whole-call budget sendAll() enforces across its retry loop (issue #70); sendAll
    // reads the value back with getsockopt rather than caching it, so there is no second
    // copy to keep in step. Both are needed, because neither bounds the other's case.
    //
    // Without any of it sendAll() has no deadline whatsoever: a peer that stops reading
    // closes our receive window, the send buffer fills, and ::send parks in the kernel
    // with nothing to wake it. That is not merely a slow send — every send on a connection
    // funnels through one mutex held across this call
    // (AudioProtocolHandler::sendPacket), so one parked writer wedges every other sender,
    // including a teardown path whose own socket close is the only thing that would free
    // it (issue #69).
    //
    // WHAT THE KERNEL'S HALF DOES NOT COVER, and why the budget exists. SO_SNDTIMEO is a
    // deadline per ::send call, so a send that moves some bytes and then stalls returns
    // the partial count and sendAll's success limb arms a FRESH one. MEASURED on
    // macOS/arm64, peer draining 32 KB every 40 ms against a 200 ms deadline: 9 partial
    // writes and 3263 ms — 16.3x — with no budget, and 201 ms with it. Do not restate the
    // old "about 2x" anywhere; 2 is what ONE partial write costs, and the count is set by
    // how the peer drip-feeds, not by the deadline.
    //
    // WHAT THE BUDGET'S HALF DOES NOT COVER, so the pair is not read as an exact bound.
    // MEASURED on macOS/arm64: SO_SNDTIMEO bounds a WAIT FOR SPACE, not a call — an 8 MB
    // send under a 1000 ms deadline ran past 4 s inside ONE ::send with zero partial
    // returns, because room kept appearing before any single wait expired. A budget in the
    // retry loop cannot interrupt that; the loop never iterates. So the honest contract is
    // "the budget, plus at most one in-flight ::send", and that residual is bounded by how
    // much a single call hands the kernel — for naudio, one maximum-payload 0xAF01 frame
    // (16407 bytes), measured at 1.0x with a dead peer and 0.7x with one draining 8 KB per
    // 700 ms. Truly bounding a single ::send needs non-blocking send + poll, which would
    // make the shared fd non-blocking underneath the concurrent receive thread.
    //
    // #56 CLOSED THAT RESIDUAL WITHOUT THAT CHANGE, by measuring what it actually is here
    // rather than what it is in the worst case. Two independent bounds, both measured:
    //
    //   1. THE RESIDUAL IS EXACTLY ONE DEADLINE, because naudio never hands ::send more than
    //      one packet. There is a single sendAll call site (AudioProtocolHandler.cpp:68) and
    //      it passes one serialized 0xAF01 frame, so the ceiling is 19 + MAX_PAYLOAD + 4 =
    //      16407 bytes — not the 8 MB that produced the 4 s overshoot above. MEASURED on
    //      Linux (gcc:13 container), 16407 B to a dead peer under a 5000 ms deadline:
    //      returned after 5017 ms, 1.00x. So the honest whole bound is "the budget plus at
    //      most one deadline", i.e. CONNECTION_TIMEOUT_MS end to end, which is the same
    //      liveness window the rest of the protocol already uses.
    //   2. TEARDOWN NOW INTERRUPTS IT ANYWAY. Since #56 item 1, AudioProtocolHandler::close()
    //      calls Socket::shutdownBoth() before close(), and a ::send already parked in the
    //      kernel returns promptly on that. MEASURED on the same Linux container: a thread
    //      parked in send() was still parked 5 s after a bare close(), and returned 11 ms
    //      after shutdown()+close(). macOS returns immediately either way, which is why the
    //      difference stayed invisible in this project for so long.
    //
    // So non-blocking send + poll would buy a tighter constant, not a different guarantee,
    // and would cost making the shared fd non-blocking under the concurrent receive thread.
    // If that trade is ever revisited, revisit it against these numbers and not against the
    // 8 MB figure, which no naudio call path can produce.
    //
    // It needs no new error handling: the timeout reports EAGAIN/EWOULDBLOCK
    // (WSAETIMEDOUT on Windows), which isInterrupted() does not match, so sendAll returns
    // false on the existing limb and every caller already treats false as fatal. That is
    // the right response — a timed-out send has left a TRUNCATED frame on the wire, so it
    // is not retryable and the connection must go down. The budget returns false on the
    // same footing and for the same reason.
    bool setSendTimeout(int ms);

    // Raises SO_SNDBUF / SO_RCVBUF to at least `bytes` (never lowers an already-larger
    // buffer). Returns false if the resulting buffer is still smaller than `bytes`.
    //
    // Load-bearing for UDP, not a tuning knob: macOS refuses a sendto() larger than
    // SO_SNDBUF with EMSGSIZE, and its default UDP send buffer is only 9216 bytes
    // (net.inet.udp.maxdgram) — smaller than a maximum-payload 0xAF01 audio packet
    // (19 + 16384 + 4 = 16407). Without this, sending a legal packet fails, and the
    // caller cannot tell "the wire is broken" from "this kernel won't take it".
    bool setSendBufferAtLeast(int bytes);
    bool setRecvBufferAtLeast(int bytes);

    // Sets SO_SNDBUF / SO_RCVBUF toward `bytes` and, unlike the two above, will SHRINK a buffer
    // that is already larger. Returns the size the kernel actually settled on, or 0 if the
    // socket is invalid or the option could not be read back.
    //
    // THE RETURN VALUE IS THE CONTRACT, not the argument. No kernel is obliged to honour the
    // request, and the ways they decline differ enough that "did it work" is only answerable by
    // reading it back — which is why this returns the effective size rather than a bool.
    // MEASURED on macOS/arm64 over loopback TCP: a request of 65536 AFTER connect is honoured
    // exactly (65536), while the SAME request made BEFORE connect is clamped up to a floor
    // (8192 -> 65328 SO_SNDBUF, 8192 -> 326640 SO_RCVBUF) because auto-sizing has not yet been
    // pinned. Linux commonly reports back double what was asked. So callers that depend on the
    // size — as the send-budget arm in tests/net/test_socket.cpp does — must assert on this
    // return, never on the request (issue #74).
    //
    // Shrinking a TCP receive buffer bounds the peer's advertised window on POSIX. It does NOT
    // do so on Winsock: MEASURED on windows-latest, a 64 KiB receive buffer — set on the
    // listener before the handshake AND on the accepted socket after it — still absorbed an
    // 8 MB send in 30 ms with the peer reading nothing, while getsockopt reported 65536 back
    // the whole time. Windows enables DYNAMIC SEND BUFFERING by default and auto-tunes
    // SO_SNDBUF, so a nonzero request there is advisory.
    //
    // `bytes == 0` is legal and is the one way to defeat that: on Winsock it disables buffering
    // for that direction outright, so ::send cannot return until the peer accepts the bytes.
    // POSIX kernels clamp 0 up to their own minimum instead, so it is not portable — check the
    // return value, and prefer a positive size everywhere else.
    int setSendBufferSize(int bytes);
    int setRecvBufferSize(int bytes);

    // The bound local port, or 0 if unbound/unknown.
    std::uint16_t localPort() const;

    // The connected peer as "ip:port", or "" if unavailable.
    std::string remoteAddress() const;

    // --- stream I/O (TCP) ---

    // Reads up to len bytes. Honors the receive timeout: TimedOut on deadline,
    // Closed on peer EOF (recv == 0), Error otherwise. Retries EINTR internally.
    RecvResult recv(void* buf, std::size_t len);

    // Writes all len bytes, looping over partial sends. Returns false on error, and also
    // once the socket's configured send timeout has been spent across the whole call —
    // see setSendTimeout for what that does and does not bound. With no send timeout set
    // this loops until the bytes are gone or a send fails, exactly as it always did.
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
