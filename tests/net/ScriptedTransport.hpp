// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio tests — scripted ServerTransport / ClientTransport / ClientConnection doubles.
//
// Copyright (C) 2025-2026 Terrell Deppe
//
// WHY THIS EXISTS (issue #67). AudioStreamServer::ClientSession is private to
// src/net/AudioStreamServer.cpp, so the only way to drive a real session is to hand the
// server a transport. These doubles do exactly that and nothing else: the session, the
// handshake, the receive/writer threads, the mixer and its arbitration are all REAL. The
// only thing scripted is what comes off the wire — which is the point, because the value
// under test (Provenance) is not a wire field and cannot be spelled by a real peer.
//
// THE CLIENT SIDE (issue #71) reuses the same ScriptedClientConnection through
// ScriptedClientTransport, because ClientConnection is the one interface BOTH ends drive —
// the server reaches it through ServerTransport::acceptClient, the client through
// ClientTransport::connect. Only two members are client-specific (pushConnectAccept and the
// failControlSends latch) and both are inert unless an arm asks for them, so the server arms
// below are untouched by their presence.
//
// THIS FILE COVERS HOP 2 OF A TWO-HOP CHAIN, and is worthless without hop 1:
//   hop 1  a real server-fed UDP connection genuinely marks an FEC-reconstructed frame
//          Recovered  ->  UdpConnection.ServerFedRecoveredFrameReachesTheCallerMarkedRecovered
//                         (tests/net/test_udp_connection.cpp:867)
//   hop 2  the server acts on that mark  ->  the arms in test_server.cpp that use this file
// If hop 1 is ever deleted, the arms here are asserting on a value nothing in production
// produces. Do not delete one without re-homing the other.
//
// WHY NOT AN END-TO-END FEC RELAY. A relay that induces real loss cannot reach the state
// these arms need. FEC recovers only when exactly one slot is missing
// (src/FecDecoder.cpp:378), so a repair is always preceded into the ordered queue by the
// block's surviving members — and the first of those claims the TX channel. The mixer's
// unowned row, which is where a repair's arbitration behaviour is observable, is therefore
// never the state a repaired frame arrives in. Waiting out the lease does not help either:
// on the only FEC-enabled preset the decoder discards the pending block at 490 ms
// (5*10 + 40 + 400, src/net/UdpClientConnection.cpp:98-101) while the default lease runs to
// 500 ms, so the block is gone before the channel is free. Attempt 1 of #67 built the relay,
// confirmed the fixture worked, and still could not land a detector.
//
// ANTI-DRIFT CONTRACT. These classes implement the Transport.hpp interfaces by hand, on
// purpose. If Transport.hpp gains a pure virtual, THIS FILE IS SUPPOSED TO STOP COMPILING —
// that break is the signal that a real transport gained a behaviour the doubles do not model.
// Do not silence it by giving the interface method a default in Transport.hpp; implement it
// here and decide what the scripted answer should be.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include "naudio/AudioPacket.hpp"
#include "naudio/ControlMessage.hpp"
#include "naudio/net/ClientAddress.hpp"
#include "naudio/net/Socket.hpp"
#include "naudio/net/Transport.hpp"

namespace naudio::test {

// Nested inside `naudio`, so AudioPacket / ControlType / PacketType / Provenance resolve
// unqualified; the transport types are named with their `net::` prefix rather than dragged
// in with a using-directive, which in a header would leak into every includer.
using net::ClientAddress;
using net::ClientConnection;
using net::IoStatus;
using net::ReceiveResult;
using net::ServerTransport;

// One delivery the scripted connection handed to the session, recorded as the session saw
// it. The provenance is the whole reason this record exists: it is the only proof that the
// frame under test was actually presented as Recovered, rather than the arm having quietly
// tested a Live frame against a Live expectation.
struct Handed {
    PacketType type;
    Provenance provenance;

