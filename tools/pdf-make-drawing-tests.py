#!/usr/bin/env python3
# Writes one-page PDFs of drawing only (no text) that exercise the
# renderer's graphics: paths and strokes, color spaces, clips, shadings,
# patterns, images and transparency. Each is compared with poppler's
# pdftoppm by pdf_probe --render --compare (tools/pdf-render-corpus.sh).
#   pdf-make-drawing-tests.py <out-folder>
import os, struct, sys, zlib

out_dir = sys.argv[1]
os.makedirs(out_dir, exist_ok=True)

def pdf(name, content, resources=b'<< >>', extra_objects=None, size=(300, 300)):
    # extra_objects: {number: bytes}, numbered from 10 up; the page content is object 4.
    objs = {
        1: b'<< /Type /Catalog /Pages 2 0 R >>',
        2: b'<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
        3: b'<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %d %d] /Resources ' % size + resources + b' /Contents 4 0 R >>',
    }
    data = zlib.compress(content)
    objs[4] = b'<< /Length %d /Filter /FlateDecode >>\nstream\n' % len(data) + data + b'\nendstream'
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

def stream(dict_body, data, compress=True):
    if compress:
        data = zlib.compress(data)
        dict_body += b' /Filter /FlateDecode'
    return b'<< ' + dict_body + b' /Length %d >>\nstream\n' % len(data) + data + b'\nendstream'

# 1. Paths: fills by both rules, curves, strokes with every cap, join and dashes.
pdf('paths', b'''
0.9 0.2 0.2 rg 20 20 100 80 re f
0.2 0.6 0.9 rg 150 20 m 280 20 l 215 120 l h f
0 0 0 RG 4 w 20 150 m 60 260 100 140 140 250 c S
0.2 0.8 0.3 rg 160 140 m 280 140 l 180 280 l 220 130 l 260 280 l h f*
0 J 10 w 0 0 1 RG 20 280 m 80 280 l S
1 J 0 1 0 RG 100 280 m 140 280 l S
2 J 1 0 0 RG 160 280 m 200 280 l S
0 J 0 j 6 w 0 0 0 RG 220 230 m 250 270 l 280 230 l S
[6 3] 0 d 2 w 20 120 m 130 130 l S
[] 0 d 0 w 20 110 m 130 110 l S
''')

# 2. Color spaces: device, CMYK, Lab, Indexed, Separation with a type 2 function, DeviceN with type 4.
pdf('colors', b'''
0.8 g 0 0 300 300 re f
1 0 0 rg 10 10 60 60 re f
0 1 0 0 k 80 10 60 60 re f
0 0 1 0 k 150 10 60 60 re f
0.6 0.1 0.9 0.2 k 220 10 60 60 re f
/Lab cs 60 50 -30 sc 10 90 60 60 re f
/Idx cs 2 sc 80 90 60 60 re f
/Spot cs 0.7 sc 150 90 60 60 re f
/DN cs 0.3 0.8 sc 220 90 60 60 re f
0.25 0.5 0.75 rg 10 170 270 40 re f
''', resources=b'''<< /ColorSpace << /Lab [/Lab << /WhitePoint [0.9505 1 1.089] /Range [-100 100 -100 100] >>]
 /Idx [/Indexed /DeviceRGB 3 <FF0000 00FF00 0000FF FFFF00>]
 /Spot [/Separation /PANTONE#20Orange /DeviceCMYK << /FunctionType 2 /Domain [0 1] /C0 [0 0 0 0] /C1 [0 0.6 1 0] /N 1 >>]
 /DN [/DeviceN [/Cyan /Magenta] /DeviceCMYK 10 0 R] >> >>''',
    extra_objects={10: stream(b'/FunctionType 4 /Domain [0 1 0 1] /Range [0 1 0 1 0 1 0 1]', b'{ 0 0 }', compress=False)})

