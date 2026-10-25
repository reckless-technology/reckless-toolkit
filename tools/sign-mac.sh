#!/usr/bin/env bash
# Developer ID signing and notarisation for a product's macOS release, on a keychain of the job's own that holds the
# identities for the job alone - so the runner keeps no certificate, and a runner rebuilt signs with no setup. Any
# product's release runs it from wherever it holds the toolkit (external/reckless-toolkit/tools/sign-mac.sh); it finds
# what it carries beside it by its own directory.
#
#   tools/sign-mac.sh open              a keychain from the secrets below, first on the user's search list (macOS
#                                       finds a certificate's issuer only in a keychain on it); prints its path
#   tools/sign-mac.sh sign <dist-dir>   every .vst3, .component and .app at the top of the staged release signed with
#                                       the Developer ID Application identity, the hardened runtime and a secure
#                                       timestamp - an app with installer/app.entitlements beside this script, for its
#                                       audio input - each then held as the system checks it
#   tools/sign-mac.sh notarize <pkg>    the signed installer sent to Apple's notary service and waited for - up to
#                                       RECKLESS_NOTARY_TIMEOUT, 3h by default, since a new team's first submissions
#                                       sit in Apple's in-depth review for hours where later ones take minutes - its
#                                       ticket stapled to it, and the result held to Gatekeeper's assessment; Apple's
#                                       ticket covers the code the package holds, so the same bundles in the zip pass
#                                       Gatekeeper's online check too
#   tools/sign-mac.sh close             the keychain deleted and the search list put back
#
# The environment, from the secrets of the product's own repository (each product holds its own copy of them):
# MACOS_DEVELOPER_ID_APPLICATION_P12 and MACOS_DEVELOPER_ID_INSTALLER_P12, the two identities as base64 PKCS #12 in the
# format macOS's keychain reads (3DES, a SHA-1 MAC); MACOS_SIGNING_P12_PASSWORD their password; MACOS_TEAM_ID;
# MACOS_NOTARY_KEY_P8, MACOS_NOTARY_KEY_ID and MACOS_NOTARY_ISSUER_ID, an App Store Connect API key with the Developer
# role; RECKLESS_NOTARY_TIMEOUT optionally. The keychain is RUNNER_TEMP's (or TMPDIR's) signing.keychain-db; its
# password is made at random and never leaves the job.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
keychain="${RUNNER_TEMP:-${TMPDIR:-/tmp}}/signing.keychain-db"
saved_list="${keychain%.keychain-db}.search-list"

need() {
    for name in "$@"; do
        [ -n "${!name:-}" ] || { echo "::error::$name is not set: the signing secrets (tools/sign-mac.sh says which)" >&2; exit 1; }
    done
}

identity() { # the identity of `kind` (Application or Installer) in the keychain, by its common name's start
    security find-identity -v -p "$2" "$keychain" | sed -n "s/.*\"\(Developer ID $1: .*\)\"/\1/p" | head -n 1
}

case "${1:-}" in
open)
    need MACOS_DEVELOPER_ID_APPLICATION_P12 MACOS_DEVELOPER_ID_INSTALLER_P12 MACOS_SIGNING_P12_PASSWORD MACOS_TEAM_ID
    password=$(openssl rand -hex 24)
    rm -f "$keychain"
    security create-keychain -p "$password" "$keychain"
    security set-keychain-settings -lut 7200 "$keychain"
    security unlock-keychain -p "$password" "$keychain"
    security list-keychains -d user | tr -d '"' | sed 's/^ *//' > "$saved_list"
    # shellcheck disable=SC2046
    security list-keychains -d user -s "$keychain" $(cat "$saved_list")
    for kind in APPLICATION INSTALLER; do
        var="MACOS_DEVELOPER_ID_${kind}_P12"
        p12="${keychain%.keychain-db}-$kind.p12"
        printf '%s' "${!var}" | base64 --decode > "$p12"
        security import "$p12" -k "$keychain" -P "$MACOS_SIGNING_P12_PASSWORD" \
            -T /usr/bin/codesign -T /usr/bin/productbuild -T /usr/bin/pkgbuild > /dev/null
        rm -f "$p12"
    done
    security set-key-partition-list -S apple-tool:,apple: -s -k "$password" "$keychain" > /dev/null
    app=$(identity Application codesigning)
    installer=$(identity Installer basic)
    [ -n "$app" ] || { echo "::error::no valid Developer ID Application identity in the secrets" >&2; exit 1; }
    [ -n "$installer" ] || { echo "::error::no valid Developer ID Installer identity in the secrets" >&2; exit 1; }
    case "$app$installer" in *"($MACOS_TEAM_ID)"*"($MACOS_TEAM_ID)"*) ;; *)
        echo "::error::the identities are not team $MACOS_TEAM_ID's: $app / $installer" >&2; exit 1 ;; esac
    echo "signing as: $app"
    echo "installer:  $installer"
    echo "$keychain"
    ;;