    bool operator==(const Handed& o) const { return type == o.type && provenance == o.provenance; }
};

inline std::ostream& operator<<(std::ostream& os, const Handed& h) {
    os << "{" << static_cast<int>(h.type) << ","
       << (h.provenance == Provenance::Recovered ? "Recovered" : "Live") << "}";
    return os;
}

// A ClientConnection whose receive side is a script and whose send side is a ledger.
//
// Threading: the session's run thread and receive thread both call in, and the test thread
// pushes and reads. Every member is guarded by mutex_; every wait is bounded. NOTHING here
// may block indefinitely — AudioStreamServer::stop() waits on its thread barrier with no
// timeout (src/net/AudioStreamServer.cpp:532-533) and ~AudioStreamServer calls stop(), so a
// double that parks a session thread turns a test failure into a ctest hang.
class ScriptedClientConnection : public ClientConnection {
public:
    // Send kinds for the per-kind stall latch (see stallTxAudioSends and friends).
    static constexpr unsigned kTxAudio = 1u << 0;
    static constexpr unsigned kControl = 1u << 1;
    static constexpr unsigned kHeartbeat = 1u << 2;
    static constexpr unsigned kRawPacket = 1u << 3;

    explicit ScriptedClientConnection(std::string id)
        : address_(id), remote_(std::move(id)) {}

    // --- Script (test thread) ---

