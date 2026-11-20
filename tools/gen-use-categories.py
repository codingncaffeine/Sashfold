#!/usr/bin/env python3
# Writes src/text/UseCategoryData.h: the character classes of the Universal
# Shaping Engine ("Creating and supporting OpenType fonts for the Universal
# Shaping Engine", §Character classification), derived from the Unicode
# data the way the specification's two tables derive them: the class from
# the Indic syllabic category, the general category and the Arabic joining
# type; the subclass of the classes that vary by position from the Indic
# positional category. The specification's override files
# (IndicSyllabicCategory-Additional.txt, IndicPositionalCategory-Additional.txt,
# tools/ucd-fetch.sh puts them in ucd/p17) replace or add values first.
#
#   tools/gen-use-categories.py ucd/p17 > src/text/UseCategoryData.h
#
# The directory holds UnicodeData.txt, ArabicShaping.txt,
# IndicSyllabicCategory.txt, IndicPositionalCategory.txt and the two
# override files. What the specification's tables leave without a subclass
# (a position the table has no row for) is listed on standard error and
# takes the subclass written in POSITION_FALLBACK below.
import os
import sys

data = sys.argv[1]


def ranges(path):
    for line in open(os.path.join(data, path), encoding='utf-8'):
        line = line.split('#', 1)[0].strip()
        if not line:
            continue
        fields = [f.strip() for f in line.split(';')]
        span = fields[0]
        if '..' in span:
            a, b = span.split('..')
            first, last = int(a, 16), int(b, 16)
        else:
            first = last = int(span, 16)
        yield first, last, fields[1:]


def load_property(path, extra=None):
    values = {}
    for first, last, fields in ranges(path):
        for c in range(first, last + 1):
            values[c] = fields[0]
    if extra:
        for first, last, fields in ranges(extra):
            for c in range(first, last + 1):
                values[c] = fields[0]
    return values


general = {}
first_of_range = None
for line in open(os.path.join(data, 'UnicodeData.txt'), encoding='utf-8'):
    fields = line.rstrip('\n').split(';')
    if len(fields) < 3:
        continue
    code = int(fields[0], 16)
    if fields[1].endswith(', First>'):
        first_of_range = code
        continue
    if fields[1].endswith(', Last>') and first_of_range is not None:
        for c in range(first_of_range, code + 1):
            general[c] = fields[2]
        first_of_range = None
        continue
    general[code] = fields[2]

joining = {}
for first, last, fields in ranges('ArabicShaping.txt'):
    for c in range(first, last + 1):
        joining[c] = fields[1]

syllabic = load_property('IndicSyllabicCategory.txt', 'IndicSyllabicCategory-Additional.txt')
positional = load_property('IndicPositionalCategory.txt', 'IndicPositionalCategory-Additional.txt')

BASE_WHEN_LO = {'Avagraha', 'Bindu', 'Consonant_Final', 'Consonant_Medial', 'Consonant_Subjoined', 'Vowel',
                'Vowel_Dependent'}
BASE = {'Number', 'Consonant', 'Consonant_Head_Letter', 'Tone_Letter', 'Vowel_Independent'}
HIEROGLYPH = {
    'Hieroglyph': 'G',
    'Hieroglyph_Joiner': 'J',
    'Hieroglyph_Mark_Begin': 'SB',
    'Hieroglyph_Segment_Begin': 'SB',
    'Hieroglyph_Mark_End': 'SE',
    'Hieroglyph_Segment_End': 'SE',
    'Hieroglyph_Mirror': 'HR',
    'Hieroglyph_Modifier': 'HM',
}


# Classes the specification's tables do not give, as shaping engines in
# browsers have them (settled by comparing outputs, each with strings that
# tell the readings apart): Sinhala's al-lakuna joins consonants as a
# virama does and also follows the dependent vowels, before the vowel
# modifiers (HVM, a halant or vowel modifier).
MEASURED = {
    0x0DCA: 'HVM',
}


