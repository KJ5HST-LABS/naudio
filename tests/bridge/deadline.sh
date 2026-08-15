# SPDX-License-Identifier: LGPL-2.1-or-later
#
# naudio tests — deadlines for the bridge harnesses. Issue #88 item 2 (from #41).
#
# Copyright (C) 2025-2026 Terrell Deppe
#
# SOURCED, never executed:  . "$(dirname "${BASH_SOURCE[0]}")/deadline.sh"
#
# WHAT THIS IS FOR. #41 watched a full ctest sit in `naudio_bridge_arm` for **~29 minutes** with the
# bridge alive and healthy, the arm past its readiness gate, and the probe simply never returning.
# Every readiness poll in both harnesses was already bounded; every *probe invocation* and every
# `wait` was not. A hung arm reports nothing at all — it is strictly worse than a failing one,
# because ctest eventually kills it with no diagnosis and the log ends mid-arm.
#
# WHY THIS IS NOT `timeout(1)`, which is the obvious answer and the wrong one. Two independent
# reasons, either sufficient:
#
#   1. macOS SIP strips DYLD_* from the environment the moment a SIP-protected binary is exec'd,
#      with no error and no diagnostic (CLAUDE.md Learning 6). `bridge_fault_arm.sh` injects every
#      one of its faults through DYLD_INSERT_LIBRARIES, so wrapping its probe or its bridge in
#      `timeout` would silently disable the injection — and its arms would then fail reporting
#      issues #5 and #6 as live bridge bugs. Both harness headers already state this rule; this
#      file exists so that honouring it does not mean going unbounded.
#   2. macOS ships no `timeout` at all. It arrives with Homebrew coreutils (as both `timeout` and
#      `gtimeout`), so a harness depending on it is broken or silently skipped on a stock Mac
#      rather than bounded on one.
#
# Polling `kill -0` from THIS shell adds no exec layer whatsoever, which is exactly the mechanism
# both harnesses already use for their readiness gates — this generalises it rather than
# introducing anything new.
#
# WHY POLLING `kill -0` IS SOUND. bash reaps its own background children as it notices them and
# remembers their exit statuses, so once `kill -0` fails the child is gone AND a later `wait` still
# returns its real status rather than an error. Both harnesses already depend on this property in
# their readiness loops; nothing here is a new assumption.
#
# WHY ONE SHARED FILE rather than the same helpers pasted into both harnesses: a contract restated
# in two places drifts from itself (Learning 118), and the two would drift in exactly the way #41's
# own citations did.

# Deadlines in seconds. GENEROUS ON PURPOSE. A deadline tight enough to turn a slow-but-correct run
# into a red arm costs more than it saves, and tightness is not where the win is: bounded at all is
# the win. 60 s against ~29 minutes is the difference between a diagnosis and a hung job, and 45 s
# against 44 s is worth nothing at all.
NA_PROBE_MARGIN="${NA_BRIDGE_PROBE_MARGIN:-45}"     # added to a probe's own --seconds
NA_STOP_DEADLINE="${NA_BRIDGE_STOP_DEADLINE:-30}"   # a child asked to stop must be gone within this

# Set by every helper below to the child's exit status — or to 124 (the status `timeout(1)` uses for
# the same event) when the deadline expired and the child had to be killed. Callers that care about
# the difference test for 124 explicitly; callers that only care whether the child ended cleanly can
# keep testing it against 0, because 124 is not 0.
NA_DEADLINE_RC=0

# na_wait_pid <pid> <seconds> [label] — reap a child under a deadline.
# Returns 0 if it ended on its own within the deadline, 1 if it had to be SIGKILLed.
na_wait_pid () {
    local pid="$1" secs="$2" label="${3:-pid $1}" i
    for i in $(seq 1 $(( secs * 10 ))); do
        if ! kill -0 "$pid" 2>/dev/null; then
            wait "$pid" 2>/dev/null
            NA_DEADLINE_RC=$?
            return 0
        fi
        sleep 0.1
    done
    kill -9 "$pid" 2>/dev/null
    wait "$pid" 2>/dev/null
    NA_DEADLINE_RC=124
    echo "DEADLINE $label: still alive ${secs}s after it should have ended — SIGKILLed." >&2
    return 1
}

# na_stop_pid <pid> <signal> <seconds> <label> — signal a child and reap it under a deadline.
# The signal is sent from this shell, so a child that ignores it is escalated to SIGKILL rather than
# waited on forever. Every `kill -INT <pid>; wait <pid>` pair in these harnesses is this function.
na_stop_pid () {
    local pid="$1" sig="$2" secs="$3" label="$4"
    kill "-$sig" "$pid" 2>/dev/null
    na_wait_pid "$pid" "$secs" "$label (signalled SIG$sig)"
}

# na_run_deadline <seconds> <label> <logfile|-> <cmd...> — run a command with a hard deadline.
#
# The command is launched DIRECTLY from this shell and reaped by polling, never exec'd through a
# wrapper — see the SIP note above. Pass `-` as the logfile to let the command inherit this shell's
# stdout/stderr, which is what the arms that show probe output on the console want.
#
# Returns the command's own success/failure within the deadline, or 1 with NA_DEADLINE_RC=124 if it
# expired. A wedged probe therefore FAILS its arm and says so, instead of hanging the suite.
na_run_deadline () {
    local secs="$1" label="$2" out="$3"
    shift 3
    if [ "$out" = "-" ]; then
        "$@" &
    else
        "$@" >"$out" 2>&1 &
    fi
    local pid=$!
    if na_wait_pid "$pid" "$secs" "$label"; then
        return 0
    fi
    echo "FAIL $label: produced no result within ${secs}s and was killed. A wedged probe must" >&2
    echo "  FAIL this arm, never hang it — issue #41 watched one sit here for ~29 minutes with" >&2
    echo "  the bridge alive and healthy and the arm simply never returning." >&2
    return 1
}