    // Queues one frame for the session to receive, with the provenance the session will see.
    void push(AudioPacket packet, Provenance provenance) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            inbox_.push_back(ReceiveResult::of(std::move(packet), provenance));
        }
        cv_.notify_all();
    }

    // The ConnectRequest performHandshake needs. A single 0x01 payload byte is enough: the
    // handshake checks only the packet type, that the payload deserializes, and that the
    // message type is ConnectRequest (src/net/AudioStreamServer.cpp:173-178).
    void pushConnectRequest() {
        push(AudioPacket::createControl(0, {static_cast<std::uint8_t>(0x01)}), Provenance::Live);
    }

    // The CLIENT-direction counterpart: what AudioStreamClient::performHandshake waits for
    // after sending its ConnectRequest. It loops on receivePacket until it sees ConnectAccept
    // or ConnectReject (src/net/AudioStreamClient.cpp:208-234), so this one push is the whole
    // handshake as far as the client is concerned.
    void pushConnectAccept() {
        push(AudioPacket::createControl(0, ControlMessage::connectAccept().serialize()),
             Provenance::Live);
    }

    // THE BARRIER. A LatencyProbe is answered by the session's RECEIVE thread with a direct
    // connection_->sendControl (src/net/AudioStreamServer.cpp:378), and receiveLoop is
    // strictly sequential (:266-289). So a LatencyResponse in the ledger proves every frame
    // pushed BEFORE the probe has already been fully handled — handleTxAudio returned and its
    // submitTxAudio committed under the mixer's lock. This is a happens-after edge in
    // PRODUCTION code, not an observation of the double's own bookkeeping, which is what
    // makes it survive a refactor of how the double counts calls.
    void pushLatencyProbe() {
        push(AudioPacket::createControl(0, ControlMessage::latencyProbe(0).serialize()),
             Provenance::Live);
    }

    // Makes sendRxAudio PARK on entry until close(), modelling a TCP peer whose receive
    // window has closed: the production writerLoop blocks inside Socket::sendAll. Off by
    // default, so no pre-existing arm changes behaviour.
    //
    // That park is now UNBOUNDED WHERE PRODUCTION IS BOUNDED, and deliberately so. Since
    // #56/#70 a real session's sends carry a deadline and a whole-call budget
    // (AudioProtocolHandler's constructor arms it), so a real wedged writer gives up after
    // CONNECTION_TIMEOUT_MS / 2. This double keeps parking until close() because the arms
    // built on it are about the BACKLOG CAP and the eviction it triggers, which must hold
    // however long the writer is stuck; a double that released on its own would let a
    // passing arm mean "the deadline fired" instead of "the cap fired".
    //
    // close() MUST release it, and does — close() already sets closed_ and notifies. This is
    // not optional politeness: AudioStreamServer::stop() closes each session and THEN waits
    // on an untimed thread barrier, so a latch that survived close() would turn a failed
    // ASSERT into a ctest hang rather than a test failure. That is the hazard this file's
    // threading note above already names, arrived at from the other direction.
    void stallRxAudio() {
        std::lock_guard<std::mutex> lock(mutex_);
        stallRx_ = true;
    }

    // How many times the session's writer thread entered sendRxAudio. With the latch armed
    // this saturates at 1 and stays there — which is the observable proving the writer is
    // PARKED rather than merely slow, and it is the guard that separates "the induced fault
    // fired" from "the setup ran".
    int rxAudioCalls() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return rxAudioCalls_;
    }

    // --- Ledger (test thread) ---

    std::vector<Handed> handedOut() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return handedOut_;
    }

    int countSent(ControlType type) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = sentControls_.find(type);
        return it == sentControls_.end() ? 0 : it->second;
    }

    // Waits for the Nth control message of `type` to be sent. Returns false on timeout, and
    // the caller is expected to dump diagnostics() — a timeout here means the session never
    // got where the arm assumed it was, which is a different failure from the arm's subject.
    bool waitForControl(ControlType type, int count, int timeoutMs) const {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&]() {
            auto it = sentControls_.find(type);
            return it != sentControls_.end() && it->second >= count;
        });
    }

    // Everything the arm might want in a failure message, so a red arm two years from now
    // names its own cause instead of printing a bare expectation.
    std::string diagnostics() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string s = "scripted connection " + remote_ + ": handed out " +
                        std::to_string(handedOut_.size()) + " frame(s), inbox " +
                        std::to_string(inbox_.size()) + ", closed=" + (closed_ ? "yes" : "no") +
                        ", rxAudioCalls=" + std::to_string(rxAudioCalls_) +
                        ", stallRx=" + (stallRx_ ? "armed" : "off") +
                        ", failControls=" + (failControls_ ? "armed" : "off") +
                        ", stallMask=" + std::to_string(stallMask_) +
                        ", sendsEntered=" + std::to_string(sendsEntered_) +
                        ", sendsParked=" + std::to_string(sendsParked_) +
                        ", txAudioCalls=" + std::to_string(txAudioCalls_) +
                        ", heartbeatsSent=" + std::to_string(heartbeatsSent_) +
                        ", timeoutChecks=" + std::to_string(timeoutChecks_.load()) +
                        ", timedOut=" + (timedOut_.load() ? "yes" : "no") +
                        ", controls sent {";
        for (const auto& [type, n] : sentControls_) {
            s += std::string(" ") + controlTypeName(type) + "x" + std::to_string(n);
        }
        return s + " }";
    }

    // --- ClientConnection: the eight methods AudioStreamServer actually calls ---

    ReceiveResult receivePacket(int timeoutMs) override {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                     [this]() { return !inbox_.empty() || closed_; });
        // Closed wins over a queued frame: a session that has been told to go away must not
        // keep consuming script. Returning dead() here is also what lets receiveLoop exit
        // promptly on stop() instead of idling out its timeout.
        if (closed_) return ReceiveResult::dead();
        if (inbox_.empty()) return ReceiveResult::noData();
        ReceiveResult r = std::move(inbox_.front());
        inbox_.pop_front();
        handedOut_.push_back(Handed{r.packet->packetType(), r.provenance});
        return r;
    }

    // Makes every subsequent sendControl FAIL, modelling a peer whose socket is broken or
    // whose receive window has closed for longer than the send budget (#56/#70). Off by
    // default, so no pre-existing arm changes behaviour.
    //
    // Failed sends are still RECORDED, and that is what makes the #71 arm discriminate rather
    // than pass vacuously: the arm counts ATTEMPTS. If a failure went unrecorded, the salvo
    // would count 0 both before and after the fix and the assertion would hold for the wrong
    // reason. Recording the attempt is the whole instrument.
    void failControlSends() {
        std::lock_guard<std::mutex> lock(mutex_);
        failControls_ = true;
    }

    // --- The client-side wedge (#71) ---
    //
    // WHY A SHARED sendMutex_ AND NOT JUST A SLEEP. Every TCP send on a real connection
    // funnels through AudioProtocolHandler::sendPacket, which holds ONE sendMutex_ across the
    // whole Socket::sendAll (src/net/AudioProtocolHandler.cpp, sendPacket). So a wedged send
    // does not stall one frame — it blocks every OTHER sender on the connection, which on the
    // client means the heartbeat, the latency probe and disconnect()'s courtesy salvo. #71
    // says this explicitly: "a double whose sendControl merely sleeps proves nothing", because
    // the defect is CONTENTION BETWEEN TWO ClientConnection METHODS, not the latency of one.
    //
    // THE LATCH IS PER-KIND, and that precision is what makes the arms deterministic rather
    // than racy. Stalling every kind at once leaves it to scheduling whether the parked sender
    // is sendLoop or the heartbeat loop's own probe — and those two stage OPPOSITE situations:
    // one wedges a sender the watchdog must see past, the other wedges the watchdog itself.
    //
    // Armed: a send of that kind records its entry, then parks HOLDING sendMutex_, so every
    // other kind blocks behind it exactly as production blocks behind sendPacket's lock.
    // releaseSends() lets it go with a true; close() lets it go with a FALSE, which is what a
    // real parked ::send returns once the socket is closed under it — MEASURED on macOS/arm64
    // at ~5 ms after the close call, errno 9 (a bare ::close, no shutdown).
    void stallTxAudioSends() { armStall(kTxAudio); }
    void stallHeartbeatSends() { armStall(kHeartbeat); }
    void stallControlSends() { armStall(kControl); }

    void releaseSends() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stallMask_ = 0;
        }
        cv_.notify_all();
    }

    // THE L136 GUARD for every arm built on stallSends(): it separates "the induced fault
    // fired" from "the setup ran". An arm that asserts something happened WHILE a send is
    // wedged is vacuous unless a send is actually wedged at that moment.
    int sendsParked() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return sendsParked_;
    }

    // Blocks until a sender is parked in the serialised path. Returns false on timeout, and
    // the caller is expected to dump diagnostics().
    bool waitForParkedSend(int timeoutMs) const {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                            [this]() { return sendsParked_ > 0; });
    }

    // Every entry into the serialised send path, parked or not. The arms about the watchdog
    // assert on this rather than on wall clock: "no send was attempted" is a statement about
    // the mechanism, where "it finished quickly" is a statement about the machine (L84).
    int sendsEntered() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return sendsEntered_;
    }

    int txAudioCalls() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return txAudioCalls_;
    }

    int heartbeatsSent() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return heartbeatsSent_;
    }

    // How many times heartbeatLoop consulted the watchdog. Proves the loop is ITERATING, which
    // is what tells a working check apart from a loop that never got there.
    int timeoutChecks() const { return timeoutChecks_.load(); }

    void setShouldSendHeartbeat(bool v) { shouldHeartbeat_.store(v); }
    void setTimedOut(bool v) { timedOut_.store(v); }

    // Records unconditionally, including after close(). Returns true unless failControlSends()
    // has been armed: a false in the SERVER arms would make the session tear itself down
    // (src/net/AudioStreamServer.cpp:209-211) and would drop the very messages the absence
    // arms assert are absent — the harness would then produce the passing answer for the wrong
    // reason. Those arms never arm the latch, so they still see an unconditional true.
    bool sendControl(const ControlMessage& message) override {
        std::lock_guard<std::mutex> serialize(sendMutex_);
        bool fail;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++sentControls_[message.messageType()];
            fail = failControls_;
        }
        cv_.notify_all();
        if (!enterSend(kControl)) return false;
        return !fail;
    }

    // NOT SERIALISED ON sendMutex_, and that exclusion is load-bearing rather than an
    // oversight. Production does serialise it — sendRxAudio funnels through the same
    // AudioProtocolHandler::sendPacket — but this method's stall latch parks until close(),
    // and the server arms that arm it rely on the session's RECEIVE thread still being able to
    // answer a LatencyProbe with sendControl while the writer is parked (the BARRIER above).
    // Put this under sendMutex_ and that barrier deadlocks, turning every one of those arms
    // into a ctest hang rather than a failure. The client-side serialisation below models the
    // wedge #71 is about; this one keeps the server-side arms working.
    bool sendRxAudio(const std::uint8_t*, std::size_t, std::size_t) override {
        std::unique_lock<std::mutex> lock(mutex_);
        ++rxAudioCalls_;
        cv_.notify_all();  // wake a test waiting to observe that the writer arrived
        if (stallRx_) cv_.wait(lock, [this]() { return !stallRx_ || closed_; });
        return true;
    }

    bool sendHeartbeat() override {
        std::lock_guard<std::mutex> serialize(sendMutex_);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++heartbeatsSent_;
        }
        cv_.notify_all();
        return enterSend(kHeartbeat);
    }

    // Both default to the answers the server arms have always seen — no heartbeats, never
    // timed out — because those arms are about TX provenance and a session that reaped itself
    // mid-arm would erase the state under assertion. The client arms opt in via the setters.
    bool shouldSendHeartbeat() override { return shouldHeartbeat_.load(); }
    bool isConnectionTimedOut() const override {
        timeoutChecks_.fetch_add(1);
        return timedOut_.load();
    }

    void close() override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        cv_.notify_all();
    }

    std::string remoteAddress() const override { return remote_; }

    // --- ClientConnection: the ten the server never calls (Transport.hpp, verified) ---

    const ClientAddress& clientAddress() const override { return address_; }

    // Serialised and stallable: this is the send AudioStreamClient::sendLoop drives, and
    // parking it here is how an arm stages "the TX writer is wedged holding the send lock".
    bool sendTxAudio(const std::uint8_t*, std::size_t) override {
        std::lock_guard<std::mutex> serialize(sendMutex_);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++txAudioCalls_;
        }
        cv_.notify_all();
        return enterSend(kTxAudio);
    }

    bool sendPacket(const AudioPacket&) override {
        std::lock_guard<std::mutex> serialize(sendMutex_);
        return enterSend(kRawPacket);
    }
    std::int64_t timeSinceLastReceive() const override { return 0; }
    std::int64_t packetsSent() const override { return 0; }
    std::int64_t packetsReceived() const override { return 0; }
    std::int64_t bytesSent() const override { return 0; }
    std::int64_t bytesReceived() const override { return 0; }
    int crcErrors() const override { return 0; }
    bool isClosed() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }

