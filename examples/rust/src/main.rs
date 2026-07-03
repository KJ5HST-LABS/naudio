// SPDX-License-Identifier: LGPL-2.1-or-later
//
// naudio examples — play a network audio stream to your speakers (Rust).
//
// Copyright (C) 2025-2026 Terrell Deppe
//
// A dead-simple client: connect to a network-audio server and play whatever it
// is streaming on your local speakers. It drives the public `naudio.h` C ABI over
// hand-written `extern "C"` declarations — and only that C ABI — so it is also a
// worked example of calling a C library from Rust with no `bindgen` and no other
// third-party crate. The bindings are just the dozen `na_*` functions this client
// uses (see the `extern "C"` block below).
//
// The playback is done by naudio itself: you pick a local output device and the
// library renders the received audio to it. By default the client picks your first
// output device and starts playing — no flags required:
//
//     cargo run -- --host 127.0.0.1 --port 4533
//
// Pair it with the demo source to hear something without a radio or a second
// machine (see examples/server):
//
//     na_audio_source --test-tone --port 4533      # one terminal: a test tone
//     cargo run -- --port 4533                      # another — you hear the tone
//
// Use `--list-devices` to see the output-device ids, then `--playback-id N` to
// choose one. `--backend null` runs hardware-free (the audio is still received,
// it just is not played) — handy on a headless box, in CI, or to verify a server
// end to end without a sound card.
//
// THREADING (from naudio.h): the RX audio callback fires on naudio's receive
// WORKER thread, once per frame. Keep it SHORT and non-blocking, do not call client
// lifecycle methods from it, and — the Rust-specific part — never let a panic unwind
// across the `extern "C"` boundary (we wrap the body in catch_unwind). The user
// pointer we hand to the callback (an `&RxStats` of atomics) must outlive the client,
// so it lives on main's stack until after na_client_destroy.
//
// Locating libnaudio: this client LINKS libnaudio at build time (it is not a runtime
// loader like the Python/Java examples), so there is no `--lib` flag. build.rs finds
// the library via $NAUDIO_LIB or the sibling build tree (../../build) and bakes an
// rpath so `cargo run` finds it at runtime; see build.rs and the README.
//
// Output: the machine-readable `RESULT ...` line goes to STDOUT; all human logs
// (events, the throughput meter, the summary) go to STDERR.

use std::ffi::{c_char, c_int, c_void, CStr, CString};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::thread;
use std::time::{Duration, Instant};

// ---- C ABI constants (mirror include/naudio.h) -------------------------------------------
const NA_OK: c_int = 0;
const NA_CLIENT_BACKEND_SYSTEM: c_int = 0;
const NA_CLIENT_BACKEND_NULL: c_int = 1;
const NA_TRANSPORT_TCP: c_int = 0;
const NA_TRANSPORT_UDP: c_int = 1;
const NA_CAP_PLAYBACK: c_int = 1;
const NA_CAP_DUPLEX: c_int = 2;
const MAX_DEVICES: usize = 128;

// POSIX signals (macOS/Linux) — used to catch Ctrl-C without a third-party crate.
const SIGINT: c_int = 2;
const SIGTERM: c_int = 15;

// Opaque ABI handles. We never look inside them — only pass the pointers back to the ABI.
enum NaContext {}
enum NaClient {}

// na_device, byte-for-byte as declared in naudio.h. `#[repr(C)]` fixes the layout; the
// char arrays make it 408 bytes on every platform (3*4 + 256 + 128 + 3*4). Several fields
// are unread here but must be present so the struct's size/offsets match the C side.
#[repr(C)]
#[allow(dead_code)]
struct NaDevice {
    backend_id: c_int,
    capture_backend_id: c_int,
    playback_backend_id: c_int,
    name: [c_char; 256],
    host_api: [c_char; 128],
    type_: c_int,
    capability: c_int,
    is_virtual: c_int,
}

// Hot-path RX PCM callback: void(const unsigned char* pcm, size_t n_bytes, void* user).
// `size_t` maps to Rust's `usize` — an exact match on every platform (no 64-bit assumption,
// unlike a fixed-width binding).
type NaAudioCb = extern "C" fn(pcm: *const u8, n_bytes: usize, user: *mut c_void);

