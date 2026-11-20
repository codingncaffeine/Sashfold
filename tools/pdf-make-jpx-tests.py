#!/usr/bin/env python3
# Writes one-page PDFs whose one picture is JPEG 2000 made by an independent
# encoder (OpenJPEG's opj_compress), in the variants a decoder must read:
# lossless 5/3 and lossy 9/7, one to many decomposition levels, tiles, the
# five progression orders, precincts, small code-blocks, SOP and EPH
# markers, each code-block style (bypass, reset, terminate all, causal,
# predictable termination, segmentation symbols), gray, RGB with and
# without the component transform, odd sizes, an alpha channel. Beside
# each, OpenJPEG's own decoding (opj_decompress) as <name>.truth.png. The
# page is the picture's size in points: a 72 dpi render is one pixel a
# sample, to compare with the truth (pdf_probe --render).
#   pdf-make-jpx-tests.py <out-folder>
#   pdf-make-jpx-tests.py --fixtures <folder>   (test_pdf's: small files
#       and OpenJPEG's decoding of each as raw 8-bit samples, <name>.raw)
import os, random, struct, subprocess, sys
from PIL import Image, ImageDraw

fixtures = sys.argv[1] == '--fixtures'
out_dir = sys.argv[2] if fixtures else sys.argv[1]
work = os.path.join(out_dir, 'work')
os.makedirs(work, exist_ok=True)


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


