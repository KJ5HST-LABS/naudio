// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Copyright (C) 2025-2026 Terrell Deppe
//
// Java arm of the FFI example-client gate (tests/ffi-clients/run.sh).
//
// This probe declares NOTHING of its own. It reads examples/java/PlayToSpeakers.java's
// OWN NA_DEVICE MemoryLayout and calls the ABI through that client's OWN downcall
// handles, so what gets compared against na_device_truth.c is the shipped example
// rather than a restatement of it that could drift in the same direction.
//
// It reaches those members because run.sh copies both files into one directory and
// runs `java Probe.java` there: JDK 22's multi-file source-code launcher (JEP 458)
// compiles PlayToSpeakers.java on demand from the same directory, and both classes
// are in the unnamed package, so package-private statics are visible. That is also
// why this file lives in tests/ and not in examples/ -- the shipped example
// directory stays exactly as a reader finds it.
//
// Requires JDK 22+, the same floor the example itself documents.
//
// Emits the same key=value manifest na_device_truth.c does, on stdout, and nothing
// else -- run.sh diffs the two. na_client_callbacks is absent on purpose: this
// client is RX-only and never declares it, so run.sh compares Java against the
// truth with that section removed.

import static java.lang.foreign.ValueLayout.JAVA_INT;

import java.lang.foreign.Arena;
import java.lang.foreign.MemoryLayout.PathElement;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.SymbolLookup;

public final class Probe {

    // Field order matches na_device_truth.c's; run.sh sorts both sides anyway.
    static final String[] FIELDS = {
        "backend_id", "capture_backend_id", "playback_backend_id",
        "name", "host_api", "type", "capability", "is_virtual",
    };

    static long offset(String field) {
        return PlayToSpeakers.NA_DEVICE.byteOffset(PathElement.groupElement(field));
    }

    static long fieldSize(String field) {
        return PlayToSpeakers.NA_DEVICE.select(PathElement.groupElement(field)).byteSize();
    }

    public static void main(String[] args) throws Throwable {
        // ---- 1. the client's own declared layout ------------------------------------
        System.out.println("na_device.sizeof=" + PlayToSpeakers.NA_DEVICE.byteSize());
        System.out.println("na_device.alignof=" + PlayToSpeakers.NA_DEVICE.byteAlignment());
        for (String f : FIELDS) {
            System.out.println("na_device.offset." + f + "=" + offset(f));
        }
        // The array extents. Redundant with the offsets -- either array changing size
        // moves a later offset -- but they name the wrong field directly instead of
        // reporting it as a shift in everything that follows.
        System.out.println("na_device.fieldsizeof.name=" + fieldSize("name"));
        System.out.println("na_device.fieldsizeof.host_api=" + fieldSize("host_api"));

        // ---- 2. the devices na_enumerate writes, read back through the client -------
        try (Arena arena = Arena.ofConfined()) {
            // loadNaudio honours $NAUDIO_LIB, which run.sh sets to the built library.
            SymbolLookup lib = PlayToSpeakers.loadNaudio(null, arena);
            PlayToSpeakers.bind(lib);

            MemorySegment ctx = (MemorySegment) PlayToSpeakers.naContextCreate.invokeExact();
            if (ctx.address() == 0) {
                System.err.println("Probe: na_context_create failed");
                System.exit(1);
            }
            try {
                long devSize = PlayToSpeakers.DEV_SIZE;
                int max = PlayToSpeakers.MAX_DEVICES;
                MemorySegment arr =
                    arena.allocate(devSize * max, PlayToSpeakers.NA_DEVICE.byteAlignment());

                // invokeExact is SIGNATURE-EXACT: devSize must be a long here, matching the
                // JAVA_LONG the example's FunctionDescriptor declares for struct_size.
                int n = (int) PlayToSpeakers.naEnumerate.invokeExact(ctx, arr, max, devSize);
                if (n < 0) {
                    System.err.println("Probe: na_enumerate failed: " + n);
                    System.exit(1);
                }

                System.out.println("device.count=" + n);
                for (int i = 0; i < n; i++) {
                    MemorySegment d = arr.asSlice((long) i * devSize, devSize);
                    System.out.println("device." + i + ".backend_id=" + d.get(JAVA_INT, offset("backend_id")));
                    System.out.println("device." + i + ".capture_backend_id=" + d.get(JAVA_INT, offset("capture_backend_id")));
                    System.out.println("device." + i + ".playback_backend_id=" + d.get(JAVA_INT, offset("playback_backend_id")));
                    System.out.println("device." + i + ".type=" + d.get(JAVA_INT, offset("type")));
                    System.out.println("device." + i + ".capability=" + d.get(JAVA_INT, offset("capability")));
                    System.out.println("device." + i + ".is_virtual=" + d.get(JAVA_INT, offset("is_virtual")));
                    System.out.println("device." + i + ".name=" + d.getString(offset("name")));
                    System.out.println("device." + i + ".host_api=" + d.getString(offset("host_api")));
                }
            } finally {
                PlayToSpeakers.naContextDestroy.invokeExact(ctx);
            }
        }
    }
}
