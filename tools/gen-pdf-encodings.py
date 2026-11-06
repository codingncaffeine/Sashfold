#!/usr/bin/env python3
# Writes src/pdf/EncodingData.cpp: PDF's built-in simple-font encodings
# (ISO 32000-2 Annex D: Standard, WinAnsi, MacRoman, MacExpert's absence
# noted, Symbol, ZapfDingbats) as 256 glyph names each, the 258 standard
# Macintosh glyph names a TrueType 'post' table refers to, and the Unicode
# value of every glyph name those use (beside the algorithmic names the
# reader decodes itself: uniXXXX, uXXXX, afii Cyrillic, a.suffix).
# WinAnsi and MacRoman take their code positions from Python's cp1252 and
# mac_roman codecs, with the differences the PDF standard states.
#   tools/gen-pdf-encodings.py > src/pdf/EncodingData.cpp
import codecs

# Glyph names of the Latin text set and the Macintosh standard set, with their Unicode values.
names = {
    'space': 0x20, 'exclam': 0x21, 'quotedbl': 0x22, 'numbersign': 0x23, 'dollar': 0x24, 'percent': 0x25,
    'ampersand': 0x26, 'quotesingle': 0x27, 'quoteright': 0x2019, 'parenleft': 0x28, 'parenright': 0x29,
    'asterisk': 0x2A, 'plus': 0x2B, 'comma': 0x2C, 'hyphen': 0x2D, 'period': 0x2E, 'slash': 0x2F,
    'zero': 0x30, 'one': 0x31, 'two': 0x32, 'three': 0x33, 'four': 0x34, 'five': 0x35, 'six': 0x36,
    'seven': 0x37, 'eight': 0x38, 'nine': 0x39, 'colon': 0x3A, 'semicolon': 0x3B, 'less': 0x3C,
    'equal': 0x3D, 'greater': 0x3E, 'question': 0x3F, 'at': 0x40, 'bracketleft': 0x5B, 'backslash': 0x5C,
    'bracketright': 0x5D, 'asciicircum': 0x5E, 'underscore': 0x5F, 'grave': 0x60, 'quoteleft': 0x2018,
    'braceleft': 0x7B, 'bar': 0x7C, 'braceright': 0x7D, 'asciitilde': 0x7E,
    'exclamdown': 0xA1, 'cent': 0xA2, 'sterling': 0xA3, 'currency': 0xA4, 'yen': 0xA5, 'brokenbar': 0xA6,
    'section': 0xA7, 'dieresis': 0xA8, 'copyright': 0xA9, 'ordfeminine': 0xAA, 'guillemotleft': 0xAB,
    'logicalnot': 0xAC, 'registered': 0xAE, 'macron': 0xAF, 'degree': 0xB0, 'plusminus': 0xB1,
    'twosuperior': 0xB2, 'threesuperior': 0xB3, 'acute': 0xB4, 'mu': 0xB5, 'paragraph': 0xB6,
    'periodcentered': 0xB7, 'cedilla': 0xB8, 'onesuperior': 0xB9, 'ordmasculine': 0xBA,
    'guillemotright': 0xBB, 'onequarter': 0xBC, 'onehalf': 0xBD, 'threequarters': 0xBE, 'questiondown': 0xBF,
    'Agrave': 0xC0, 'Aacute': 0xC1, 'Acircumflex': 0xC2, 'Atilde': 0xC3, 'Adieresis': 0xC4, 'Aring': 0xC5,
    'AE': 0xC6, 'Ccedilla': 0xC7, 'Egrave': 0xC8, 'Eacute': 0xC9, 'Ecircumflex': 0xCA, 'Edieresis': 0xCB,
    'Igrave': 0xCC, 'Iacute': 0xCD, 'Icircumflex': 0xCE, 'Idieresis': 0xCF, 'Eth': 0xD0, 'Ntilde': 0xD1,
    'Ograve': 0xD2, 'Oacute': 0xD3, 'Ocircumflex': 0xD4, 'Otilde': 0xD5, 'Odieresis': 0xD6, 'multiply': 0xD7,
    'Oslash': 0xD8, 'Ugrave': 0xD9, 'Uacute': 0xDA, 'Ucircumflex': 0xDB, 'Udieresis': 0xDC, 'Yacute': 0xDD,
    'Thorn': 0xDE, 'germandbls': 0xDF, 'agrave': 0xE0, 'aacute': 0xE1, 'acircumflex': 0xE2, 'atilde': 0xE3,
    'adieresis': 0xE4, 'aring': 0xE5, 'ae': 0xE6, 'ccedilla': 0xE7, 'egrave': 0xE8, 'eacute': 0xE9,
    'ecircumflex': 0xEA, 'edieresis': 0xEB, 'igrave': 0xEC, 'iacute': 0xED, 'icircumflex': 0xEE,
    'idieresis': 0xEF, 'eth': 0xF0, 'ntilde': 0xF1, 'ograve': 0xF2, 'oacute': 0xF3, 'ocircumflex': 0xF4,
    'otilde': 0xF5, 'odieresis': 0xF6, 'divide': 0xF7, 'oslash': 0xF8, 'ugrave': 0xF9, 'uacute': 0xFA,
    'ucircumflex': 0xFB, 'udieresis': 0xFC, 'yacute': 0xFD, 'thorn': 0xFE, 'ydieresis': 0xFF,
    'Euro': 0x20AC, 'quotesinglbase': 0x201A, 'florin': 0x192, 'quotedblbase': 0x201E, 'ellipsis': 0x2026,
    'dagger': 0x2020, 'daggerdbl': 0x2021, 'circumflex': 0x2C6, 'perthousand': 0x2030, 'Scaron': 0x160,
    'guilsinglleft': 0x2039, 'OE': 0x152, 'Zcaron': 0x17D, 'quotedblleft': 0x201C, 'quotedblright': 0x201D,
    'bullet': 0x2022, 'endash': 0x2013, 'emdash': 0x2014, 'tilde': 0x2DC, 'trademark': 0x2122,
    'scaron': 0x161, 'guilsinglright': 0x203A, 'oe': 0x153, 'zcaron': 0x17E, 'Ydieresis': 0x178,
    'fraction': 0x2044, 'fi': 0xFB01, 'fl': 0xFB02, 'breve': 0x2D8, 'dotaccent': 0x2D9, 'ring': 0x2DA,
    'hungarumlaut': 0x2DD, 'ogonek': 0x2DB, 'caron': 0x2C7, 'Lslash': 0x141, 'lslash': 0x142,
    'dotlessi': 0x131, 'minus': 0x2212, 'notequal': 0x2260, 'infinity': 0x221E, 'lessequal': 0x2264,
    'greaterequal': 0x2265, 'partialdiff': 0x2202, 'summation': 0x2211, 'product': 0x220F, 'pi': 0x3C0,
    'integral': 0x222B, 'Omega': 0x2126, 'radical': 0x221A, 'approxequal': 0x2248, 'Delta': 0x2206,
    'lozenge': 0x25CA, 'apple': 0xF8FF, 'franc': 0x20A3, 'Gbreve': 0x11E, 'gbreve': 0x11F,
    'Idotaccent': 0x130, 'Scedilla': 0x15E, 'scedilla': 0x15F, 'Cacute': 0x106, 'cacute': 0x107,
    'Ccaron': 0x10C, 'ccaron': 0x10D, 'dcroat': 0x111, 'nonbreakingspace': 0xA0, 'nbspace': 0xA0,
    'sfthyphen': 0xAD, 'ff': 0xFB00, 'ffi': 0xFB03, 'ffl': 0xFB04,
    'Abreve': 0x102, 'abreve': 0x103, 'Aogonek': 0x104, 'aogonek': 0x105, 'Amacron': 0x100, 'amacron': 0x101,
    'Dcaron': 0x10E, 'dcaron': 0x10F, 'Dcroat': 0x110, 'Ecaron': 0x11A, 'ecaron': 0x11B, 'Eogonek': 0x118,
    'eogonek': 0x119, 'Emacron': 0x112, 'emacron': 0x113, 'Edotaccent': 0x116, 'edotaccent': 0x117,
    'Gcommaaccent': 0x122, 'gcommaaccent': 0x123, 'Imacron': 0x12A, 'imacron': 0x12B, 'Iogonek': 0x12E,
    'iogonek': 0x12F, 'Kcommaaccent': 0x136, 'kcommaaccent': 0x137, 'Lacute': 0x139, 'lacute': 0x13A,
    'Lcaron': 0x13D, 'lcaron': 0x13E, 'Lcommaaccent': 0x13B, 'lcommaaccent': 0x13C, 'Nacute': 0x143,
    'nacute': 0x144, 'Ncaron': 0x147, 'ncaron': 0x148, 'Ncommaaccent': 0x145, 'ncommaaccent': 0x146,
    'Ohungarumlaut': 0x150, 'ohungarumlaut': 0x151, 'Omacron': 0x14C, 'omacron': 0x14D, 'Racute': 0x154,
    'racute': 0x155, 'Rcaron': 0x158, 'rcaron': 0x159, 'Rcommaaccent': 0x156, 'rcommaaccent': 0x157,
    'Sacute': 0x15A, 'sacute': 0x15B, 'Scommaaccent': 0x218, 'scommaaccent': 0x219, 'Tcaron': 0x164,
    'tcaron': 0x165, 'Tcommaaccent': 0x162, 'tcommaaccent': 0x163, 'Uhungarumlaut': 0x170,
    'uhungarumlaut': 0x171, 'Umacron': 0x16A, 'umacron': 0x16B, 'Uogonek': 0x172, 'uogonek': 0x173,
    'Uring': 0x16E, 'uring': 0x16F, 'Zacute': 0x179, 'zacute': 0x17A, 'Zdotaccent': 0x17B, 'zdotaccent': 0x17C,
    'commaaccent': 0xF6C3, 'Ldot': 0x13F, 'ldot': 0x140, 'napostrophe': 0x149, 'Eng': 0x14A, 'eng': 0x14B,
    'Hbar': 0x126, 'hbar': 0x127, 'IJ': 0x132, 'ij': 0x133, 'kgreenlandic': 0x138, 'Tbar': 0x166, 'tbar': 0x167,
    'longs': 0x17F, 'Wcircumflex': 0x174, 'wcircumflex': 0x175, 'Ycircumflex': 0x176, 'ycircumflex': 0x177,
    'Ccircumflex': 0x108, 'ccircumflex': 0x109, 'Cdotaccent': 0x10A, 'cdotaccent': 0x10B,
    'Gcircumflex': 0x11C, 'gcircumflex': 0x11D, 'Gdotaccent': 0x120, 'gdotaccent': 0x121,
    'Hcircumflex': 0x124, 'hcircumflex': 0x125, 'Itilde': 0x128, 'itilde': 0x129, 'Ibreve': 0x12C,
    'ibreve': 0x12D, 'Jcircumflex': 0x134, 'jcircumflex': 0x135, 'Obreve': 0x14E, 'obreve': 0x14F,
    'Scircumflex': 0x15C, 'scircumflex': 0x15D, 'Utilde': 0x168, 'utilde': 0x169, 'Ubreve': 0x16C,
    'ubreve': 0x16D, 'Ebreve': 0x114, 'ebreve': 0x115, 'Gcaron': 0x1E6, 'gcaron': 0x1E7,
    'dotlessj': 0x237, 'quoteleftreversed': 0x201B, 'onethird': 0x2153, 'twothirds': 0x2154,
    'oneeighth': 0x215B, 'threeeighths': 0x215C, 'fiveeighths': 0x215D, 'seveneighths': 0x215E,
    'estimated': 0x212E, 'afii61352': 0x2116, 'numero': 0x2116, 'arrowleft': 0x2190, 'arrowup': 0x2191,
    'arrowright': 0x2192, 'arrowdown': 0x2193, 'arrowboth': 0x2194, 'arrowupdn': 0x2195,
    'Alpha': 0x391, 'Beta': 0x392, 'Gamma': 0x393, 'Epsilon': 0x395, 'Zeta': 0x396, 'Eta': 0x397,
    'Theta': 0x398, 'Iota': 0x399, 'Kappa': 0x39A, 'Lambda': 0x39B, 'Mu': 0x39C, 'Nu': 0x39D, 'Xi': 0x39E,
    'Omicron': 0x39F, 'Pi': 0x3A0, 'Rho': 0x3A1, 'Sigma': 0x3A3, 'Tau': 0x3A4, 'Upsilon': 0x3A5,
    'Phi': 0x3A6, 'Chi': 0x3A7, 'Psi': 0x3A8, 'alpha': 0x3B1, 'beta': 0x3B2, 'gamma': 0x3B3,
    'delta': 0x3B4, 'epsilon': 0x3B5, 'zeta': 0x3B6, 'eta': 0x3B7, 'theta': 0x3B8, 'iota': 0x3B9,
    'kappa': 0x3BA, 'lambda': 0x3BB, 'nu': 0x3BD, 'xi': 0x3BE, 'omicron': 0x3BF, 'rho': 0x3C1,
    'sigma1': 0x3C2, 'sigma': 0x3C3, 'tau': 0x3C4, 'upsilon': 0x3C5, 'phi': 0x3C6, 'chi': 0x3C7,
    'psi': 0x3C8, 'omega': 0x3C9, 'theta1': 0x3D1, 'Upsilon1': 0x3D2, 'phi1': 0x3D5, 'omega1': 0x3D6,
    'universal': 0x2200, 'existential': 0x2203, 'suchthat': 0x220B, 'asteriskmath': 0x2217,
    'congruent': 0x2245, 'therefore': 0x2234, 'perpendicular': 0x22A5, 'radicalex': 0xF8E5,
    'similar': 0x223C, 'minute': 0x2032, 'second': 0x2033, 'club': 0x2663, 'diamond': 0x2666,
    'heart': 0x2665, 'spade': 0x2660, 'proportional': 0x221D, 'equivalence': 0x2261,
    'arrowvertex': 0xF8E6, 'arrowhorizex': 0xF8E7, 'carriagereturn': 0x21B5, 'aleph': 0x2135,
    'Ifraktur': 0x2111, 'Rfraktur': 0x211C, 'weierstrass': 0x2118, 'circlemultiply': 0x2297,
    'circleplus': 0x2295, 'emptyset': 0x2205, 'intersection': 0x2229, 'union': 0x222A,
    'propersuperset': 0x2283, 'reflexsuperset': 0x2287, 'notsubset': 0x2284, 'propersubset': 0x2282,
    'reflexsubset': 0x2286, 'element': 0x2208, 'notelement': 0x2209, 'angle': 0x2220, 'gradient': 0x2207,
    'registerserif': 0xF6DA, 'copyrightserif': 0xF6D9, 'trademarkserif': 0xF6DB, 'dotmath': 0x22C5,
    'logicaland': 0x2227, 'logicalor': 0x2228, 'arrowdblboth': 0x21D4, 'arrowdblleft': 0x21D0,
    'arrowdblup': 0x21D1, 'arrowdblright': 0x21D2, 'arrowdbldown': 0x21D3, 'angleleft': 0x2329,
    'registersans': 0xF8E8, 'copyrightsans': 0xF8E9, 'trademarksans': 0xF8EA, 'parenlefttp': 0xF8EB,
    'parenleftex': 0xF8EC, 'parenleftbt': 0xF8ED, 'bracketlefttp': 0xF8EE, 'bracketleftex': 0xF8EF,
    'bracketleftbt': 0xF8F0, 'bracelefttp': 0xF8F1, 'braceleftmid': 0xF8F2, 'braceleftbt': 0xF8F3,
    'braceex': 0xF8F4, 'angleright': 0x232A, 'integraltp': 0x2320, 'integralex': 0xF8F5,
    'integralbt': 0x2321, 'parenrighttp': 0xF8F6, 'parenrightex': 0xF8F7, 'parenrightbt': 0xF8F8,
    'bracketrighttp': 0xF8F9, 'bracketrightex': 0xF8FA, 'bracketrightbt': 0xF8FB, 'bracerighttp': 0xF8FC,
    'bracerightmid': 0xF8FD, 'bracerightbt': 0xF8FE,
}

