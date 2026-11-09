#!/usr/bin/env python3
# Writes src/media/H264Data.cpp: the tables of ITU-T H.264 a software
# decoder reads, taken from the freely published Recommendation itself
# (the PDF at itu.int, read with poppler's pdftotext):
#   - CABAC's context initialization values m and n for ctxIdx 0 to 1023
#     (Tables 9-12 to 9-33), for I slices and each cabac_init_idc;
#   - rangeTabLPS (Table 9-44) and the state transitions (Table 9-45);
#   - CAVLC's codes: coeff_token (Table 9-5), total_zeros (Tables 9-7, 9-8,
#     9-9) and run_before (Table 9-10).
# Each value is placed by the column it stands under on the page, so a
# cell the page wraps over two lines (a sign above its digits) is read
# whole. Every context gets a value exactly once; every code table is
# checked to be prefix free with a Kraft sum of at most one.
#
#   tools/gen-h264-tables.py <T-REC-H.264 PDF> [out.cpp]
import re, subprocess, sys
from fractions import Fraction

pdf = sys.argv[1]
out_path = sys.argv[2] if len(sys.argv) > 2 else 'src/media/H264Data.cpp'

def layout(page):
    return subprocess.run(['pdftotext', '-f', str(page), '-l', str(page), '-layout', pdf, '-'],
                          capture_output=True, text=True, check=True).stdout.replace('−', '-')

def words(page):
    html = subprocess.run(['pdftotext', '-f', str(page), '-l', str(page), '-bbox', pdf, '-'],
                          capture_output=True, text=True, check=True).stdout
    return [(float(a), float(b), float(c), float(d), t.replace('−', '-'))
            for a, b, c, d, t in re.findall(r'xMin="([\d.]+)" yMin="([\d.]+)" xMax="([\d.]+)" yMax="([\d.]+)">([^<]*)<', html)]

def find_page(title, first=200, last=330):
    for p in range(first, last):
        if re.search(r'^\s*' + re.escape(title), layout(p), re.M):
            return p
    sys.exit('not found: ' + title)

def fail(message):
    sys.exit('gen-h264-tables: ' + message)

# m, n per ctxIdx: index 0 for I (and SI) slices, 1 to 3 for cabac_init_idc 0 to 2.
init = [[None] * 4 for _ in range(1024)]

def put(ctx, kind, m, n):
    if init[ctx][kind] is not None:
        fail('ctxIdx %d kind %d given twice' % (ctx, kind))
    init[ctx][kind] = (m, n)

