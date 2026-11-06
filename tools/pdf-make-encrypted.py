#!/usr/bin/env python3
# Writes an encrypted one-page PDF (RC4-40 R2, AES-128 R4 or AES-256 R6)
# independently of Sashfold's code: hashlib MD5, a Python RC4 and the openssl CLI for AES.
# poppler (pdfinfo/pdftotext) is the judge that this writer is right.
#   pdf-make-encrypted.py r2|r4|r6 out.pdf user-password owner-password
import hashlib, os, subprocess, sys

mode, out, upw, opw = sys.argv[1], sys.argv[2], sys.argv[3].encode(), sys.argv[4].encode()
PAD = bytes([0x28,0xBF,0x4E,0x5E,0x4E,0x75,0x8A,0x41,0x64,0x00,0x4E,0x56,0xFF,0xFA,0x01,0x08,
             0x2E,0x2E,0x00,0xB6,0xD0,0x68,0x3E,0x80,0x2F,0x0C,0xA9,0xFE,0x64,0x53,0x69,0x7A])

def pad(p): return (p + PAD)[:32]
def md5(b): return hashlib.md5(b).digest()
def rc4(key, data):
    s = list(range(256)); j = 0
    for i in range(256):
        j = (j + s[i] + key[i % len(key)]) & 255; s[i], s[j] = s[j], s[i]
    i = j = 0; out = bytearray()
    for c in data:
        i = (i + 1) & 255; j = (j + s[i]) & 255; s[i], s[j] = s[j], s[i]
        out.append(c ^ s[(s[i] + s[j]) & 255])
    return bytes(out)
def aes_raw(key, iv, data, decrypt=False):
    r = subprocess.run(['openssl', 'enc', '-aes-%d-cbc' % (len(key) * 8), '-K', key.hex(), '-iv', iv.hex(), '-nopad']
                       + (['-d'] if decrypt else []), input=data, capture_output=True, check=True)
    return r.stdout
def aes_cbc(key, iv, data):
    n = 16 - len(data) % 16
    return iv + aes_raw(key, iv, data + bytes([n]) * n)
def hash_r6(pw, salt, udata):
    k = hashlib.sha256(pw + salt + udata).digest()
    i = 0
    while True:
        k1 = (pw + k + udata) * 64
        e = aes_raw(k[:16], k[16:32], k1)
        m = sum(e[:16]) % 3
        k = [hashlib.sha256, hashlib.sha384, hashlib.sha512][m](e).digest()
        i += 1
        if i >= 64 and e[-1] <= i - 32:
            break
    return k[:32]

ID0 = bytes(range(16))
P = -1340
n = 5 if mode == 'r2' else 16
if mode == 'r6':
    key = os.urandom(32)
    uvs, uks, ovs, oks = os.urandom(8), os.urandom(8), os.urandom(8), os.urandom(8)
    u = hash_r6(upw, uvs, b'') + uvs + uks
    ue = aes_raw(hash_r6(upw, uks, b''), bytes(16), key)
    o = hash_r6(opw, ovs, u) + ovs + oks
    oe = aes_raw(hash_r6(opw, oks, u), bytes(16), key)
else:
    # Algorithm 3: O
    k = md5(pad(opw))
    if mode != 'r2':
        for _ in range(50): k = md5(k)
    k = k[:n]
    o = pad(upw)
    if mode == 'r2':
        o = rc4(k, o)
    else:
        for i in range(20): o = rc4(bytes(b ^ i for b in k), o)
    # Algorithm 2: the file key
    data = pad(upw) + o + (P & 0xFFFFFFFF).to_bytes(4, 'little') + ID0
    key = md5(data)
    if mode != 'r2':
        for _ in range(50): key = md5(key[:n])
    key = key[:n]
    # Algorithms 4 and 5: U
    if mode == 'r2':
        u = rc4(key, PAD)
    else:
        u = rc4(key, md5(PAD + ID0))
        for i in range(1, 20): u = rc4(bytes(b ^ i for b in key), u)
        u = u + bytes(16)

def objkey(num, gen, salt):
    return md5(key + num.to_bytes(3, 'little') + gen.to_bytes(2, 'little') + salt)[:min(n + 5, 16)]
def enc(num, data):
    if mode == 'r6':
        return aes_cbc(key, os.urandom(16), data)
    if mode == 'r2':
        return rc4(objkey(num, 0, b''), data)
    return aes_cbc(objkey(num, 0, b'sAlT'), os.urandom(16), data)
def hexs(b): return b'<' + b.hex().encode() + b'>'

content = b'BT /F1 24 Tf 72 700 Td (Hello encrypted page) Tj ET'
objs = {}
objs[1] = b'<< /Type /Catalog /Pages 2 0 R >>'
objs[2] = b'<< /Type /Pages /Kids [3 0 R] /Count 1 >>'
objs[3] = b'<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>'
c = enc(4, content)
objs[4] = b'<< /Length ' + str(len(c)).encode() + b' >>\nstream\n' + c + b'\nendstream'
objs[5] = b'<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>'
objs[6] = b'<< /Title ' + hexs(enc(6, b'Encrypted Title Check')) + b' >>'
if mode == 'r2':
    objs[7] = b'<< /Filter /Standard /V 1 /R 2 /P ' + str(P).encode() + b' /O ' + hexs(o) + b' /U ' + hexs(u) + b' >>'
elif mode == 'r4':
    objs[7] = (b'<< /Filter /Standard /V 4 /R 4 /Length 128 /P ' + str(P).encode() +
               b' /O ' + hexs(o) + b' /U ' + hexs(u) +
               b' /CF << /StdCF << /CFM /AESV2 /AuthEvent /DocOpen /Length 16 >> >> /StmF /StdCF /StrF /StdCF >>')
else:
    perms = subprocess.run(['openssl', 'enc', '-aes-256-ecb', '-K', key.hex(), '-nopad'],
                           input=(P & 0xFFFFFFFF).to_bytes(4, 'little') + b'\xff\xff\xff\xffTadb' + os.urandom(4),
                           capture_output=True, check=True).stdout
    objs[7] = (b'<< /Filter /Standard /V 5 /R 6 /Length 256 /P ' + str(P).encode() +
               b' /O ' + hexs(o) + b' /U ' + hexs(u) + b' /OE ' + hexs(oe) + b' /UE ' + hexs(ue) + b' /Perms ' + hexs(perms) +
               b' /CF << /StdCF << /CFM /AESV3 /AuthEvent /DocOpen /Length 32 >> >> /StmF /StdCF /StrF /StdCF >>')
body = bytearray((b'%PDF-2.0\n' if mode == 'r6' else b'%PDF-1.6\n') + b'%\xe2\xe3\xcf\xd3\n')
offsets = {}
for num in sorted(objs):
    offsets[num] = len(body)
    body += str(num).encode() + b' 0 obj\n' + objs[num] + b'\nendobj\n'
xref = len(body)
body += b'xref\n0 8\n0000000000 65535 f \n'
for num in range(1, 8):
    body += b'%010d 00000 n \n' % offsets[num]
body += (b'trailer\n<< /Size 8 /Root 1 0 R /Info 6 0 R /Encrypt 7 0 R /ID [' + hexs(ID0) + hexs(ID0) +
         b'] >>\nstartxref\n' + str(xref).encode() + b'\n%%EOF\n')
open(out, 'wb').write(body)