standard = {32: 'space', 33: 'exclam', 34: 'quotedbl', 35: 'numbersign', 36: 'dollar', 37: 'percent',
    38: 'ampersand', 39: 'quoteright', 40: 'parenleft', 41: 'parenright', 42: 'asterisk', 43: 'plus',
    44: 'comma', 45: 'hyphen', 46: 'period', 47: 'slash', 58: 'colon', 59: 'semicolon', 60: 'less',
    61: 'equal', 62: 'greater', 63: 'question', 64: 'at', 91: 'bracketleft', 92: 'backslash',
    93: 'bracketright', 94: 'asciicircum', 95: 'underscore', 96: 'quoteleft', 123: 'braceleft', 124: 'bar',
    125: 'braceright', 126: 'asciitilde', 161: 'exclamdown', 162: 'cent', 163: 'sterling', 164: 'fraction',
    165: 'yen', 166: 'florin', 167: 'section', 168: 'currency', 169: 'quotesingle', 170: 'quotedblleft',
    171: 'guillemotleft', 172: 'guilsinglleft', 173: 'guilsinglright', 174: 'fi', 175: 'fl', 177: 'endash',
    178: 'dagger', 179: 'daggerdbl', 180: 'periodcentered', 182: 'paragraph', 183: 'bullet',
    184: 'quotesinglbase', 185: 'quotedblbase', 186: 'quotedblright', 187: 'guillemotright', 188: 'ellipsis',
    189: 'perthousand', 191: 'questiondown', 193: 'grave', 194: 'acute', 195: 'circumflex', 196: 'tilde',
    197: 'macron', 198: 'breve', 199: 'dotaccent', 200: 'dieresis', 202: 'ring', 203: 'cedilla',
    205: 'hungarumlaut', 206: 'ogonek', 207: 'caron', 208: 'emdash', 225: 'AE', 227: 'ordfeminine',
    232: 'Lslash', 233: 'Oslash', 234: 'OE', 235: 'ordmasculine', 241: 'ae', 245: 'dotlessi', 248: 'lslash',
    249: 'oslash', 250: 'oe', 251: 'germandbls'}
