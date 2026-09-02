#!/usr/bin/env bash
# "One fetch choke point" as a build gate: socket and
# name-resolution primitives may appear only under src/net/ and src/platform/.
# A source-level tripwire — it catches accidents, not adversaries; the
# binary-level version can join pledge-check later.
set -uo pipefail
export LC_ALL=C
cd "$(dirname "$0")/.." || exit 2

# Sharp tokens only: common words like "send" or "connect" would drown this
# in noise, and every real socket user needs at least one of these.
pattern='getaddrinfo|WSAStartup|socket\(AF_|::socket\(|SOCK_STREAM|ws2tcpip|winsock2|sys/socket'

violations=$(grep -rEln "$pattern" src --include='*.cpp' --include='*.h' \
    | grep -v '^src/net/' | grep -v '^src/platform/' || true)

if [ -n "$violations" ]; then
    echo "EGRESS VIOLATION: network primitives outside src/net/ and src/platform/:"
    echo "$violations"
    exit 1
fi
echo "egress-check PASS: network primitives stay behind the choke point"
