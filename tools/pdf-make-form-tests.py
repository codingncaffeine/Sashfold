#!/usr/bin/env python3
# Writes the form fixtures test_pdf and the viewer's form test read, each
# independently of Sashfold's code (ISO 32000-2 §12.7): a page of fields of
# every kind with no appearance streams (a viewer draws them), one whose
# stale appearance must be ignored under NeedAppearances, the same form
# behind a cross-reference stream (an update must then write one), and
# encrypted with RC4 (an update must encrypt what it adds). poppler's
# pdftotext and pdftoppm are the judges of a saved copy.
#   pdf-make-form-tests.py <out-folder>
import hashlib, os, sys, zlib

out_dir = sys.argv[1]
os.makedirs(out_dir, exist_ok=True)

PAD = bytes([0x28, 0xBF, 0x4E, 0x5E, 0x4E, 0x75, 0x8A, 0x41, 0x64, 0x00, 0x4E, 0x56, 0xFF, 0xFA, 0x01, 0x08,
             0x2E, 0x2E, 0x00, 0xB6, 0xD0, 0x68, 0x3E, 0x80, 0x2F, 0x0C, 0xA9, 0xFE, 0x64, 0x53, 0x69, 0x7A])


def rc4(key, data):
    s = list(range(256)); j = 0
    for i in range(256):
        j = (j + s[i] + key[i % len(key)]) & 255; s[i], s[j] = s[j], s[i]
    i = j = 0; result = bytearray()
    for c in data:
        i = (i + 1) & 255; j = (j + s[i]) & 255; s[i], s[j] = s[j], s[i]
        result.append(c ^ s[(s[i] + s[j]) & 255])
    return bytes(result)


def md5(b):
    return hashlib.md5(b).digest()


class Encryption:
    # RC4 40-bit, revision 2, user password empty, owner password "owner".
    def __init__(self, id0):
        self.p = -1
        self.id0 = id0
        opad = (b'owner' + PAD)[:32]
        self.o = rc4(md5(opad)[:5], PAD[:32])
        key_input = PAD[:32] + self.o + (self.p & 0xFFFFFFFF).to_bytes(4, 'little') + id0
        self.key = md5(key_input)[:5]
        self.u = rc4(self.key, PAD[:32])

    def dict(self):
        return (b'<< /Filter /Standard /V 1 /R 2 /Length 40 /P -1 /O <' + self.o.hex().encode() + b'> /U <'
                + self.u.hex().encode() + b'> >>')

    def object_key(self, num, gen):
        return md5(self.key + num.to_bytes(3, 'little') + gen.to_bytes(2, 'little'))[:10]

    def string(self, num, gen, s):
        return b'<' + rc4(self.object_key(num, gen), s).hex().encode() + b'>'

    def stream(self, num, gen, data):
        return rc4(self.object_key(num, gen), data)


def pdf(name, objects, root=1, xref_stream=False, encryption=None, info=None):
    # objects: {number: bytes | (dict_bytes, stream_bytes)}; strings in
    # dict bytes are written as (...) and left as they are — an encrypted
    # file's objects give their strings through enc.string() themselves.
    out = bytearray(b'%PDF-1.7\n%\xe2\xe3\xcf\xd3\n')
    offsets = {}
    for n in sorted(objects):
        offsets[n] = len(out)
        body = objects[n]
        if isinstance(body, tuple):
            d, data = body
            if encryption:
                data = encryption.stream(n, 0, data)
            body = d[:-2] + b' /Length %d >>\nstream\n' % len(data) + data + b'\nendstream'
        out += b'%d 0 obj\n' % n + body + b'\nendobj\n'
    size = max(objects) + 1
    id0 = bytes(range(16))
    trailer_extra = b' /ID [<' + id0.hex().encode() + b'> <' + id0.hex().encode() + b'>]'
    if encryption:
        trailer_extra += b' /Encrypt %d 0 R' % (size - 1)
    if info:
        trailer_extra += b' /Info %d 0 R' % info
    x = len(out)
    if xref_stream:
        rows = bytearray()
        rows += bytes([0, 0, 0, 0, 0, 255, 255])
        for n in range(1, size + 1):
            if n in offsets:
                rows += bytes([1]) + offsets[n].to_bytes(4, 'big') + b'\x00\x00'
            elif n == size:
                rows += bytes([1]) + x.to_bytes(4, 'big') + b'\x00\x00'
            else:
                rows += bytes([0, 0, 0, 0, 0, 0, 0])
        out += (b'%d 0 obj\n<< /Type /XRef /Size %d /W [1 4 2] /Root %d 0 R%s /Length %d >>\nstream\n' % (size, size + 1, root, trailer_extra, len(rows))
                + bytes(rows) + b'\nendstream\nendobj\n')
    else:
        out += b'xref\n0 %d\n0000000000 65535 f \n' % size
        for n in range(1, size):
            out += (b'%010d 00000 n \n' % offsets[n]) if n in offsets else b'0000000000 00000 f \n'
        out += b'trailer\n<< /Size %d /Root %d 0 R%s >>\n' % (size, root, trailer_extra)
    out += b'startxref\n%d\n%%%%EOF\n' % x
    open(os.path.join(out_dir, name + '.pdf'), 'wb').write(bytes(out))


