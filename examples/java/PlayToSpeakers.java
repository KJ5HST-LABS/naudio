// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio examples — play a network audio stream to your speakers (Java).
//
// Copyright (C) 2025-2026 Terrell Deppe
//
// A dead-simple client: connect to a network-audio server and play whatever it
// is streaming on your local speakers. It drives the public `naudio.h` C ABI over
// the Java Foreign Function & Memory API (Panama, java.lang.foreign) — and only
// that C ABI — so it is also a worked example of calling a C library from Java
// with no JNI shim, no Maven, and no third-party dependency. It is a single source
// file you run directly with the `java` launcher.
//
// The playback is done by naudio itself: you pick a local output device and the
// library renders the received audio to it. By default the client picks your first
// output device and starts playing — no flags required:
//
//     java --enable-native-access=ALL-UNNAMED PlayToSpeakers.java --host 127.0.0.1 --port 4533
//
// Pair it with the demo source to hear something without a radio or a second
// machine (see examples/server):
//
//     na_audio_source --test-tone --port 4533               # one terminal: a test tone
//     java --enable-native-access=ALL-UNNAMED PlayToSpeakers.java --port 4533   # another: plays it
//
// Use `--list-devices` to see the output-device ids, then `--playback-id N` to
// choose one. `--backend null` runs hardware-free (the audio is still received,
// it just is not played) — handy on a headless box, in CI, or to verify a server
// end to end without a sound card.
//
// THREADING (from naudio.h): the RX audio callback fires on naudio's receive
// WORKER thread, once per frame. Keep it SHORT and non-blocking, do not call client
// lifecycle methods from it, and — the Java-specific part — do not let an exception
// escape it back into native code, and keep the FFM Arena that backs the upcall stub
// alive for the client's lifetime (we hold a single shared Arena open until after
// na_client_destroy). The FFM runtime attaches naudio's worker thread to the JVM for
// the duration of each upcall automatically.
//
// Requires JDK 22+ (the Foreign Function & Memory API is final since JDK 22). The
// library is found via (in order): --lib PATH, $NAUDIO_LIB, the sibling build tree
// (build/ and ../../build), then the platform's default library loader.
//
// Output: the machine-readable `RESULT ...` line goes to STDOUT; all human logs
// (events, the throughput meter, the summary) go to STDERR.

import static java.lang.foreign.ValueLayout.ADDRESS;
import static java.lang.foreign.ValueLayout.JAVA_BYTE;
import static java.lang.foreign.ValueLayout.JAVA_INT;
import static java.lang.foreign.ValueLayout.JAVA_LONG;

import java.io.File;
import java.lang.foreign.Arena;
import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.Linker;
import java.lang.foreign.MemoryLayout;
import java.lang.foreign.MemoryLayout.PathElement;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.SymbolLookup;
import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.atomic.AtomicLong;

public final class PlayToSpeakers {

    // ---- C ABI constants (mirror include/naudio.h) -------------------------------------------
    static final int NA_OK                    = 0;
    static final int NA_CLIENT_BACKEND_SYSTEM = 0;
    static final int NA_CLIENT_BACKEND_NULL   = 1;
    static final int NA_TRANSPORT_TCP         = 0;
    static final int NA_TRANSPORT_UDP         = 1;
    static final int NA_CAP_PLAYBACK          = 1;
    static final int NA_CAP_DUPLEX            = 2;
    static final int MAX_DEVICES              = 128;

    // na_device struct layout. Deriving the field offsets from a MemoryLayout (rather than
    // hand-counting bytes) keeps this honest if the struct ever changes. `size_t` in the RX
    // callback maps to JAVA_LONG: this client targets 64-bit platforms (size_t == 8 bytes).
    static final MemoryLayout NA_DEVICE = MemoryLayout.structLayout(
            JAVA_INT.withName("backend_id"),
            JAVA_INT.withName("capture_backend_id"),
            JAVA_INT.withName("playback_backend_id"),
            MemoryLayout.sequenceLayout(256, JAVA_BYTE).withName("name"),
            MemoryLayout.sequenceLayout(128, JAVA_BYTE).withName("host_api"),
            JAVA_INT.withName("type"),
            JAVA_INT.withName("capability"),
            JAVA_INT.withName("is_virtual"));
    static final long DEV_SIZE        = NA_DEVICE.byteSize();
    static final long DEV_PLAYBACK_ID = NA_DEVICE.byteOffset(PathElement.groupElement("playback_backend_id"));
    static final long DEV_NAME        = NA_DEVICE.byteOffset(PathElement.groupElement("name"));
    static final long DEV_HOST_API    = NA_DEVICE.byteOffset(PathElement.groupElement("host_api"));
    static final long DEV_CAPABILITY  = NA_DEVICE.byteOffset(PathElement.groupElement("capability"));
    static final long DEV_IS_VIRTUAL  = NA_DEVICE.byteOffset(PathElement.groupElement("is_virtual"));

