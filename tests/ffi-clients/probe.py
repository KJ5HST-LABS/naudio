#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Copyright (C) 2025-2026 Terrell Deppe
#
# Python arm of the FFI example-client gate (tests/ffi-clients/run.sh).
#
# This probe declares NOTHING of its own. It imports examples/python/play_to_speakers.py
# and reports that client's OWN ctypes declarations and its OWN enumeration path, so the
# thing being compared against na_device_truth.c is the shipped example rather than a
# restatement of it that could drift in the same direction. Importing is safe: the example
# does all its work under `if __name__ == "__main__"`.
#
# Emits the same sorted `key=value` manifest na_device_truth.c does, on stdout, and
# nothing else -- run.sh diffs the two verbatim.

import ctypes
import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CLIENT = os.path.normpath(os.path.join(HERE, "..", "..", "examples", "python", "play_to_speakers.py"))


def load_client():
    """Import the example client module by path (its filename is not an identifier)."""
    spec = importlib.util.spec_from_file_location("na_example_client", CLIENT)
    if spec is None or spec.loader is None:
        sys.exit("probe.py: cannot load %s" % CLIENT)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def emit_layout(struct, name, char_array_fields=()):
    """Print sizeof / alignof / every field offset for one ctypes Structure."""
    print("%s.sizeof=%d" % (name, ctypes.sizeof(struct)))
    print("%s.alignof=%d" % (name, ctypes.alignment(struct)))
    for field_name, _ in struct._fields_:
        print("%s.offset.%s=%d" % (name, field_name, getattr(struct, field_name).offset))
    for field_name in char_array_fields:
        print("%s.fieldsizeof.%s=%d" % (name, field_name, getattr(struct, field_name).size))


def main():
    client = load_client()

    emit_layout(client.NaDevice, "na_device", char_array_fields=("name", "host_api"))
    emit_layout(client.NaClientCallbacks, "na_client_callbacks")

    # The devices the example's own enumerate_devices() reads back, through its own
    # binding of na_enumerate. load_naudio() honours $NAUDIO_LIB, which run.sh sets.
    lib = client.load_naudio(None)
    client.bind(lib)
    devices = client.enumerate_devices(lib)

    print("device.count=%d" % len(devices))
    for i, d in enumerate(devices):
        print("device.%d.backend_id=%d" % (i, d.backend_id))
        print("device.%d.capture_backend_id=%d" % (i, d.capture_backend_id))
        print("device.%d.playback_backend_id=%d" % (i, d.playback_backend_id))
        print("device.%d.type=%d" % (i, d.type))
        print("device.%d.capability=%d" % (i, d.capability))
        print("device.%d.is_virtual=%d" % (i, d.is_virtual))
        print("device.%d.name=%s" % (i, d.name.decode("utf-8", "replace")))
        print("device.%d.host_api=%s" % (i, d.host_api.decode("utf-8", "replace")))

    return 0


if __name__ == "__main__":
    sys.exit(main())
