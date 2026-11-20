#!/usr/bin/env python3
# Rewrites a fragmented 'cenc' MP4 that ffmpeg wrote (one protected video
# track, 8-byte per-sample IVs, a 'senc' with subsample maps in each moof)
# as the same stream under the 'cbcs' scheme of ISO/IEC 23001-7 §10.4: a
# version 1 'tenc' with the 1:9 pattern and a constant 16-byte IV, the
# 'senc' entries without IVs, 'saiz', 'saio' and the runs' data offsets
# moved to match, and every sample encrypted again from the clear file's
# copy of it — by the openssl command, not by Sashfold's own AES, so the
# fixture is written by another implementation than the one it tests.
#   tools/cenc-to-cbcs.py <cenc.mp4> <clear.mp4> <key hex> <iv hex> <out.mp4>
import struct
import subprocess
import sys


def boxes(data, start=0, end=None):
    end = len(data) if end is None else end
    at = start
    while at + 8 <= end:
        size, kind = struct.unpack(">I4s", data[at:at + 8])
        header = 8
        if size == 1:
            size = struct.unpack(">Q", data[at + 8:at + 16])[0]
            header = 16
        elif size == 0:
            size = end - at
        yield kind.decode("latin-1"), at, header, size
        at += size


def box(kind, body):
    return struct.pack(">I4s", 8 + len(body), kind.encode("latin-1")) + body


# Containers whose children are rewritten, and how many bytes of their own
# come before the children.
PREFIX = {"moov": 0, "trak": 0, "mdia": 0, "minf": 0, "stbl": 0, "stsd": 8, "encv": 78, "sinf": 0, "schi": 0}


def rewrite_init(data, start, end, constant_iv):
    out = b""
    for kind, at, header, size in boxes(data, start, end):
        body_at = at + header
        if kind in PREFIX:
            prefix = data[body_at:body_at + PREFIX[kind]]
            out += box(kind, prefix + rewrite_init(data, body_at + PREFIX[kind], at + size, constant_iv))
        elif kind == "schm":
            body = bytearray(data[body_at:at + size])
            body[4:8] = b"cbcs"
            out += box(kind, bytes(body))
        elif kind == "tenc":
            kid = data[body_at + 8:body_at + 24]
            # Version 1: reserved, crypt 1 : skip 9, protected, no per-sample
            # IV, the KID, then the constant IV.
            body = bytes([1, 0, 0, 0, 0, 0x19, 1, 0]) + kid + bytes([16]) + constant_iv
            out += box(kind, body)
        else:
            out += data[at:at + size]
    return out


def openssl_cbc(key, iv, plain):
    if not plain:
        return b""
    return subprocess.run(["openssl", "enc", "-aes-128-cbc", "-nopad", "-K", key.hex(), "-iv", iv.hex()],
                          input=plain, stdout=subprocess.PIPE, check=True).stdout


def encrypt_cbcs(sample, subsamples, key, iv):
    # §9.6 and §10.4: in each protected range, one block of every ten
    # encrypted, as one CBC chain from the constant IV; fewer than ten left
    # at the end still begin with an encrypted block; a partial block stays.
    out = bytearray(sample)
    at = 0
    for clear, protected in subsamples:
        at += clear
        whole = protected // 16 * 16
        picked = [at + offset for offset in range(0, whole, 160)]
        plain = b"".join(bytes(out[p:p + 16]) for p in picked)
        cipher = openssl_cbc(key, iv, plain)
        for i, p in enumerate(picked):
            out[p:p + 16] = cipher[16 * i:16 * i + 16]
        at += protected
    assert at == len(sample), "the subsample map does not cover the sample"
    return bytes(out)


def fragment_samples(data):
    # Each moof's samples in order: (offset in the file, size).
    found = []
    for kind, at, header, size in boxes(data):
        if kind != "moof":
            continue
        for tkind, tat, theader, tsize in boxes(data, at + header, at + size):
            if tkind != "traf":
                continue
            for rkind, rat, rheader, rsize in boxes(data, tat + theader, tat + tsize):
                if rkind != "trun":
                    continue
                flags = struct.unpack(">I", data[rat + rheader:rat + rheader + 4])[0] & 0xFFFFFF
                count, = struct.unpack(">I", data[rat + rheader + 4:rat + rheader + 8])
                p = rat + rheader + 8
                offset = 0
                if flags & 1:
                    offset = struct.unpack(">i", data[p:p + 4])[0]
                    p += 4
                if flags & 4:
                    p += 4
                where = at + offset
                for _ in range(count):
                    if flags & 0x100:
                        p += 4
                    sample_size, = struct.unpack(">I", data[p:p + 4]) if flags & 0x200 else (None,)
                    if flags & 0x200:
                        p += 4
                    if flags & 0x400:
                        p += 4
                    if flags & 0x800:
                        p += 4
                    found.append((where, sample_size))
                    where += sample_size
    return found


