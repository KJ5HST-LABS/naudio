#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# naudio tests — the generated service units for na_audio_daemon (issue #96, item 2).
#
# Copyright (C) 2025-2026 Terrell Deppe
#
# A unit file is code that nothing compiles. A typo in a launchd ProgramArguments entry or a
# systemd ExecStart= line produces a service that installs perfectly and then refuses to start on
# the operator's machine, with the diagnosis buried in a log they have no reason to read. This arm
# closes that gap by RUNNING the unit's own argument list against the built binary — not by
# reading the file and agreeing with itself.
#
# Three things are checked, in increasing order of what they can catch:
#
#   1. Structure — the generated file parses (plutil on macOS) and carries the keys the service
#      story depends on.
#   2. Truthfulness — the absolute program path the unit names is exactly the path this build
#      installs to. A unit pointing at a path the package does not create is the single most
#      likely way this feature breaks, and it cannot be caught by running the built tree.
#   3. Behaviour — the unit's OWN option list, extracted from the file and handed to the built
#      binary, parses. This is the check that would catch `--duration-mss`.
#
# The arm ends with a can-fail control (L222): the same extraction and execution are run against
# a deliberately corrupted copy of the unit, and the run FAILS the assertion. An arm that cannot
# produce a failure has not shown that its passes mean anything.
#
# Hardware-free, like argcheck.sh and configcheck.sh and for the same reason: every case appends
# `--mode zzz`, rejected at the mode dispatch, which is the last check before any device work.
set -u

DAEMON="${1:?usage: servicecheck.sh <path-to-na_audio_daemon> <unit-file> <expected-exec-path>}"
UNIT="${2:?missing unit file}"
WANT_EXEC="${3:?missing expected exec path}"

[ -x "$DAEMON" ] || { echo "FAIL: not executable: $DAEMON"; exit 1; }
[ -f "$UNIT" ]   || { echo "FAIL: no generated unit file at: $UNIT"; exit 1; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

fails=0
ok()   { echo "ok: $*"; }
bad()  { echo "FAIL: $*"; fails=$((fails + 1)); }

# `--mode zzz` is the containment guard (configcheck.sh's, not argcheck's --transport: mode is
# validated last, so it never masks an earlier refusal). `--no-config` is the hermeticity guard:
# a real daemon.conf on the developer's machine would otherwise merge into every case and change
# what the unit's own arguments mean. Neither appears in the unit itself.
GUARD=(--mode zzz --no-config)

# Which KIND of unit is decided by the SUBJECT — the file handed to this script — and not by
# asking the shell what platform it is on. That is S146's lesson from configcheck, where a
# `uname` spelling that stopped matching would have skipped three cases and still passed. Here
# the failure would be worse than a skip: on Windows a `uname`-derived default of "systemd"
# would grep a Task Scheduler XML for `Type=simple`, find nothing, and report the daemon broken.
# An unrecognised unit is a HARNESS FAULT and fails, rather than falling through to a guess.
case "$UNIT" in
    *.plist)   KIND=launchd  ;;
    *.service) KIND=systemd  ;;
    *.xml)     KIND=schtasks ;;
    *) echo "FAIL: harness fault — cannot classify the unit file '$UNIT'"; exit 1 ;;
esac
echo "servicecheck: $KIND unit $UNIT"

