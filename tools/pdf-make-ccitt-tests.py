#!/usr/bin/env python3
# Writes one-page PDFs whose one picture is CCITT fax data made by an
# independent encoder (libtiff, through Pillow): Group 3 one-dimensional
# with end-of-lines, Group 3 two-dimensional (K > 0), Group 4, and the
# byte-aligned Modified Huffman of TIFF compression 2. Each page is the
# picture's size in points, so a 72 dpi render is one pixel a sample and
# ours must equal poppler's exactly (tools/pdf-render-corpus.sh --dpi 72).
# The pictures: text, noise (a change every few pixels), runs past 2560
# (the extended make-up codes) and a width that is not a multiple of 8.
#   pdf-make-ccitt-tests.py <out-folder>
#   pdf-make-ccitt-tests.py --fixtures <folder>   (test_pdf's: a small
#       picture's strip in each variant, <variant>.bin, and the rows the
#       decoder must give for it with BlackIs1 false, <variant>.rows)
import io, os, random, sys, zlib
from PIL import Image, ImageDraw, TiffImagePlugin

fixtures = sys.argv[1] == '--fixtures'
out_dir = sys.argv[2] if fixtures else sys.argv[1]
os.makedirs(out_dir, exist_ok=True)


def pdf(name, image_dict, image_data, width, height):
    content = b'q %d 0 0 %d 0 0 cm /Im Do Q' % (width, height)
    objs = {
        1: b'<< /Type /Catalog /Pages 2 0 R >>',
        2: b'<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
        3: b'<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %d %d] /Resources << /XObject << /Im 5 0 R >> >> /Contents 4 0 R >>'
        % (width, height),
        4: b'<< /Length %d >>\nstream\n' % len(content) + content + b'\nendstream',
        5: b'<< ' + image_dict + b' /Length %d >>\nstream\n' % len(image_data) + image_data + b'\nendstream',
    }
    out = bytearray(b'%PDF-1.7\n%\xe2\xe3\xcf\xd3\n')
    offsets = {}
    for n in sorted(objs):
        offsets[n] = len(out)
        out += b'%d 0 obj\n' % n + objs[n] + b'\nendobj\n'
    x = len(out)
    out += b'xref\n0 6\n0000000000 65535 f \n'
    for n in range(1, 6):
        out += b'%010d 00000 n \n' % offsets[n]
    out += b'trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n' % x
    open(os.path.join(out_dir, name + '.pdf'), 'wb').write(out)


def encoded(image, compression, t4options=None):
    # One strip; its bytes and the photometric libtiff wrote.
    buffer = io.BytesIO()
    info = TiffImagePlugin.ImageFileDirectory_v2()
    info[278] = image.height  # RowsPerStrip
    if t4options is not None:
        info[292] = t4options
    image.save(buffer, format='TIFF', compression=compression, tiffinfo=info)
    buffer.seek(0)
    tiff = Image.open(buffer)
    offsets = tiff.tag_v2[273]
    counts = tiff.tag_v2[279]
    photometric = tiff.tag_v2[262]
    fill_order = tiff.tag_v2.get(266, 1)
    assert len(offsets) == 1 and fill_order == 1, (offsets, fill_order)
    raw = buffer.getvalue()
    return raw[offsets[0]:offsets[0] + counts[0]], photometric