    static final Linker LINKER = Linker.nativeLinker();

    // ---- downcall handles for the na_* functions this example uses ---------------------------
    static MethodHandle naStrerror, naLastError, naContextCreate, naContextDestroy, naEnumerate,
            naClientCreate, naClientDestroy, naClientSetTransport, naClientSetPlaybackDevice,
            naClientSetAudioCb, naClientConnect, naClientDisconnect, naClientIsConnected;

    static void bind(SymbolLookup lib) {
        naStrerror               = dh(lib, "na_strerror",                   FunctionDescriptor.of(ADDRESS, JAVA_INT));
        naLastError              = dh(lib, "na_last_error",                 FunctionDescriptor.of(JAVA_INT));
        naContextCreate          = dh(lib, "na_context_create",            FunctionDescriptor.of(ADDRESS));
        naContextDestroy         = dh(lib, "na_context_destroy",           FunctionDescriptor.ofVoid(ADDRESS));
        naEnumerate              = dh(lib, "na_enumerate",                 FunctionDescriptor.of(JAVA_INT, ADDRESS, ADDRESS, JAVA_INT));
        naClientCreate           = dh(lib, "na_client_create",            FunctionDescriptor.of(ADDRESS, JAVA_INT, ADDRESS, JAVA_INT, ADDRESS));
        naClientDestroy          = dh(lib, "na_client_destroy",           FunctionDescriptor.ofVoid(ADDRESS));
        naClientSetTransport     = dh(lib, "na_client_set_transport",     FunctionDescriptor.of(JAVA_INT, ADDRESS, JAVA_INT));
        naClientSetPlaybackDevice= dh(lib, "na_client_set_playback_device", FunctionDescriptor.of(JAVA_INT, ADDRESS, JAVA_INT));
        naClientSetAudioCb       = dh(lib, "na_client_set_audio_cb",      FunctionDescriptor.of(JAVA_INT, ADDRESS, ADDRESS, ADDRESS));
        naClientConnect          = dh(lib, "na_client_connect",           FunctionDescriptor.of(JAVA_INT, ADDRESS, ADDRESS, JAVA_INT));
        naClientDisconnect       = dh(lib, "na_client_disconnect",        FunctionDescriptor.ofVoid(ADDRESS));
        naClientIsConnected      = dh(lib, "na_client_is_connected",      FunctionDescriptor.of(JAVA_INT, ADDRESS));
    }

    static MethodHandle dh(SymbolLookup lib, String name, FunctionDescriptor fd) {
        MemorySegment sym = lib.find(name)
                .orElseThrow(() -> new RuntimeException("symbol not found in libnaudio: " + name));
        return LINKER.downcallHandle(sym, fd);
    }

    // ---- RX stats: written on the receive worker thread, read from main ----------------------
    // The hot-path audio callback fires on naudio's receive worker thread (one call per RX frame);
    // main polls these for a live meter. AtomicLong keeps the meter reads race-free. first_hex is
    // written by a single worker thread and read by main only after disconnect() has joined it.
    static final AtomicLong rxFrames  = new AtomicLong();
    static final AtomicLong rxBytes   = new AtomicLong();
    static final AtomicLong rxNonzero = new AtomicLong();   // a frame of underrun silence is all-zero
    static volatile String  firstHex  = null;               // first up-to-8 bytes of frame 1, hex