digits = ['zero', 'one', 'two', 'three', 'four', 'five', 'six', 'seven', 'eight', 'nine']
for i, d in enumerate(digits):
    standard[48 + i] = d
for i in range(26):
    standard[65 + i] = chr(65 + i)
    standard[97 + i] = chr(97 + i)
    names[chr(65 + i)] = 65 + i
    names[chr(97 + i)] = 97 + i

by_unicode = {}
for name, u in names.items():
    by_unicode.setdefault(u, name)
# The PDF names where Unicode has two candidates.
by_unicode[0x2D] = 'hyphen'
by_unicode[0xA0] = 'space'
by_unicode[0xAD] = 'hyphen'
by_unicode[0x27] = 'quotesingle'
by_unicode[0x60] = 'grave'

def codec_table(codec):
    table = [None] * 256
    for code in range(32, 256):
        try:
            u = ord(bytes([code]).decode(codec))
        except UnicodeDecodeError:
            continue
        table[code] = by_unicode.get(u)
    return table

win = codec_table('cp1252')
win[0x7F] = None
# Annex D note: every unused WinAnsi code above 40 octal is the bullet.
for code in range(0x21, 256):
    if win[code] is None:
        win[code] = 'bullet'
mac = codec_table('mac_roman')
mac[0xDB] = 'currency'  # PDF keeps the original Mac Roman: currency, not the euro
mac[0xCA] = 'space'
mac[0x7F] = None