def pictures():
    rng = random.Random(85)
    text = Image.new('1', (613, 260), 1)
    draw = ImageDraw.Draw(text)
    for i, line in enumerate(['Sashfold reads fax pictures.', 'THE QUICK BROWN FOX 0123456789',
                              'jumps over the lazy dog; a scanned page.', '#$%&*()[]{} <=> ~ @ ^ | \\ /']):
        draw.text((10, 10 + i * 60), line, fill=0, font_size=36)
    draw.rectangle([400, 5, 600, 40], fill=0)
    yield 'text', text
    noise = Image.new('1', (301, 157), 1)
    noise.putdata([rng.choice((0, 1, 1)) for _ in range(301 * 157)])
    yield 'noise', noise
    runs = Image.new('1', (3001, 40), 1)
    draw = ImageDraw.Draw(runs)
    draw.rectangle([5, 0, 2705, 9], fill=0)          # a black run of 2701
    draw.rectangle([0, 10, 3000, 19], fill=0)        # a whole black line
    draw.rectangle([2990, 20, 3000, 29], fill=0)     # white 2990 then black to the end
    for x in range(0, 3001, 3):
        draw.line([x, 30, x, 39], fill=0)            # a change every pixel or two
    yield 'runs', runs
    shapes = Image.new('1', (250, 250), 1)
    draw = ImageDraw.Draw(shapes)
    for r in range(120, 0, -12):
        draw.ellipse([125 - r, 125 - r, 125 + r, 125 + r], fill=(r // 12) % 2)
    draw.polygon([(0, 249), (249, 0), (249, 249)], outline=0)
    yield 'shapes', shapes


VARIANTS = [
        ('g3-1d', 'group3', 0, 0),        # K 0, end-of-lines
        ('g3-2d', 'group3', 1, 4),        # T4Options 2D coding: K > 0
        ('g4', 'group4', None, -1),
        ('mh', 'tiff_ccitt', None, 0),    # compression 2: rows byte-aligned, no end-of-lines
    ]


def write_fixtures():
    # 61 x 23: a width past a byte's end, noise beside long runs.
    rng = random.Random(5)
    image = Image.new('1', (61, 23), 1)
    pixels = [rng.choice((0, 1, 1)) for _ in range(61 * 12)] + [0] * (61 * 5) + [1] * (61 * 6)
    image.putdata(pixels)
    for variant, compression, t4, k in VARIANTS:
        data, photometric = encoded(image, compression, t4)
        # The decoder gives 1 for a coded white run: the picture's white
        # where 0 bits were white (photometric 0), its black otherwise.
        stride = (image.width + 7) // 8
        rows = bytearray(stride * image.height)
        for y in range(image.height):
            for x in range(image.width):
                white = image.getpixel((x, y)) != 0
                coded_white = white if photometric == 0 else not white
                if coded_white:
                    rows[y * stride + x // 8] |= 0x80 >> (x % 8)
        open(os.path.join(out_dir, 'ccitt-%s.bin' % variant), 'wb').write(data)
        open(os.path.join(out_dir, 'ccitt-%s.rows' % variant), 'wb').write(bytes(rows))


def main():
    variants = [
        ('g3-1d', 'group3', 0, 0),        # K 0, end-of-lines
        ('g3-2d', 'group3', 1, 4),        # T4Options 2D coding: K > 0
        ('g4', 'group4', None, -1),
        ('mh', 'tiff_ccitt', None, 0),    # compression 2: rows byte-aligned, no end-of-lines
    ]
    for picture_name, image in pictures():
        for variant, compression, t4, k in variants:
            data, photometric = encoded(image, compression, t4)
            params = b'/K %d /Columns %d /Rows %d' % (k, image.width, image.height)
            if variant == 'mh':
                params += b' /EncodedByteAlign true'
            # libtiff codes runs of 0 bits as white: with photometric 1
            # (0 is black) those are the picture's black, so the samples
            # read inverted.
            decode = b' /Decode [1 0]' if photometric == 1 else b''
            image_dict = (b'/Type /XObject /Subtype /Image /Width %d /Height %d /ColorSpace /DeviceGray '
                          b'/BitsPerComponent 1 /Filter /CCITTFaxDecode /DecodeParms << ' % (image.width, image.height)
                          + params + b' >>' + decode)
            pdf('ccitt-%s-%s' % (picture_name, variant), image_dict, data, image.width, image.height)


if fixtures:
    write_fixtures()
else:
    main()