# 3. Clips: a circle clip with even-odd, nested in a rectangle clip.
pdf('clips', b'''
q 40 40 220 220 re W n
q 150 260 m 210 260 260 210 260 150 c 260 90 210 40 150 40 c 90 40 40 90 40 150 c 40 210 90 260 150 260 c h
150 200 m 177 200 200 177 200 150 c 200 123 177 100 150 100 c 123 100 100 123 100 150 c 100 177 123 200 150 200 c h W* n
0.1 0.3 0.8 rg 0 0 300 300 re f
1 0.8 0 rg 0 140 300 20 re f
Q
0 0 0 RG 3 w 0 0 m 300 300 l S
Q
''')

# 4. Shadings: axial (extend both), radial (extend), function-based (type 1 with a sampled function).
sampled = bytes([int(255 * (x / 3)) if c == 0 else int(255 * (y / 3)) if c == 1 else 128
                 for y in range(4) for x in range(4) for c in range(3)])
pdf('shadings', b'''
q 10 160 135 130 re W n /Ax sh Q
q 155 160 135 130 re W n /Rad sh Q
q 10 10 135 130 re W n /Fn sh Q
q 155 10 135 130 re W n /Rad2 sh Q
''', resources=b'''<< /Shading <<
 /Ax << /ShadingType 2 /ColorSpace /DeviceRGB /Coords [30 0 120 0] /Extend [true true] /Function << /FunctionType 2 /Domain [0 1] /C0 [1 0 0] /C1 [0 0 1] /N 1 >> >>
 /Rad << /ShadingType 3 /ColorSpace /DeviceRGB /Coords [200 240 5 222 225 60] /Extend [true true] /Function << /FunctionType 2 /Domain [0 1] /C0 [1 1 0] /C1 [0 0.5 0] /N 1 >> >>
 /Fn << /ShadingType 1 /ColorSpace /DeviceRGB /Domain [0 1 0 1] /Matrix [135 0 0 130 10 10] /Function 10 0 R >>
 /Rad2 << /ShadingType 3 /ColorSpace /DeviceGray /Coords [220 75 0 220 75 60] /Extend [false false] /Function << /FunctionType 3 /Domain [0 1] /Functions [<< /FunctionType 2 /Domain [0 1] /C0 [0] /C1 [1] /N 1 >> << /FunctionType 2 /Domain [0 1] /C0 [1] /C1 [0.3] /N 1 >>] /Bounds [0.5] /Encode [0 1 0 1] >> >>
 >> >>''', extra_objects={10: stream(b'/FunctionType 0 /Domain [0 1 0 1] /Range [0 1 0 1 0 1] /Size [4 4] /BitsPerSample 8', sampled)})

# 5. Meshes: a free-form triangle mesh (type 4) and a Coons patch (type 6).
def mesh4():
    data = bytearray()
    def v(flag, x, y, r, g, b):
        data.extend(struct.pack('>BHHBBB', flag, int(x / 300 * 65535), int(y / 300 * 65535), r, g, b))
    v(0, 20, 20, 255, 0, 0); v(0, 140, 20, 0, 255, 0); v(0, 80, 140, 0, 0, 255)
    v(2, 140, 140, 255, 255, 0)
    return bytes(data)
def mesh6():
    data = bytearray()
    pts = [(160, 20), (160, 60), (160, 100), (160, 140), (200, 150), (240, 130), (280, 140), (270, 100), (290, 60), (280, 20), (240, 30), (200, 10)]
    data.append(0)
    for x, y in pts:
        data.extend(struct.pack('>HH', int(x / 300 * 65535), int(y / 300 * 65535)))
    for c in [(255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 0)]:
        data.extend(bytes(c))
    return bytes(data)
pdf('meshes', b'/M4 sh /M6 sh', resources=b'<< /Shading << /M4 10 0 R /M6 11 0 R >> >>', extra_objects={
    10: stream(b'/ShadingType 4 /ColorSpace /DeviceRGB /BitsPerCoordinate 16 /BitsPerComponent 8 /BitsPerFlag 8 /Decode [0 300 0 300 0 1 0 1 0 1]', mesh4()),
    11: stream(b'/ShadingType 6 /ColorSpace /DeviceRGB /BitsPerCoordinate 16 /BitsPerComponent 8 /BitsPerFlag 8 /Decode [0 300 0 300 0 1 0 1 0 1]', mesh6()),
})

