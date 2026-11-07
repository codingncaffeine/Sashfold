#!/usr/bin/env python3
# Writes one-page PDFs whose annotations exercise how a viewer draws them
# (ISO 32000-2 §12.5): appearance streams fitted to their rectangles
# (Algorithm 8.1: a box through a matrix, scaled and moved into /Rect),
# appearance states chosen by /AS, hidden and NoView flags, Popups never
# drawn, /CA opacity, and the markup kinds a viewer draws from their
# fields when they carry no appearance (Square, Circle, Line, Polygon,
# PolyLine, Ink, Highlight, Underline, StrikeOut). Each is compared with
# poppler's and Ghostscript's renders by tools/pdf-render-corpus.sh.
#   pdf-make-annotation-tests.py <out-folder>
import os, sys, zlib

out_dir = sys.argv[1]
os.makedirs(out_dir, exist_ok=True)


def pdf(name, content, annots, extra_objects=None, size=(300, 300)):
    # annots: list of (object number, dict bytes); extra_objects: {number: bytes}, 10 up.
    objs = {
        1: b'<< /Type /Catalog /Pages 2 0 R >>',
        2: b'<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
        3: b'<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %d %d] /Contents 4 0 R /Annots [' % size
        + b' '.join(b'%d 0 R' % n for n, _ in annots) + b'] >>',
    }
    data = zlib.compress(content)
    objs[4] = b'<< /Length %d /Filter /FlateDecode >>\nstream\n' % len(data) + data + b'\nendstream'
    for n, body in annots:
        objs[n] = body
    for k, v in (extra_objects or {}).items():
        objs[k] = v
    out = bytearray(b'%PDF-1.7\n%\xe2\xe3\xcf\xd3\n')
    offsets = {}
    for n in sorted(objs):
        offsets[n] = len(out)
        out += b'%d 0 obj\n' % n + objs[n] + b'\nendobj\n'
    size_n = max(objs) + 1
    x = len(out)
    out += b'xref\n0 %d\n0000000000 65535 f \n' % size_n
    for n in range(1, size_n):
        out += (b'%010d 00000 n \n' % offsets[n]) if n in offsets else b'0000000000 00000 f \n'
    out += b'trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n' % (size_n, x)
    open(os.path.join(out_dir, name + '.pdf'), 'wb').write(out)


def form(body, bbox, matrix=None, resources=b''):
    d = b'/Type /XObject /Subtype /Form /BBox [%s]' % bbox
    if matrix:
        d += b' /Matrix [%s]' % matrix
    if resources:
        d += b' /Resources ' + resources
    return b'<< ' + d + b' /Length %d >>\nstream\n' % len(body) + body + b'\nendstream'


PAGE = b'0.9 g 0 0 300 300 re f 0 g 20 280 m 280 280 l S'

# 1. Appearance streams: a box the rectangle's size, one half the size
# (scaled up), one whose matrix rotates it (fitted after the rotation), and
# one placed by a rectangle given upside down.
pdf('annot-appearance', PAGE, [
    (20, b'<< /Type /Annot /Subtype /Stamp /Rect [20 20 120 120] /F 4 /AP << /N 30 0 R >> >>'),
    (21, b'<< /Type /Annot /Subtype /Stamp /Rect [150 20 280 120] /F 4 /AP << /N 31 0 R >> >>'),
    (22, b'<< /Type /Annot /Subtype /Stamp /Rect [20 150 120 250] /F 4 /AP << /N 32 0 R >> >>'),
    (23, b'<< /Type /Annot /Subtype /Stamp /Rect [280 250 150 150] /F 4 /AP << /N 30 0 R >> >>'),
], {
    30: form(b'1 0 0 rg 0 0 100 100 re f 0 0 1 rg 10 10 30 30 re f', b'0 0 100 100'),
    31: form(b'0 0.6 0 rg 0 0 50 50 re f 1 1 0 rg 5 5 10 10 re f', b'0 0 50 50'),
    32: form(b'0 0 0 rg 0 0 100 40 re f 1 0 0 rg 0 0 20 20 re f', b'0 0 100 40', b'0.7071 0.7071 -0.7071 0.7071 0 0'),
})