symbol = {32: 'space', 33: 'exclam', 34: 'universal', 35: 'numbersign', 36: 'existential', 37: 'percent',
    38: 'ampersand', 39: 'suchthat', 40: 'parenleft', 41: 'parenright', 42: 'asteriskmath', 43: 'plus',
    44: 'comma', 45: 'minus', 46: 'period', 47: 'slash', 58: 'colon', 59: 'semicolon', 60: 'less',
    61: 'equal', 62: 'greater', 63: 'question', 64: 'congruent', 65: 'Alpha', 66: 'Beta', 67: 'Chi',
    68: 'Delta', 69: 'Epsilon', 70: 'Phi', 71: 'Gamma', 72: 'Eta', 73: 'Iota', 74: 'theta1', 75: 'Kappa',
    76: 'Lambda', 77: 'Mu', 78: 'Nu', 79: 'Omicron', 80: 'Pi', 81: 'Theta', 82: 'Rho', 83: 'Sigma',
    84: 'Tau', 85: 'Upsilon', 86: 'sigma1', 87: 'Omega', 88: 'Xi', 89: 'Psi', 90: 'Zeta', 91: 'bracketleft',
    92: 'therefore', 93: 'bracketright', 94: 'perpendicular', 95: 'underscore', 96: 'radicalex',
    97: 'alpha', 98: 'beta', 99: 'chi', 100: 'delta', 101: 'epsilon', 102: 'phi', 103: 'gamma', 104: 'eta',
    105: 'iota', 106: 'phi1', 107: 'kappa', 108: 'lambda', 109: 'mu', 110: 'nu', 111: 'omicron', 112: 'pi',
    113: 'theta', 114: 'rho', 115: 'sigma', 116: 'tau', 117: 'upsilon', 118: 'omega1', 119: 'omega',
    120: 'xi', 121: 'psi', 122: 'zeta', 123: 'braceleft', 124: 'bar', 125: 'braceright', 126: 'similar',
    160: 'Euro', 161: 'Upsilon1', 162: 'minute', 163: 'lessequal', 164: 'fraction', 165: 'infinity',
    166: 'florin', 167: 'club', 168: 'diamond', 169: 'heart', 170: 'spade', 171: 'arrowboth',
    172: 'arrowleft', 173: 'arrowup', 174: 'arrowright', 175: 'arrowdown', 176: 'degree', 177: 'plusminus',
    178: 'second', 179: 'greaterequal', 180: 'multiply', 181: 'proportional', 182: 'partialdiff',
    183: 'bullet', 184: 'divide', 185: 'notequal', 186: 'equivalence', 187: 'approxequal', 188: 'ellipsis',
    189: 'arrowvertex', 190: 'arrowhorizex', 191: 'carriagereturn', 192: 'aleph', 193: 'Ifraktur',
    194: 'Rfraktur', 195: 'weierstrass', 196: 'circlemultiply', 197: 'circleplus', 198: 'emptyset',
    199: 'intersection', 200: 'union', 201: 'propersuperset', 202: 'reflexsuperset', 203: 'notsubset',
    204: 'propersubset', 205: 'reflexsubset', 206: 'element', 207: 'notelement', 208: 'angle',
    209: 'gradient', 210: 'registerserif', 211: 'copyrightserif', 212: 'trademarkserif', 213: 'product',
    214: 'radical', 215: 'dotmath', 216: 'logicalnot', 217: 'logicaland', 218: 'logicalor',
    219: 'arrowdblboth', 220: 'arrowdblleft', 221: 'arrowdblup', 222: 'arrowdblright', 223: 'arrowdbldown',
    224: 'lozenge', 225: 'angleleft', 226: 'registersans', 227: 'copyrightsans', 228: 'trademarksans',
    229: 'summation', 230: 'parenlefttp', 231: 'parenleftex', 232: 'parenleftbt', 233: 'bracketlefttp',
    234: 'bracketleftex', 235: 'bracketleftbt', 236: 'bracelefttp', 237: 'braceleftmid', 238: 'braceleftbt',
    239: 'braceex', 241: 'angleright', 242: 'integral', 243: 'integraltp', 244: 'integralex',
    245: 'integralbt', 246: 'parenrighttp', 247: 'parenrightex', 248: 'parenrightbt', 249: 'bracketrighttp',
    250: 'bracketrightex', 251: 'bracketrightbt', 252: 'bracerighttp', 253: 'bracerightmid',
    254: 'bracerightbt'}