PAGE = b'0.95 g 0 0 400 400 re f 0 g BT /Helv 10 Tf 20 385 Td (Form fields) Tj ET'


def form_objects(enc=None):
    # Strings (values, names, options) through the encryption when there is one.
    def s(num, text):
        return enc.string(num, 0, text) if enc else b'(' + text + b')'
    objs = {
        1: b'<< /Type /Catalog /Pages 2 0 R /AcroForm << /Fields [10 0 R 11 0 R 12 0 R 13 0 R 14 0 R 15 0 R 18 0 R 19 0 R 20 0 R 21 0 R] '
           b'/DR << /Font << /Helv 5 0 R >> >> /DA ' + s(1, b'/Helv 0 Tf 0 g') + b' >> >>',
        2: b'<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
        3: b'<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 400] /Resources << /Font << /Helv 5 0 R >> >> /Contents 4 0 R '
           b'/Annots [10 0 R 11 0 R 12 0 R 13 0 R 16 0 R 17 0 R 15 0 R 18 0 R 19 0 R 20 0 R 21 0 R 30 0 R 31 0 R] >>',
        4: (b'<< /Filter /FlateDecode >>', zlib.compress(PAGE)),
        5: b'<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>',
        # 10: a text field with a light blue background and a black border, no appearance.
        10: b'<< /Type /Annot /Subtype /Widget /FT /Tx /T ' + s(10, b'name') + b' /V ' + s(10, b'Hello') + b' /Rect [20 340 220 370] /F 4 '
            b'/MK << /BG [0.85 0.9 1] /BC [0 0 0] >> /DA ' + s(10, b'/Helv 12 Tf 0 g') + b' >>',
        # 11: multiline, auto size, centered.
        11: b'<< /Type /Annot /Subtype /Widget /FT /Tx /T ' + s(11, b'notes') + b' /Ff 4096 /Q 1 /V ' + s(11, b'The quick brown fox jumps over the lazy dog, twice over.')
            + b' /Rect [20 260 220 330] /F 4 /MK << /BC [0 0 1] >> /BS << /W 2 /S /D >> >>',
        # 12: comb of 5 cells.
        12: b'<< /Type /Annot /Subtype /Widget /FT /Tx /T ' + s(12, b'code') + b' /Ff 16777216 /MaxLen 5 /V ' + s(12, b'12345')
            + b' /Rect [240 340 380 370] /F 4 /MK << /BC [0 0 0] >> /DA ' + s(12, b'/Helv 0 Tf 0 0 1 rg') + b' >>',
        # 13: a check box with its own appearance states.
        13: b'<< /Type /Annot /Subtype /Widget /FT /Btn /T ' + s(13, b'agree') + b' /V /Yes /AS /Yes /Rect [240 290 270 320] /F 4 '
            b'/MK << /BC [0 0 0] >> /AP << /N << /Yes 40 0 R /Off 41 0 R >> >> >>',
        # 14: a radio group with two kids, 16 and 17.
        14: b'<< /FT /Btn /Ff 32768 /T ' + s(14, b'choice') + b' /V /B /Kids [16 0 R 17 0 R] >>',
        16: b'<< /Type /Annot /Subtype /Widget /Parent 14 0 R /AS /Off /Rect [290 290 320 320] /F 4 /MK << /BC [0 0 0] >> '
            b'/AP << /N << /A 42 0 R /Off 43 0 R >> >> >>',
        17: b'<< /Type /Annot /Subtype /Widget /Parent 14 0 R /AS /B /Rect [340 290 370 320] /F 4 /MK << /BC [0 0 0] >> '
            b'/AP << /N << /B 42 0 R /Off 43 0 R >> >> >>',
        # 15: a hidden text field (never drawn, never listed as shown).
        15: b'<< /Type /Annot /Subtype /Widget /FT /Tx /T ' + s(15, b'secret') + b' /V ' + s(15, b'hidden') + b' /Rect [20 20 100 40] /F 6 >>',
        # 18: a combo box.
        18: b'<< /Type /Annot /Subtype /Widget /FT /Ch /Ff 131072 /T ' + s(18, b'color') + b' /V ' + s(18, b'g') + b' /Opt [[' + s(18, b'r') + b' ' + s(18, b'Red')
            + b'] [' + s(18, b'g') + b' ' + s(18, b'Green') + b'] [' + s(18, b'b') + b' ' + s(18, b'Blue') + b']] /Rect [240 230 380 260] /F 4 '
            b'/MK << /BC [0 0 0] /BG [1 1 0.8] >> /DA ' + s(18, b'/Helv 12 Tf 0 g') + b' >>',
        # 19: a list box, two of four chosen.
        19: b'<< /Type /Annot /Subtype /Widget /FT /Ch /Ff 2097152 /T ' + s(19, b'days') + b' /V [' + s(19, b'Tue') + b' ' + s(19, b'Thu') + b'] /Opt ['
            + s(19, b'Mon') + b' ' + s(19, b'Tue') + b' ' + s(19, b'Wed') + b' ' + s(19, b'Thu') + b'] /Rect [240 140 380 220] /F 4 /MK << /BC [0 0 0] >> '
            b'/DA ' + s(19, b'/Helv 12 Tf 0 g') + b' >>',
        # 20: a push button with a caption, beveled.
        20: b'<< /Type /Annot /Subtype /Widget /FT /Btn /Ff 65536 /T ' + s(20, b'go') + b' /Rect [20 200 120 240] /F 4 '
            b'/MK << /BC [0.3 0.3 0.3] /BG [0.8 0.8 0.8] /CA ' + s(20, b'Submit') + b' >> /BS << /W 2 /S /B >> /DA ' + s(20, b'/Helv 0 Tf 0 g') + b' >>',
        # 21: a read-only text field.
        21: b'<< /Type /Annot /Subtype /Widget /FT /Tx /Ff 1 /T ' + s(21, b'fixed') + b' /V ' + s(21, b'Read only') + b' /Rect [20 140 220 170] /F 4 '
            b'/MK << /BG [0.9 0.9 0.9] >> /DA ' + s(21, b'/Helv 12 Tf 0.5 g') + b' >>',
        # 30: a note (Text annotation) with no appearance; 31: a FreeText.
        30: b'<< /Type /Annot /Subtype /Text /Name /Comment /Rect [20 90 40 110] /F 4 /C [1 1 0] /Contents ' + s(30, b'A note') + b' >>',
        31: b'<< /Type /Annot /Subtype /FreeText /Rect [60 60 220 120] /F 4 /DA ' + s(31, b'/Helv 11 Tf 0 0 0.6 rg') + b' /Contents '
            + s(31, b'Free text drawn from its default appearance') + b' /BS << /W 1 >> >>',
        # 40-43: the check box and radio appearances.
        40: (b'<< /Type /XObject /Subtype /Form /BBox [0 0 30 30] >>', b'0 0 1 rg 5 5 20 20 re f'),
        41: (b'<< /Type /XObject /Subtype /Form /BBox [0 0 30 30] >>', b'0.7 g 5 5 20 20 re f'),
        42: (b'<< /Type /XObject /Subtype /Form /BBox [0 0 30 30] >>', b'1 0 0 rg 8 8 14 14 re f'),
        43: (b'<< /Type /XObject /Subtype /Form /BBox [0 0 30 30] >>', b'0.7 g 8 8 14 14 re f'),
    }
    return objs