private:
    // Called with sendMutex_ HELD, which is the whole point — a parked sender keeps the send
    // lock exactly as Socket::sendAll does under AudioProtocolHandler::sendPacket. Returns
    // what the send should report: true normally, false if close() broke it.
    //
    // LOCK ORDER, and it is one-way everywhere in this class: sendMutex_ THEN mutex_. Never
    // the reverse. The cv wait below releases mutex_ while parked, so close() and the ledger
    // readers stay live against a wedged sender.
    bool enterSend(unsigned kind) const {
        std::unique_lock<std::mutex> lock(mutex_);
        ++sendsEntered_;
        if ((stallMask_ & kind) != 0 && !closed_) {
            ++sendsParked_;
            cv_.notify_all();  // wake a test waiting to observe the wedge
            cv_.wait(lock, [this, kind]() { return (stallMask_ & kind) == 0 || closed_; });
            --sendsParked_;
            cv_.notify_all();
        }
        return !closed_;
    }

    void armStall(unsigned kind) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stallMask_ |= kind;
        }
        cv_.notify_all();
    }

    mutable std::mutex mutex_;
    // Mirrors AudioProtocolHandler::sendPacket's sendMutex_. Acquired BEFORE mutex_, always.
    mutable std::mutex sendMutex_;
    mutable std::condition_variable cv_;
    std::deque<ReceiveResult> inbox_;
    std::vector<Handed> handedOut_;
    std::map<ControlType, int> sentControls_;
    bool failControls_ = false;
    bool stallRx_ = false;
    int rxAudioCalls_ = 0;
    bool closed_ = false;
    mutable unsigned stallMask_ = 0;
    mutable int sendsParked_ = 0;
    mutable int sendsEntered_ = 0;
    int txAudioCalls_ = 0;
    int heartbeatsSent_ = 0;
    std::atomic<bool> shouldHeartbeat_{false};
    std::atomic<bool> timedOut_{false};
    mutable std::atomic<int> timeoutChecks_{0};
    ClientAddress address_;
    std::string remote_;
};