def picture(mode, width, height, seed):
    # A photograph's smooth parts, edges and text, and some noise.
    rng = random.Random(seed)
    image = Image.new('RGB', (width, height))
    pixels = image.load()
    for y in range(height):
        for x in range(width):
            pixels[x, y] = ((x * 255) // max(1, width - 1), (y * 255) // max(1, height - 1), ((x + y) * 7) % 256)
    draw = ImageDraw.Draw(image)
    if width < 16 or height < 16:
        return image.convert('L') if mode == 'L' else image
    draw.ellipse([width // 5, height // 5, width * 3 // 5, height * 4 // 5], fill=(240, 200, 30))
    draw.rectangle([width // 2, height // 3, width - 5, height // 2], fill=(10, 40, 160))
    draw.text((6, 4), 'Sashfold JPX', fill=(0, 0, 0), font_size=max(10, height // 8))
    for _ in range(width * height // 40):
        pixels[rng.randrange(width), rng.randrange(height)] = (rng.randrange(256), rng.randrange(256), rng.randrange(256))
    if mode == 'L':
        return image.convert('L')
    if mode == 'RGBA':
        alpha = Image.new('L', (width, height))
        ImageDraw.Draw(alpha).ellipse([0, 0, width - 1, height - 1], fill=255)
        image.putalpha(alpha)
    return image


def make(name, mode, size, options, ext='jp2'):
    source = os.path.join(work, name + ('.pgm' if mode == 'L' else '.png' if mode == 'RGBA' else '.ppm'))
    picture(mode, size[0], size[1], len(name)).save(source)
    coded = os.path.join(work, name + '.' + ext)
    subprocess.run(['opj_compress', '-i', source, '-o', coded] + options, check=True, stdout=subprocess.DEVNULL)
    truth = os.path.join(out_dir, name + '.truth.png')
    subprocess.run(['opj_decompress', '-i', coded, '-o', truth], check=True, stdout=subprocess.DEVNULL)
    data = open(coded, 'rb').read()
    space = b'' if mode == 'RGBA' else b' /ColorSpace /DeviceGray' if mode == 'L' else b' /ColorSpace /DeviceRGB'
    smask = b' /SMaskInData 1' if mode == 'RGBA' else b''
    image_dict = (b'/Type /XObject /Subtype /Image /Width %d /Height %d /BitsPerComponent 8 /Filter /JPXDecode'
                  % size + space + smask)
    pdf('jpx-' + name, image_dict, data, size[0], size[1])


def segments(data, at, end):
    # Marker segments from `at` to the first of `end` (marker values): [(marker, bytes)], and where it stopped.
    out = []
    while at + 2 <= len(data):
        marker = struct.unpack('>H', data[at:at + 2])[0]
        if marker in end:
            break
        length = struct.unpack('>H', data[at + 2:at + 4])[0]
        out.append((marker, data[at:at + 2 + length]))
        at += 2 + length
    return out, at


def pack_headers(codestream, where):
    # Rewrites a codestream made with SOP and EPH markers so its packet
    # headers move into PPM segments of the main header (where='ppm') or PPT
    # segments of each tile-part header ('ppt') (T.800 A.7.4, A.7.5). Each
    # header ends at its EPH (stuffing keeps 0xFF92 out of headers) and each
    # body runs to the next SOP (0xFF91 cannot occur in coded data); SOP stays
    # with the body, EPH goes with the header. The segments are split in
    # awkward places: a PPM's lengths run across segments.
    main, at = segments(codestream, 2, (0xFF90,))
    parts = []
    while at + 12 <= len(codestream) and codestream[at:at + 2] == b'\xff\x90':
        _, _, isot, psot, tpsot, tnsot = struct.unpack('>HHHIBB', codestream[at:at + 12])
        header, sod = segments(codestream, at + 12, (0xFF93,))
        body = codestream[sod + 2:at + psot]
        headers, bodies = b'', b''
        starts = [i for i in range(len(body) - 1) if body[i] == 0xFF and body[i + 1] == 0x91]
        for k, start in enumerate(starts):
            stop = starts[k + 1] if k + 1 < len(starts) else len(body)
            eph = body.index(b'\xff\x92', start + 6) + 2
            headers += body[start + 6:eph]
            bodies += body[start:start + 6] + body[eph:stop]
        parts.append((isot, tpsot, tnsot, header, headers, bodies))
        at += psot
    out = b'\xff\x4f' + b''.join(seg for _, seg in main)
    if where == 'ppm':
        joined = b''.join(struct.pack('>I', len(p[4])) + p[4] for p in parts)
        cuts = [0, len(joined) // 3 + 1, 2 * len(joined) // 3 + 2, len(joined)]
        for z in range(3):
            piece = joined[cuts[z]:cuts[z + 1]]
            out += struct.pack('>HHB', 0xFF60, 3 + len(piece), z) + piece
    z_of_tile = {}
    for isot, tpsot, tnsot, header, headers, bodies in parts:
        extra = b''
        if where == 'ppt':
            for piece in (headers[:len(headers) // 2], headers[len(headers) // 2:]):
                z = z_of_tile.get(isot, 0)
                z_of_tile[isot] = z + 1
                extra += struct.pack('>HHB', 0xFF61, 3 + len(piece), z) + piece
        head = b''.join(seg for _, seg in header) + extra
        psot = 12 + len(head) + 2 + len(bodies)
        out += struct.pack('>HHHIBB', 0xFF90, 10, isot, psot, tpsot, tnsot) + head + b'\xff\x93' + bodies
    return out + b'\xff\xd9'


def poc_to_main(codestream):
    # A one-tile codestream's tile-part POC moved into the main header,
    # where it means the same (A.6.6).
    main, at = segments(codestream, 2, (0xFF90,))
    _, _, isot, psot, tpsot, tnsot = struct.unpack('>HHHIBB', codestream[at:at + 12])
    header, sod = segments(codestream, at + 12, (0xFF93,))
    moved = b''.join(seg for marker, seg in header if marker == 0xFF5F)
    kept = b''.join(seg for marker, seg in header if marker != 0xFF5F)
    body = codestream[sod:at + psot]
    part = struct.pack('>HHHIBB', 0xFF90, 10, isot, 12 + len(kept) + len(body), tpsot, tnsot) + kept + body
    return b'\xff\x4f' + b''.join(seg for _, seg in main) + moved + part + codestream[at + psot:]


def poc_fixtures(size, options):
    # Progression order changes in a tile-part header (as OpenJPEG writes
    # them; its tile T1 is tile 0) and the same in the main header.
    source = os.path.join(work, 'poc.ppm')
    picture('RGB', size[0], size[1], 7).save(source)
    tile = os.path.join(out_dir, 'jpx-poc.j2k')
    subprocess.run(['opj_compress', '-i', source, '-o', tile] + options, check=True, stdout=subprocess.DEVNULL)
    main = os.path.join(out_dir, 'jpx-poc-main.j2k')
    open(main, 'wb').write(poc_to_main(open(tile, 'rb').read()))
    truths = []
    for name, stream in (('jpx-poc', tile), ('jpx-poc-main', main)):
        truth = os.path.join(work, name + '.truth.png')
        subprocess.run(['opj_decompress', '-i', stream, '-o', truth], check=True, stdout=subprocess.DEVNULL)
        truths.append(Image.open(truth).convert('RGB').tobytes())
        open(os.path.join(out_dir, name + '.raw'), 'wb').write(truths[-1])
    assert truths[0] == truths[1], 'OpenJPEG decodes the main-header POC differently'


def fixture(name, mode, size, options, ext):
    source = os.path.join(work, name + ('.pgm' if mode == 'L' else '.ppm'))
    picture(mode, size[0], size[1], len(name)).save(source)
    coded = os.path.join(out_dir, name + '.' + ext)
    subprocess.run(['opj_compress', '-i', source, '-o', coded] + options, check=True, stdout=subprocess.DEVNULL)
    truth = os.path.join(work, name + '.truth.png')
    subprocess.run(['opj_decompress', '-i', coded, '-o', truth], check=True, stdout=subprocess.DEVNULL)
    open(os.path.join(out_dir, name + '.raw'), 'wb').write(Image.open(truth).convert(mode).tobytes())


def packed_fixture(name, where, size, options):
    # A codestream made with SOP and EPH, its packet headers then moved into
    # PPM or PPT segments; OpenJPEG decodes the rewritten stream, and its
    # decoding must equal that of the original.
    source = os.path.join(work, name + '.ppm')
    picture('RGB', size[0], size[1], len(name)).save(source)
    plain = os.path.join(work, name + '-plain.j2k')
    subprocess.run(['opj_compress', '-i', source, '-o', plain, '-SOP', '-EPH'] + options, check=True, stdout=subprocess.DEVNULL)
    coded = os.path.join(out_dir, name + '.j2k')
    open(coded, 'wb').write(pack_headers(open(plain, 'rb').read(), where))
    truths = []
    for stream in (plain, coded):
        truth = os.path.join(work, os.path.basename(stream) + '.truth.png')
        subprocess.run(['opj_decompress', '-i', stream, '-o', truth], check=True, stdout=subprocess.DEVNULL)
        truths.append(Image.open(truth).convert('RGB').tobytes())
    assert truths[0] == truths[1], name + ': OpenJPEG decodes the rewritten stream differently'
    open(os.path.join(out_dir, name + '.raw'), 'wb').write(truths[1])


def signed_fixture(name, size):
    # Signed gray: a PDF reader shows a sample plus half the range. The
    # truth is OpenJPEG's decoding (as PGX, at the depth it coded), so mapped.
    gray = picture('L', size[0], size[1], len(name))
    source = os.path.join(work, name + '.pgx')
    with open(source, 'wb') as f:
        f.write(b'PG ML - 8 %d %d\n' % size)
        f.write(bytes((v - 128) & 0xFF for v in gray.tobytes()))
    coded = os.path.join(out_dir, name + '.j2k')
    subprocess.run(['opj_compress', '-i', source, '-o', coded, '-n', '3'], check=True, stdout=subprocess.DEVNULL)
    decoded = os.path.join(work, name + '-out.pgx')
    subprocess.run(['opj_decompress', '-i', coded, '-o', decoded], check=True, stdout=subprocess.DEVNULL)
    data = open(decoded[:-4] + '_0.pgx', 'rb').read() # one file a component
    head = data[:data.index(b'\n') + 1].split()
    depth = int(head[-3])
    top = (1 << depth) - 1
    values = struct.unpack('>%db' % (size[0] * size[1]), data[len(b' '.join(head)) + 1:])
    open(os.path.join(out_dir, name + '.raw'), 'wb').write(bytes(round(min(max(v + (1 << (depth - 1)), 0), top) * 255 / top) for v in values))


if fixtures:
    fixture('jpx-lossless', 'RGB', (45, 31), ['-n', '3'], 'jp2')
    fixture('jpx-lossy', 'L', (40, 33), ['-n', '4', '-I', '-r', '12'], 'j2k')
    fixture('jpx-modes', 'RGB', (52, 37), ['-n', '3', '-M', '59', '-p', 'RPCL', '-c', '[16,16]', '-t', '30,20', '-SOP', '-EPH'], 'jp2')
    # Progression order changes: three volumes in three orders over four
    # layers, each later one reaching packets an earlier one read.
    poc_fixtures((40, 30), ['-n', '3', '-r', '30,10,3,1', '-POC', 'T1=0,0,2,2,3,RLCP/T1=0,0,4,4,2,CPRL/T1=0,2,4,4,3,LRCP'])
    # Precincts on a tile grid that starts off the precinct grid, position-major.
    fixture('jpx-pcrl-offset', 'RGB', (41, 33), ['-n', '3', '-p', 'PCRL', '-c', '[8,8]', '-d', '5,3', '-T', '1,1', '-t', '13,11'], 'j2k')
    packed_fixture('jpx-ppm', 'ppm', (36, 28), ['-n', '3', '-r', '20,4,1', '-t', '20,16'])
    packed_fixture('jpx-ppt', 'ppt', (36, 28), ['-n', '3', '-r', '20,4,1', '-t', '20,16', '-TP', 'R'])
    signed_fixture('jpx-signed', (33, 21))
    import shutil
    shutil.rmtree(work)
    sys.exit(0)

variants = [
    ('lossless-rgb', 'RGB', (160, 120), []),
    ('lossless-gray', 'L', (161, 97), []),
    ('lossless-j2k', 'RGB', (64, 48), [], 'j2k'),
    ('lossy-rgb', 'RGB', (160, 120), ['-I', '-r', '20']),
    ('lossy-gray', 'L', (150, 101), ['-I', '-r', '10']),
    ('levels-1', 'RGB', (90, 70), ['-n', '2']),
    ('levels-7', 'RGB', (300, 200), ['-n', '7']),
    ('tiles', 'RGB', (200, 150), ['-t', '64,48']),
    ('tiles-lossy', 'RGB', (200, 150), ['-t', '70,50', '-I', '-r', '15']),
    ('rlcp', 'RGB', (120, 90), ['-p', 'RLCP', '-r', '40,10,1']),
    ('rpcl', 'RGB', (120, 90), ['-p', 'RPCL', '-c', '[32,32],[32,32]']),
    ('pcrl', 'RGB', (120, 90), ['-p', 'PCRL', '-c', '[32,32],[32,32]']),
    ('cprl', 'RGB', (120, 90), ['-p', 'CPRL', '-c', '[32,32]']),
    ('layers', 'RGB', (120, 90), ['-r', '50,20,5,1']),
    ('precincts', 'RGB', (130, 100), ['-c', '[64,64],[32,32],[16,16]']),
    ('blocks-small', 'RGB', (100, 80), ['-b', '8,16']),
    ('sop-eph', 'RGB', (100, 80), ['-SOP', '-EPH']),
    ('mode-bypass', 'RGB', (100, 80), ['-M', '1']),
    ('mode-reset', 'RGB', (100, 80), ['-M', '2']),
    ('mode-termall', 'RGB', (100, 80), ['-M', '4']),
    ('mode-causal', 'RGB', (100, 80), ['-M', '8']),
    ('mode-predictable', 'RGB', (100, 80), ['-M', '16']),
    ('mode-segsym', 'RGB', (100, 80), ['-M', '32']),
    ('mode-all', 'RGB', (100, 80), ['-M', '63', '-I', '-r', '8']),
    ('no-mct', 'RGB', (100, 80), ['-mct', '0']),
    ('odd', 'RGB', (37, 23), ['-n', '4']),
    ('tiny', 'L', (1, 1), ['-n', '1']),
    ('offset', 'RGB', (90, 60), ['-d', '13,7', '-t', '40,40', '-T', '5,3']),
    ('alpha', 'RGBA', (96, 64), []),
]
for variant in variants:
    make(*variant)