# 2. States, flags, opacity: a checkbox on and one off by /AS, a hidden
# annotation, a NoView one, a Popup with an appearance (never drawn), a
# half-transparent one, and one without /AS whose dictionary has one state.
pdf('annot-states', PAGE, [
    (20, b'<< /Type /Annot /Subtype /Widget /Rect [20 20 60 60] /F 4 /AS /On /AP << /N << /On 30 0 R /Off 31 0 R >> >> >>'),
    (21, b'<< /Type /Annot /Subtype /Widget /Rect [80 20 120 60] /F 4 /AS /Off /AP << /N << /On 30 0 R /Off 31 0 R >> >> >>'),
    (22, b'<< /Type /Annot /Subtype /Stamp /Rect [140 20 180 60] /F 6 /AP << /N 30 0 R >> >>'),
    (23, b'<< /Type /Annot /Subtype /Stamp /Rect [200 20 240 60] /F 36 /AP << /N 30 0 R >> >>'),
    (24, b'<< /Type /Annot /Subtype /Popup /Rect [20 100 120 200] /F 4 /AP << /N 30 0 R >> >>'),
    (25, b'<< /Type /Annot /Subtype /Stamp /Rect [150 100 250 200] /F 4 /CA 0.5 /AP << /N 30 0 R >> >>'),
    (26, b'<< /Type /Annot /Subtype /Widget /Rect [20 220 60 260] /F 4 /AP << /N << /Only 30 0 R >> >> >>'),
], {
    30: form(b'0 0 1 rg 0 0 40 40 re f 1 1 1 rg 10 10 20 20 re f', b'0 0 40 40'),
    31: form(b'0.5 g 0 0 40 40 re f', b'0 0 40 40'),
})

# 3. Markup kinds without appearances, drawn from their fields.
pdf('annot-shapes', PAGE, [
    (20, b'<< /Type /Annot /Subtype /Square /Rect [20 20 120 100] /F 4 /C [1 0 0] /IC [1 1 0] /BS << /W 4 >> >>'),
    (21, b'<< /Type /Annot /Subtype /Circle /Rect [150 20 280 100] /F 4 /C [0 0 1] /BS << /W 2 >> >>'),
    (22, b'<< /Type /Annot /Subtype /Line /Rect [20 120 280 140] /F 4 /C [0 0.5 0] /L [25 130 275 130] /BS << /W 3 >> >>'),
    (23, b'<< /Type /Annot /Subtype /Polygon /Rect [20 150 120 250] /F 4 /C [0.5 0 0.5] /Vertices [30 160 110 170 70 240] /BS << /W 2 >> >>'),
    (24, b'<< /Type /Annot /Subtype /Ink /Rect [150 150 280 250] /F 4 /C [0 0 0] /InkList [[160 160 200 240 240 160 270 240]] /BS << /W 3 >> >>'),
    (25, b'<< /Type /Annot /Subtype /Square /Rect [20 260 120 290] /F 4 /C [0 0 0] /IC [0 1 1] /BS << /W 0 >> >>'),
], {})

# 4. Text markup over drawn text-like bars: highlight multiplies, underline
# and strikeout sit on the quads.
BARS = PAGE + b' 0.2 g 30 200 240 14 re f 30 150 240 14 re f 30 100 240 14 re f'
pdf('annot-markup', BARS, [
    (20, b'<< /Type /Annot /Subtype /Highlight /Rect [30 196 270 218] /F 4 /C [1 1 0] /QuadPoints [30 218 270 218 30 196 270 196] >>'),
    (21, b'<< /Type /Annot /Subtype /Underline /Rect [30 146 270 168] /F 4 /C [1 0 0] /QuadPoints [30 168 270 168 30 146 270 146] >>'),
    (22, b'<< /Type /Annot /Subtype /StrikeOut /Rect [30 96 270 118] /F 4 /C [0 0 1] /QuadPoints [30 118 270 118 30 96 270 96] >>'),
], {})