sign)
    dist=${2:?usage: tools/sign-mac.sh sign <dist-dir>}
    app=$(identity Application codesigning)
    [ -n "$app" ] || { echo "::error::no signing keychain: run tools/sign-mac.sh open first" >&2; exit 1; }
    count=0
    for bundle in "$dist"/*.vst3 "$dist"/*.component "$dist"/*.app; do
        [ -e "$bundle" ] || continue
        extra=()
        [[ "$bundle" == *.app ]] && extra=(--entitlements "$here/installer/app.entitlements")
        codesign --force --options runtime --timestamp --keychain "$keychain" --sign "$app" ${extra[@]+"${extra[@]}"} \
            "$bundle"
        codesign --verify --deep --strict "$bundle"
        count=$((count + 1))
    done
    [ "$count" -gt 0 ] || { echo "::error::$dist holds no bundle to sign" >&2; exit 1; }
    echo "signed $count bundles as $app"
    ;;
notarize)
    pkg=${2:?usage: tools/sign-mac.sh notarize <pkg>}
    need MACOS_NOTARY_KEY_P8 MACOS_NOTARY_KEY_ID MACOS_NOTARY_ISSUER_ID
    key="${keychain%.keychain-db}-notary.p8"
    (umask 077; printf '%s\n' "$MACOS_NOTARY_KEY_P8" > "$key")
    wait=${RECKLESS_NOTARY_TIMEOUT:-3h}
    result=$(xcrun notarytool submit "$pkg" --key "$key" --key-id "$MACOS_NOTARY_KEY_ID" \
        --issuer "$MACOS_NOTARY_ISSUER_ID" --wait --timeout "$wait" --output-format json) || true
    field() { printf '%s' "$result" | python3 -c "import json,sys; print(json.load(sys.stdin).get('$1',''))" 2>/dev/null || true; }
    id=$(field id)
    status=$(field status)
    echo "notarisation $id: ${status:-$(field message)}"
    if [ -z "$status" ] && [ -n "$id" ]; then
        rm -f "$key"
        echo "::error::Apple's notary service had not finished with $pkg after $wait (submission $id): not a rejection - it is still in Apple's queue. Its outcome: xcrun notarytool info $id with the API key; a release run again once it is accepted notarises in minutes" >&2
        exit 1
    fi
    if [ "$status" != "Accepted" ]; then
        [ -n "$id" ] && xcrun notarytool log "$id" --key "$key" --key-id "$MACOS_NOTARY_KEY_ID" \
            --issuer "$MACOS_NOTARY_ISSUER_ID" || true
        rm -f "$key"
        echo "::error::Apple's notary service did not accept $pkg (status '$status'): its log is above" >&2
        exit 1
    fi
    rm -f "$key"
    xcrun stapler staple "$pkg"
    xcrun stapler validate "$pkg"
    spctl --assess --type install --verbose=2 "$pkg"
    pkgutil --check-signature "$pkg" | sed -n '1,4p'
    ;;
close)
    if [ -f "$saved_list" ]; then
        # shellcheck disable=SC2046
        security list-keychains -d user -s $(cat "$saved_list")
        rm -f "$saved_list"
    fi
    security delete-keychain "$keychain" 2> /dev/null || true
    rm -f "${keychain%.keychain-db}"-*.p12 "${keychain%.keychain-db}-notary.p8"
    echo "signing keychain closed"
    ;;
*)
    echo "usage: tools/sign-mac.sh open | sign <dist-dir> | notarize <pkg> | close" >&2
    exit 2
    ;;
esac
