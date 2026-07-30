// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Copyright (C) 2025-2026 Terrell Deppe
//
// Link the probe against the same shared libnaudio the example client links, and
// bake an rpath so it resolves at run time. Deliberately simpler than
// examples/rust/build.rs: run.sh always sets $NAUDIO_LIB, so the only fallback
// needed is the in-tree build directory for someone running cargo here by hand.

use std::env;
use std::path::{Path, PathBuf};

const LIB_NAMES: &[&str] = &["libnaudio.dylib", "libnaudio.so", "naudio.dll", "libnaudio.dll"];

fn has_naudio(dir: &Path) -> bool {
    LIB_NAMES.iter().any(|n| dir.join(n).exists())
}

fn lib_dir() -> PathBuf {
    // $NAUDIO_LIB is a path to the library file OR the directory holding it.
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
    // tests/ffi-clients/rust-probe -> the repo's build tree.
    let manifest = PathBuf::from(env::var("CARGO_MANIFEST_DIR").expect("CARGO_MANIFEST_DIR"));
    manifest.join("../../../build")
}

fn main() {
    println!("cargo:rerun-if-env-changed=NAUDIO_LIB");
    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rerun-if-changed=../../../examples/rust/src/main.rs");

    let dir = lib_dir();
    let dir = dir.canonicalize().unwrap_or(dir);
    let display = dir.display();

    if !has_naudio(&dir) {
        println!(
            "cargo:warning=libnaudio not found in {display} — build it first \
             (cmake -S . -B build && cmake --build build -j) or set NAUDIO_LIB"
        );
    }

    println!("cargo:rustc-link-search=native={display}");
    println!("cargo:rustc-link-lib=dylib=naudio");
    println!("cargo:rustc-link-arg=-Wl,-rpath,{display}");
}
