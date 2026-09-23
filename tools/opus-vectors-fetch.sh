#!/usr/bin/env bash
# Fetches the Opus test vectors of RFC 8251 §11 — twelve bitstreams with the
# final range the reference encoder ended each packet in, and the reference
# decoder's output for each — which test_opus decodes and holds our decoder
# to. They are about 120 MB unpacked, which is why they are downloaded rather
# than committed (opus-vectors/ is ignored).
#
#   tools/opus-vectors-fetch.sh [directory]      (default: opus-vectors/ at the repo root)
#
# Every bitstream is checked against the SHA-1 the RFC lists for it.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
dest="${1:-$root/opus-vectors}"
url="https://opus-codec.org/static/testvectors/opus_testvectors-rfc8251.tar.gz"

if [ -s "$dest/testvector12.bit" ]; then
  echo "opus test vectors already in $dest"
  exit 0
fi

work="$(mktemp -d "${TMPDIR:-/tmp}/opus-vectors.XXXXXX")"
trap 'rm -rf "$work"' EXIT
echo "fetching $url"
curl -fsSL -o "$work/vectors.tar.gz" "$url"
tar xzf "$work/vectors.tar.gz" -C "$work"

# RFC 8251 §11: the bitstreams' SHA-1s.
cat > "$work/sums" <<'SUMS'
e49b2862ceec7324790ed8019eb9744596d5be01  testvector01.bit
b809795ae1bcd606049d76de4ad24236257135e0  testvector02.bit
e0c4ecaeab44d35a2f5b6575cd996848e5ee2acc  testvector03.bit
a0f870cbe14ebb71fa9066ef3ee96e59c9a75187  testvector04.bit
9b3d92b48b965dfe9edf7b8a85edd4309f8cf7c8  testvector05.bit
28e66769ab17e17f72875283c14b19690cbc4e57  testvector06.bit
bacf467be3215fc7ec288f29e2477de1192947a6  testvector07.bit
ddbe08b688bbf934071f3893cd0030ce48dba12f  testvector08.bit
3932d9d61944dab1201645b8eeaad595d5705ecb  testvector09.bit
521eb2a1e0cc9c31b8b740673307c2d3b10c1900  testvector10.bit
6bc8f3146fcb96450c901b16c3d464ccdf4d5d96  testvector11.bit
338c3f1b4b97226bc60bc41038becbc6de06b28f  testvector12.bit
SUMS
(cd "$work/opus_newvectors" && sha1sum -c --quiet "$work/sums")

mkdir -p "$dest"
cp "$work"/opus_newvectors/* "$dest/"
echo "opus test vectors in $dest"