// A ServerTransport that accepts exactly the connections a test hands it.
//
// port() reports a sentinel rather than a plausible number, so an arm asserting on
// AudioStreamServer::port() (which forwards to transport_->port(),
// src/net/AudioStreamServer.cpp:788-795) proves the INJECTED transport is the one in use.
// A realistic-looking port would be satisfied by the real UDP/TCP transport too.
class ScriptedServerTransport : public ServerTransport {
public:
    static constexpr int kScriptedPort = 45671;

    // Queues a connection for the server's accept loop to pick up.
    void offer(std::shared_ptr<ClientConnection> connection) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_.push_back(std::move(connection));
        }
        cv_.notify_all();
    }

    int acceptCalls() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return acceptCalls_;
    }

    bool bind(std::uint16_t, std::string*) override {
        bound_ = true;
        // Reset, so a rebind after close() does not leave acceptClient returning instantly
        // forever — which would spin a restarted accept loop rather than poll it.
        closed_ = false;
        return true;
    }
    bool isBound() const override { return bound_; }
    int port() const override { return kScriptedPort; }

    IoStatus acceptClient(int timeoutMs, std::shared_ptr<ClientConnection>& out,
                          std::string*) override {
        std::unique_lock<std::mutex> lock(mutex_);
        ++acceptCalls_;
        cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                     [this]() { return !pending_.empty() || closed_; });
        if (pending_.empty()) return IoStatus::TimedOut;  // incl. closed — stop() then joins
        out = std::move(pending_.front());
        pending_.pop_front();
        return IoStatus::Ok;
    }

    void disconnectClient(const std::shared_ptr<ClientConnection>& connection) override {
        if (connection) connection->close();
    }

    std::int64_t packetsSent() const override { return 0; }
    std::int64_t packetsReceived() const override { return 0; }
    std::int64_t bytesSent() const override { return 0; }
    std::int64_t bytesReceived() const override { return 0; }
    int crcErrors() const override { return 0; }
    std::int64_t controlRetransmits() const override { return 0; }
    std::int64_t orderedQueueDrops() const override { return 0; }

    // Must wake the accept loop: stop() closes the transport and then JOINS the accept
    // thread (src/net/AudioStreamServer.cpp:548-549), so a close() that only sets a flag
    // leaves that join waiting on the full acceptClient timeout every time.
    void close() override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        cv_.notify_all();
    }