    // The hot-path RX PCM sink (na_audio_cb). Copy the frame once, then count + fingerprint it.
    // It MUST NOT throw back into native code, so the whole body is guarded.
    static void onRxAudio(MemorySegment pcm, long nBytes, MemorySegment user) {
        try {
            rxFrames.incrementAndGet();
            rxBytes.addAndGet(nBytes);
            if (nBytes <= 0) return;
            byte[] buf = pcm.reinterpret(nBytes).toArray(JAVA_BYTE);  // copy once (the segment is valid only now)
            long nz = 0;
            for (byte b : buf) if (b != 0) nz++;
            rxNonzero.addAndGet(nz);
            if (firstHex == null) {
                int k = (int) Math.min(8, nBytes);
                StringBuilder sb = new StringBuilder(k * 2);
                for (int i = 0; i < k; i++) sb.append(String.format("%02x", buf[i] & 0xff));
                firstHex = sb.toString();
            }
        } catch (Throwable ignored) {
            // Swallow: an exception crossing the native boundary would crash the JVM.
        }
    }

    // ---- Ctrl-C: flip a flag the monitor loop polls ------------------------------------------
    static volatile boolean stop = false;

    static void log(String s) {
        System.err.println(s);
        System.err.flush();
    }

    // Read a C string returned by the ABI (na_strerror). The returned pointer is a static string.
    static String cString(MemorySegment p) {
        if (p == null || p.address() == 0) return "";
        return p.reinterpret(Long.MAX_VALUE).getString(0);  // stops at the first NUL
    }

    static String strerror(int code) throws Throwable {
        MemorySegment p = (MemorySegment) naStrerror.invokeExact(code);
        return cString(p);
    }

    static int lastError() throws Throwable {
        return (int) naLastError.invokeExact();
    }

    // ---- library locating (mirrors the Python loader's search order) -------------------------
    static String[] libFilenames() {
        String os = System.getProperty("os.name", "").toLowerCase();
        if (os.contains("mac") || os.contains("darwin")) return new String[]{"libnaudio.dylib"};
        if (os.contains("win")) return new String[]{"naudio.dll", "libnaudio.dll"};
        return new String[]{"libnaudio.so"};
    }

    static SymbolLookup loadNaudio(String explicit, Arena arena) {
        List<String> candidates = new ArrayList<>();
        if (explicit != null) candidates.add(explicit);
        String env = System.getenv("NAUDIO_LIB");
        if (env != null && !env.isEmpty()) candidates.add(env);
        for (String dir : new String[]{"build", ".." + File.separator + ".." + File.separator + "build", "."})
            for (String fn : libFilenames())
                candidates.add(dir + File.separator + fn);

        List<String> tried = new ArrayList<>();
        for (String c : candidates) {
            try {
                return SymbolLookup.libraryLookup(c, arena);
            } catch (IllegalArgumentException e) {
                tried.add("  " + c + ": " + e.getMessage());
            }
        }
        // Last resort: bare names via the platform's default loader search (DYLD/LD path, system dirs).
        for (String fn : libFilenames()) {
            try {
                return SymbolLookup.libraryLookup(fn, arena);
            } catch (IllegalArgumentException e) {
                tried.add("  " + fn + " (loader): " + e.getMessage());
            }
        }
        log("could not load libnaudio. Build it first:\n"
                + "  cmake -S . -B build && cmake --build build -j\n"
                + "or pass --lib /path/to/libnaudio, or set NAUDIO_LIB.\nTried:\n"
                + String.join("\n", tried));
        System.exit(1);
        return null;  // unreachable
    }

    // ---- device listing / default-playback selection (SYSTEM backend) ------------------------
    // Print every playback-capable device (to stderr — stdout is reserved for the RESULT line).
    static int listDevices(Arena arena) throws Throwable {
        MemorySegment ctx = (MemorySegment) naContextCreate.invokeExact();
        if (ctx.address() == 0) {
            log("error: na_context_create: " + strerror(lastError()));
            return 1;
        }
        try {
            MemorySegment arr = arena.allocate(DEV_SIZE * MAX_DEVICES, NA_DEVICE.byteAlignment());
            int n = (int) naEnumerate.invokeExact(ctx, arr, MAX_DEVICES);
            if (n < 0) {
                log("error: na_enumerate: " + strerror(n));
                return 1;
            }
            log("Playback-capable devices (use the id with --playback-id):");
            int shown = 0;
            for (int i = 0; i < n; i++) {
                MemorySegment d = arr.asSlice((long) i * DEV_SIZE, DEV_SIZE);
                int cap = d.get(JAVA_INT, DEV_CAPABILITY);
                if (cap == NA_CAP_PLAYBACK || cap == NA_CAP_DUPLEX) {
                    log(String.format("  [%2d] %-40s %s%s",
                            d.get(JAVA_INT, DEV_PLAYBACK_ID), d.getString(DEV_NAME), d.getString(DEV_HOST_API),
                            d.get(JAVA_INT, DEV_IS_VIRTUAL) != 0 ? "  (virtual)" : ""));
                    shown++;
                }
            }
            if (shown == 0)
                log("  (none — use --backend null to run without a playback device)");
            return 0;
        } finally {
            naContextDestroy.invokeExact(ctx);
        }
    }