# ---- 1/3 structure + 2/3 truthfulness, and extract the unit's own argument list ---------------
unit_args=()
if [ "$KIND" = launchd ]; then
    plutil -lint "$UNIT" > /dev/null 2>&1 \
        && ok "plist parses" || bad "plist does not parse: $(plutil -lint "$UNIT" 2>&1)"

    # plutil is NOT an XML conformance check, and neither is launchd. Both accept a
    # literal double hyphen inside an XML comment, which the XML spec forbids — measured:
    # a generated plist that plutil -lint called OK was refused outright by Python's
    # plistlib, and launchd ran it regardless. So the file can be malformed, work
    # perfectly, and break the first strict parser that ever reads it (item 4's control
    # page being the obvious candidate). xmllint is the check plutil cannot be.
    xmllint --noout "$UNIT" 2>/dev/null \
        && ok "plist is well-formed XML" \
        || bad "plist is not well-formed XML: $(xmllint --noout "$UNIT" 2>&1 | head -1)"

    # Installing a package must never start capturing a microphone at the next login. Measured
    # during design: a Disabled:true agent refuses to bootstrap at all, so this key IS the
    # inertness, and `launchctl enable` is what overrides it.
    [ "$(plutil -extract Disabled raw -o - "$UNIT" 2>/dev/null)" = "true" ] \
        && ok "agent ships disabled" || bad "agent is not Disabled:true — install would auto-start"

    [ "$(plutil -extract Label raw -o - "$UNIT" 2>/dev/null)" = "org.kj5hst.naudio.daemon" ] \
        && ok "label is org.kj5hst.naudio.daemon" || bad "wrong Label"

    # KeepAlive without a throttle turns a permanent config error into a hot loop.
    [ "$(plutil -extract ThrottleInterval raw -o - "$UNIT" 2>/dev/null)" -ge 10 ] 2>/dev/null \
        && ok "restart is throttled" || bad "ThrottleInterval missing or under 10 s"

    # launchd expands neither ~ nor $HOME, so an absent log path means a config refusal the
    # operator can never see — the one diagnosis this platform has.
    log="$(plutil -extract StandardErrorPath raw -o - "$UNIT" 2>/dev/null)"
    case "$log" in
        /*) ok "stderr goes to an absolute path ($log)" ;;
        *)  bad "StandardErrorPath is not an absolute path: '$log'" ;;
    esac

    n=0
    while v="$(plutil -extract "ProgramArguments.$n" raw -o - "$UNIT" 2>/dev/null)"; do
        unit_args+=("$v"); n=$((n + 1))
    done
elif [ "$KIND" = schtasks ]; then
    # A Task Scheduler logon task (issue #96 item 2, Windows half). Three of its defaults would
    # break a long-running audio service SILENTLY, so each is asserted rather than left to the
    # scheduler: the run would simply stop, or never start, with nothing written anywhere.
    if command -v xmllint > /dev/null 2>&1; then
        xmllint --noout "$UNIT" 2>/dev/null \
            && ok "the task XML is well-formed" \
            || bad "the task XML is not well-formed: $(xmllint --noout "$UNIT" 2>&1 | head -1)"
    else
        echo "note: no xmllint here; skipping the XML conformance check (CI has one)"
    fi

    grep -q '<LogonTrigger>' "$UNIT" && ok "runs at logon" \
        || bad "no LogonTrigger — the task would never start by itself"

    # Every assertion below is TAG-ANCHORED, which is not style: this template documents its own
    # defaults in an XML comment, and a bare grep for PT0S would match the prose that explains
    # why PT0S is needed and pass on a file that had lost the setting (L230).
    settings="$(sed -n '/<Settings>/,/<\/Settings>/p' "$UNIT")"
    [ -n "$settings" ] || bad "no <Settings> block in the task XML"

    # The trigger's own <Enabled> is true; this is the task-level one, which is the inertness.
    printf '%s' "$settings" | grep -q '<Enabled>false</Enabled>' \
        && ok "the task ships disabled" \
        || bad "the task is not disabled — installing would start capturing at the next logon"

    # Defaults to PT72H. A service that must run indefinitely would be killed after three days.
    printf '%s' "$settings" | grep -q '<ExecutionTimeLimit>PT0S</ExecutionTimeLimit>' \
        && ok "no execution time limit (PT0S)" \
        || bad "ExecutionTimeLimit is not PT0S — the task would be killed after three days"

    # Both default to true. On a laptop the task would refuse to start on battery, or stop
    # mid-stream the moment the charger came out.
    printf '%s' "$settings" | grep -q '<DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>' \
        && ok "starts on battery" || bad "DisallowStartIfOnBatteries is not false — no laptop would start it"
    printf '%s' "$settings" | grep -q '<StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>' \
        && ok "keeps running on battery" || bad "StopIfGoingOnBatteries is not false"

    cmd="$(sed -n 's|.*<Command>\(.*\)</Command>.*|\1|p' "$UNIT")"
    xargs_line="$(sed -n 's|.*<Arguments>\(.*\)</Arguments>.*|\1|p' "$UNIT")"
    [ -n "$cmd" ] || bad "no <Command> in the task XML"
    # shellcheck disable=SC2206
    unit_args=("$cmd" $xargs_line)
else
    grep -q '^Type=simple$'                  "$UNIT" && ok "Type=simple"           || bad "no Type=simple"
    grep -q '^Restart=on-failure$'           "$UNIT" && ok "Restart=on-failure"    || bad "no Restart=on-failure"
    # The daemon's exit-code contract: 2 is a config-file or usage error, which no restart can
    # fix. Without this the unit retries a typo forever instead of landing in `failed`.
    grep -q '^RestartPreventExitStatus=2$'   "$UNIT" && ok "config errors are not retried" \
        || bad "no RestartPreventExitStatus=2 — a bad daemon.conf would restart-loop"
    # A user unit, never a system one: system scope cannot reach the session audio server.
    grep -q '^WantedBy=default\.target$'     "$UNIT" && ok "installs into default.target (user scope)" \
        || bad "no WantedBy=default.target"

    line="$(sed -n 's/^ExecStart=//p' "$UNIT")"
    [ -n "$line" ] || bad "no ExecStart= line"
    # Safe to word-split: every token this template emits is space-free, and case 3 below would
    # fail loudly if that ever stopped being true.
    # shellcheck disable=SC2206
    unit_args=($line)
fi

[ "${#unit_args[@]}" -ge 1 ] || { echo "FAIL: extracted no arguments from $UNIT"; exit 1; }

# 2/3 — the path the unit names must be the path this build installs to. A unit that points
# anywhere else is a service that cannot start, and no amount of running the build tree shows it.
if [ "${unit_args[0]}" = "$WANT_EXEC" ]; then
    ok "unit names the installed binary ($WANT_EXEC)"
else
    bad "unit names '${unit_args[0]}', but this build installs to '$WANT_EXEC'"
fi

# All three units run control mode since issue #96 item 4. It is a strict superset of what they
# ran before — the page is served and NOTHING is captured until the operator asks — so a unit
# that lost this would silently go back to opening a microphone at every login, which is exactly
# the behaviour the arc decided against.
printf '%s\n' "${unit_args[@]}" | grep -qx -- '--mode' \
    && [ "$(printf '%s\n' "${unit_args[@]}" | grep -A1 -x -- '--mode' | tail -1)" = "control" ] \
    && ok "unit serves the control page (--mode control)" \
    || bad "unit does not pass --mode control — it would capture at login with no page to stop it"

# A service that inherits the 30 s default would exit half a minute after every login.
printf '%s\n' "${unit_args[@]}" | grep -qx -- '--duration-ms' \
    && [ "$(printf '%s\n' "${unit_args[@]}" | grep -A1 -x -- '--duration-ms' | tail -1)" = "0" ] \
    && ok "unit runs until stopped (--duration-ms 0)" \
    || bad "unit does not pass --duration-ms 0 — the service would exit after 30 s"

# ---- 3/3 behaviour: the unit's own options, run against the built binary ----------------------
# argv[0] is the INSTALL path, which does not exist in a build tree; everything after it is the
# option list under test, and it is handed to the binary verbatim.
run_unit_args() {  # $1.. = the argument list to test; echoes "rc<TAB>output"
    local out rc
    out="$("$DAEMON" "${@:2}" "${GUARD[@]}" 2>&1)"; rc=$?
    printf '%s\t%s' "$rc" "$out"
}

res="$(run_unit_args "${unit_args[@]}")"
rc="${res%%$'\t'*}"; out="${res#*$'\t'}"
if [ "$rc" = 2 ] && printf '%s' "$out" | grep -q "invalid --mode 'zzz'"; then
    ok "every option in the unit parses (reached the mode guard)"
else
    bad "the unit's own options did not parse: exit $rc"
    echo "  args:   ${unit_args[*]:1}"
    echo "  output: $out"
fi

# ---- can-fail control (L222) -----------------------------------------------------------------
# Corrupt one option token and re-run the SAME check. If this still passes, case 3 above proves
# nothing — it would mean the binary accepts anything, or that the arguments never reached it.
corrupt=("${unit_args[@]}")
for i in "${!corrupt[@]}"; do
    [ "${corrupt[$i]}" = "--duration-ms" ] && corrupt[$i]="--duration-mss"
done
res="$(run_unit_args "${corrupt[@]}")"
rc="${res%%$'\t'*}"; out="${res#*$'\t'}"
if [ "$rc" = 2 ] && printf '%s' "$out" | grep -q "unknown option: --duration-mss"; then
    ok "control fires: a corrupted unit option is caught"
else
    bad "control did NOT fire — case 3 cannot be believed (exit $rc): $out"
fi

if [ "$fails" -ne 0 ]; then
    echo "servicecheck: $fails case(s) FAILED"
    exit 1
fi
echo "servicecheck: all cases passed"
