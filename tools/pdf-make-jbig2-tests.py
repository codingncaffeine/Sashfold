#!/usr/bin/env python3
# Writes JBIG2 (ITU-T T.88) test material made by an independent encoder
# written from the specification's procedures: an MQ arithmetic encoder
# (Annex E), the integer encoding procedures (Annex A), the generic,
# refinement, symbol/text, halftone and page procedures. The judge is
# jbig2dec: every file here must decode with it to exactly the truth bitmap.
#   pdf-make-jbig2-tests.py <out-folder>
#       jbig2-<picture>-<variant>.pdf  one page, the picture's size in
#                                      points, /JBIG2Decode (embedded
#                                      organization; dictionaries in a
#                                      /JBIG2Globals stream)
#       jbig2-<picture>-<variant>.jb2  the same data with a file header
#       jbig2-<picture>-<variant>.pbm  the truth (P4, 1 = black)
#   pdf-make-jbig2-tests.py --fixtures <folder>
#       jbig2-<variant>.bin / .globals / .rows for a small picture, and
#       jbig2-fixtures.txt: "name width height has_globals" per line.
#       .rows are packed rows, (width+7)//8 bytes, 1 = black (JBIG2's
#       sense, before PDF's inversion).
# A JBIG2Decode filter delivers 1 = black as sample 0, so the PDFs need no
# /Decode array (ISO 32000-1 7.4.7).
import io, math, os, random, struct, sys, heapq
from PIL import Image, ImageDraw, TiffImagePlugin

# ---------------------------------------------------------------- bitmaps