pdf('forms', form_objects())
pdf('forms-xrefstream', form_objects(), xref_stream=True)

enc = Encryption(bytes(range(16)))
objs = form_objects(enc)
objs[50] = enc.dict()
pdf('forms-rc4', objs, encryption=enc)

# NeedAppearances: the field's own appearance says OLD in red; the value
# says NEW; a viewer draws NEW.
need = {
    1: b'<< /Type /Catalog /Pages 2 0 R /AcroForm << /Fields [10 0 R] /NeedAppearances true /DR << /Font << /Helv 5 0 R >> >> /DA (/Helv 0 Tf 0 g) >> >>',
    2: b'<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
    3: b'<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 200] /Resources << /Font << /Helv 5 0 R >> >> /Contents 4 0 R /Annots [10 0 R] >>',
    4: (b'<< >>', b'1 g 0 0 300 200 re f'),
    5: b'<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>',
    10: b'<< /Type /Annot /Subtype /Widget /FT /Tx /T (field) /V (NEW) /Rect [20 80 280 120] /F 4 /DA (/Helv 24 Tf 0 g) /AP << /N 11 0 R >> >>',
    11: (b'<< /Type /XObject /Subtype /Form /BBox [0 0 260 40] /Resources << /Font << /Helv 5 0 R >> >> >>', b'1 0 0 rg 0 0 260 40 re f'),
}
pdf('forms-need', need)
print('wrote forms, forms-xrefstream, forms-rc4, forms-need into', out_dir)
