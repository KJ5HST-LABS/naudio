// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Copyright (C) 2025-2026 Terrell Deppe
//
// Rust arm of the FFI example-client gate (tests/ffi-clients/run.sh).
//
// This probe declares NOTHING of its own. It `include!`s examples/rust/src/main.rs
// INSIDE a module, so the shipped client's `NaDevice` and its `extern "C"` block
// become items of that module and this file -- a sibling item in the same module --
// can read them despite their being private. What gets compared against
// na_device_truth.c is therefore the example itself, not a restatement of it that
// could drift in the same direction.
//
// Including inside `mod client` rather than at the crate root is what makes it a
// plain binary: the example's own `fn main` lands at `client::main`, unused, and
// leaves the crate root's `fn main` free to print the manifest to a clean stdout.
// (A `cargo test` would have worked too, but its harness writes to stdout and this
// output is diffed verbatim.)
//
// Emits the same key=value manifest na_device_truth.c does, and nothing else.
// na_client_callbacks is absent on purpose: this client is RX-only and never
// declares it, so run.sh compares Rust against the truth with that section removed.

#[allow(dead_code, unused_imports, unused_unsafe)]
mod client {
    include!(concat!(env!("CARGO_MANIFEST_DIR"), "/../../../examples/rust/src/main.rs"));

    /// Print the example's own declared layout, then the devices its own
    /// `na_enumerate` binding writes.
    pub fn probe() {
        // ---- 1. the client's own declared layout --------------------------------
        println!("na_device.sizeof={}", std::mem::size_of::<NaDevice>());
        println!("na_device.alignof={}", std::mem::align_of::<NaDevice>());
        println!("na_device.offset.backend_id={}", std::mem::offset_of!(NaDevice, backend_id));
        println!("na_device.offset.capture_backend_id={}", std::mem::offset_of!(NaDevice, capture_backend_id));
        println!("na_device.offset.playback_backend_id={}", std::mem::offset_of!(NaDevice, playback_backend_id));
        println!("na_device.offset.name={}", std::mem::offset_of!(NaDevice, name));
        println!("na_device.offset.host_api={}", std::mem::offset_of!(NaDevice, host_api));
        // `type` is a Rust keyword, so the example names the field `type_`. The KEY
        // must stay `type` -- it is the C field name, and the manifest is diffed
        // against C.
        println!("na_device.offset.type={}", std::mem::offset_of!(NaDevice, type_));
        println!("na_device.offset.capability={}", std::mem::offset_of!(NaDevice, capability));
        println!("na_device.offset.is_virtual={}", std::mem::offset_of!(NaDevice, is_virtual));
        // The array extents. Redundant with the offsets -- either array changing size
        // moves a later offset -- but they name the wrong field directly instead of
        // reporting it as a shift in everything that follows.
        //
        // Taken via size_of_val on a real value, NOT as size_of::<[c_char; 256]>():
        // spelling the length here would report this file's opinion of the extent
        // instead of the client's, which is the one thing a probe must never do.
        // `zeroed` is sound for this struct -- all-zero is a valid bit pattern for
        // c_int and c_char arrays alike.
        let d: NaDevice = unsafe { std::mem::zeroed() };
        println!("na_device.fieldsizeof.name={}", std::mem::size_of_val(&d.name));
        println!("na_device.fieldsizeof.host_api={}", std::mem::size_of_val(&d.host_api));

        // ---- 2. the devices na_enumerate writes ---------------------------------
        unsafe {
            let ctx = na_context_create();
            if ctx.is_null() {
                eprintln!("na_ffi_probe: na_context_create failed");
                std::process::exit(1);
            }
            let mut devs: Vec<NaDevice> = Vec::with_capacity(MAX_DEVICES);
            let n = na_enumerate(ctx, devs.as_mut_ptr(), MAX_DEVICES as c_int,
                                 std::mem::size_of::<NaDevice>());
            if n < 0 {
                eprintln!("na_ffi_probe: na_enumerate failed: {n}");
                na_context_destroy(ctx);
                std::process::exit(1);
            }
            devs.set_len(n as usize);

            println!("device.count={n}");
            for (i, d) in devs.iter().enumerate() {
                println!("device.{i}.backend_id={}", d.backend_id);
                println!("device.{i}.capture_backend_id={}", d.capture_backend_id);
                println!("device.{i}.playback_backend_id={}", d.playback_backend_id);
                println!("device.{i}.type={}", d.type_);
                println!("device.{i}.capability={}", d.capability);
                println!("device.{i}.is_virtual={}", d.is_virtual);
                println!("device.{i}.name={}", CStr::from_ptr(d.name.as_ptr()).to_string_lossy());
                println!("device.{i}.host_api={}", CStr::from_ptr(d.host_api.as_ptr()).to_string_lossy());
            }
            na_context_destroy(ctx);
        }
    }
}

fn main() {
    client::probe();
}