# Tables 9-12 to 9-17: ctxIdx across, the rows m and n (per slice kind) down.
# Read from the words' boxes: a column is the ctxIdx heading above it; a
# row band runs from its m or n label to the next label.
def column_table(title, kinds_by_label_order):
    page = find_page(title)
    ws = words(page)
    head = [w for w in ws if w[4] == title.split()[0]]  # 'Table'
    top = None
    for w in ws:
        if w[4] == 'Table':
            # the title line that matches
            line = ' '.join(x[4] for x in ws if abs(x[1] - w[1]) < 1)
            if title in line:
                top = w[1]
    if top is None:
        fail('no title box for ' + title)
    nxt = [w[1] for w in ws if w[4] == 'Table' and w[1] > top + 5]
    bottom = min(nxt) if nxt else 10000
    region = [w for w in ws if top + 5 < w[1] < bottom]
    # (an "n" the heading "Initializatio n variables" wraps is no label)
    # A label is an m or n with numbers to its right on its own line.
    labels = sorted([w for w in region if w[4] in ('m', 'n')
                     and not any(x[4] == 'variables' and abs(x[1] - w[1]) < 1 for x in region)
                     and any(re.fullmatch(r'-?\d+|na', x[4]) and x[0] > w[2] and abs(x[1] - w[1]) < 12 for x in region)],
                    key=lambda w: w[1])
    if len(labels) != 2 * len(kinds_by_label_order):
        fail('%s: %d row labels' % (title, len(labels)))
    numbers = [w for w in region if re.fullmatch(r'-?\d+|na|-', w[4]) and w[0] > labels[0][2]]
    # The heading row: the numbers above the first label's band.
    first_band = labels[0][1] - 2
    heads = [w for w in numbers if w[1] < first_band]
    cols = sorted(((w[0] + w[2]) / 2, int(w[4])) for w in heads)
    cells = [w for w in numbers if w[1] >= first_band]
    for i, lab in enumerate(labels):
        y0 = lab[1] - 6
        y1 = labels[i + 1][1] - 6 if i + 1 < len(labels) else lab[1] + 20
        kind = kinds_by_label_order[i // 2]
        row = {}
        for w in cells:
            if not (y0 <= w[1] < y1):
                continue
            cx = (w[0] + w[2]) / 2
            col = min(cols, key=lambda c: abs(c[0] - cx))
            if abs(col[0] - cx) > 10:
                fail('%s: a cell at %.1f under no heading' % (title, cx))
            row.setdefault(col[1], []).append((w[1], w[4]))
        for ctx, parts in row.items():
            text = ''.join(t for _, t in sorted(parts))
            if text == 'na':
                continue
            value = int(text)
            if lab[4] == 'm':
                for k in kind:
                    pending.setdefault((ctx, k), [None, None])[0] = value
            else:
                for k in kind:
                    pending.setdefault((ctx, k), [None, None])[1] = value

pending = {}
ALL = (0, 1, 2, 3)
column_table('Table 9-12 – Values of variables m and n for ctxIdx from 0 to 10', [ALL])
column_table('Table 9-13 – Values of variables m and n for ctxIdx from 11 to 23', [(1,), (2,), (3,)])
column_table('Table 9-14 – Values of variables m and n for ctxIdx from 24 to 39', [(1,), (2,), (3,)])
column_table('Table 9-15 – Values of variables m and n for ctxIdx from 40 to 53', [(1,), (2,), (3,)])
column_table('Table 9-16 – Values of variables m and n for ctxIdx from 54 to 59, and 399 to 401', [(0,), (1,), (2,), (3,)])
column_table('Table 9-17 – Values of variables m and n for ctxIdx from 60 to 69', [ALL])
for (ctx, k), (m, n) in sorted(pending.items()):
    if m is None or n is None:
        fail('ctxIdx %d kind %d half given' % (ctx, k))
    put(ctx, k, m, n)

# Tables 9-18 to 9-33: ctxIdx down, I then cabac_init_idc 0 to 2 across,
# two blocks of columns side by side.
row_titles = [
    'Table 9-18 – Values of variables m and n for ctxIdx from 70 to 104',
    'Table 9-19 – Values of variables m and n for ctxIdx from 105 to 165',
    'Table 9-20 – Values of variables m and n for ctxIdx from 166 to 226',
    'Table 9-21 – Values of variables m and n for ctxIdx from 227 to 275',
    'Table 9-22 – Values of variables m and n for ctxIdx from 277 to 337',
    'Table 9-23 – Values of variables m and n for ctxIdx from 338 to 398',
    'Table 9-24 – Values of variables m and n for ctxIdx from 402 to 459',
    'Table 9-25 – Values of variables m and n for ctxIdx from 460 to 483',
    'Table 9-26 – Values of variables m and n for ctxIdx from 484 to 571',
    'Table 9-27 – Values of variables m and n for ctxIdx from 572 to 659',
    'Table 9-28 – Values of variables m and n for ctxIdx from 660 to 717',
    'Table 9-29 – Values of variables m and n for ctxIdx from 718 to 775',
    'Table 9-30 – Values of variables m and n for ctxIdx from 776 to 863',
    'Table 9-31 – Values of variables m and n for ctxIdx from 864 to 951',
    'Table 9-32 – Values of variables m and n for ctxIdx from 952 to 1011',
    'Table 9-33 – Values of variables m and n for ctxIdx from 1012 to 1023',
]
seen_pages = set()
# Misprints of the 06/2026 edition: (the table's first ctxIdx, the number
# printed) -> the ctxIdx meant. Table 9-26 prints 541's row as "641"
# (between 540 and 542; its values are 497's, as each right-hand row
# there repeats the row 44 before it).
ERRATA = {(484, 641): 541}
for title in row_titles:
    lo, hi = map(int, re.search(r'from (\d+) to (\d+)', title).groups())
    page = find_page(title)
    pages = [page, page + 1]  # a table may run onto the next page
    for p in pages:
        if p in seen_pages:
            continue
        text = layout(p)
        for line in text.splitlines():
            tokens = line.split()
            if not tokens or not all(re.fullmatch(r'-?\d+|na', t) for t in tokens):
                continue
            # ctxIdx then four pairs (I and the three cabac_init_idc), or
            # three where a table has no I slice column; one or two blocks.
            if len(tokens) in (9, 18):
                width, has_i = 9, True
            elif len(tokens) in (7, 14):
                width, has_i = 7, False
            else:
                continue
            for g in range(0, len(tokens), width):
                group = tokens[g:g + width]
                ctx = int(group[0])
                ctx = ERRATA.get((lo, ctx), ctx)
                if not (lo <= ctx <= hi):
                    continue
                values = group[1:]
                kinds = (0, 1, 2, 3) if has_i else (1, 2, 3)
                for i, k in enumerate(kinds):
                    m, n = values[2 * i], values[2 * i + 1]
                    if m == 'na':
                        continue
                    if init[ctx][k] is None:
                        put(ctx, k, int(m), int(n))
    # the next table may start on this table's last page; it is read by title, so pages are not marked seen

# Every ctxIdx but 276 (end_of_slice_flag, not adapting) has P/B values;
# the I column is the I slices' contexts.
missing = [c for c in range(1024) if c != 276 and any(init[c][k] is None for k in (1, 2, 3))]
if missing:
    fail('contexts without P/B values: %s' % missing[:20])
i_missing = [c for c in list(range(0, 11)) + list(range(60, 70)) + list(range(70, 276)) + list(range(277, 1024))
             if c not in range(11, 60) and init[c][0] is None and not (399 <= c <= 401)]
if i_missing:
    fail('I contexts without values: %s' % i_missing[:20])
for c in range(1024):
    for k in range(4):
        if init[c][k] is not None and not (-128 <= init[c][k][0] <= 127 and -128 <= init[c][k][1] <= 127):
            fail('ctxIdx %d out of range' % c)

# Table 9-44 and 9-45.
def number_rows(page, start):
    text = layout(page)
    i = text.index(start)
    return text[i:].splitlines()[1:]

range_lps = [None] * 64
for line in number_rows(find_page('Table 9-44 –'), 'Table 9-44 –'):
    t = line.split()
    if len(t) == 10 and all(x.isdigit() for x in t):
        for g in (t[0:5], t[5:10]):
            range_lps[int(g[0])] = [int(x) for x in g[1:]]
if None in range_lps:
    fail('rangeTabLPS incomplete')
trans_lps, trans_mps = [], []
state_rows = number_rows(find_page('Table 9-45 –'), 'Table 9-45 –')
for line in state_rows:
    t = line.split()
    if t and t[0] == 'transIdxLPS':
        trans_lps += [int(x) for x in t[1:]]
    if t and t[0] == 'transIdxMPS':
        trans_mps += [int(x) for x in t[1:]]
    if len(trans_lps) == 64 and len(trans_mps) == 64:
        break
if len(trans_lps) != 64 or len(trans_mps) != 64:
    fail('state transitions incomplete')

# Code tables: cells placed by the column of the heading they stand under.
# A cell and where it starts: the code columns are left-aligned under
# their headings, so a cell belongs to the last heading starting at or
# before it (a short code's centre can sit nearer the next column's).
def cells(line):
    return [(m.start(), m.group()) for m in re.finditer(r'\S+(?: \S+)*', line)]

def column_of(columns, pos):
    best = 0
    for i, start in enumerate(columns):
        if start <= pos + 1:
            best = i
    return best

def code_table(title, n_cols, key_count, end_marker):
    """Rows of a code table: a row with all its cells (a "-" is a cell) is
    read in order; a shorter one by where its cells start, against the
    column starts the full rows of the same page show (the columns are
    left-aligned; the headings above them are not)."""
    page = find_page(title)
    pages = [layout(page), layout(page + 1), layout(page + 2)]
    rows = []
    started = False
    for text in pages:
        lines = text.splitlines()
        if not started:
            lines = text[text.index(title):].splitlines()[1:]
            started = True
        stop = False
        data = []
        for line in lines:
            if end_marker and end_marker in line:
                stop = True
                break
            c = cells(line)
            if len(c) < key_count + 1 or not all(re.fullmatch(r'\d+', c[i][1]) for i in range(key_count)):
                continue
            if not all(re.fullmatch(r'[01 ]+|-', v) for _, v in c[key_count:]):
                continue
            data.append(c)
        full = [c for c in data if len(c) == key_count + n_cols]
        starts = [min(c[key_count + i][0] for c in full) for i in range(n_cols)] if full else None
        for c in data:
            row = [None] * n_cols
            if len(c) == key_count + n_cols:
                placed = list(enumerate(v for _, v in c[key_count:]))
            elif starts:
                placed = [(column_of(starts, pos), v) for pos, v in c[key_count:]]
            else:
                fail('%s: a short row and no full row to place it by' % title)
            for col, v in placed:
                if v != '-':
                    row[col] = v.replace(' ', '')
            rows.append(([int(c[i][1]) for i in range(key_count)], row))
        if stop:
            break
    return rows

def check_codes(name, codes):
    codes = [c for c in codes if c is not None]
    kraft = sum(Fraction(1, 2 ** len(c)) for c in codes)
    if kraft > 1:
        fail('%s: Kraft sum %s' % (name, kraft))
    for a in codes:
        for b in codes:
            if a is not b and b.startswith(a) and a != b:
                fail('%s: %s prefixes %s' % (name, a, b))
    if len(set(codes)) != len(codes):
        fail('%s: a code twice' % name)

# Table 9-5: rows (TrailingOnes, TotalCoeff), six columns of nC.
coeff = code_table('Table 9-5 – coeff_token mapping', 6, 2, None)
seen = {}
for key, row in coeff:
    t1, tc = key
    if (t1, tc) in seen or t1 > 3 or tc > 16 or t1 > tc:
        continue
    seen[(t1, tc)] = row
if len(seen) != 62:
    fail('coeff_token rows: %d' % len(seen))
for col in range(6):
    check_codes('coeff_token column %d' % col, [seen[k][col] for k in seen])
# The nC == -1 column (chroma DC 4:2:0) codes TotalCoeff 0 to 4 only, the
# nC == -2 column 0 to 8, the others all 62 pairs.
for col, max_tc in ((0, 16), (1, 16), (2, 16), (3, 16), (4, 4), (5, 8)):
    for (t1, tc), row in seen.items():
        if (row[col] is not None) != (tc <= max_tc):
            fail('coeff_token column %d: (%d, %d) %s' % (col, t1, tc, 'missing' if row[col] is None else 'unexpected'))
    kraft = sum(Fraction(1, 2 ** len(seen[k][col])) for k in seen if seen[k][col])
    print('coeff_token column %d: %d codes, Kraft sum %s' % (col, sum(1 for k in seen if seen[k][col]), kraft))

def tz_table(title, n_cols, first_index, rows_wanted, end):
    rows = code_table(title, n_cols, 1, end)
    out = {}
    for key, row in rows:
        if key[0] < rows_wanted and key[0] not in out:
            out[key[0]] = row
    if len(out) != rows_wanted:
        fail('%s: %d rows' % (title, len(out)))
    return [[out[z][c] for z in range(rows_wanted)] for c in range(n_cols)]

tz_a = tz_table('Table 9-7 – total_zeros tables for 4x4 blocks with tzVlcIndex 1 to 7', 7, 1, 16, 'Table 9-8')
tz_b = tz_table('Table 9-8 – total_zeros tables for 4x4 blocks with tzVlcIndex 8 to 15', 8, 8, 9, 'Rec. ITU-T')
tz4x4 = tz_a + tz_b
for i, col in enumerate(tz4x4):
    check_codes('total_zeros %d' % (i + 1), col)
tz2x2 = tz_table('Table 9-9 – total_zeros tables for chroma DC 2x2', 3, 1, 4, '(b) Chroma DC 2x4')
for i, col in enumerate(tz2x2):
    check_codes('total_zeros 2x2 %d' % (i + 1), col)
# (b) is in the same table: start reading after its own heading.
page = find_page('Table 9-9 –')
text = layout(page)
sub = text[text.index('(b) Chroma DC 2x4'):]
columns = None
tz2x4_rows = {}
for line in sub.splitlines()[1:]:
    c = cells(line)
    if columns is None:
        if len(c) == 7 and all(re.fullmatch(r'\d', x[1]) for x in c):
            columns = [x[0] for x in c]
        continue
    if 'Rec. ITU-T' in line:
        break
    if c and re.fullmatch(r'\d', c[0][1]):
        row = [None] * 7
        for pos, value in c[1:]:
            col = column_of(columns, pos)
            if value != '-':
                row[col] = value.replace(' ', '')
        tz2x4_rows[int(c[0][1])] = row
if len(tz2x4_rows) != 8:
    fail('total_zeros 2x4 rows: %d' % len(tz2x4_rows))
tz2x4 = [[tz2x4_rows[z][c] for z in range(8)] for c in range(7)]
for i, col in enumerate(tz2x4):
    check_codes('total_zeros 2x4 %d' % (i + 1), col)

rb = tz_table('Table 9-10 – Tables for run_before', 7, 1, 15, '9.2.4')
for i, col in enumerate(rb):
    check_codes('run_before %d' % (i + 1), col)

# Table 9-4 (a): codeNum to coded_block_pattern, Intra_4x4/8x8 and Inter.
cbp_intra, cbp_inter = [None] * 48, [None] * 48
page = find_page('Table 9-4 –')
text = layout(page) + '\n' + layout(page + 1)
part = text[text.index('(a) ChromaArrayType is equal to 1 or 2'):]
part = part[:part.index('(b)')] if '(b)' in part else part
for line in part.splitlines():
    t = line.split()
    if len(t) == 3 and all(x.isdigit() for x in t) and int(t[0]) < 48 and cbp_intra[int(t[0])] is None:
        cbp_intra[int(t[0])], cbp_inter[int(t[0])] = int(t[1]), int(t[2])
if None in cbp_intra or sorted(cbp_intra) != list(range(48)) or sorted(cbp_inter) != list(range(48)):
    fail('Table 9-4 (a) incomplete or not a permutation')

# Table 9-43: levelListIdx to ctxIdxInc of significant_coeff_flag (frame
# coded) and last_significant_coeff_flag, for the 8x8 blocks.
sig8, last8 = [None] * 63, [None] * 63
page = find_page('Table 9-43 –')
for p in (page, page + 1, page + 2):
    for line in layout(p).splitlines():
        t = line.split()
        if t and all(x.isdigit() for x in t) and len(t) in (4, 8):
            for g in range(0, len(t), 4):
                i, sig, _field, last = map(int, t[g:g + 4])
                if i < 63 and sig8[i] is None:
                    sig8[i], last8[i] = sig, last
if None in sig8:
    fail('Table 9-43 incomplete: %s' % [i for i in range(63) if sig8[i] is None])

# The C++.
def code_list(codes):
    # (bits, length) per index; length 0 = no code
    return ', '.join('{ 0x%x, %d }' % (int(c, 2), len(c)) if c else '{ 0, 0 }' for c in codes)

o = []
o.append('// Generated by tools/gen-h264-tables.py from ITU-T H.264 (06/2026); do not edit.')
o.append('#include "media/H264Data.h"')
o.append('')
o.append('namespace sashfold::media::h264data {')
o.append('')
o.append('// Tables 9-12 to 9-33: { m, n } for I slices, then cabac_init_idc 0, 1, 2.')
o.append('CabacInit const cabac_init[1024][4] = {')
for c in range(1024):
    vals = []
    for k in range(4):
        v = init[c][k]
        if v is None:
            v = (0, 0) if c != 276 else (0, 0)
        vals.append('{ %d, %d }' % v)
    o.append('    { %s }, // %d' % (', '.join(vals), c))
o.append('};')
o.append('')
o.append('// Table 9-44.')
o.append('std::uint8_t const range_tab_lps[64][4] = {')
for r in range_lps:
    o.append('    { %s },' % ', '.join(map(str, r)))
o.append('};')
o.append('')
o.append('// Table 9-45.')
o.append('std::uint8_t const trans_idx_lps[64] = { %s };' % ', '.join(map(str, trans_lps)))
o.append('std::uint8_t const trans_idx_mps[64] = { %s };' % ', '.join(map(str, trans_mps)))
o.append('')
o.append('// Table 9-5: [nC class][TrailingOnes][TotalCoeff]; classes 0<=nC<2, 2<=nC<4, 4<=nC<8, 8<=nC, nC==-1, nC==-2.')
o.append('VlcCode const coeff_token[6][4][17] = {')
for col in range(6):
    o.append('    {')
    for t1 in range(4):
        o.append('        { %s },' % code_list([seen.get((t1, tc), [None] * 6)[col] for tc in range(17)]))
    o.append('    },')
o.append('};')
o.append('')
o.append('// Tables 9-7 and 9-8: [tzVlcIndex - 1][total_zeros].')
o.append('VlcCode const total_zeros_4x4[15][16] = {')
for col in tz4x4:
    o.append('    { %s },' % code_list(col + [None] * (16 - len(col))))
o.append('};')
o.append('// Table 9-9 (a): [tzVlcIndex - 1][total_zeros].')
o.append('VlcCode const total_zeros_2x2[3][4] = {')
for col in tz2x2:
    o.append('    { %s },' % code_list(col))
o.append('};')
o.append('// Table 9-9 (b).')
o.append('VlcCode const total_zeros_2x4[7][8] = {')
for col in tz2x4:
    o.append('    { %s },' % code_list(col))
o.append('};')
o.append('// Table 9-10: [min(zerosLeft, 7) - 1][run_before].')
o.append('VlcCode const run_before[7][15] = {')
for col in rb:
    o.append('    { %s },' % code_list(col))
o.append('};')
o.append('')
o.append('// Table 9-4 (a): coded_block_pattern by codeNum, for Intra_4x4 and Intra_8x8, then Inter.')
o.append('std::uint8_t const cbp_intra[48] = { %s };' % ', '.join(map(str, cbp_intra)))
o.append('std::uint8_t const cbp_inter[48] = { %s };' % ', '.join(map(str, cbp_inter)))
o.append('')
o.append('// Table 9-43: ctxIdxInc of significant_coeff_flag (frame coded) and last_significant_coeff_flag by levelListIdx, 8x8 blocks.')
o.append('std::uint8_t const significant_8x8_frame[63] = { %s };' % ', '.join(map(str, sig8)))
o.append('std::uint8_t const last_significant_8x8[63] = { %s };' % ', '.join(map(str, last8)))
o.append('')
o.append('}')
open(out_path, 'w').write('\n'.join(o) + '\n')
print('wrote', out_path)