    // Return the first playback-capable device's id, or -1 if there is none.
    static int defaultPlaybackId(Arena arena) throws Throwable {
        MemorySegment ctx = (MemorySegment) naContextCreate.invokeExact();
        if (ctx.address() == 0) return -1;
        try {
            MemorySegment arr = arena.allocate(DEV_SIZE * MAX_DEVICES, NA_DEVICE.byteAlignment());
            int n = (int) naEnumerate.invokeExact(ctx, arr, MAX_DEVICES);
            for (int i = 0; i < n; i++) {
                MemorySegment d = arr.asSlice((long) i * DEV_SIZE, DEV_SIZE);
                int cap = d.get(JAVA_INT, DEV_CAPABILITY);
                if (cap == NA_CAP_PLAYBACK || cap == NA_CAP_DUPLEX)
                    return d.get(JAVA_INT, DEV_PLAYBACK_ID);
            }
            return -1;
        } finally {
            naContextDestroy.invokeExact(ctx);
        }
    }

    static void usage() {
        log("usage: java PlayToSpeakers.java [--host H] [--port N] [--name S] [--playback-id N]\n"
                + "                                [--transport tcp|udp] [--seconds N] [--backend system|null]\n"
                + "       java PlayToSpeakers.java --list-devices\n\n"
                + "  --host H          server host (default 127.0.0.1)\n"
                + "  --port N          server port (default 4533)\n"
                + "  --name S          this client's name in the server roster (default na-java-client)\n"
                + "  --playback-id N   output device id to play on (default: first output device)\n"
                + "  --transport T     tcp (default) | udp\n"
                + "  --seconds N       run time; 0 = until Ctrl-C (default 0)\n"
                + "  --backend B       system (default; plays RX to the output device) or\n"
                + "                    null (hardware-free; RX is received but not played)\n"
                + "  --list-devices    print output device ids, then exit\n"
                + "  --lib PATH        path to libnaudio (else $NAUDIO_LIB, build tree, system loader)\n"
                + "  -h, --help        print this message");
    }