class Bm:
    # w x h pixels, one byte each, 1 = black.
    def __init__(s, w, h):
        s.w, s.h = w, h
        s.rows = [bytearray(w) for _ in range(h)]

    def get(s, x, y):
        return s.rows[y][x] if 0 <= x < s.w and 0 <= y < s.h else 0

    def copy(s):
        b = Bm(s.w, s.h)
        b.rows = [bytearray(r) for r in s.rows]
        return b

    def crop(s, x, y, w, h):
        b = Bm(w, h)
        for j in range(h):
            for i in range(w):
                b.rows[j][i] = s.get(x + i, y + j)
        return b

    def blit(s, o, x, y, op=0):
        # 6.2.5.2 combination operators: 0 OR, 1 AND, 2 XOR, 3 XNOR, 4 REPLACE.
        for j in range(o.h):
            if not 0 <= y + j < s.h:
                continue
            for i in range(o.w):
                if not 0 <= x + i < s.w:
                    continue
                d, v = s.rows[y + j][x + i], o.rows[j][i]
                s.rows[y + j][x + i] = (d | v, d & v, d ^ v, 1 - (d ^ v), v)[op]

    def packed(s):
        out = bytearray()
        for r in s.rows:
            line = bytearray((s.w + 7) // 8)
            for x, v in enumerate(r):
                if v:
                    line[x >> 3] |= 0x80 >> (x & 7)
            out += line
        return bytes(out)

    def inverted(s):
        b = Bm(s.w, s.h)
        b.rows = [bytearray(1 - v for v in r) for r in s.rows]
        return b


def from_pil(img):
    b = Bm(img.width, img.height)
    for y in range(img.height):
        for x in range(img.width):
            b.rows[y][x] = 0 if img.getpixel((x, y)) else 1
    return b


def padded(bm, p):
    # Rows with p blank rows/columns around, so a context pixel read needs no
    # bounds test (pixels outside the bitmap are 0, 6.2.5.3).
    blank = bytearray(bm.w + 2 * p)
    rows = [blank] * p
    rows += [bytearray(p) + r + bytearray(p) for r in bm.rows]
    return rows + [blank] * p

# ------------------------------------------------------- MQ encoder (E.2)

# Qe, NMPS, NLPS, SWITCH: Table E.1, the 47 probability states.
QE = [(0x5601, 1, 1, 1), (0x3401, 2, 6, 0), (0x1801, 3, 9, 0), (0x0AC1, 4, 12, 0), (0x0521, 5, 29, 0),
      (0x0221, 38, 33, 0), (0x5601, 7, 6, 1), (0x5401, 8, 14, 0), (0x4801, 9, 14, 0), (0x3801, 10, 14, 0),
      (0x3001, 11, 17, 0), (0x2401, 12, 18, 0), (0x1C01, 13, 20, 0), (0x1601, 29, 21, 0), (0x5601, 15, 14, 1),
      (0x5401, 16, 14, 0), (0x5101, 17, 15, 0), (0x4801, 18, 16, 0), (0x3801, 19, 17, 0), (0x3401, 20, 18, 0),
      (0x3001, 21, 19, 0), (0x2801, 22, 19, 0), (0x2401, 23, 20, 0), (0x2201, 24, 21, 0), (0x1C01, 25, 22, 0),
      (0x1801, 26, 23, 0), (0x1601, 27, 24, 0), (0x1401, 28, 25, 0), (0x1201, 29, 26, 0), (0x1101, 30, 27, 0),
      (0x0AC1, 31, 28, 0), (0x09C1, 32, 29, 0), (0x08A1, 33, 30, 0), (0x0521, 34, 31, 0), (0x0441, 35, 32, 0),
      (0x02A1, 36, 33, 0), (0x0221, 37, 34, 0), (0x0141, 38, 35, 0), (0x0111, 39, 36, 0), (0x0085, 40, 37, 0),
      (0x0049, 41, 38, 0), (0x0025, 42, 39, 0), (0x0015, 43, 40, 0), (0x0009, 44, 41, 0), (0x0005, 45, 42, 0),
      (0x0001, 45, 43, 0), (0x5601, 46, 46, 0)]


class Ctx:
    # A context array: the state index and the MPS for each context.
    def __init__(s, n):
        s.i = [0] * n
        s.m = [0] * n


class MQ:
    def __init__(s):
        s.a, s.c, s.ct = 0x8000, 0, 12     # INITENC
        s.out = bytearray([0])             # the byte "before" the first (BP = BPST - 1)

    def enc(s, cx, k, d):
        qe, nmps, nlps, sw = QE[cx.i[k]]
        s.a -= qe
        if d == cx.m[k]:                   # CODEMPS
            if s.a & 0x8000:
                s.c += qe
                return
            if s.a < qe:
                s.a = qe
            else:
                s.c += qe
            cx.i[k] = nmps
        else:                              # CODELPS
            if s.a < qe:
                s.c += qe
            else:
                s.a = qe
            if sw:
                cx.m[k] ^= 1
            cx.i[k] = nlps
        while True:                        # RENORME
            s.a <<= 1
            s.c <<= 1
            s.ct -= 1
            if s.ct == 0:
                s.byteout()
            if s.a & 0x8000:
                break

    def byteout(s):
        o = s.out
        if o[-1] == 0xFF:                  # bit stuffing after 0xFF: 7 bits follow
            o.append((s.c >> 20) & 0xFF)
            s.c &= 0xFFFFF
            s.ct = 7
        elif s.c < 0x8000000:
            o.append((s.c >> 19) & 0xFF)
            s.c &= 0x7FFFF
            s.ct = 8
        else:                              # a carry into the previous byte
            o[-1] += 1
            if o[-1] == 0xFF:
                s.c &= 0x7FFFFFF
                o.append((s.c >> 20) & 0xFF)
                s.c &= 0xFFFFF
                s.ct = 7
            else:
                o.append((s.c >> 19) & 0xFF)
                s.c &= 0x7FFFF
                s.ct = 8

    def flush(s):
        # FLUSH with SETBITS, then the 0xFF 0xAC terminator (7.4 / E.2.9).
        t = s.c + s.a
        s.c |= 0xFFFF
        if s.c >= t:
            s.c -= 0x8000
        s.c <<= s.ct
        s.byteout()
        s.c <<= s.ct
        s.byteout()
        if s.out[-1] != 0xFF:
            s.out.append(0xFF)
        s.out.append(0xAC)
        return bytes(s.out[1:])

# --------------------------------------- integer arithmetic coding (Annex A)

class IntCtx:
    # One of IADH, IADW, IAEX, IAAI, IADT, IAFS, IADS, IAIT, IARI, IARDW...:
    # 512 contexts, PREV as in A.3 (9 bits, with bit 8 kept once set).
    def __init__(s):
        s.cx = Ctx(512)

    def enc(s, mq, v):                      # v None = OOB
        prev = [1]

        def bit(b):
            mq.enc(s.cx, prev[0], b)
            p = prev[0]
            prev[0] = (p << 1) | b if p < 256 else (((p << 1) | b) & 511) | 256

        def bits(v, n):
            for k in range(n - 1, -1, -1):
                bit((v >> k) & 1)
        sign, mag = (1, 0) if v is None else (1 if v < 0 else 0, abs(v))
        bit(sign)
        for limit, prefix, n, base in ((4, [], 2, 0), (20, [1], 4, 4), (84, [1, 1], 6, 20),
                                       (340, [1, 1, 1], 8, 84), (4436, [1, 1, 1, 1], 12, 340),
                                       (None, [1, 1, 1, 1], 32, 4436)):
            if limit is None or mag < limit:
                for b in prefix:
                    bit(b)
                if limit is not None:
                    bit(0)
                else:
                    bit(1)
                bits(mag - base, n)
                return


class IaidCtx:
    # A.3: SBSYMCODELEN bits, MSB first, PREV = (PREV << 1) | bit from 1.
    def __init__(s, codelen):
        s.cx, s.len = Ctx(1 << (codelen + 1)), codelen

    def enc(s, mq, code):
        prev = 1
        for k in range(s.len - 1, -1, -1):
            b = (code >> k) & 1
            mq.enc(s.cx, prev, b)
            prev = (prev << 1) | b

# --------------------------------------------------- generic region (6.2)

NOMINAL_AT = {0: [(3, -1), (-3, -1), (2, -2), (-2, -2)], 1: [(3, -1)], 2: [(2, -1)], 3: [(2, -1)]}
SLTP_CTX = {0: 0x9B25, 1: 0x0795, 2: 0x00E5, 3: 0x0195}   # 6.2.5.7, figures 8-11


def gb_template(t, at):
    # Context pixels, most significant bit first (Figures 3-6); the AT pixels
    # sit where their nominal positions are.
    if t == 0:
        a1, a2, a3, a4 = at
        return [a4, (-1, -2), (0, -2), (1, -2), a3, a2, (-2, -1), (-1, -1), (0, -1), (1, -1), (2, -1), a1,
                (-4, 0), (-3, 0), (-2, 0), (-1, 0)]
    a1 = at[0]
    if t == 1:
        return [(-1, -2), (0, -2), (1, -2), (2, -2), (-2, -1), (-1, -1), (0, -1), (1, -1), (2, -1), a1,
                (-3, 0), (-2, 0), (-1, 0)]
    if t == 2:
        return [(-1, -2), (0, -2), (1, -2), (-2, -1), (-1, -1), (0, -1), (1, -1), a1, (-2, 0), (-1, 0)]
    return [(-3, -1), (-2, -1), (-1, -1), (0, -1), (1, -1), a1, (-4, 0), (-3, 0), (-2, 0), (-1, 0)]


def encode_generic(mq, cx, bm, t, at, tpgdon=False):
    P = 130
    pad = padded(bm, P)
    offs = gb_template(t, at)
    ltp = 0
    for y in range(bm.h):
        if tpgdon:                          # 6.2.5.7: a row equal to the one above is skipped
            above = bm.rows[y - 1] if y else bytearray(bm.w)
            same = int(bm.rows[y] == above)
            mq.enc(cx, SLTP_CTX[t], same ^ ltp)
            ltp = same
            if same:
                continue
        rws = [(pad[y + P + dy], P + dx) for dx, dy in offs]
        row = bm.rows[y]
        for x in range(bm.w):
            c = 0
            for r, o in rws:
                c = (c << 1) | r[x + o]
            mq.enc(cx, c, row[x])

# ------------------------------------------------- refinement region (6.3)

def gr_template(t, at):
    # (source, dx, dy), most significant bit first: 'c' = the bitmap being
    # coded, 'r' = the reference (Figures 12 and 13).
    a1, a2 = at
    if t == 0:
        return [('c', 0, -1), ('c', 1, -1), ('c', -1, 0), ('c',) + a1,
                ('r', 1, 1), ('r', 0, 1), ('r', -1, 1), ('r', 1, 0), ('r', 0, 0), ('r', -1, 0),
                ('r', 1, -1), ('r', 0, -1), ('r',) + a2]
    return [('c', -1, -1), ('c', 0, -1), ('c', 1, -1), ('c', -1, 0),
            ('r', 1, 1), ('r', 0, 1), ('r', 1, 0), ('r', 0, 0), ('r', -1, 0), ('r', 0, -1)]


def encode_refine(mq, cx, bm, ref, dx, dy, t, tpgron=False, at=((-1, -1), (-1, -1))):
    # The reference pixel for (x, y) is ref(x - dx, y - dy) (6.3.5.3).
    P = 40 + abs(dx) + abs(dy) + abs(bm.w - ref.w) + abs(bm.h - ref.h)
    cur, rf = padded(bm, P), padded(ref, P)
    offs = gr_template(t, at)
    sltp = 0                                # the context of 6.3.5.6: only the reference's center set
    for src, ox, oy in offs:
        sltp = (sltp << 1) | (1 if (src, ox, oy) == ('r', 0, 0) else 0)
    ltp = 0

    def uniform(x, y):                      # 6.3.5.6: the 3x3 reference neighborhood all one value
        v = rf[y - dy + P][x - dx + P]
        for j in (-1, 0, 1):
            for i in (-1, 0, 1):
                if rf[y - dy + j + P][x - dx + i + P] != v:
                    return None
        return v
    for y in range(bm.h):
        if tpgron:
            ok = all(uniform(x, y) in (None, bm.rows[y][x]) for x in range(bm.w))
            mq.enc(cx, sltp, int(ok) ^ ltp)
            ltp = int(ok)
        for x in range(bm.w):
            if ltp and uniform(x, y) is not None:
                continue
            c = 0
            for src, ox, oy in offs:
                if src == 'c':
                    c = (c << 1) | cur[y + P + oy][x + P + ox]
                else:
                    c = (c << 1) | rf[y - dy + P + oy][x - dx + P + ox]
            mq.enc(cx, c, bm.rows[y][x])

# --------------------------------------------------------------- MMR (6.2.6)

def _tiff_strip(image):
    buffer = io.BytesIO()
    info = TiffImagePlugin.ImageFileDirectory_v2()
    info[278] = image.height
    image.save(buffer, format='TIFF', compression='group4', tiffinfo=info)
    buffer.seek(0)
    tiff = Image.open(buffer)
    offsets, counts = tiff.tag_v2[273], tiff.tag_v2[279]
    assert len(offsets) == 1 and tiff.tag_v2.get(266, 1) == 1
    return buffer.getvalue()[offsets[0]:offsets[0] + counts[0]], tiff.tag_v2[262]


def mmr_bytes(bm):
    # T.6 coding by libtiff. JBIG2's MMR codes the 1 (black) pixels as the
    # "black" runs, so the Pillow white must be the JBIG2 0: with
    # photometric 0 libtiff's coded white is Pillow's white, with
    # photometric 1 it is Pillow's black (as the CCITT tool reads it).
    def make(flip):
        img = Image.new('1', (bm.w, bm.h))
        img.putdata([(v ^ 1 if not flip else v) for r in bm.rows for v in r])
        return img
    data, photometric = _tiff_strip(make(False))
    if photometric == 1:
        data, photometric = _tiff_strip(make(True))
    return data

# ----------------------------------------------------- Huffman coding (B)

class Bits:
    def __init__(s):
        s.b, s.acc, s.n = bytearray(), 0, 0

    def put(s, v, n):
        for k in range(n - 1, -1, -1):
            s.acc = (s.acc << 1) | ((v >> k) & 1)
            s.n += 1
            if s.n == 8:
                s.b.append(s.acc)
                s.acc, s.n = 0, 0

    def align(s):
        while s.n:
            s.put(0, 1)

    def bytes(s):
        s.align()
        return bytes(s.b)


def N(low, plen, rlen):
    return (low, plen, rlen, 'n')


# Standard tables B.1, B.2, B.4, B.6, B.8, B.11, B.14: (low, prefix length,
# range length, kind) with kinds n = normal, lo = lower range (value = low -
# offset), up = upper range, oob. Order as in the tables: it fixes the codes.
B1 = [N(0, 1, 4), N(16, 2, 8), N(272, 3, 16), (65808, 3, 32, 'up')]
B2 = [N(0, 1, 0), N(1, 2, 0), N(2, 3, 0), N(3, 4, 3), N(11, 5, 6), (75, 6, 32, 'up'), (0, 6, 0, 'oob')]
B4 = [N(1, 1, 0), N(2, 2, 0), N(3, 3, 0), N(4, 4, 3), N(12, 5, 6), (76, 5, 32, 'up')]
B6 = [N(-2048, 5, 10), N(-1024, 4, 9), N(-512, 4, 8), N(-256, 4, 7), N(-128, 5, 6), N(-64, 5, 5), N(-32, 4, 5),
      N(0, 2, 7), N(128, 3, 7), N(256, 3, 8), N(512, 4, 9), N(1024, 4, 10), (-2049, 6, 32, 'lo'),
      (2048, 6, 32, 'up')]
B8 = [N(-15, 8, 3), N(-7, 9, 1), N(-5, 8, 1), N(-3, 9, 0), N(-2, 7, 0), N(-1, 4, 0), N(0, 2, 1), N(2, 5, 0),
      N(3, 6, 0), N(4, 3, 4), N(20, 6, 1), N(22, 4, 4), N(38, 4, 5), N(70, 5, 6), N(134, 5, 7), N(262, 6, 7),
      N(390, 7, 8), N(646, 6, 10), (-16, 9, 32, 'lo'), (1670, 9, 32, 'up'), (0, 2, 0, 'oob')]
B11 = [N(1, 1, 0), N(2, 2, 1), N(4, 4, 0), N(5, 4, 1), N(7, 5, 1), N(9, 5, 2), N(13, 6, 2), N(17, 7, 2),
       N(21, 7, 3), N(29, 7, 4), N(45, 7, 5), N(77, 7, 6), (141, 7, 32, 'up')]


class Huff:
    def __init__(s, lines):
        s.lines = lines
        s.codes = [0] * len(lines)
        # B.3: canonical codes, shortest prefix first, table order within a length.
        first, counts = 0, {}
        for l in lines:
            counts[l[1]] = counts.get(l[1], 0) + 1
        counts[0] = 0
        for length in range(1, max(counts) + 1):
            first = (first + counts.get(length - 1, 0)) * 2
            code = first
            for i, l in enumerate(lines):
                if l[1] == length:
                    s.codes[i] = code
                    code += 1

    def put(s, bw, v):                      # v None = OOB
        for i, (low, plen, rlen, kind) in enumerate(s.lines):
            if plen == 0:
                continue
            if v is None:
                hit = kind == 'oob'
            elif kind == 'n':
                hit = low <= v < low + (1 << rlen)
            elif kind == 'lo':
                hit = v <= low
            else:
                hit = kind == 'up' and v >= low
            if hit:
                bw.put(s.codes[i], plen)
                if kind != 'oob':
                    bw.put(low - v if kind == 'lo' else v - low, rlen)
                return
        raise ValueError('value %r not in table' % (v,))


def huffman_lengths(counts):
    heap = [(c, i, [i]) for i, c in enumerate(counts)]
    heapq.heapify(heap)
    depth = [0] * len(counts)
    while len(heap) > 1:
        c1, i1, m1 = heapq.heappop(heap)
        c2, i2, m2 = heapq.heappop(heap)
        for m in m1 + m2:
            depth[m] += 1
        heapq.heappush(heap, (c1 + c2, min(i1, i2), m1 + m2))
    return [max(d, 1) for d in depth]

# ------------------------------------------------------------ segments (7)

def region_info(w, h, x, y, op):
    return struct.pack('>IIIIB', w, h, x, y, op)


class Doc:
    # Globals (page association 0) and the page's own segments, numbered
    # in order so referred-to segments always come first.
    def __init__(s, width, height):
        s.width, s.height = width, height
        s.glob, s.page, s.n = [], [], 0

    def add(s, typ, data, refs=(), glob=False, length=None):
        n = s.n
        s.n += 1
        retain = 1 | sum(2 << i for i in range(len(refs)))
        assert len(refs) <= 4
        head = struct.pack('>IBB', n, typ, (len(refs) << 5) | retain) + bytes(refs)   # refs < 256: one byte each
        head += struct.pack('>BI', 0 if glob else 1, len(data) if length is None else length)
        (s.glob if glob else s.page).append(head + data)
        return n

    def page_info(s, height=None, flags=0, striping=0):
        s.add(48, struct.pack('>IIIIBH', s.width, s.height if height is None else height, 0, 0, flags, striping))

    def embedded(s):
        return b''.join(s.glob), b''.join(s.page)

    def file(s):
        end = struct.pack('>IBBBI', s.n, 49, 0, 1, 0)
        return (b'\x97JB2\r\n\x1a\n' + b'\x01' + struct.pack('>I', 1) + b''.join(s.glob) + b''.join(s.page) + end)


def generic_data(bm, t=0, at=None, tpgdon=False, mmr=False):
    # 7.4.6.2 flags + AT bytes, then the coded data.
    at = at or NOMINAL_AT[t]
    head = bytes([int(mmr) | (t << 1) | (int(tpgdon) << 3)])
    if mmr:
        return head + mmr_bytes(bm)
    head += b''.join(struct.pack('>bb', *p) for p in at)
    mq, cx = MQ(), Ctx(1 << 16)
    encode_generic(mq, cx, bm, t, at, tpgdon)
    return head + mq.flush()


def refine_data(bm, ref, t, tpgron, dx=0, dy=0):
    # 7.4.7.2: flags, AT (4 bytes for template 0), data.
    head = bytes([t | (int(tpgron) << 1)]) + (struct.pack('>bbbb', -1, -1, -1, -1) if t == 0 else b'')
    mq, cx = MQ(), Ctx(1 << 13)
    encode_refine(mq, cx, bm, ref, dx, dy, t, tpgron)
    return head + mq.flush()

# ------------------------------------------------ symbols and text (6.4, 6.5)

def clog2(n):
    return max(0, (n - 1).bit_length())


def symdict_arith(syms, nin=0, t=0, refagg=None, rt=0, exports=None):
    # 6.5. syms are sorted by (height, width). refagg, when given, maps a
    # new symbol's index to (input symbol id, dx, dy): REFAGGNINST = 1.
    at = NOMINAL_AT[t]
    flags = (int(refagg is not None) << 1) | (t << 10) | (rt << 12)
    head = struct.pack('>H', flags) + b''.join(struct.pack('>bb', *p) for p in at)
    if refagg is not None and rt == 0:
        head += struct.pack('>bbbb', -1, -1, -1, -1)
    n = len(syms)
    nex = nin + n if exports is None else exports
    head += struct.pack('>II', nex, n)
    mq, gb, gr = MQ(), Ctx(1 << 16), Ctx(1 << 13)
    iadh, iadw, iaex, iaai, iardx, iardy = [IntCtx() for _ in range(6)]
    iaid = IaidCtx(clog2(nin + n))
    hc, i = 0, 0
    while i < n:
        h = syms[i].h
        iadh.enc(mq, h - hc)
        hc, w = h, 0
        while i < n and syms[i].h == h:
            iadw.enc(mq, syms[i].w - w)
            w = syms[i].w
            if refagg is None:
                encode_generic(mq, gb, syms[i], t, at, False)
            else:                           # 6.5.8.2.2 with REFAGGNINST = 1
                sid, dx, dy = refagg[i]
                iaai.enc(mq, 1)
                iaid.enc(mq, sid)
                iardx.enc(mq, dx)
                iardy.enc(mq, dy)
                encode_refine(mq, gr, syms[i], refagg['bm'][sid], dx, dy, rt)
            i += 1
        iadw.enc(mq, None)                  # OOB closes the height class
    iaex.enc(mq, 0)                         # 6.5.10: runs of not-exported, exported ...
    iaex.enc(mq, nex)
    return head + mq.flush()


def symdict_huff(syms, mmr=False):
    # 6.5 with SDHUFF = 1: tables B.4 (DH), B.2 (DW), B.1 (BMSIZE, export runs).
    head = struct.pack('>H', 1) + struct.pack('>II', len(syms), len(syms))
    bw = Bits()
    hc, i, tables = 0, 0, (Huff(B4), Huff(B2), Huff(B1))
    while i < len(syms):
        h = syms[i].h
        tables[0].put(bw, h - hc)
        hc, w, first = h, 0, i
        while i < len(syms) and syms[i].h == h:
            tables[1].put(bw, syms[i].w - w)
            w = syms[i].w
            i += 1
        tables[1].put(bw, None)
        coll = Bm(sum(s.w for s in syms[first:i]), h)    # 6.5.9: the symbols side by side
        x = 0
        for s in syms[first:i]:
            coll.blit(s, x, 0)
            x += s.w
        if mmr:
            data = mmr_bytes(coll)
            tables[2].put(bw, len(data))
            bw.align()
            bw.b += data
        else:
            tables[2].put(bw, 0)
            bw.align()
            bw.b += coll.packed()
    tables[2].put(bw, 0)
    tables[2].put(bw, len(syms))
    return head + bw.bytes()


REFCORNER = {'BL': 0, 'TL': 1, 'BR': 2, 'TR': 3}


def text_strips(insts, corner, transposed, S):
    # 6.4.5: per instance the reference corner's S and T and the extent along S.
    # inst = dict(id, x, y, bm, rd). Returns strips [(k, [(s0, L, curt, inst)])].
    strips = {}
    for it in insts:
        b = it['bm']
        if not transposed:
            s0, ext_s, t_low, ext_t = it['x'], b.w, it['y'], b.h
            high_t = corner in ('BL', 'BR')
        else:
            s0, ext_s, t_low, ext_t = it['y'], b.h, it['x'], b.w
            high_t = corner in ('TR', 'BR')
        t = t_low + ext_t - 1 if high_t else t_low
        strips.setdefault(t // S, []).append((s0, ext_s, t % S, it))
    return [(k, sorted(strips[k], key=lambda e: e[0])) for k in sorted(strips)]


def text_region(syms, insts, w, h, huff=False, logstrips=0, corner='TL', transposed=False, dsoffset=0,
                refine=False, rt=0):
    # 7.4.3: flags, [Huffman flags], [refinement AT], SBNUMINSTANCES, [ID codes], data.
    S = 1 << logstrips
    flags = (int(huff)) | (int(refine) << 1) | (logstrips << 2) | (REFCORNER[corner] << 4) \
        | (int(transposed) << 6) | ((dsoffset & 31) << 10) | (rt << 15)
    head = struct.pack('>H', flags)
    if huff:
        head += struct.pack('>H', 0)        # FS B.6, DS B.8, DT B.11, RD* B.14, RSIZE B.1
    if refine and rt == 0:
        head += struct.pack('>bbbb', -1, -1, -1, -1)
    head += struct.pack('>I', len(insts))
    strips = text_strips(insts, corner, transposed, S)
    init = 1 if huff else 0                 # B.11 starts at 1; STRIPT starts as -init * SBSTRIPS
    if not huff:
        mq, gr = MQ(), Ctx(1 << 13)
        iadt, iafs, iads, iait, iari, iardw, iardh, iardx, iardy = [IntCtx() for _ in range(9)]
        iaid = IaidCtx(clog2(len(syms)))
        iadt.enc(mq, init)
    else:
        bw = Bits()
        counts = [1] * len(syms)
        for it in insts:
            counts[it['id']] += 1
        lens = huffman_lengths(counts)
        codes = Huff([(i, l, 0, 'n') for i, l in enumerate(lens)])
        head += id_table(lens)
        tdt, tfs, tds = Huff(B11), Huff(B6), Huff(B8)
        tdt.put(bw, init)
    cur, firsts = -init, 0                  # FIRSTS runs on across strips (6.4.5 step 3)
    for k, items in strips:
        (iadt.enc(mq, k - cur) if not huff else tdt.put(bw, k - cur))
        cur = k
        curs = 0
        for n, (s0, L, curt, it) in enumerate(items):
            if n == 0:
                (iafs.enc(mq, s0 - firsts) if not huff else tfs.put(bw, s0 - firsts))
                firsts = s0
            else:
                d = s0 - curs - dsoffset
                (iads.enc(mq, d) if not huff else tds.put(bw, d))
            curs = s0 + L - 1
            if S > 1:
                (iait.enc(mq, curt) if not huff else bw.put(curt, logstrips))
            if huff:
                bw.put(codes.codes[it['id']], lens[it['id']])
            else:
                iaid.enc(mq, it['id'])
                if refine:
                    rd = it['rd']
                    iari.enc(mq, int(rd is not None))
                    if rd:                  # 6.4.11: refinement of the symbol
                        rdw, rdh, rdx, rdy = rd
                        for ctx, v in ((iardw, rdw), (iardh, rdh), (iardx, rdx), (iardy, rdy)):
                            ctx.enc(mq, v)
                        encode_refine(mq, gr, it['bm'], syms[it['id']], (rdw >> 1) + rdx, (rdh >> 1) + rdy, rt)
        (iads.enc(mq, None) if not huff else tds.put(bw, None))
    return head + (bw.bytes() if huff else mq.flush())


def id_table(lens):
    # 7.4.3.1.7: 35 four-bit run-code lengths, then the symbols' code lengths
    # in run codes (32 = repeat previous 3-6x, 33 = 0 x 3-10, 34 = 0 x 11-138).
    seq, i, n = [], 0, len(lens)
    while i < n:
        L, j = lens[i], i
        while j < n and lens[j] == L:
            j += 1
        run = j - i
        if L == 0 and run >= 11:
            take = min(run, 138)
            seq.append((34, take - 11, 7))
        elif L == 0 and run >= 3:
            take = min(run, 10)
            seq.append((33, take - 3, 3))
        elif L and run >= 4:
            take = min(run, 7)
            seq += [(L, 0, 0), (32, take - 4, 2)]   # the literal, then 3-6 repeats
        else:
            take = 1
            seq.append((L, 0, 0))
        i += take
    used = sorted({c for c, _, _ in seq})
    plen = max(1, clog2(len(used)))
    rl = [plen if k in used else 0 for k in range(35)]
    table = Huff([(k, rl[k], 0, 'n') for k in range(35)])
    bw = Bits()
    for k in range(35):
        bw.put(rl[k], 4)
    for c, extra, nb in seq:
        bw.put(table.codes[c], plen)
        if nb:
            bw.put(extra, nb)
    return bw.bytes()

# ---------------------------------------------------------- halftone (6.6, 6.7)

BAYER = [[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]


def patterns(n=16, size=4):
    # Pattern k has k black pixels in an ordered-dither order.
    pats = []
    for k in range(n):
        p = Bm(size, size)
        for y in range(size):
            for x in range(size):
                p.rows[y][x] = int(BAYER[y][x] < k)
        pats.append(p)
    return pats


def pattern_dict(pats, mmr):
    # 7.4.4: flags, HDPW, HDPH, GRAYMAX, then the collective bitmap, coded as
    # a generic region whose first AT pixel is (-HDPW, 0).
    pw, ph = pats[0].w, pats[0].h
    coll = Bm(pw * len(pats), ph)
    for i, p in enumerate(pats):
        coll.blit(p, i * pw, 0)
    t = 0
    head = struct.pack('>BBBI', int(mmr) | (t << 1), pw, ph, len(pats) - 1)
    if mmr:
        return head + mmr_bytes(coll)
    at = [(-pw, 0), (-3, -1), (2, -2), (-2, -2)]
    mq, cx = MQ(), Ctx(1 << 16)
    encode_generic(mq, cx, coll, t, at)
    return head + mq.flush()


def halftone_render(pats, gray, gw, gh, hgx, hgy, hrx, hry, w, h):
    # 6.6.5.2: the pattern for GI[ng, mg] at ((HGX + mg*HRY + ng*HRX) >> 8,
    # (HGY + mg*HRX - ng*HRY) >> 8), combined with OR.
    out = Bm(w, h)
    for mg in range(gh):
        for ng in range(gw):
            out.blit(pats[gray[mg][ng]], (hgx + mg * hry + ng * hrx) >> 8, (hgy + mg * hrx - ng * hry) >> 8, 0)
    return out


def halftone_data(gray, gw, gh, nbits, mmr, hgx, hgy, hrx, hry, t=0):
    # 7.4.5 header; the gray-scale image of Annex C: bit planes coded from the
    # most significant, each plane Gray-coded (plane J coded as bit J XOR bit J+1).
    head = struct.pack('>BIIiiHH', int(mmr) | (t << 1), gw, gh, hgx, hgy, hrx, hry)
    planes = []
    for j in range(nbits - 1, -1, -1):
        p = Bm(gw, gh)
        for y in range(gh):
            for x in range(gw):
                v = (gray[y][x] >> j) & 1
                if j < nbits - 1:
                    v ^= (gray[y][x] >> (j + 1)) & 1
                p.rows[y][x] = v
        planes.append(p)
    if mmr:
        return head + b''.join(mmr_bytes(p) for p in planes)
    at = [(3 if t <= 1 else 2, -1), (-3, -1), (2, -2), (-2, -2)]
    mq, cx = MQ(), Ctx(1 << 16)
    for p in planes:                        # one arithmetic coder and one context set for all planes
        encode_generic(mq, cx, p, t, at)
    return head + mq.flush()

# ------------------------------------------------------------------ pictures

def pictures():
    rng = random.Random(85)
    text = Image.new('1', (600, 260), 1)
    draw = ImageDraw.Draw(text)
    for i, line in enumerate(['Sashfold reads JBIG2 pictures.', 'THE QUICK BROWN FOX 0123456789',
                              'jumps over the lazy dog; a scanned page.', '#$%&*()[]{} <=> ~ @ ^ | \\ /']):
        draw.text((10, 10 + i * 60), line, fill=0, font_size=32)
    draw.rectangle([400, 5, 590, 30], fill=0)
    yield 'text', from_pil(text)
    noise = Image.new('1', (301, 157), 1)
    noise.putdata([rng.choice((0, 1, 1)) for _ in range(301 * 157)])
    yield 'noise', from_pil(noise)
    shapes = Image.new('1', (250, 250), 1)
    draw = ImageDraw.Draw(shapes)
    for r in range(120, 0, -12):
        draw.ellipse([125 - r, 125 - r, 125 + r, 125 + r], fill=(r // 12) % 2)
    draw.polygon([(0, 249), (249, 0), (249, 249)], outline=0)
    yield 'shapes', from_pil(shapes)


def fixture_picture():
    # 61 x 23: a width past a byte's end, noise beside long runs (as the CCITT fixtures).
    rng = random.Random(5)
    bm = Bm(61, 23)
    for y in range(23):
        for x in range(61):
            bm.rows[y][x] = rng.choice((1, 0, 0)) if y < 12 else (1 if y < 17 else 0)
    return bm


def glyph_scene(w, h, fs, lines, per, seed):
    # Distinct glyph bitmaps cropped from drawn letters, sorted by (height,
    # width) as a symbol dictionary orders them, and instances placed on lines.
    rng = random.Random(seed)
    glyphs = []
    for ch in 'aeHlgyTo':
        img = Image.new('L', (fs * 2, fs * 2), 0)
        ImageDraw.Draw(img).text((2, 2), ch, fill=255, font_size=fs)
        box = img.point(lambda p: 255 if p > 127 else 0).getbbox()
        g = from_pil(img.crop(box).point(lambda p: 0 if p > 127 else 255).convert('1'))
        if all(g.rows != o.rows for o in glyphs):
            glyphs.append(g)
    glyphs = sorted(glyphs[:6], key=lambda g: (g.h, g.w, g.packed()))
    insts, maxh = [], max(g.h for g in glyphs)
    for line in range(lines):
        x = 3
        for _ in range(per):
            gid = rng.randrange(len(glyphs))
            g = glyphs[gid]
            y = 2 + line * (h // lines) + (maxh - g.h) + rng.randint(0, 2)
            assert x + g.w <= w and y + g.h <= h, 'scene too small'
            insts.append(dict(id=gid, x=x, y=y, bm=g, rd=None))
            x += g.w + rng.randint(1, 3)
    return glyphs, insts, w, h


def compose(insts, w, h):
    c = Bm(w, h)
    for it in insts:
        c.blit(it['bm'], it['x'], it['y'], 0)
    return c


def gray_scene(gw, gh):
    return [[((x + y) * 15 // (gw + gh - 2) + (x // 5 + y // 4) % 3) % 16 for x in range(gw)] for y in range(gh)]

# ------------------------------------------------------------------ variants

REFINE_VIA_PAGE = True      # jbig2dec 0.20 does not handle intermediate regions (NYI), so refine the page


def b_generic(pic, **kw):
    doc = Doc(pic.w, pic.h)
    doc.page_info()
    data = region_info(pic.w, pic.h, 0, 0, 0) + generic_data(pic, **{k: v for k, v in kw.items() if k != 'unknown'})
    if kw.get('unknown'):                   # 7.2.7: row count follows the terminator
        doc.add(38, data + struct.pack('>I', pic.h), length=0xFFFFFFFF)
    else:
        doc.add(39, data)
    return pic, doc


def noisy(pic, seed=7):
    rng = random.Random(seed)
    b = pic.copy()
    for _ in range(max(4, pic.w * pic.h // 33)):
        x, y = rng.randrange(pic.w), rng.randrange(pic.h)
        b.rows[y][x] ^= 1
    return b


def b_refine(pic, t, tpgron):
    doc = Doc(pic.w, pic.h)
    doc.page_info()
    rough = noisy(pic)
    info = region_info(pic.w, pic.h, 0, 0, 4)       # combination operator REPLACE
    if REFINE_VIA_PAGE:                    # 7.4.7.5: no referred-to segment: the page is the reference
        doc.add(39, region_info(pic.w, pic.h, 0, 0, 0) + generic_data(rough))
        doc.add(43, info + refine_data(pic, rough, t, tpgron))
    else:
        n = doc.add(36, region_info(pic.w, pic.h, 0, 0, 0) + generic_data(rough))   # intermediate
        doc.add(43, info + refine_data(pic, rough, t, tpgron), refs=[n])
    return pic, doc


def b_stripes(pic):
    doc = Doc(pic.w, pic.h)
    half = pic.h // 2
    doc.page_info(height=0xFFFFFFFF, striping=0x8000 | (pic.h - half))
    doc.add(39, region_info(pic.w, half, 0, 0, 0) + generic_data(pic.crop(0, 0, pic.w, half)))
    doc.add(50, struct.pack('>I', half - 1))
    doc.add(39, region_info(pic.w, pic.h - half, 0, half, 0) + generic_data(pic.crop(0, half, pic.w, pic.h - half)))
    doc.add(50, struct.pack('>I', pic.h - 1))
    return pic, doc


def b_default_black(pic):
    # Page default pixel 1, default operator XOR (flags bits 2, 3-4): black XOR region.
    doc = Doc(pic.w, pic.h)
    doc.page_info(flags=0x04 | (2 << 3))
    inv = pic.inverted()
    doc.add(39, region_info(pic.w, pic.h, 0, 0, 2) + generic_data(inv))
    return pic, doc


def b_two_regions(pic):
    doc = Doc(pic.w, pic.h)
    doc.page_info()
    a = pic.crop(0, 0, pic.w * 2 // 3, pic.h * 2 // 3)
    b = pic.crop(pic.w // 4, pic.h // 4, pic.w * 3 // 4, pic.h * 3 // 4)
    bx, by = pic.w // 3, pic.h // 3                  # B runs past the page's right and bottom edge
    doc.add(39, region_info(a.w, a.h, 0, 0, 0) + generic_data(a))
    doc.add(39, region_info(b.w, b.h, bx, by, 0) + generic_data(b, t=1, tpgdon=True))
    truth = Bm(pic.w, pic.h)
    truth.blit(a, 0, 0)
    truth.blit(b, bx, by)
    return truth, doc


def b_symbol(scene, huff=False, mmr=False, **kw):
    glyphs, insts, w, h = scene
    doc = Doc(w, h)
    d = doc.add(0, symdict_huff(glyphs, mmr) if huff else symdict_arith(glyphs), glob=True)
    doc.page_info()
    doc.add(7, region_info(w, h, 0, 0, 0) + text_region(glyphs, insts, w, h, huff=huff, **kw), refs=[d])
    return compose(insts, w, h), doc


def b_refagg(scene):
    glyphs, insts, w, h = scene
    rng = random.Random(3)
    # Two dictionaries: the base symbols, then new ones each coded as a
    # refinement of one base symbol (REFAGGNINST = 1) with an offset.
    new = []
    for k, (dx, dy) in zip((0, 2, 3, 5), ((0, 0), (1, 1), (0, 0), (1, 0))):
        g = glyphs[k]
        b = Bm(g.w + dx, g.h + dy)
        b.blit(g, dx, dy)
        for _ in range(2):
            b.rows[rng.randrange(b.h)][rng.randrange(b.w)] ^= 1
        new.append((b, k, dx, dy))
    new.sort(key=lambda e: (e[0].h, e[0].w))
    nsyms = [e[0] for e in new]
    plan = {i: (e[1], e[2], e[3]) for i, e in enumerate(new)}
    plan['bm'] = glyphs
    doc = Doc(w, h)
    d1 = doc.add(0, symdict_arith(glyphs), glob=True)
    d2 = doc.add(0, symdict_arith(nsyms, nin=len(glyphs), refagg=plan), refs=[d1], glob=True)
    allsyms = glyphs + nsyms
    insts = [dict(it) for it in insts]
    for i, it in enumerate(insts):         # use the new symbols too, and refine some instances
        if i % 4 == 1:
            it['id'] = len(glyphs) + (i // 4) % len(nsyms)
            it['bm'] = allsyms[it['id']]
        if i % 5 == 2:
            base = allsyms[it['id']]
            if i % 2:
                b, it['rd'] = base.copy(), (0, 0, 0, 0)
            else:
                b = Bm(base.w + 1, base.h + 1)
                b.blit(base, 1, 1)
                it['rd'] = (1, 1, 1, 1)
            b.rows[rng.randrange(b.h)][rng.randrange(b.w)] ^= 1
            it['bm'] = b
    doc.page_info()
    doc.add(7, region_info(w, h, 0, 0, 0) + text_region(allsyms, insts, w, h, refine=True), refs=[d2])
    return compose(insts, w, h), doc


def b_halftone(gray, mmr):
    gw, gh = len(gray[0]), len(gray)
    pats = patterns()
    w, h = gw * 4, gh * 4
    hrx, hry, hgx, hgy = 4 << 8, 0, 0, 0
    doc = Doc(w, h)
    d = doc.add(16, pattern_dict(pats, mmr), glob=True)
    doc.page_info()
    doc.add(23, region_info(w, h, 0, 0, 0) + halftone_data(gray, gw, gh, 4, mmr, hgx, hgy, hrx, hry), refs=[d])
    return halftone_render(pats, gray, gw, gh, hgx, hgy, hrx, hry, w, h), doc


AT_ODD = [(5, -3), (-7, -2), (1, -4), (-6, 0)]
IMG_VARIANTS = [
    ('generic-t0', lambda p: b_generic(p, t=0)),
    ('generic-t0-tpgdon', lambda p: b_generic(p, t=0, tpgdon=True)),
    ('generic-t1', lambda p: b_generic(p, t=1, tpgdon=True)),
    ('generic-t2', lambda p: b_generic(p, t=2, tpgdon=True)),
    ('generic-t3', lambda p: b_generic(p, t=3, tpgdon=True)),
    ('generic-t0-at', lambda p: b_generic(p, t=0, at=AT_ODD)),
    ('generic-mmr', lambda p: b_generic(p, mmr=True)),
    ('generic-unknown-length', lambda p: b_generic(p, t=0, unknown=True)),
    ('refine-t0', lambda p: b_refine(p, 0, False)),
    ('refine-t1', lambda p: b_refine(p, 1, False)),
    ('refine-tpgron', lambda p: b_refine(p, 0, True)),
    ('page-stripes', b_stripes),
    ('page-default-black', b_default_black),
    ('page-two-regions', b_two_regions),
]
GLYPH_VARIANTS = [
    ('symbol-arith', lambda s: b_symbol(s)),
    ('symbol-arith-strips', lambda s: b_symbol(s, logstrips=2)),
    ('symbol-arith-bl', lambda s: b_symbol(s, corner='BL')),
    ('symbol-arith-dsoffset', lambda s: b_symbol(s, dsoffset=-3, corner='TR')),
    ('symbol-transposed', lambda s: b_symbol(s, transposed=True, logstrips=1, corner='BR')),
    ('symbol-huffman', lambda s: b_symbol(s, huff=True, logstrips=1)),
    ('symbol-huffman-mmr', lambda s: b_symbol(s, huff=True, mmr=True)),
    ('symbol-refagg', b_refagg),
]
HALFTONE_VARIANTS = [('halftone', lambda g: b_halftone(g, False)), ('halftone-mmr', lambda g: b_halftone(g, True))]

# ------------------------------------------------------------------- output

def pdf_bytes(width, height, page, glob):
    content = b'q %d 0 0 %d 0 0 cm /Im Do Q' % (width, height)
    parms = b' /DecodeParms << /JBIG2Globals 6 0 R >>' if glob else b''
    objs = {
        1: b'<< /Type /Catalog /Pages 2 0 R >>',
        2: b'<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
        3: b'<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %d %d] /Resources << /XObject << /Im 5 0 R >> >> /Contents 4 0 R >>'
        % (width, height),
        4: b'<< /Length %d >>\nstream\n' % len(content) + content + b'\nendstream',
        5: (b'<< /Type /XObject /Subtype /Image /Width %d /Height %d /ColorSpace /DeviceGray /BitsPerComponent 1 '
            b'/Filter /JBIG2Decode' % (width, height)) + parms + b' /Length %d >>\nstream\n' % len(page) + page
        + b'\nendstream',
    }
    if glob:
        objs[6] = b'<< /Length %d >>\nstream\n' % len(glob) + glob + b'\nendstream'
    out = bytearray(b'%PDF-1.7\n%\xe2\xe3\xcf\xd3\n')
    offsets = {}
    for n in sorted(objs):
        offsets[n] = len(out)
        out += b'%d 0 obj\n' % n + objs[n] + b'\nendobj\n'
    x, count = len(out), len(objs) + 1
    out += b'xref\n0 %d\n0000000000 65535 f \n' % count
    for n in range(1, count):
        out += b'%010d 00000 n \n' % offsets[n]
    out += b'trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n' % (count, x)
    return bytes(out)


def write_outputs(out_dir, name, truth, doc):
    glob, page = doc.embedded()
    base = os.path.join(out_dir, name)
    open(base + '.pdf', 'wb').write(pdf_bytes(truth.w, truth.h, page, glob))
    open(base + '.jb2', 'wb').write(doc.file())
    open(base + '.pbm', 'wb').write(b'P4\n%d %d\n' % (truth.w, truth.h) + truth.packed())


def small_scenes():
    return glyph_scene(64, 28, 9, 2, 4, 11), gray_scene(8, 6)


def main(out_dir, only=None):
    os.makedirs(out_dir, exist_ok=True)
    jobs = []
    for pname, pic in pictures():
        jobs += [('jbig2-%s-%s' % (pname, v), lambda f=f, pic=pic: f(pic)) for v, f in IMG_VARIANTS]
    scene = glyph_scene(200, 90, 14, 3, 10, 21)
    jobs += [('jbig2-glyphs-%s' % v, lambda f=f: f(scene)) for v, f in GLYPH_VARIANTS]
    gray = gray_scene(40, 30)
    jobs += [('jbig2-gradient-%s' % v, lambda f=f: f(gray)) for v, f in HALFTONE_VARIANTS]
    for name, job in jobs:
        if only and only not in name:
            continue
        truth, doc = job()
        write_outputs(out_dir, name, truth, doc)
        print(name)


def fixtures(out_dir, only=None):
    os.makedirs(out_dir, exist_ok=True)
    pic, (scene, gray) = fixture_picture(), small_scenes()
    jobs = [(v, lambda f=f: f(pic)) for v, f in IMG_VARIANTS]
    jobs += [(v, lambda f=f: f(scene)) for v, f in GLYPH_VARIANTS]
    jobs += [(v, lambda f=f: f(gray)) for v, f in HALFTONE_VARIANTS]
    lines = []
    for v, job in jobs:
        if only and only not in v:
            continue
        truth, doc = job()
        glob, page = doc.embedded()
        base = os.path.join(out_dir, 'jbig2-' + v)
        open(base + '.bin', 'wb').write(page)
        if glob:
            open(base + '.globals', 'wb').write(glob)
        open(base + '.rows', 'wb').write(truth.packed())
        lines.append('jbig2-%s %d %d %d' % (v, truth.w, truth.h, int(bool(glob))))
    open(os.path.join(out_dir, 'jbig2-fixtures.txt'), 'w').write('\n'.join(lines) + '\n')


if __name__ == '__main__':
    args = sys.argv[1:]
    only = None
    if '--only' in args:                    # a name filter, for debugging one variant
        i = args.index('--only')
        only = args[i + 1]
        del args[i:i + 2]
    if args[0] == '--fixtures':
        fixtures(args[1], only)
    else:
        main(args[0], only)