for i, d in enumerate(digits):
    symbol[48 + i] = d
# Symbol's own names that differ from the Latin set's meanings.
names['Omega'] = 0x2126
names['Delta'] = 0x2206
names['mu'] = 0xB5

# ITC Zapf Dingbats: (code, name, Unicode).
dingbats_list = [
    (32, 'space', 0x20), (33, 'a1', 0x2701), (34, 'a2', 0x2702), (35, 'a202', 0x2703), (36, 'a3', 0x2704),
    (37, 'a4', 0x260E), (38, 'a5', 0x2706), (39, 'a119', 0x2707), (40, 'a118', 0x2708), (41, 'a117', 0x2709),
    (42, 'a11', 0x261B), (43, 'a12', 0x261E), (44, 'a13', 0x270C), (45, 'a14', 0x270D), (46, 'a15', 0x270E),
    (47, 'a16', 0x270F), (48, 'a105', 0x2710), (49, 'a17', 0x2711), (50, 'a18', 0x2712), (51, 'a19', 0x2713),
    (52, 'a20', 0x2714), (53, 'a21', 0x2715), (54, 'a22', 0x2716), (55, 'a23', 0x2717), (56, 'a24', 0x2718),
    (57, 'a25', 0x2719), (58, 'a26', 0x271A), (59, 'a27', 0x271B), (60, 'a28', 0x271C), (61, 'a6', 0x271D),
    (62, 'a7', 0x271E), (63, 'a8', 0x271F), (64, 'a9', 0x2720), (65, 'a10', 0x2721), (66, 'a29', 0x2722),
    (67, 'a30', 0x2723), (68, 'a31', 0x2724), (69, 'a32', 0x2725), (70, 'a33', 0x2726), (71, 'a34', 0x2727),
    (72, 'a35', 0x2605), (73, 'a36', 0x2729), (74, 'a37', 0x272A), (75, 'a38', 0x272B), (76, 'a39', 0x272C),
    (77, 'a40', 0x272D), (78, 'a41', 0x272E), (79, 'a42', 0x272F), (80, 'a43', 0x2730), (81, 'a44', 0x2731),
    (82, 'a45', 0x2732), (83, 'a46', 0x2733), (84, 'a47', 0x2734), (85, 'a48', 0x2735), (86, 'a49', 0x2736),
    (87, 'a50', 0x2737), (88, 'a51', 0x2738), (89, 'a52', 0x2739), (90, 'a53', 0x273A), (91, 'a54', 0x273B),
    (92, 'a55', 0x273C), (93, 'a56', 0x273D), (94, 'a57', 0x273E), (95, 'a58', 0x273F), (96, 'a59', 0x2740),
    (97, 'a60', 0x2741), (98, 'a61', 0x2742), (99, 'a62', 0x2743), (100, 'a63', 0x2744), (101, 'a64', 0x2745),
    (102, 'a65', 0x2746), (103, 'a66', 0x2747), (104, 'a67', 0x2748), (105, 'a68', 0x2749), (106, 'a69', 0x274A),
    (107, 'a70', 0x274B), (108, 'a71', 0x25CF), (109, 'a72', 0x274D), (110, 'a73', 0x25A0), (111, 'a74', 0x274F),
    (112, 'a203', 0x2750), (113, 'a75', 0x2751), (114, 'a204', 0x2752), (115, 'a76', 0x25B2), (116, 'a77', 0x25BC),
    (117, 'a78', 0x25C6), (118, 'a79', 0x2756), (119, 'a81', 0x25D7), (120, 'a82', 0x2758), (121, 'a83', 0x2759),
    (122, 'a84', 0x275A), (123, 'a97', 0x275B), (124, 'a98', 0x275C), (125, 'a99', 0x275D), (126, 'a100', 0x275E),
    (161, 'a101', 0x2761), (162, 'a102', 0x2762), (163, 'a103', 0x2763), (164, 'a104', 0x2764), (165, 'a106', 0x2765),
    (166, 'a107', 0x2766), (167, 'a108', 0x2767), (168, 'a112', 0x2663), (169, 'a111', 0x2666), (170, 'a110', 0x2665),
    (171, 'a109', 0x2660), (172, 'a120', 0x2460), (173, 'a121', 0x2461), (174, 'a122', 0x2462), (175, 'a123', 0x2463),
    (176, 'a124', 0x2464), (177, 'a125', 0x2465), (178, 'a126', 0x2466), (179, 'a127', 0x2467), (180, 'a128', 0x2468),
    (181, 'a129', 0x2469), (182, 'a130', 0x2776), (183, 'a131', 0x2777), (184, 'a132', 0x2778), (185, 'a133', 0x2779),
    (186, 'a134', 0x277A), (187, 'a135', 0x277B), (188, 'a136', 0x277C), (189, 'a137', 0x277D), (190, 'a138', 0x277E),
    (191, 'a139', 0x277F), (192, 'a140', 0x2780), (193, 'a141', 0x2781), (194, 'a142', 0x2782), (195, 'a143', 0x2783),
    (196, 'a144', 0x2784), (197, 'a145', 0x2785), (198, 'a146', 0x2786), (199, 'a147', 0x2787), (200, 'a148', 0x2788),
    (201, 'a149', 0x2789), (202, 'a150', 0x278A), (203, 'a151', 0x278B), (204, 'a152', 0x278C), (205, 'a153', 0x278D),
    (206, 'a154', 0x278E), (207, 'a155', 0x278F), (208, 'a156', 0x2790), (209, 'a157', 0x2791), (210, 'a158', 0x2792),
    (211, 'a159', 0x2793), (212, 'a160', 0x2794), (213, 'a161', 0x2192), (214, 'a163', 0x2194), (215, 'a164', 0x2195),
    (216, 'a196', 0x2798), (217, 'a165', 0x2799), (218, 'a192', 0x279A), (219, 'a166', 0x279B), (220, 'a167', 0x279C),
    (221, 'a168', 0x279D), (222, 'a169', 0x279E), (223, 'a170', 0x279F), (224, 'a171', 0x27A0), (225, 'a172', 0x27A1),
    (226, 'a173', 0x27A2), (227, 'a162', 0x27A3), (228, 'a174', 0x27A4), (229, 'a175', 0x27A5), (230, 'a176', 0x27A6),
    (231, 'a177', 0x27A7), (232, 'a178', 0x27A8), (233, 'a179', 0x27A9), (234, 'a193', 0x27AA), (235, 'a180', 0x27AB),
    (236, 'a199', 0x27AC), (237, 'a181', 0x27AD), (238, 'a200', 0x27AE), (239, 'a182', 0x27AF), (241, 'a201', 0x27B1),
    (242, 'a183', 0x27B2), (243, 'a184', 0x27B3), (244, 'a197', 0x27B4), (245, 'a185', 0x27B5), (246, 'a194', 0x27B6),
    (247, 'a198', 0x27B7), (248, 'a186', 0x27B8), (249, 'a195', 0x27B9), (250, 'a187', 0x27BA), (251, 'a188', 0x27BB),
    (252, 'a189', 0x27BC), (253, 'a190', 0x27BD), (254, 'a191', 0x27BE),
]
dingbats = {code: name for code, name, _ in dingbats_list}
for _, name, u in dingbats_list:
    names[name] = u

