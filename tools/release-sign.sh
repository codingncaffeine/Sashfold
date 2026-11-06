#!/usr/bin/env bash
# Signs a release's downloads, so that a build which replaces itself
# (src/ui/UpdatePackage.h) will take them: writes SHA256SUMS — the SHA-256 of
# each file named, as sha256sum writes it — and SHA256SUMS.sig, an Ed25519
# signature over that file's bytes by the release key. Both are uploaded
# with the release, beside the downloads.
#
# The key is the maintainer's and is in no repository: the file named by
# SASHFOLD_RELEASE_KEY, else ~/.config/sashfold-release/release-signing-key.pem.
# Nothing of it is printed. Before signing, its public half is compared
# with the one built into the browser; after, the signature is verified.
#
# usage: tools/release-sign.sh <out-dir> <download>...
set -euo pipefail
export LC_ALL=C
here="$(cd "$(dirname "$0")/.." && pwd)"
out="${1:?usage: release-sign.sh <out-dir> <download>...}"
shift
[ "$#" -ge 1 ] || { echo "release-sign: no download named" >&2; exit 2; }
key="${SASHFOLD_RELEASE_KEY:-$HOME/.config/sashfold-release/release-signing-key.pem}"
[ -r "$key" ] || { echo "release-sign: no release key at $key" >&2; exit 2; }

# The key's public half, as the 32 bytes the browser holds.
mine="$(openssl pkey -in "$key" -pubout -outform DER | tail -c 32 | od -An -tx1 | tr -d ' \n')"
built_in="$(awk '/release_key = \{/,/\};/' "$here/src/ui/UpdatePackage.h" | grep -o '0x[0-9a-f][0-9a-f]' | sed 's/0x//' | tr -d '\n')"
if [ "$mine" != "$built_in" ] || [ "${#mine}" -ne 64 ]; then
    echo "release-sign: the key at $key is not the one built into src/ui/UpdatePackage.h; nothing signed" >&2
    exit 1
fi

mkdir -p "$out"
: > "$out/SHA256SUMS"
for file in "$@"; do
    [ -f "$file" ] || { echo "release-sign: no file $file" >&2; exit 2; }
    # By its name alone: the list names downloads, not where they were packed.
    (cd "$(dirname "$file")" && sha256sum "$(basename "$file")") >> "$out/SHA256SUMS"
done
openssl pkeyutl -sign -inkey "$key" -rawin -in "$out/SHA256SUMS" -out "$out/SHA256SUMS.sig"
public="$(mktemp)"
trap 'rm -f "$public"' EXIT
openssl pkey -in "$key" -pubout -out "$public"
openssl pkeyutl -verify -pubin -inkey "$public" -rawin -in "$out/SHA256SUMS" -sigfile "$out/SHA256SUMS.sig" > /dev/null
[ "$(wc -c < "$out/SHA256SUMS.sig")" -eq 64 ] || { echo "release-sign: the signature is not 64 bytes" >&2; exit 1; }
echo "release-sign: $out/SHA256SUMS and SHA256SUMS.sig written for $# download(s), signature verified"
cat "$out/SHA256SUMS"