// The dozen na_* functions this client uses — hand-written, no bindgen. RX-only: we bind
// na_client_set_audio_cb (the single PCM sink) and skip na_client_set_callbacks / the
// na_client_callbacks event struct entirely.
extern "C" {
    fn na_strerror(err: c_int) -> *const c_char;
    fn na_last_error() -> c_int;

    fn na_context_create() -> *mut NaContext;
    fn na_context_destroy(ctx: *mut NaContext);
    fn na_enumerate(ctx: *mut NaContext, out: *mut NaDevice, max: c_int) -> c_int;

    fn na_client_create(
        backend: c_int,
        host: *const c_char,
        port: c_int,
        name: *const c_char,
    ) -> *mut NaClient;
    fn na_client_destroy(client: *mut NaClient);
    fn na_client_set_transport(client: *mut NaClient, transport: c_int) -> c_int;
    fn na_client_set_playback_device(client: *mut NaClient, backend_id: c_int) -> c_int;
    fn na_client_set_audio_cb(client: *mut NaClient, cb: NaAudioCb, user: *mut c_void) -> c_int;
    fn na_client_connect(client: *mut NaClient, errbuf: *mut c_char, errlen: c_int) -> c_int;
    fn na_client_disconnect(client: *mut NaClient);
    fn na_client_is_connected(client: *mut NaClient) -> c_int;

    // POSIX signal(2). The return value is the previous handler (or SIG_ERR); we discard it.
    fn signal(signum: c_int, handler: extern "C" fn(c_int)) -> *mut c_void;
}

// ---- RX stats: written on the receive worker thread, read from main ----------------------
// The hot-path audio callback fires on naudio's receive worker thread (one call per RX frame);
// main polls these for a live meter. The fields are atomic so the meter reads them without a
// data race. Only ONE worker thread writes them, and main's final read happens after
// na_client_disconnect() has joined that thread.
#[derive(Default)]
struct RxStats {
    frames: AtomicU64,
    bytes: AtomicU64,
    nonzero: AtomicU64, // a frame of underrun silence is all-zero; this proves content arrived
    have_first: AtomicBool,
    first_bytes: AtomicU64, // first 8 bytes of frame 1, packed little-endian (a wire fingerprint)
}

// The hot-path RX PCM sink (na_audio_cb). `user` is the &RxStats passed at registration.
// It MUST NOT let a panic unwind back into native code, so the whole body is caught.
extern "C" fn on_rx_audio(pcm: *const u8, n: usize, user: *mut c_void) {
    let _ = std::panic::catch_unwind(|| unsafe {
        if user.is_null() {
            return;
        }
        let s = &*(user as *const RxStats);
        s.frames.fetch_add(1, Ordering::Relaxed);
        s.bytes.fetch_add(n as u64, Ordering::Relaxed);
        if n == 0 || pcm.is_null() {
            return;
        }
        let buf = std::slice::from_raw_parts(pcm, n); // valid only for this call
        let nz = buf.iter().filter(|&&b| b != 0).count() as u64;
        s.nonzero.fetch_add(nz, Ordering::Relaxed);
        if n >= 8 && !s.have_first.load(Ordering::Relaxed) {
            let mut v = 0u64;
            for (i, &b) in buf[..8].iter().enumerate() {
                v |= (b as u64) << (8 * i);
            }
            s.first_bytes.store(v, Ordering::Relaxed);
            s.have_first.store(true, Ordering::Release);
        }
    });
}

// Reconstruct the first-frame fingerprint as lowercase hex (or "" if no frame arrived).
fn first_hex(stats: &RxStats) -> String {
    if !stats.have_first.load(Ordering::Acquire) {
        return String::new();
    }
    let v = stats.first_bytes.load(Ordering::Acquire);
    let mut s = String::with_capacity(16);
    for i in 0..8 {
        s.push_str(&format!("{:02x}", (v >> (8 * i)) & 0xff));
    }
    s
}

// ---- Ctrl-C: flip a flag the monitor loop polls (na_client_disconnect joins workers) -----
static STOP: AtomicBool = AtomicBool::new(false);
extern "C" fn on_signal(_sig: c_int) {
    STOP.store(true, Ordering::SeqCst);
}

fn log(msg: &str) {
    eprintln!("{msg}");
}

