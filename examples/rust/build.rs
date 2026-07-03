// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Build script: locate and link the shared `libnaudio`, and bake an rpath so the
// example finds the library at runtime.
//
// Search order (mirrors the Python/Java loaders' build-time half):
//   1. $NAUDIO_LIB — a path to the library file OR the directory that holds it.
//   2. the sibling build tree, relative to this crate: ../../build, then ./build.
//
// Why an rpath, not just a link path: the dylib's install name is
// `@rpath/libnaudio.0.dylib` on macOS (SONAME `libnaudio.so.0` on Linux), so the
// dynamic loader resolves it against the executable's rpath at runtime. We emit an
// rpath pointing at the directory that holds the library, so `cargo run` Just Works
// from the build tree without setting DYLD_LIBRARY_PATH / LD_LIBRARY_PATH.

use std::env;
use std::path::{Path, PathBuf};

const LIB_NAMES: &[&str] = &["libnaudio.dylib", "libnaudio.so", "naudio.dll", "libnaudio.dll"];

fn has_naudio(dir: &Path) -> bool {
    LIB_NAMES.iter().any(|n| dir.join(n).exists())
}

// Resolve the directory that contains libnaudio.
fn lib_dir() -> PathBuf {
    // 1. $NAUDIO_LIB (a file path -> its parent dir, or a directory as-is).
    if let Ok(p) = env::var("NAUDIO_LIB") {
        let path = PathBuf::from(&p);
        if path.is_file() {
            if let Some(parent) = path.parent() {
                return parent.to_path_buf();
            }
        } else if path.is_dir() {
            return path;
        }
    }
    // 2. the sibling build tree, relative to this crate (examples/rust/).
    let manifest = PathBuf::from(env::var("CARGO_MANIFEST_DIR").expect("CARGO_MANIFEST_DIR"));
    for rel in ["../../build", "build"] {
        let cand = manifest.join(rel);
        if has_naudio(&cand) {
            return cand;
        }
    }
    // 3. Fall back to the canonical sibling build tree even if it is not built yet,
    //    so the (expected) "library not found" link error points somewhere sensible.
    manifest.join("../../build")
}

fn main() {
    println!("cargo:rerun-if-env-changed=NAUDIO_LIB");
    println!("cargo:rerun-if-changed=build.rs");

    let dir = lib_dir();
    let dir = dir.canonicalize().unwrap_or(dir); // absolute path makes the rpath robust
    let display = dir.display();

    if !has_naudio(&dir) {
        println!(
            "cargo:warning=libnaudio not found in {display} — build it first \
             (cmake -S . -B build && cmake --build build -j) or set NAUDIO_LIB"
        );
    }

    // Link against the shared libnaudio.
    println!("cargo:rustc-link-search=native={display}");
    println!("cargo:rustc-link-lib=dylib=naudio");

    // Bake an rpath to the build tree so the @rpath/SONAME install name resolves at runtime.
    println!("cargo:rustc-link-arg=-Wl,-rpath,{display}");
}