    public static void main(String[] args) throws Throwable {
        String host        = "127.0.0.1";
        int    port        = 4533;            // AudioStreamConfig default audio port
        String name        = "na-java-client";
        int    backend     = NA_CLIENT_BACKEND_SYSTEM;  // default: play to speakers
        int    playbackId  = -1;              // -1 => auto-select the first output device (SYSTEM)
        int    transport   = NA_TRANSPORT_TCP;
        double seconds     = 0;               // 0 = until Ctrl-C
        boolean listOnly   = false;
        String libPath     = null;

        for (int i = 0; i < args.length; i++) {
            String a = args[i];
            switch (a) {
                case "--host"        -> host = need(args, ++i, "--host");
                case "--port"        -> port = parseInt(need(args, ++i, "--port"), "--port");
                case "--name"        -> name = need(args, ++i, "--name");
                case "--playback-id" -> playbackId = parseInt(need(args, ++i, "--playback-id"), "--playback-id");
                case "--seconds"     -> seconds = parseDouble(need(args, ++i, "--seconds"));
                case "--lib"         -> libPath = need(args, ++i, "--lib");
                case "--list-devices"-> listOnly = true;
                case "--backend" -> {
                    String b = need(args, ++i, "--backend");
                    if (b.equals("system")) backend = NA_CLIENT_BACKEND_SYSTEM;
                    else if (b.equals("null")) backend = NA_CLIENT_BACKEND_NULL;
                    else { log("error: invalid --backend '" + b + "' (system|null)"); System.exit(2); }
                }
                case "--transport" -> {
                    String t = need(args, ++i, "--transport");
                    if (t.equals("tcp")) transport = NA_TRANSPORT_TCP;
                    else if (t.equals("udp")) transport = NA_TRANSPORT_UDP;
                    else { log("error: invalid --transport '" + t + "' (tcp|udp)"); System.exit(2); }
                }
                case "-h", "--help" -> { usage(); System.exit(0); }
                default -> { log("unknown option: " + a); usage(); System.exit(2); }
            }
        }

        // One shared Arena backs every native allocation AND the RX upcall stub. It must outlive
        // the worker thread that fires the callback, so we keep it open until after the client is
        // destroyed (no callback can be in flight once na_client_destroy returns).
        Arena arena = Arena.ofShared();
        int exitCode = 0;
        Thread hook = null;
        try {
            SymbolLookup lib = loadNaudio(libPath, arena);
            bind(lib);

            if (listOnly) {
                exitCode = listDevices(arena);
                arena.close();
                System.exit(exitCode);
            }

            // Resolve the playback device up front so we can fail early with guidance. The SYSTEM
            // backend needs a real output device; the NULL backend accepts any id (it plays nothing).
            if (backend == NA_CLIENT_BACKEND_SYSTEM && playbackId < 0) {
                playbackId = defaultPlaybackId(arena);
                if (playbackId < 0) {
                    log("error: no output device found. Run --list-devices, pass --playback-id N, "
                            + "or use --backend null to run without one.");
                    arena.close();
                    System.exit(1);
                }
                log("[client] auto-selected output device id=" + playbackId
                        + " (run --list-devices to choose another)");
            } else if (playbackId < 0) {
                playbackId = 0;  // NULL backend: any id is accepted
            }

            MemorySegment client = (MemorySegment) naClientCreate.invokeExact(
                    backend, arena.allocateFrom(host), port, arena.allocateFrom(name));
            if (client.address() == 0) {
                log("error: na_client_create: " + strerror(lastError()));
                arena.close();
                System.exit(1);
            }

            // The RX upcall (na_audio_cb). The stub lives in `arena`, kept open for the client's life.
            MethodHandle rxTarget = MethodHandles.lookup().findStatic(PlayToSpeakers.class, "onRxAudio",
                    MethodType.methodType(void.class, MemorySegment.class, long.class, MemorySegment.class));
            MemorySegment rxStub = LINKER.upcallStub(rxTarget,
                    FunctionDescriptor.ofVoid(ADDRESS, JAVA_LONG, ADDRESS), arena);

            // Configure BEFORE connect — the worker threads read callbacks/config once streaming starts.
            // RX-only: we register just the audio sink (na_client_set_audio_cb) and skip the
            // na_client_callbacks event struct — the single function pointer is all the demo needs.
            int rc;
            rc = (int) naClientSetAudioCb.invokeExact(client, rxStub, MemorySegment.NULL);
            rc = (int) naClientSetTransport.invokeExact(client, transport);
            rc = (int) naClientSetPlaybackDevice.invokeExact(client, playbackId);  // REQUIRED for RX

            String backendName   = backend == NA_CLIENT_BACKEND_SYSTEM ? "system" : "null";
            String transportName = transport == NA_TRANSPORT_TCP ? "tcp" : "udp";

            MemorySegment errbuf = arena.allocate(256);
            rc = (int) naClientConnect.invokeExact(client, errbuf, 256);
            if (rc != NA_OK) {
                log("error: connect to " + host + ":" + port + " failed ("
                        + strerror(rc) + "): " + errbuf.getString(0));
                naClientDestroy.invokeExact(client);
                arena.close();
                System.exit(1);
            }
            log("connected to " + host + ":" + port + " (backend=" + backendName + ", transport="
                    + transportName + "); " + (backend == NA_CLIENT_BACKEND_SYSTEM ? "playing" : "receiving")
                    + (seconds > 0 ? " for " + trimNum(seconds) + "s" : " until Ctrl-C") + " ...");

            // Graceful Ctrl-C: flip the stop flag, then wait (briefly) for main to print its summary.
            final Thread mainThread = Thread.currentThread();
            hook = new Thread(() -> {
                stop = true;
                try { mainThread.join(2000); } catch (InterruptedException ignored) { }
            }, "naudio-shutdown");
            Runtime.getRuntime().addShutdownHook(hook);

            // Monitor loop: a per-second throughput meter so you can see audio arriving.
            final long start    = System.nanoTime();
            final long deadline = seconds > 0 ? start + (long) (seconds * 1_000_000_000.0) : 0;
            long prevBytes = 0, prevNs = start;
            while (!stop && (deadline == 0 || System.nanoTime() < deadline)) {
                Thread.sleep(200);
                long t = System.nanoTime();
                if (t - prevNs < 1_000_000_000L) continue;
                long b   = rxBytes.get();
                long bps = (b - prevBytes) * 1_000_000_000L / (t - prevNs);
                int  conn = (int) naClientIsConnected.invokeExact(client);
                log(String.format("  t=%2ds  rx=%-9d B  frames=%-7d  %d B/s  conn=%d",
                        (t - start) / 1_000_000_000L, b, rxFrames.get(), bps, conn));
                prevBytes = b;
                prevNs = t;
            }

            int connectedEnd = (int) naClientIsConnected.invokeExact(client);
            naClientDisconnect.invokeExact(client);  // stop reconnection + join workers; no callback after this

            long elapsedNs  = System.nanoTime() - start;
            long frames     = rxFrames.get();
            long bytes      = rxBytes.get();
            long nonzero    = rxNonzero.get();
            long avgBps     = elapsedNs > 0 ? bytes * 1_000_000_000L / elapsedNs : 0;
            String fhex     = firstHex != null ? firstHex : "";

            // PASS = received real (non-silent) audio. A bounded run (--seconds) that got nothing FAILS
            // (exit 1); an interactive (Ctrl-C) run always exits 0 — you chose when to stop.
            boolean gate = seconds > 0;
            boolean pass = frames > 0 && nonzero > 0;

            log("\n=== play to speakers — summary ===");
            log("  server         : " + host + ":" + port + " (" + transportName + ")");
            log("  rx             : " + frames + " frames, " + bytes + " bytes (" + nonzero + " non-zero)");
            log("  throughput     : avg " + avgBps + " B/s over " + (elapsedNs / 1_000_000L) + " ms");
            log("  first_frame_hex: " + (fhex.isEmpty() ? "(none)" : fhex));
            log("  connected      : " + (connectedEnd != 0 ? "yes" : "no"));

            // Machine-readable result on STDOUT (handy for a scripted check).
            System.out.println("RESULT status=" + (pass ? "PASS" : "FAIL")
                    + " connected=" + (connectedEnd != 0 ? "true" : "false")
                    + " rx_frames=" + frames + " rx_bytes=" + bytes + " nonzero_bytes=" + nonzero
                    + " first_frame_hex=" + fhex + " avg_bps=" + avgBps);
            System.out.flush();

            naClientDestroy.invokeExact(client);
            exitCode = (gate && !pass) ? 1 : 0;
        } finally {
            arena.close();  // frees the upcall stub + every allocation; safe now that the client is destroyed
        }
        exit(exitCode, hook);
    }