# 6. Patterns: a colored tiling pattern, an uncolored one, and a shading pattern.
pdf('patterns', b'''
/Pattern cs /Tile scn 10 160 135 130 re f
/P2 cs 0 0 0 /Hatch scn 155 160 135 130 re f
/P2 cs 0.9 0.1 0.1 /Hatch scn 10 10 135 130 re f
/Pattern cs /Shade scn 155 10 135 130 re f
''', resources=b'''<< /ColorSpace << /P2 [/Pattern /DeviceRGB] >> /Pattern << /Tile 10 0 R /Hatch 11 0 R
 /Shade << /PatternType 2 /Matrix [1 0 0 1 0 0] /Shading << /ShadingType 2 /ColorSpace /DeviceRGB /Coords [155 10 290 140] /Function << /FunctionType 2 /Domain [0 1] /C0 [0 1 1] /C1 [1 0 1] /N 1 >> >> >> >> >>''',
    extra_objects={
        10: stream(b'/PatternType 1 /PaintType 1 /TilingType 1 /BBox [0 0 20 20] /XStep 20 /YStep 20 /Resources << >>',
                   b'1 0 0 rg 0 0 10 10 re f 0 0 1 rg 10 10 10 10 re f', compress=False),
        11: stream(b'/PatternType 1 /PaintType 2 /TilingType 1 /BBox [0 0 12 12] /XStep 12 /YStep 12 /Matrix [0.7071 0.7071 -0.7071 0.7071 0 0] /Resources << >>',
                   b'0 0 6 12 re f', compress=False),
    })