mac_standard = ['.notdef', '.null', 'nonmarkingreturn', 'space', 'exclam', 'quotedbl', 'numbersign', 'dollar',
    'percent', 'ampersand', 'quotesingle', 'parenleft', 'parenright', 'asterisk', 'plus', 'comma', 'hyphen',
    'period', 'slash'] + digits + ['colon', 'semicolon', 'less', 'equal', 'greater', 'question', 'at'] + \
    [chr(65 + i) for i in range(26)] + ['bracketleft', 'backslash', 'bracketright', 'asciicircum', 'underscore',
    'grave'] + [chr(97 + i) for i in range(26)] + ['braceleft', 'bar', 'braceright', 'asciitilde', 'Adieresis',
    'Aring', 'Ccedilla', 'Eacute', 'Ntilde', 'Odieresis', 'Udieresis', 'aacute', 'agrave', 'acircumflex',
    'adieresis', 'atilde', 'aring', 'ccedilla', 'eacute', 'egrave', 'ecircumflex', 'edieresis', 'iacute',
    'igrave', 'icircumflex', 'idieresis', 'ntilde', 'oacute', 'ograve', 'ocircumflex', 'odieresis', 'otilde',
    'uacute', 'ugrave', 'ucircumflex', 'udieresis', 'dagger', 'degree', 'cent', 'sterling', 'section', 'bullet',
    'paragraph', 'germandbls', 'registered', 'copyright', 'trademark', 'acute', 'dieresis', 'notequal', 'AE',
    'Oslash', 'infinity', 'plusminus', 'lessequal', 'greaterequal', 'yen', 'mu', 'partialdiff', 'summation',
    'product', 'pi', 'integral', 'ordfeminine', 'ordmasculine', 'Omega', 'ae', 'oslash', 'questiondown',
    'exclamdown', 'logicalnot', 'radical', 'florin', 'approxequal', 'Delta', 'guillemotleft', 'guillemotright',
    'ellipsis', 'nonbreakingspace', 'Agrave', 'Atilde', 'Otilde', 'OE', 'oe', 'endash', 'emdash',
    'quotedblleft', 'quotedblright', 'quoteleft', 'quoteright', 'divide', 'lozenge', 'ydieresis', 'Ydieresis',
    'fraction', 'currency', 'guilsinglleft', 'guilsinglright', 'fi', 'fl', 'daggerdbl', 'periodcentered',
    'quotesinglbase', 'quotedblbase', 'perthousand', 'Acircumflex', 'Ecircumflex', 'Aacute', 'Edieresis',
    'Egrave', 'Iacute', 'Icircumflex', 'Idieresis', 'Igrave', 'Oacute', 'Ocircumflex', 'apple', 'Ograve',
    'Uacute', 'Ucircumflex', 'Ugrave', 'dotlessi', 'circumflex', 'tilde', 'macron', 'breve', 'dotaccent',
    'ring', 'cedilla', 'hungarumlaut', 'ogonek', 'caron', 'Lslash', 'lslash', 'Scaron', 'scaron', 'Zcaron',
    'zcaron', 'brokenbar', 'Eth', 'eth', 'Yacute', 'yacute', 'Thorn', 'thorn', 'minus', 'multiply',
    'onesuperior', 'twosuperior', 'threesuperior', 'onehalf', 'onequarter', 'threequarters', 'franc', 'Gbreve',
    'gbreve', 'Idotaccent', 'Scedilla', 'scedilla', 'Cacute', 'cacute', 'Ccaron', 'ccaron', 'dcroat']
