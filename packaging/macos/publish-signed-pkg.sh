#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# naudio — sign, notarize and staple a published macOS .pkg, then make the release say so.
#
# Copyright (C) 2025-2026 Terrell Deppe
#
# Issue #99. The release workflow cannot sign: no signing identity is available to it, so
# the .pkg it publishes is unsigned and the release note says exactly that (measured, not
# assumed — see signing-status.sh). This is the manual step that changes the fact, and it
# is one command rather than four so the parts cannot be done separately. THAT is the bug
# it fixes: signing, re-checksumming and correcting the note were three manual steps, and
# for 36 minutes of v1.0.0rc3 the published sums described a file that had been replaced
# while the note described a signature that did not exist yet.
#
# It refuses to publish anything it has not verified. The signed .pkg must measure as
# `signed` BEFORE it is uploaded, and the published artifact is re-downloaded and measured
# again afterwards — a round trip, because the thing users get is the thing on the release
# page, not the thing in this temp directory.
#
# Requires: a .pkg whose PAYLOAD BINARIES are already Developer ID Application signed with
# --timestamp and --options runtime. This script signs the installer WRAPPER; the notary
# validates what is inside it, and CI produces ad-hoc linker signatures, so a .pkg built
# by CI cannot be notarized as-is. The script refuses up front rather than discovering it
# three minutes later in a notary rejection. Also requires: a Developer ID Installer
# identity in the keychain, and a notarytool keychain
# profile (create one once with:
#     xcrun notarytool store-credentials <profile> --apple-id <id> \
#           --team-id <team> --password <app-specific-password>
# ).
#
# Usage:
#   packaging/macos/publish-signed-pkg.sh v1.0.0rc4
#   packaging/macos/publish-signed-pkg.sh v1.0.0rc4 --dry-run
#   packaging/macos/publish-signed-pkg.sh v1.0.0rc4 --identity "Developer ID Installer: ..." \
#                                                   --keychain-profile naudio-notary

set -eu

here=$(cd "$(dirname "$0")" && pwd)
status_sh="$here/signing-status.sh"

tag=""
identity=""
profile="naudio-notary"
dry_run=0

while [ $# -gt 0 ]; do
    case "$1" in
        --identity)         identity="${2-}"; shift 2 ;;
        --keychain-profile) profile="${2-}";  shift 2 ;;
        --dry-run)          dry_run=1; shift ;;
        -h|--help)          awk '/^set -eu/{exit} NR>2 && /^#/{sub(/^# ?/,""); print}' "$0"; exit 0 ;;
        -*)                 echo "unknown option: $1" >&2; exit 2 ;;
        *)                  tag="$1"; shift ;;
    esac
done

[ -n "$tag" ] || { echo "usage: publish-signed-pkg.sh <tag> [--dry-run]" >&2; exit 2; }

say() { printf '\n== %s\n' "$*"; }
die() { printf '\nERROR: %s\n' "$*" >&2; exit 1; }

command -v gh > /dev/null || die "gh is not installed"
gh release view "$tag" > /dev/null 2>&1 || die "no release for tag $tag"

# Pick the identity if one was not named. Naming it is better when the keychain holds
# several; auto-detection keeps the script portable to whoever inherits the release role.
if [ -z "$identity" ]; then
    identity=$(security find-identity -v 2>/dev/null \
               | sed -n 's/.*"\(Developer ID Installer: [^"]*\)".*/\1/p' | head -1)
    [ -n "$identity" ] || die "no 'Developer ID Installer' identity in the keychain; pass --identity"
fi
say "identity: $identity"

work=$(mktemp -d) || die "mktemp failed"
trap 'rm -rf "$work"' EXIT