def main():
    cenc_path, clear_path, key_hex, iv_hex, out_path = sys.argv[1:6]
    key = bytes.fromhex(key_hex)
    iv = bytes.fromhex(iv_hex)
    data = open(cenc_path, "rb").read()
    clear = open(clear_path, "rb").read()
    clear_samples = fragment_samples(clear)
    next_clear = 0
    out = b""
    top = list(boxes(data))
    i = 0
    while i < len(top):
        kind, at, header, size = top[i]
        if kind == "moov":
            out += box("moov", rewrite_init(data, at + header, at + size, iv))
        elif kind == "moof":
            # The moof and the mdat after it, rewritten together.
            mkind, mat, mheader, msize = top[i + 1]
            assert mkind == "mdat"
            parts = []
            sample_maps = []
            for tkind, tat, theader, tsize in boxes(data, at + header, at + size):
                if tkind == "mfhd":
                    parts.append(("raw", data[tat:tat + tsize]))
                    continue
                assert tkind == "traf"
                children = []
                for ckind, cat, cheader, csize in boxes(data, tat + theader, tat + tsize):
                    body = data[cat + cheader:cat + csize]
                    if ckind == "senc":
                        flags = struct.unpack(">I", body[0:4])[0] & 0xFFFFFF
                        count, = struct.unpack(">I", body[4:8])
                        p = 8
                        entries = []
                        for _ in range(count):
                            p += 8  # the per-sample IV, dropped
                            pairs = []
                            if flags & 2:
                                n, = struct.unpack(">H", body[p:p + 2])
                                p += 2
                                for _ in range(n):
                                    pairs.append(struct.unpack(">HI", body[p:p + 6]))
                                    p += 6
                            entries.append(pairs)
                        sample_maps = entries
                        children.append(("senc", (flags, entries)))
                    else:
                        children.append((ckind, body))
                parts.append(("traf", children))

            def build(saio_value, data_offset):
                senc_body_at = None
                moof_body = b""
                for part in parts:
                    if part[0] == "raw":
                        moof_body += part[1]
                        continue
                    traf_body = b""
                    traf_start = 8 + len(moof_body) + 8  # moof header + what came before + traf header
                    for ckind, cbody in part[1]:
                        if ckind == "senc":
                            flags, entries = cbody
                            body = struct.pack(">II", flags, len(entries))
                            for pairs in entries:
                                if flags & 2:
                                    body += struct.pack(">H", len(pairs))
                                    for clear_bytes, protected in pairs:
                                        body += struct.pack(">HI", clear_bytes, protected)
                            senc_body_at = traf_start + len(traf_body) + 8 + 8  # header, version and flags, count
                            traf_body += box("senc", body)
                        elif ckind == "saiz":
                            flags = struct.unpack(">I", cbody[0:4])[0] & 0xFFFFFF
                            p = 4 + (8 if flags & 1 else 0)
                            count, = struct.unpack(">I", cbody[p + 1:p + 5])
                            sizes = [2 + 6 * len(pairs) for pairs in sample_maps]
                            body = cbody[:p] + bytes([0]) + struct.pack(">I", count) + bytes(sizes)
                            traf_body += box("saiz", body)
                        elif ckind == "saio":
                            flags = struct.unpack(">I", cbody[0:4])[0] & 0xFFFFFF
                            version = cbody[0]
                            p = 4 + (8 if flags & 1 else 0)
                            body = cbody[:p] + struct.pack(">I", 1) + (struct.pack(">Q", saio_value) if version == 1 else struct.pack(">I", saio_value))
                            traf_body += box("saio", body)
                        elif ckind == "trun":
                            body = bytearray(cbody)
                            flags = struct.unpack(">I", body[0:4])[0] & 0xFFFFFF
                            assert flags & 1
                            body[8:12] = struct.pack(">i", data_offset)
                            traf_body += box("trun", bytes(body))
                        else:
                            traf_body += box(ckind, cbody)
                    moof_body += box("traf", traf_body)
                return box("moof", moof_body), senc_body_at

            # Twice: the second time with the offsets the first one found.
            moof, senc_at = build(0, 0)
            moof, senc_at2 = build(senc_at, len(moof) + 8)
            assert senc_at == senc_at2
            mdat_body = b""
            for pairs in sample_maps:
                where, sample_size = clear_samples[next_clear]
                next_clear += 1
                mdat_body += encrypt_cbcs(clear[where:where + sample_size], pairs, key, iv)
            assert len(mdat_body) == msize - mheader
            out += moof + box("mdat", mdat_body)
            i += 1
        else:
            out += data[at:at + size]
        i += 1
    open(out_path, "wb").write(out)


main()