# §Character classification, the first table: the class of a character.
def use_class(c):
    if c in MEASURED:
        return MEASURED[c]
    gc = general.get(c, 'Cn')
    isc = syllabic.get(c, 'Other')
    ajt = joining.get(c)
    if isc in HIEROGLYPH:
        return HIEROGLYPH[isc]
    if c == 0x034F:
        return 'CGJ'
    if 0xFE00 <= c <= 0xFE0F:
        return 'VS'
    if c == 0x2060:
        return 'WJ'
    if isc == 'Joiner':
        return 'ZWJ'
    if isc == 'Non_Joiner':
        return 'ZWNJ'
    if isc == 'Consonant_Placeholder' or c in (0x2015, 0x2022) or 0x25FB <= c <= 0x25FE:
        return 'GB'
    if ajt in ('C', 'D', 'L', 'R') or isc in BASE or (isc in BASE_WHEN_LO and gc == 'Lo'):
        return 'B'
    # The specification counts a symbol modifier as a consonant modifier;
    # engines in browsers let it modify only a symbol (SM, settled by
    # comparing outputs: one after a consonant gets a dotted circle).
    if isc == 'Symbol_Modifier':
        return 'SM'
    if isc in ('Nukta', 'Gemination_Mark', 'Consonant_Killer'):
        return 'CM'
    if isc == 'Consonant_With_Stacker':
        return 'CS'
    if isc in ('Consonant_Final', 'Consonant_Succeeding_Repha'):
        return 'F'
    if isc in ('Syllable_Modifier', 'Consonant_Final_Modifier'):
        return 'FM'
    if isc in ('Virama', 'Invisible_Stacker'):
        return 'H'
    if isc == 'Number_Joiner':
        return 'HN'
    if isc == 'Consonant_Medial':
        return 'M'
    if isc == 'Brahmi_Joining_Number':
        return 'N'
    if isc in ('Consonant_Preceding_Repha', 'Consonant_Prefixed'):
        return 'R'
    if isc == 'Reordering_Killer':
        return 'RK'
    if isc == 'Consonant_Subjoined':
        return 'SUB'
    if isc in ('Vowel', 'Vowel_Dependent', 'Pure_Killer'):
        return 'V'
    # A mark with no syllabic category but a position (Tibetan's yar tshes
    # and mar tshes, its astrological signs below) is placed as a dependent
    # vowel there, as engines in browsers have it (settled by comparing
    # outputs: mar tshes reorders before the base like a pre-base vowel).
    if isc == 'Other' and gc in ('Mn', 'Mc') and positional.get(c, 'NA') != 'NA':
        return 'V'
    if isc in ('Bindu', 'Tone_Mark', 'Cantillation_Mark', 'Register_Shifter', 'Visarga'):
        return 'VM'
    if (gc == 'So' and c != 0x25CC) or gc == 'Sc':
        return 'S'
    if isc in ('Consonant_Dead', 'Modifying_Letter') or (gc == 'Po' and c not in (0x104E, 0x2022)):
        return 'IND'
    if gc == 'Cn':
        return 'Rsv'
    return 'O'


# The second table: the subclasses by position.
CARDINAL = {
    'Top': 'Abv',
    'Bottom': 'Blw',
    'Right': 'Pst',
    'Left': 'Pre',
}
VOWEL_POSITION = {
    'Top': 'Abv',
    'Top_And_Bottom': 'Abv',
    'Top_And_Bottom_And_Right': 'Abv',
    'Top_And_Right': 'Abv',
    'Bottom': 'Blw',
    'Overstruck': 'Blw',
    'Bottom_And_Right': 'Blw',
    'Left': 'Pre',
    'Top_And_Left': 'Pre',
    'Top_And_Left_And_Right': 'Pre',
    'Left_And_Right': 'Pre',
    'Right': 'Pst',
}
VOWEL_MOD_POSITION = {
    'Top': 'Abv',
    'Bottom': 'Blw',
    'Overstruck': 'Blw',
    'Left': 'Pre',
    'Right': 'Pst',
}
ALLOWED = {
    'CM': ({'Top': 'Abv', 'Bottom': 'Blw'}),
    'SM': ({'Top': 'Abv', 'Bottom': 'Blw'}),
    'F': ({'Top': 'Abv', 'Bottom': 'Blw', 'Right': 'Pst'}),
    'M': CARDINAL,
    'V': VOWEL_POSITION,
    'VM': VOWEL_MOD_POSITION,
}
# Positions the tables have no row for: the nearest cardinal one (a mark
# without a position sits above, a medial drawn below and to the right is
# a below one).
POSITION_FALLBACK = {
    'CM': 'Abv',
    'SM': 'Abv',
    'F': 'Pst',
    'M': 'Blw',
    'V': 'Abv',
    'VM': 'Abv',
}
FALLBACK_BY_POSITION = {
    'Bottom_And_Right': 'Blw',
    'Bottom_And_Left': 'Blw',
    'Top_And_Bottom': 'Abv',
    'Top_And_Right': 'Abv',
    'Top_And_Left': 'Abv',
    'Overstruck': 'Blw',
    'Right': 'Pst',
    'Left': 'Pre',
    'Top': 'Abv',
    'Bottom': 'Blw',
}

