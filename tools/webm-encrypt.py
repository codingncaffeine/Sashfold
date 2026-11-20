#!/usr/bin/env python3
# Encrypts a clear WebM file as the WebM Encryption specification lays it
# out: each TrackEntry gains a ContentEncodings element (type 1,
# ContentEncAlgo 5 = AES, AESSettingsCipherMode 1 = CTR, the key ID), and
# each frame becomes a signal byte, an 8-byte IV and the frame encrypted
# with AES-CTR from the counter block IV || 0^64 — by the openssl command,
# not by Sashfold's own AES. Every third video frame is partitioned (a clear
# head, an encrypted middle, a clear tail), every fifth frame of a track is
# left clear (signal byte 0). SeekHead and Cues, whose positions would be
# stale, are dropped; sizes are written in eight bytes.
#   tools/webm-encrypt.py <clear.webm> <key hex> <key id hex> <out.webm>
import struct
import subprocess
import sys

MASTERS = {0x18538067, 0x1654AE6B, 0xAE, 0x1F43B675, 0xA0}  # Segment, Tracks, TrackEntry, Cluster, BlockGroup
DROPPED = {0x114D9B74, 0x1C53BB6B}  # SeekHead, Cues


def read_id(data, at):
    first = data[at]
    length = 1
    while length <= 4 and not (first & (0x80 >> (length - 1))):
        length += 1
    return int.from_bytes(data[at:at + length], "big"), length


def read_size(data, at):
    first = data[at]
    length = 1
    while not (first & (0x80 >> (length - 1))):
        length += 1
    value = first & (0xFF >> length)
    for b in data[at + 1:at + length]:
        value = (value << 8) | b
    unknown = value == (1 << (7 * length)) - 1
    return (None if unknown else value), length


def elements(data, start, end):
    at = start
    while at < end:
        ident, id_length = read_id(data, at)
        size, size_length = read_size(data, at + id_length)
        body = at + id_length + size_length
        if size is None:
            size = end - body
        yield ident, at, body, body + size
        at = body + size


def element(ident, body):
    id_bytes = ident.to_bytes((ident.bit_length() + 7) // 8, "big")
    return id_bytes + bytes([0x01]) + len(body).to_bytes(7, "big") + body


def uint(ident, value):
    return element(ident, value.to_bytes(max(1, (value.bit_length() + 7) // 8), "big"))


def ctr(key, iv8, plain):
    if not plain:
        return b""
    return subprocess.run(["openssl", "enc", "-aes-128-ctr", "-nopad", "-K", key.hex(), "-iv", (iv8 + bytes(8)).hex()],
                          input=plain, stdout=subprocess.PIPE, check=True).stdout


class Encryptor:
    def __init__(self, key, kinds):
        self.key = key
        self.kinds = kinds  # track number -> 1 video, 2 audio
        self.counts = {}

    def frame(self, track, data):
        n = self.counts.get(track, 0)
        self.counts[track] = n + 1
        if n % 5 == 4:
            return bytes([0]) + data
        iv = struct.pack(">Q", 0x0123456789000000 + track * 0x10000 + n)
        if self.kinds.get(track) == 1 and n % 3 == 0 and len(data) >= 8:
            # Partitions at 3 and at half: clear, encrypted, clear.
            offsets = [3, len(data) // 2]
            cipher = ctr(self.key, iv, data[offsets[0]:offsets[1]])
            body = data[:offsets[0]] + cipher + data[offsets[1]:]
            return bytes([3]) + iv + bytes([len(offsets)]) + b"".join(struct.pack(">I", o) for o in offsets) + body
        return bytes([1]) + iv + ctr(self.key, iv, data)


def block(data, body, end, encryptor):
    # Track number (a vint), timecode, flags, then the frame (no lacing).
    track, length = read_size(data, body)
    flags = data[body + length + 2]
    assert (flags >> 1) & 3 == 0, "laced blocks are not written by this tool"
    head = data[body:body + length + 3]
    return head + encryptor.frame(track, data[body + length + 3:end])


def rewrite(data, start, end, encryptor, key_id):
    out = b""
    for ident, at, body, stop in elements(data, start, end):
        if ident in DROPPED:
            continue
        if ident == 0xA3 or ident == 0xA1:  # SimpleBlock, Block
            out += element(ident, block(data, body, stop, encryptor))
        elif ident in MASTERS:
            inner = rewrite(data, body, stop, encryptor, key_id)
            if ident == 0xAE:
                aes = element(0x47E7, uint(0x47E8, 1))
                encryption = element(0x5035, uint(0x47E1, 5) + element(0x47E2, key_id) + aes)
                inner += element(0x6D80, element(0x6240, uint(0x5031, 0) + uint(0x5032, 1) + uint(0x5033, 1) + encryption))
            out += element(ident, inner)
        else:
            out += data[at:stop]
    return out


def track_kinds(data, start, end, kinds):
    for ident, at, body, stop in elements(data, start, end):
        if ident in (0x18538067, 0x1654AE6B):
            track_kinds(data, body, stop, kinds)
        elif ident == 0xAE:
            number = kind = 0
            for inner, _, ibody, istop in elements(data, body, stop):
                if inner == 0xD7:
                    number = int.from_bytes(data[ibody:istop], "big")
                elif inner == 0x83:
                    kind = int.from_bytes(data[ibody:istop], "big")
            kinds[number] = kind


def main():
    source, key_hex, kid_hex, target = sys.argv[1:5]
    data = open(source, "rb").read()
    kinds = {}
    track_kinds(data, 0, len(data), kinds)
    out = rewrite(data, 0, len(data), Encryptor(bytes.fromhex(key_hex), kinds), bytes.fromhex(kid_hex))
    open(target, "wb").write(out)


main()