say "downloading the published .pkg for $tag"
gh release download "$tag" -p '*.pkg' -D "$work" --clobber
pkg=$(ls "$work"/*.pkg 2>/dev/null | head -1)
[ -n "$pkg" ] || die "the release has no .pkg asset"
name=$(basename "$pkg")
say "asset: $name"

before=$("$status_sh" "$pkg")
if [ "$before" = signed ]; then
    say "the published .pkg is ALREADY signed, notarized and stapled — nothing to sign"
else
    # PRE-FLIGHT: the notary service validates the Mach-O binaries INSIDE the package,
    # not just the installer signature productsign applies to the wrapper. Every payload
    # executable must carry a Developer ID Application signature, a secure timestamp and
    # the hardened runtime. CI builds them with none of those — a linker ad-hoc signature
    # is what you get — and nothing in this repository signs them, so submitting anyway
    # buys a ~3 minute wait and then `status: Invalid` whose reason is only visible via a
    # separate `notarytool log <id>` call.
    #
    # Found on v1.0.0rc5, the first time this script's write half ever ran. v1.0.0rc3 --
    # the only signed release -- notarized because its payload binaries had been signed by
    # hand beforehand, a step that exists in no script and was recorded nowhere. This check
    # is why that is now visible in one line instead of one round trip to Apple.
    say "checking the payload binaries the notary will validate"
    unsigned_payload=""
    probe="$work/probe"
    rm -rf "$probe"
    if pkgutil --expand-full "$pkg" "$probe" >/dev/null 2>&1; then
        while IFS= read -r bin; do
            [ -n "$bin" ] || continue
            if ! codesign -dvv "$bin" 2>&1 | grep -q "Authority=Developer ID Application"; then
                unsigned_payload="${unsigned_payload}
    $(basename "$bin")  $(codesign -dvv "$bin" 2>&1 | sed -n 's/^Signature=/signature: /p' | head -1)"
            fi
        done <<EOF_BINS
$(find "$probe" -path '*/Payload/*' -type f -perm -u+x 2>/dev/null | while read -r f; do
      file "$f" 2>/dev/null | grep -q 'Mach-O' && echo "$f"; done)
EOF_BINS
    else
        die "could not expand $name to inspect its payload"
    fi
    if [ -n "$unsigned_payload" ]; then
        printf '%s\n' "" \
          "REFUSING TO SUBMIT: the payload binaries are not Developer ID signed." \
          "" \
          "productsign signs the installer WRAPPER. Apple's notary service also validates" \
          "every Mach-O inside it, and these would be rejected:$unsigned_payload" \
          "" \
          "Each needs: codesign --sign 'Developer ID Application: ...' --timestamp" \
          "            --options runtime <binary>" \
          "before the .pkg is built. That does not happen in CI (no identity there) and no" \
          "script in this repository does it, so a .pkg downloaded from a release cannot be" \
          "notarized as-is. Signing has to move into the build that produces the package." \
          "" >&2
        die "payload binaries unsigned — see above"
    fi
    say "payload binaries carry Developer ID signatures"

    say "signing"
    productsign --sign "$identity" "$pkg" "$work/signed.pkg"

    say "notarizing (this waits for the notary service)"
    xcrun notarytool submit "$work/signed.pkg" --keychain-profile "$profile" --wait

    say "stapling"
    xcrun stapler staple "$work/signed.pkg"

    # THE GATE. Everything below publishes; nothing below runs unless the artifact in
    # hand measures as signed by the same script the release note is written from.
    after=$("$status_sh" "$work/signed.pkg")
    [ "$after" = signed ] || die "the signed .pkg still measures as '$after' — publishing nothing"

    mv "$work/signed.pkg" "$pkg"
fi

if [ "$dry_run" -eq 1 ]; then
    say "--dry-run: verified locally, uploaded nothing, edited nothing"
    exit 0
fi

say "uploading the signed .pkg"
gh release upload "$tag" "$pkg" --clobber

# The sums are part of the same step, not a follow-up. They are regenerated from the
# assets as PUBLISHED — re-downloaded, not from whatever is lying around locally.
say "regenerating sha256sums.txt from the published assets"
sums="$work/sums"
mkdir -p "$sums"
gh release download "$tag" -D "$sums" --clobber
rm -f "$sums/sha256sums.txt"
( cd "$sums" && shasum -a 256 naudio-* > sha256sums.txt && cat sha256sums.txt )
gh release upload "$tag" "$sums/sha256sums.txt" --clobber

# And the note. Both sentences come from signing-status.sh, so this is an exact swap of a
# string that script produced — never a hand-written edit that could drift from it.
say "correcting the release note"
old_sentence=$("$status_sh" --sentence unsigned)
new_sentence=$("$status_sh" --sentence signed)
gh release view "$tag" --json body -q .body > "$work/body.md"
OLD="$old_sentence" NEW="$new_sentence" python3 - "$work/body.md" <<'PY'
import os, sys
p = sys.argv[1]
body = open(p, encoding='utf-8').read()
old, new = os.environ['OLD'], os.environ['NEW']
if new in body and old not in body:
    print("the note already carries the signed sentence")
elif old not in body:
    sys.exit("the note does not carry the unsigned sentence this script replaces — "
             "correct it by hand and check signing-status.sh --sentence still owns the wording")
else:
    open(p, 'w', encoding='utf-8').write(body.replace(old, new))
    print("note updated")
PY
gh release edit "$tag" --notes-file "$work/body.md"

# ROUND TRIP. What users get is the file on the release page, so measure THAT one.
say "verifying the published release"
check="$work/check"
mkdir -p "$check"
gh release download "$tag" -D "$check" --clobber
published=$("$status_sh" "$check/$name")
[ "$published" = signed ] || die "the PUBLISHED .pkg measures as '$published'"
( cd "$check" && shasum -a 256 -c sha256sums.txt ) || die "published sums do not match published files"
gh release view "$tag" --json body -q .body | grep -qF "$new_sentence" \
    || die "the published note does not carry the signed sentence"
gh release view "$tag" --json body -q .body | grep -qF "$old_sentence" \
    && die "the published note still carries the unsigned sentence"

say "done — $tag: .pkg signed/notarized/stapled, sums regenerated, note corrected, all re-verified"