unlisted = {}


def use_category(c):
    k = use_class(c)
    if k not in ALLOWED:
        return k
    ipc = positional.get(c, 'NA')
    table = ALLOWED[k]
    if ipc in table:
        return k + table[ipc]
    sub = FALLBACK_BY_POSITION.get(ipc)
    if sub is None or k + sub not in NAMES:
        sub = POSITION_FALLBACK[k]
    unlisted.setdefault((k, ipc, k + sub), []).append(c)
    return k + sub


NAMES = ['O', 'B', 'CGJ', 'CMAbv', 'CMBlw', 'CS', 'FAbv', 'FBlw', 'FPst', 'FM', 'G', 'GB', 'H', 'HM', 'HN', 'HR',
         'IND', 'J', 'MPre', 'MAbv', 'MBlw', 'MPst', 'N', 'R', 'RK', 'Rsv', 'S', 'SB', 'SE', 'SUB', 'VPre', 'VAbv',
         'VBlw', 'VPst', 'VMPre', 'VMAbv', 'VMBlw', 'VMPst', 'VS', 'WJ', 'ZWJ', 'ZWNJ', 'HVM', 'SMAbv', 'SMBlw']

out_ranges = []
for c in range(0x110000):
    k = use_category(c)
    if k in ('O', 'Rsv'):
        continue
    if out_ranges and out_ranges[-1][2] == k and out_ranges[-1][1] == c - 1:
        out_ranges[-1][1] = c
    else:
        out_ranges.append([c, c, k])

for (k, ipc, chosen), cs in sorted(unlisted.items()):
    sample = ' '.join(f'{c:04X}' for c in cs[:8])
    print(f'gen-use-categories: no subclass for {k} at {ipc} ({len(cs)}: {sample}{" ..." if len(cs) > 8 else ""}) -> {chosen}',
          file=sys.stderr)

print('#pragma once')
print()
print('// GENERATED by tools/gen-use-categories.py from the Unicode data files')
print('// (UnicodeData.txt, ArabicShaping.txt, IndicSyllabicCategory.txt,')
print('// IndicPositionalCategory.txt) and the Universal Shaping Engine')
print('// specification\'s override files. Do not edit.')
print('// The character classes of the Universal Shaping Engine, with the')
print('// subclasses by position, as ranges; a character in none is O (other).')
print()
print('#include <array>')
print('#include <cstdint>')
print()
print('namespace sashfold::text::shaping::use_data {')
print()
print('enum Category : std::uint8_t {')
for i, n in enumerate(NAMES):
    print(f'    {n} = {i},')
print('};')
print()
print('inline constexpr char const* category_names[] = {')
print('    ' + ', '.join(f'"{n}"' for n in NAMES) + ',')
print('};')
print()
print('struct Range {')
print('    char32_t first;')
print('    char32_t last;')
print('    Category category;')
print('};')
print()
print(f'inline constexpr std::array<Range, {len(out_ranges)}> ranges {{ {{')
for first, last, k in out_ranges:
    print(f'    {{ 0x{first:04X}, 0x{last:04X}, {k} }},')
print('} };')
print()
print('}')