    // Set the process exit code, cooperating with a Ctrl-C-initiated shutdown if one is in progress.
    static void exit(int code, Thread shutdownHook) {
        if (shutdownHook != null) {
            try {
                Runtime.getRuntime().removeShutdownHook(shutdownHook);
            } catch (IllegalStateException shuttingDown) {
                // Ctrl-C already started shutdown; the hook ran and our summary printed.
                // Let the JVM finish exiting (the process code reflects the signal).
                return;
            }
        }
        System.exit(code);
    }

    // ---- tiny arg helpers --------------------------------------------------------------------
    static String need(String[] args, int i, String opt) {
        if (i >= args.length) { log("error: " + opt + " needs a value"); System.exit(2); }
        return args[i];
    }

    static int parseInt(String s, String opt) {
        try { return Integer.parseInt(s); }
        catch (NumberFormatException e) { log("error: " + opt + " needs a number, got '" + s + "'"); System.exit(2); return 0; }
    }

    static double parseDouble(String s) {
        try { return Double.parseDouble(s); }
        catch (NumberFormatException e) { log("error: --seconds needs a number, got '" + s + "'"); System.exit(2); return 0; }
    }

    static String trimNum(double d) {
        return d == Math.rint(d) ? Long.toString((long) d) : Double.toString(d);
    }
}