assert len(mac_standard) == 258, len(mac_standard)

def emit_table(cname, table):
    print('std::array<char const*, 256> const %s = {' % cname)
    row = []
    for code in range(256):
        name = table.get(code) if isinstance(table, dict) else table[code]
        row.append('"%s"' % name if name else 'nullptr')
        if len(row) == 8:
            print('    ' + ', '.join(row) + ',')
            row = []
    print('};')
    print()

for n in list(names):
    pass
used = set()
for t in (standard, symbol, dingbats):
    used.update(t.values())
for t in (win, mac):
    used.update(n for n in t if n)
used.update(mac_standard)

print('// Generated by tools/gen-pdf-encodings.py; do not edit.')
print('#include "pdf/Encoding.h"')
print()
print('namespace sashfold::pdf::encoding_data {')
print()
emit_table('k_standard', standard)
emit_table('k_win_ansi', win)
emit_table('k_mac_roman', mac)
emit_table('k_symbol', symbol)
emit_table('k_zapf_dingbats', dingbats)
print('std::array<char const*, 258> const k_mac_glyph_names = {')
for i in range(0, 258, 8):
    print('    ' + ', '.join('"%s"' % n for n in mac_standard[i:i + 8]) + ',')
print('};')
print()
print('// Sorted by name for a binary search.')
print('std::array<GlyphUnicode, %d> const k_glyph_unicode = { {' % len(names))
for name in sorted(names):
    print('    { "%s", 0x%04X },' % (name, names[name]))
print('} };')
print()
print('}')
print()
print('namespace sashfold::pdf {')
print()
print('std::span<GlyphUnicode const> glyph_unicode_table()')
print('{')
print('    return encoding_data::k_glyph_unicode;')
print('}')
print()
print('}')
missing = sorted(n for n in used if n not in names and n not in ('.notdef', '.null', 'nonmarkingreturn'))
assert not missing, missing
