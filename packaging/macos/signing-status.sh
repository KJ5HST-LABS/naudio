#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# naudio — measure whether a .pkg is Developer ID signed, notarized and stapled.
#
# Copyright (C) 2025-2026 Terrell Deppe
#
# Issue #99. The release notes used to ASSERT that the macOS .pkg "is signed and
# notarized and opens with no warning" — a property the workflow of the time neither
# performed nor checked, because signing was a manual post-build step. The note was
# therefore false at the moment it published and became true only if a human followed up.
# An earlier wording had the same defect pointed the other way ("both installers are
# unsigned"), which sent macOS users through a Gatekeeper bypass they did not need. One
# root cause: the note stated a signing outcome that nothing established. Since 2026-09-02
# release.yml signs, notarizes and staples the .pkg itself when the org's secrets are
# present — and this script is still the only thing that decides what the note says.
#
# So this is the one place that decides, and it decides by ASKING THE ARTIFACT — the
# same house rule release.yml already applies to self-containment (assert with
# otool/ldd, never infer from configuration; Learning 9's shape).
#
# Prints exactly one token on stdout — `signed` or `unsigned` — and the evidence on
# stderr. Exit status is 0 whenever the measurement itself completed; the VERDICT is
# the token, never the exit code, so a caller cannot mistake "I could not tell" for
# "it is fine" (L111's shape).
#
# `signed` requires all three, because each covers a different failure a user would
# actually hit:
#   * spctl -a -t install    — Gatekeeper's own verdict, which is what "opens with no
#                              warning" literally means.
#   * pkgutil --check-signature — the notary service trusts it (spctl can accept on a
#                              machine that has already seen the ticket).
#   * xcrun stapler validate — the ticket is stapled INTO the file, so a machine that
#                              is offline at install time still opens it cleanly.
#
# Proven able to say both words, on this machine, 2026-08-31 (L222 — an absence is a
# measurement only once something has produced a presence):
#   v1.0.0rc3's published .pkg  -> signed    (Developer ID Installer: Terrell Deppe 7SH6PYQ738)
#   a pkgbuild'd payload        -> unsigned  ("Status: no signature", spctl rejected)
# Both directions are re-proven on every CI run that holds the identity (release.yml): a
# pkgbuild'd control must measure `unsigned` before the real .pkg is measured, and the
# .pkg the workflow has just signed, notarized and stapled must measure `signed`, or the
# job fails rather than publish. A run without the identity keeps only the negative
# control and publishes whatever this script measures.

set -u

# --sentence <signed|unsigned> prints the release-note sentence for a status and exits.
#
# THE SENTENCES LIVE HERE, ONCE. release.yml composes the note on an ubuntu runner from a
# token this script measured on a macOS one, and publish-signed-pkg.sh rewrites that same
# note after signing. Three readers, one wording — a sentence restated in a second file
# drifts from the one that owns it, which is how #99's predecessor survived: the note said
# "both installers are unsigned" long after one of them was not. This branch is pure shell
# and runs anywhere; only the measurement below needs macOS.
if [ "${1-}" = "--sentence" ]; then
    case "${2-}" in
        signed)
            echo "The macOS .pkg is signed and notarized (Developer ID, stapled) and opens with no warning."
            ;;
        unsigned)
            echo "The macOS .pkg is not signed, so macOS warns on first open: right-click the .pkg and choose Open, then confirm."
            ;;
        *)
            echo "usage: signing-status.sh --sentence <signed|unsigned>" >&2
            exit 2
            ;;
    esac
    exit 0
fi

pkg="${1-}"
if [ -z "$pkg" ] || [ ! -f "$pkg" ]; then
    echo "usage: signing-status.sh <path-to-.pkg> | --sentence <signed|unsigned>" >&2
    echo "unsigned"
    exit 0
fi

verdict=signed
note() { printf '  %s\n' "$*" >&2; }

printf 'signing status of %s\n' "$(basename "$pkg")" >&2

# 1. Gatekeeper. Run it WITHOUT a pipe so the status is spctl's own, not a pager's.
spctl_out=$(spctl -a -vv -t install "$pkg" 2>&1)
spctl_rc=$?
note "spctl -a -t install: rc=$spctl_rc $(printf '%s' "$spctl_out" | tr '\n' ' ')"
[ "$spctl_rc" -eq 0 ] || verdict=unsigned

# 2. Notary trust.
pk_out=$(pkgutil --check-signature "$pkg" 2>&1)
note "pkgutil: $(printf '%s' "$pk_out" | sed -n '2,3p' | tr '\n' ' ')"
printf '%s' "$pk_out" | grep -q 'Notarization: trusted by the Apple notary service' || verdict=unsigned
printf '%s' "$pk_out" | grep -q 'Status: signed by a developer certificate' || verdict=unsigned

# 3. The ticket is stapled into the file, not merely issued.
if staple_out=$(xcrun stapler validate "$pkg" 2>&1); then
    note "stapler: validated"
else
    note "stapler: NOT stapled ($(printf '%s' "$staple_out" | tail -1))"
    verdict=unsigned
fi

note "verdict: $verdict"
echo "$verdict"