# 7. Images: RGB 8-bit, a 1-bit stencil mask, a gray image with a soft mask, an inline image, Indexed 4-bit.
w, h = 16, 16
rgb = bytes([c for y in range(h) for x in range(w) for c in (x * 16, y * 16, 128)])
stencil = bytes([0b10101010 if y % 2 else 0b01010101 for y in range(h) for _ in range(2)])
gray = bytes([x * 16 for y in range(h) for x in range(w)])
smask = bytes([255 if (x - 8) ** 2 + (y - 8) ** 2 < 40 else 0 for y in range(h) for x in range(w)])
indexed = bytes([((x // 4) << 4) | (y // 4) for y in range(h) for x in range(0, w, 2)])
pdf('images', b'''
q 120 0 0 120 10 170 cm /Im1 Do Q
0 0.5 0 rg q 120 0 0 120 170 170 cm /Mask Do Q
q 120 0 0 120 10 10 cm /Soft Do Q
q 60 0 0 60 170 80 cm BI /W 4 /H 4 /CS /RGB /BPC 8 ID
\xff\x00\x00\x00\xff\x00\x00\x00\xff\xff\xff\x00\xff\x00\x00\x00\xff\x00\x00\x00\xff\xff\xff\x00\xff\x00\x00\x00\xff\x00\x00\x00\xff\xff\xff\x00\xff\x00\x00\x00\xff\x00\x00\x00\xff\xff\xff\x00
EI Q
q 50 0 0 50 240 20 cm /Idx Do Q
''', resources=b'<< /XObject << /Im1 10 0 R /Mask 11 0 R /Soft 12 0 R /Idx 14 0 R >> >>', extra_objects={
    10: stream(b'/Type /XObject /Subtype /Image /Width 16 /Height 16 /ColorSpace /DeviceRGB /BitsPerComponent 8', rgb),
    11: stream(b'/Type /XObject /Subtype /Image /Width 16 /Height 16 /ImageMask true', stencil),
    12: stream(b'/Type /XObject /Subtype /Image /Width 16 /Height 16 /ColorSpace /DeviceGray /BitsPerComponent 8 /SMask 13 0 R', gray),
    13: stream(b'/Type /XObject /Subtype /Image /Width 16 /Height 16 /ColorSpace /DeviceGray /BitsPerComponent 8', smask),
    14: stream(b'/Type /XObject /Subtype /Image /Width 16 /Height 16 /ColorSpace [/Indexed /DeviceRGB 15 <' +
               b''.join(b'%02x%02x%02x' % (i * 17, 255 - i * 17, (i * 53) % 256) for i in range(16)) + b'>] /BitsPerComponent 4', indexed),
})

# 8. Transparency: constant alpha, blend modes, a transparency group, a luminosity soft mask.
pdf('transparency', b'''
1 0 0 rg 20 150 120 120 re f
/A50 gs 0 0 1 rg 80 180 120 120 re f
/Normal gs
0.9 0.9 0.2 rg 160 20 120 120 re f
/Mul gs 0.2 0.6 0.9 rg 200 60 90 90 re f
/Normal gs
q /Grp50 gs /G Do Q
q /SM gs 0 0.6 0 rg 160 160 130 130 re f Q
''', resources=b'''<< /ExtGState << /A50 << /ca 0.5 /CA 0.5 >> /Normal << /ca 1 /BM /Normal >> /Mul << /BM /Multiply >>
 /Grp50 << /ca 0.5 >> /SM << /SMask << /S /Luminosity /G 11 0 R >> >> >> /XObject << /G 10 0 R >> >>''', extra_objects={
    10: stream(b'/Type /XObject /Subtype /Form /BBox [0 0 300 300] /Group << /S /Transparency >> /Resources << >>',
               b'1 0.5 0 rg 20 20 80 80 re f 0 0.5 1 rg 60 60 80 80 re f', compress=False),
    11: stream(b'/Type /XObject /Subtype /Form /BBox [0 0 300 300] /Group << /S /Transparency /CS /DeviceGray >> /Resources << /Shading << /S << /ShadingType 2 /ColorSpace /DeviceGray /Coords [160 0 290 0] /Function << /FunctionType 2 /Domain [0 1] /C0 [0] /C1 [1] /N 1 >> /Extend [true true] >> >> >>',
               b'/S sh', compress=False),
})

# 9. Forms and optional content: a form drawn twice under different matrices; a hidden layer.
pdf('forms', b'''
q 1 0 0 1 20 20 cm /F Do Q
q 0.5 0 0 0.5 160 160 cm /F Do Q
/OC /Off BDC 1 0 0 rg 0 0 300 300 re f EMC
/Hidden Do
''', resources=b'''<< /XObject << /F 10 0 R /Hidden 11 0 R >> /Properties << /Off 12 0 R >> >>''', extra_objects={
    10: stream(b'/Type /XObject /Subtype /Form /BBox [0 0 120 120] /Resources << >>',
               b'0.3 0.3 0.9 rg 0 0 120 120 re f 1 1 1 rg 20 20 80 80 re f 0.9 0.3 0.3 rg 40 40 40 40 re f', compress=False),
    11: stream(b'/Type /XObject /Subtype /Form /BBox [0 0 300 300] /OC 12 0 R /Resources << >>', b'0 g 0 0 300 300 re f', compress=False),
    12: b'<< /Type /OCG /Name (Hidden layer) >>',
})
# The optional content properties live in the catalog: rewrite forms.pdf's catalog.
path = os.path.join(out_dir, 'forms.pdf')
data = open(path, 'rb').read()
data = data.replace(b'<< /Type /Catalog /Pages 2 0 R >>', b'<< /Type /Catalog /Pages 2 0 R /OCProperties << /OCGs [12 0 R] /D << /OFF [12 0 R] >> >> >>')
# Offsets shifted: rebuilt by every reader; poppler rebuilds too. Write it with a fresh table instead.
import re
objs = {int(m.group(1)): m.group(2) for m in re.finditer(rb'(\d+) 0 obj\n(.*?)\nendobj\n', data, re.S)}
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
open(path, 'wb').write(out)
print('wrote', len(os.listdir(out_dir)), 'files to', out_dir)