private:
    mutable std::mutex mutex_;
    mutable std::condition_variable cv_;
    std::deque<std::shared_ptr<ClientConnection>> pending_;
    int acceptCalls_ = 0;
    bool bound_ = false;
    bool closed_ = false;
};

// A ClientTransport that hands AudioStreamClient a connection the test already owns.
//
// The client's counterpart to ScriptedServerTransport, and the seam
// AudioStreamClient::setTransportFactory exists to reach (issue #71). It is the only way to
// make a client-side send FAIL on demand: AudioStreamClient::disconnect()'s courtesy salvo is
// gated on connected_, so an arm about the salvo needs a completed handshake AND a
// controllable send — which no real socket can stage without a genuinely wedged peer and a
// 5 s send budget per copy.
//
// connect() hands out the SAME connection every time rather than a fresh one per call, so a
// reconnect attempt lands on the object the test is already holding and its ledger keeps
// accumulating. Handing back a new connection per attempt would silently reset the counts an
// arm is about to assert on.
class ScriptedClientTransport : public net::ClientTransport {
public:
    explicit ScriptedClientTransport(std::shared_ptr<ClientConnection> connection)
        : connection_(std::move(connection)) {}

    int connectCalls() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return connectCalls_;
    }

    bool wasClosed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }

    std::shared_ptr<ClientConnection> connect(const std::string&, std::uint16_t, int,
                                              std::string* err) override {
        std::lock_guard<std::mutex> lock(mutex_);
        ++connectCalls_;
        if (!connection_ && err) *err = "scripted transport has no connection";
        return connection_;
    }

    void close() override {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
    }

private:
    mutable std::mutex mutex_;
    std::shared_ptr<ClientConnection> connection_;
    int connectCalls_ = 0;
    bool closed_ = false;
};

}  // namespace naudio::test