// Read a C string returned by the ABI (na_strerror returns a static string).
unsafe fn strerror(code: c_int) -> String {
    let p = na_strerror(code);
    if p.is_null() {
        String::new()
    } else {
        CStr::from_ptr(p).to_string_lossy().into_owned()
    }
}

// Read a NUL-terminated C string out of a fixed device-struct char array.
fn c_array_str(buf: &[c_char]) -> String {
    unsafe { CStr::from_ptr(buf.as_ptr()).to_string_lossy().into_owned() }
}

// ---- device listing / default-playback selection (SYSTEM backend) ------------------------

// Enumerate devices into a Vec. Returns Err(message) on failure.
fn enumerate() -> Result<Vec<NaDevice>, String> {
    unsafe {
        let ctx = na_context_create();
        if ctx.is_null() {
            return Err(format!("na_context_create: {}", strerror(na_last_error())));
        }
        // NaDevice is plain old data (ints + char arrays), so zeroed is a valid value.
        let mut devs: Vec<NaDevice> = (0..MAX_DEVICES).map(|_| std::mem::zeroed()).collect();
        let n = na_enumerate(ctx, devs.as_mut_ptr(), MAX_DEVICES as c_int);
        na_context_destroy(ctx);
        if n < 0 {
            return Err(format!("na_enumerate: {}", strerror(n)));
        }
        devs.truncate(n as usize);
        Ok(devs)
    }
}

// Print every playback-capable device (to stderr — stdout is reserved for the RESULT line).
fn list_devices() -> i32 {
    let devs = match enumerate() {
        Ok(d) => d,
        Err(e) => {
            log(&format!("error: {e}"));
            return 1;
        }
    };
    log("Playback-capable devices (use the id with --playback-id):");
    let mut shown = 0;
    for d in &devs {
        if d.capability == NA_CAP_PLAYBACK || d.capability == NA_CAP_DUPLEX {
            log(&format!(
                "  [{:2}] {:<40} {}{}",
                d.playback_backend_id,
                c_array_str(&d.name),
                c_array_str(&d.host_api),
                if d.is_virtual != 0 { "  (virtual)" } else { "" }
            ));
            shown += 1;
        }
    }
    if shown == 0 {
        log("  (none — use --backend null to run without a playback device)");
    }
    0
}

// Return the first playback-capable device's id, or -1 if there is none.
fn default_playback_id() -> i32 {
    match enumerate() {
        Ok(devs) => devs
            .iter()
            .find(|d| d.capability == NA_CAP_PLAYBACK || d.capability == NA_CAP_DUPLEX)
            .map(|d| d.playback_backend_id)
            .unwrap_or(-1),
        Err(_) => -1,
    }
}

fn usage() {
    log(
        "usage: play_to_speakers [--host H] [--port N] [--name S] [--playback-id N]\n\
         \x20                       [--transport tcp|udp] [--seconds N] [--backend system|null]\n\
         \x20      play_to_speakers --list-devices\n\n\
         \x20 --host H          server host (default 127.0.0.1)\n\
         \x20 --port N          server port (default 4533)\n\
         \x20 --name S          this client's name in the server roster (default na-rust-client)\n\
         \x20 --playback-id N   output device id to play on (default: first output device)\n\
         \x20 --transport T     tcp (default) | udp\n\
         \x20 --seconds N       run time; 0 = until Ctrl-C (default 0)\n\
         \x20 --backend B       system (default; plays RX to the output device) or\n\
         \x20                   null (hardware-free; RX is received but not played)\n\
         \x20 --list-devices    print output device ids, then exit\n\
         \x20 -h, --help        print this message",
    );
}

// Pull the value that follows an option, or exit(2) if it is missing.
fn need(args: &[String], i: &mut usize, opt: &str) -> String {
    *i += 1;
    if *i >= args.len() {
        log(&format!("error: {opt} needs a value"));
        std::process::exit(2);
    }
    args[*i].clone()
}

fn parse_int(s: &str, opt: &str) -> i32 {
    s.parse().unwrap_or_else(|_| {
        log(&format!("error: {opt} needs a number, got '{s}'"));
        std::process::exit(2);
    })
}

fn main() {
    std::process::exit(run());
}

fn run() -> i32 {
    let args: Vec<String> = std::env::args().skip(1).collect();

    let mut host = String::from("127.0.0.1");
    let mut port: i32 = 4533; // AudioStreamConfig default audio port
    let mut name = String::from("na-rust-client");
    let mut backend = NA_CLIENT_BACKEND_SYSTEM; // default: play to speakers
    let mut playback_id: i32 = -1; // -1 => auto-select the first output device (SYSTEM)
    let mut transport = NA_TRANSPORT_TCP;
    let mut seconds: f64 = 0.0; // 0 = until Ctrl-C

    let mut i = 0;
    while i < args.len() {
        let a = args[i].as_str();
        match a {
            "--host" => host = need(&args, &mut i, "--host"),
            "--port" => port = parse_int(&need(&args, &mut i, "--port"), "--port"),
            "--name" => name = need(&args, &mut i, "--name"),
            "--playback-id" => {
                playback_id = parse_int(&need(&args, &mut i, "--playback-id"), "--playback-id")
            }
            "--seconds" => {
                let v = need(&args, &mut i, "--seconds");
                seconds = v.parse().unwrap_or_else(|_| {
                    log(&format!("error: --seconds needs a number, got '{v}'"));
                    std::process::exit(2);
                });
            }
            "--list-devices" => return list_devices(),
            "--backend" => {
                let b = need(&args, &mut i, "--backend");
                backend = match b.as_str() {
                    "system" => NA_CLIENT_BACKEND_SYSTEM,
                    "null" => NA_CLIENT_BACKEND_NULL,
                    _ => {
                        log(&format!("error: invalid --backend '{b}' (system|null)"));
                        return 2;
                    }
                };
            }
            "--transport" => {
                let t = need(&args, &mut i, "--transport");
                transport = match t.as_str() {
                    "tcp" => NA_TRANSPORT_TCP,
                    "udp" => NA_TRANSPORT_UDP,
                    _ => {
                        log(&format!("error: invalid --transport '{t}' (tcp|udp)"));
                        return 2;
                    }
                };
            }
            "-h" | "--help" => {
                usage();
                return 0;
            }
            _ => {
                log(&format!("unknown option: {a}"));
                usage();
                return 2;
            }
        }
        i += 1;
    }

    // Resolve the playback device up front so we can fail early with guidance. The SYSTEM
    // backend needs a real output device; the NULL backend accepts any id (it plays nothing).
    if backend == NA_CLIENT_BACKEND_SYSTEM && playback_id < 0 {
        playback_id = default_playback_id();
        if playback_id < 0 {
            log("error: no output device found. Run --list-devices, pass --playback-id N, \
                 or use --backend null to run without one.");
            return 1;
        }
        log(&format!(
            "[client] auto-selected output device id={playback_id} \
             (run --list-devices to choose another)"
        ));
    } else if playback_id < 0 {
        playback_id = 0; // NULL backend: any id is accepted
    }

    unsafe {
        signal(SIGINT, on_signal);
        signal(SIGTERM, on_signal);
    }

    // The RX stats live on main's stack and outlive the client (we don't return until after
    // na_client_destroy), so the worker thread's &RxStats stays valid the whole time.
    let stats = RxStats::default();

    let host_c = CString::new(host.clone()).unwrap();
    let name_c = CString::new(name.clone()).unwrap();

    let client = unsafe { na_client_create(backend, host_c.as_ptr(), port, name_c.as_ptr()) };
    if client.is_null() {
        log(&format!("error: na_client_create: {}", unsafe {
            strerror(na_last_error())
        }));
        return 1;
    }

    // Configure BEFORE connect — the worker threads read callbacks/config once streaming starts.
    // RX-only: register just the audio sink and skip the na_client_callbacks event struct.
    unsafe {
        na_client_set_audio_cb(client, on_rx_audio, &stats as *const RxStats as *mut c_void);
        na_client_set_transport(client, transport);
        na_client_set_playback_device(client, playback_id); // REQUIRED for RX
    }

    let backend_name = if backend == NA_CLIENT_BACKEND_SYSTEM { "system" } else { "null" };
    let transport_name = if transport == NA_TRANSPORT_TCP { "tcp" } else { "udp" };

    let mut errbuf = [0 as c_char; 256];
    let rc = unsafe { na_client_connect(client, errbuf.as_mut_ptr(), errbuf.len() as c_int) };
    if rc != NA_OK {
        let detail = c_array_str(&errbuf);
        log(&format!(
            "error: connect to {host}:{port} failed ({}): {detail}",
            unsafe { strerror(rc) }
        ));
        unsafe { na_client_destroy(client) };
        return 1;
    }
    let verb = if backend == NA_CLIENT_BACKEND_SYSTEM { "playing" } else { "receiving" };
    let span = if seconds > 0.0 {
        format!("for {}s", TrimNum(seconds))
    } else {
        "until Ctrl-C".to_string()
    };
    log(&format!(
        "connected to {host}:{port} (backend={backend_name}, transport={transport_name}); {verb} {span} ..."
    ));

    // Monitor loop: a per-second throughput meter so you can see audio arriving.
    let start = Instant::now();
    let deadline = if seconds > 0.0 {
        Some(start + Duration::from_secs_f64(seconds))
    } else {
        None
    };
    let mut prev_bytes = 0u64;
    let mut prev = start;
    while !STOP.load(Ordering::Relaxed) && deadline.map_or(true, |d| Instant::now() < d) {
        thread::sleep(Duration::from_millis(200));
        let t = Instant::now();
        if t.duration_since(prev) < Duration::from_secs(1) {
            continue;
        }
        let b = stats.bytes.load(Ordering::Relaxed);
        let dt = t.duration_since(prev).as_secs_f64();
        let bps = if dt > 0.0 { ((b - prev_bytes) as f64 / dt) as u64 } else { 0 };
        let conn = unsafe { na_client_is_connected(client) };
        log(&format!(
            "  t={:2}s  rx={:<9} B  frames={:<7}  {bps} B/s  conn={conn}",
            start.elapsed().as_secs(),
            b,
            stats.frames.load(Ordering::Relaxed)
        ));
        prev_bytes = b;
        prev = t;
    }

    let connected_end = unsafe { na_client_is_connected(client) };
    unsafe { na_client_disconnect(client) }; // stop reconnection + join workers; no callback after this

    let elapsed = start.elapsed();
    let rx_frames = stats.frames.load(Ordering::Acquire);
    let rx_bytes = stats.bytes.load(Ordering::Acquire);
    let rx_nonzero = stats.nonzero.load(Ordering::Acquire);
    let elapsed_ms = elapsed.as_millis() as u64;
    let avg_bps = if elapsed_ms > 0 { rx_bytes * 1000 / elapsed_ms } else { 0 };
    let fhex = first_hex(&stats);

    // PASS = received real (non-silent) audio. A bounded run (--seconds) that got nothing FAILS
    // (exit 1); an interactive (Ctrl-C) run always exits 0 — you chose when to stop.
    let gate = seconds > 0.0;
    let pass = rx_frames > 0 && rx_nonzero > 0;

    log(&format!(
        "\n=== play to speakers — summary ===\n\
         \x20 server         : {host}:{port} ({transport_name})\n\
         \x20 rx             : {rx_frames} frames, {rx_bytes} bytes ({rx_nonzero} non-zero)\n\
         \x20 throughput     : avg {avg_bps} B/s over {elapsed_ms} ms\n\
         \x20 first_frame_hex: {}\n\
         \x20 connected      : {}",
        if fhex.is_empty() { "(none)" } else { &fhex },
        if connected_end != 0 { "yes" } else { "no" }
    ));

    // Machine-readable result on STDOUT (handy for a scripted check).
    println!(
        "RESULT status={} connected={} rx_frames={rx_frames} rx_bytes={rx_bytes} \
         nonzero_bytes={rx_nonzero} first_frame_hex={fhex} avg_bps={avg_bps}",
        if pass { "PASS" } else { "FAIL" },
        if connected_end != 0 { "true" } else { "false" }
    );

    unsafe { na_client_destroy(client) };
    if gate && !pass {
        1
    } else {
        0
    }
}

// Format seconds without a trailing ".0" for whole numbers (matches the C/Java "for Ns" log).
struct TrimNum(f64);
impl std::fmt::Display for TrimNum {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        if self.0 == self.0.trunc() {
            write!(f, "{}", self.0 as i64)
        } else {
            write!(f, "{}", self.0)
        }
    }
}
