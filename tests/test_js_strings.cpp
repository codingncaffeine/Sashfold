#include "Test.h"

#include "js/Strings.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

using namespace sashfold;

namespace {

constexpr double inf = std::numeric_limits<double>::infinity();

std::string ts(double x, int radix = 10)
{
    return js::utf8_from_utf16(js::number_to_string(x, radix));
}

double stn(std::string_view utf8)
{
    return js::string_to_number(js::utf16_from_utf8(utf8));
}

std::string fixed(double x, int fraction_digits)
{
    return js::utf8_from_utf16(js::number_to_fixed(x, fraction_digits));
}

std::string exponential(double x, std::optional<int> fraction_digits = std::nullopt)
{
    return js::utf8_from_utf16(js::number_to_exponential(x, fraction_digits));
}

std::string precision(double x, int digits)
{
    return js::utf8_from_utf16(js::number_to_precision(x, digits));
}

double pint(std::string_view utf8, int radix)
{
    return js::parse_int(js::utf16_from_utf8(utf8), radix);
}

double pfloat(std::string_view utf8)
{
    return js::parse_float(js::utf16_from_utf8(utf8));
}

bool is_negative_zero(double x)
{
    return x == 0 && std::signbit(x);
}

bool is_positive_zero(double x)
{
    return x == 0 && !std::signbit(x);
}

bool canonical(std::u16string_view s)
{
    return js::is_canonical_numeric_string(s);
}

}

int main()
{
    // ---- Number::toString, radix 10 (§6.1.6.1.20)
    CHECK_EQ(ts(0), "0"); // 1
    CHECK_EQ(ts(-0.0), "0"); // 2: −0 prints as "0"
    CHECK_EQ(ts(std::numeric_limits<double>::quiet_NaN()), "NaN"); // 3
    CHECK_EQ(ts(inf), "Infinity"); // 4
    CHECK_EQ(ts(-inf), "-Infinity"); // 5
    CHECK_EQ(ts(1), "1"); // 6
    CHECK_EQ(ts(-1), "-1"); // 7
    CHECK_EQ(ts(123), "123"); // 8
    CHECK_EQ(ts(0.1 + 0.2), "0.30000000000000004"); // 9
    CHECK_EQ(ts(1e21), "1e+21"); // 10: the first exponent form
    CHECK_EQ(ts(1e20), "100000000000000000000"); // 11: the last plain integer
    CHECK_EQ(ts(123456789012345680000.0), "123456789012345680000"); // 12: k < n ≤ 21 pads zeros
    CHECK_EQ(ts(1e-7), "1e-7"); // 13: the first small exponent form
    CHECK_EQ(ts(0.000001), "0.000001"); // 14: the last "0." form
    CHECK_EQ(ts(5e-324), "5e-324"); // 15: the smallest denormal
    CHECK_EQ(ts(1.7976931348623157e308), "1.7976931348623157e+308"); // 16: the largest double
    CHECK_EQ(ts(0.5), "0.5"); // 17
    CHECK_EQ(ts(1.5e-10), "1.5e-10"); // 18
    CHECK_EQ(ts(123.456), "123.456"); // 19
    CHECK_EQ(ts(1e300), "1e+300"); // 20
    CHECK_EQ(ts(9007199254740992.0), "9007199254740992"); // 21: 2^53
    CHECK_EQ(ts(0.1), "0.1"); // 22
    CHECK_EQ(ts(100), "100"); // 23
    CHECK_EQ(ts(1.23e21), "1.23e+21"); // 24
    CHECK_EQ(ts(-1e-7), "-1e-7"); // 25
    CHECK_EQ(ts(1.0 / 3.0), "0.3333333333333333"); // 26
    CHECK_EQ(ts(12345678901234567890.0), "12345678901234567000"); // 27
    CHECK_EQ(ts(0.000001234), "0.000001234"); // 28
    CHECK_EQ(ts(-1.5), "-1.5"); // 29
    CHECK_EQ(ts(1e16), "10000000000000000"); // 30
    CHECK_EQ(ts(4294967295.0), "4294967295"); // 31
    CHECK_EQ(js::number_to_utf8(1.5), "1.5"); // 32
    CHECK_EQ(js::number_to_utf8(-0.0), "0"); // 33

    // ---- Number::toString, other radices (V8's DoubleToRadixCString)
    CHECK_EQ(ts(0.5, 2), "0.1"); // 34
    CHECK_EQ(ts(255, 16), "ff"); // 35
    // 36: digits stop once the remainder drops under half an ulp of 0.1
    // (2^−57, tripled per digit), so base 3 gets 34 of them, the last
    // rounded up; a longer string would claim precision the double lacks.
    CHECK_EQ(ts(0.1, 3), "0.0022002200220022002200220022002201");
    CHECK_EQ(ts(-255.5, 16), "-ff.8"); // 37
    CHECK_EQ(ts(-1.5, 2), "-1.1"); // 38
    {
        // 39: 10^21 in base 36 is 5v1j4f4ds79m9s exactly; a double carries
        // ten or so base-36 digits of it, and the unrepresentable low
        // digits come out as zeros.
        std::string const r = ts(1e21, 36);
        CHECK_EQ(r.size(), std::size_t { 14 });
        CHECK(r.starts_with("5v1j4f4ds"));
        CHECK(r.ends_with("000"));
    }
    CHECK_EQ(ts(35, 36), "z"); // 40
    CHECK_EQ(ts(0, 2), "0"); // 41
    CHECK_EQ(ts(-0.0, 16), "0"); // 42
    CHECK_EQ(ts(std::numeric_limits<double>::quiet_NaN(), 16), "NaN"); // 43
    CHECK_EQ(ts(-inf, 2), "-Infinity"); // 44
    CHECK_EQ(ts(0.1, 2), "0.0001100110011001100110011001100110011001100110011001101"); // 45
    CHECK_EQ(ts(255.5, 2), "11111111.1"); // 46
    CHECK_EQ(ts(18014398509481984.0, 2), "1000000000000000000000000000000000000000000000000000000"); // 47: 2^54
    CHECK_EQ(ts(1.0 / 3.0, 3), "0.1"); // 48: the last digit rounds up
    CHECK_EQ(ts(4095, 8), "7777"); // 49
    CHECK_EQ(ts(1e21, 10), "1e+21"); // 50: radix 10 stays the spec layout

    // ---- StringToNumber (§7.1.4.1.1)
    CHECK(is_positive_zero(stn(""))); // 51
    CHECK_EQ(stn("  12  "), 12.0); // 52
    CHECK_EQ(stn("0x1F"), 31.0); // 53
    CHECK(std::isnan(stn("-0x1F"))); // 54: no sign on a hex literal
    CHECK_EQ(stn("1e3"), 1000.0); // 55
    CHECK(std::isnan(stn("1e"))); // 56: an incomplete exponent is not a literal
    CHECK_EQ(stn("+.5e-2"), 0.005); // 57
    CHECK_EQ(stn("Infinity"), inf); // 58
    CHECK_EQ(stn("-Infinity"), -inf); // 59
    CHECK(std::isnan(stn("in"))); // 60
    CHECK_EQ(stn("0b101"), 5.0); // 61
    CHECK_EQ(stn("0o17"), 15.0); // 62
    CHECK(std::isnan(stn("1_0"))); // 63: no separators here
    CHECK_EQ(js::string_to_number(u"  7  "), 7.0); // 64: NBSP and LS are trimmed
    CHECK(is_negative_zero(stn("-0"))); // 65
    CHECK_EQ(stn("1."), 1.0); // 66
    CHECK(std::isnan(stn("."))); // 67
    CHECK(std::isnan(stn("0x"))); // 68
    CHECK_EQ(stn("1e400"), inf); // 69: overflow
    CHECK(is_positive_zero(stn("1e-400"))); // 70: underflow
    CHECK_EQ(stn("5e-324"), std::numeric_limits<double>::denorm_min()); // 71
    CHECK_EQ(stn("2e-320"), 2e-320); // 72: a denormal, not zero
    CHECK_EQ(stn("0.1"), 0.1); // 73
    CHECK(std::isnan(stn("12abc"))); // 74
    CHECK(std::isnan(stn("Infinityx"))); // 75
    CHECK_EQ(stn("0X1f"), 31.0); // 76
    CHECK_EQ(stn("0xFFFFFFFFFFFFFFFFF"), 295147905179352825856.0); // 77: 2^68 − 1 rounds to 2^68
    CHECK_EQ(stn("0x20000000000001"), 9007199254740992.0); // 78: 2^53 + 1 ties to even
    CHECK_EQ(stn("0x20000000000003"), 9007199254740996.0); // 79: 2^53 + 3 ties to even, upward
    CHECK_EQ(stn("0x1FFFFFFFFFFFFF"), 9007199254740991.0); // 80: 2^53 − 1 exact
    CHECK(is_positive_zero(stn(" \t\n"))); // 81
    CHECK_EQ(stn("+Infinity"), inf); // 82
    CHECK_EQ(stn("1e+3"), 1000.0); // 83
    CHECK_EQ(stn("-.5"), -0.5); // 84
    CHECK(std::isnan(stn("\xD9\xA3"))); // 85: an Arabic-Indic digit is not a DecimalDigit
    CHECK(std::isnan(stn("1 2"))); // 86
    CHECK_EQ(stn("1.e3"), 1000.0); // 87
    CHECK(std::isnan(stn("infinity"))); // 88: case matters
    CHECK(std::isnan(stn("0b102"))); // 89
    CHECK(std::isnan(stn("0o8"))); // 90
    CHECK_EQ(stn("0x0"), 0.0); // 91
    CHECK_EQ(stn("1.7976931348623157e308"), 1.7976931348623157e308); // 92
    CHECK_EQ(stn("1.7976931348623159e308"), inf); // 93: past the largest double
    CHECK_EQ(stn("9007199254740993"), 9007199254740992.0); // 94: decimal ties to even
    CHECK_EQ(stn("0.30000000000000004"), 0.1 + 0.2); // 95
    CHECK_EQ(stn("000123"), 123.0); // 96: leading zeros are fine in a decimal literal
    CHECK_EQ(stn("\xE3\x80\x80" "42" "\xEF\xBB\xBF"), 42.0); // 97: ideographic space and ZWNBSP trimmed

    // ---- array_index_of
    CHECK(js::array_index_of(u"0") == std::optional<std::uint32_t>(0)); // 98
    CHECK(js::array_index_of(u"42") == std::optional<std::uint32_t>(42)); // 99
    CHECK(js::array_index_of(u"4294967294") == std::optional<std::uint32_t>(4294967294u)); // 100
    CHECK(!js::array_index_of(u"4294967295").has_value()); // 101: 2^32 − 1 is not an index
    CHECK(!js::array_index_of(u"01").has_value()); // 102
    CHECK(!js::array_index_of(u"").has_value()); // 103
    CHECK(!js::array_index_of(u"-1").has_value()); // 104
    CHECK(!js::array_index_of(u"1.5").has_value()); // 105
    CHECK(!js::array_index_of(u"12345678901").has_value()); // 106
    CHECK(!js::array_index_of(u"1e3").has_value()); // 107
    CHECK(!js::array_index_of(u" 1").has_value()); // 108

    // ---- CanonicalNumericIndexString (§7.1.21)
    CHECK(canonical(u"-0")); // 109
    CHECK(canonical(u"0")); // 110
    CHECK(canonical(u"1.5")); // 111
    CHECK(!canonical(u"01")); // 112
    CHECK(canonical(u"NaN")); // 113
    CHECK(canonical(u"Infinity")); // 114
    CHECK(canonical(u"-Infinity")); // 115
    CHECK(canonical(u"1e+21")); // 116
    CHECK(!canonical(u"1e21")); // 117
    CHECK(canonical(u"0.1")); // 118
    CHECK(canonical(u"-1")); // 119
    CHECK(!canonical(u" 1")); // 120
    CHECK(!canonical(u"1.0")); // 121
    CHECK(!canonical(u"")); // 122
    CHECK(canonical(u"1e-7")); // 123

    // ---- toFixed (§21.1.3.3)
    CHECK_EQ(fixed(0.5, 0), "1"); // 124: a tie goes to the larger n
    CHECK_EQ(fixed(2.5, 0), "3"); // 125
    CHECK_EQ(fixed(1.5, 0), "2"); // 126
    CHECK_EQ(fixed(-2.5, 0), "-3"); // 127
    CHECK_EQ(fixed(1.005, 2), "1.00"); // 128: 1.005 is really 1.00499999…
    CHECK_EQ(fixed(1e21, 2), "1e+21"); // 129
    CHECK_EQ(fixed(-1e21, 2), "-1e+21"); // 130
    CHECK_EQ(fixed(123.456, 1), "123.5"); // 131
    CHECK_EQ(fixed(0, 2), "0.00"); // 132
    CHECK_EQ(fixed(-0.0, 2), "0.00"); // 133: −0 loses its sign
    CHECK_EQ(fixed(-1.5, 0), "-2"); // 134
    CHECK_EQ(fixed(0.000001, 2), "0.00"); // 135
    CHECK_EQ(fixed(1.45, 1), "1.4"); // 136: 1.45 is really 1.4499999…
    CHECK_EQ(fixed(8.345, 2), "8.35"); // 137: 8.345 is really 8.3450000000000006…
    CHECK_EQ(fixed(std::numeric_limits<double>::quiet_NaN(), 2), "NaN"); // 138
    CHECK_EQ(fixed(1e-10, 3), "0.000"); // 139
    CHECK_EQ(fixed(999.995, 2), "1000.00"); // 140: 999.995 is really 999.99500000000000454…
    CHECK_EQ(fixed(999.999, 2), "1000.00"); // 141: a carry out of the top digit
    CHECK_EQ(fixed(12345.6789, 0), "12346"); // 142
    CHECK_EQ(fixed(5e-324, 0), "0"); // 143
    CHECK_EQ(fixed(5e-324, 100), "0." + std::string(100, '0')); // 144
    CHECK_EQ(fixed(-0.0000001, 2), "-0.00"); // 145: a negative that rounds to zero keeps its sign
    CHECK_EQ(fixed(1e20, 2), "100000000000000000000.00"); // 146
    CHECK_EQ(fixed(0.000001, 7), "0.0000010"); // 147
    CHECK_EQ(fixed(123.456, 10), "123.4560000000"); // 148
    CHECK_EQ(fixed(1.7976931348623157e308, 2), "1.7976931348623157e+308"); // 149
    CHECK_EQ(fixed(0.005, 2), "0.01"); // 150: 0.005 is really 0.005000000000000000104…
    CHECK_EQ(fixed(0.0005, 2), "0.00"); // 151
    CHECK_EQ(fixed(inf, 2), "Infinity"); // 152
    CHECK_EQ(fixed(10, 1), "10.0"); // 153
    CHECK_EQ(fixed(0.1, 20), "0.10000000000000000555"); // 154: the exact expansion shows

    // ---- toExponential (§21.1.3.2)
    CHECK_EQ(exponential(123.456, 2), "1.23e+2"); // 155
    CHECK_EQ(exponential(0), "0e+0"); // 156
    CHECK_EQ(exponential(0, 2), "0.00e+0"); // 157
    CHECK_EQ(exponential(123456), "1.23456e+5"); // 158: shortest digits when undefined
    CHECK_EQ(exponential(0.00001), "1e-5"); // 159
    CHECK_EQ(exponential(-1.5, 0), "-2e+0"); // 160
    CHECK_EQ(exponential(999.5, 2), "1.00e+3"); // 161: a carry raises the exponent
    CHECK_EQ(exponential(1e21, 1), "1.0e+21"); // 162
    CHECK_EQ(exponential(inf, 2), "Infinity"); // 163
    CHECK_EQ(exponential(0.1, 20), "1.00000000000000005551e-1"); // 164
    CHECK_EQ(exponential(255), "2.55e+2"); // 165
    CHECK_EQ(exponential(1), "1e+0"); // 166
    CHECK_EQ(exponential(-0.0), "0e+0"); // 167
    CHECK_EQ(exponential(5e-324), "5e-324"); // 168
    CHECK_EQ(exponential(1.5, 0), "2e+0"); // 169: a tie goes up
    CHECK_EQ(exponential(123.456, 0), "1e+2"); // 170
    CHECK_EQ(exponential(std::numeric_limits<double>::quiet_NaN(), 1), "NaN"); // 171
    CHECK_EQ(exponential(100, 3), "1.000e+2"); // 172

    // ---- toPrecision (§21.1.3.5)
    CHECK_EQ(precision(123.456, 4), "123.5"); // 173
    CHECK_EQ(precision(0.000123, 2), "0.00012"); // 174
    CHECK_EQ(precision(1234567, 2), "1.2e+6"); // 175
    CHECK_EQ(precision(0, 3), "0.00"); // 176
    CHECK_EQ(precision(123.456, 3), "123"); // 177
    CHECK_EQ(precision(123.456, 2), "1.2e+2"); // 178
    CHECK_EQ(precision(1e21, 3), "1.00e+21"); // 179
    CHECK_EQ(precision(1e-7, 2), "1.0e-7"); // 180: e < −6 is exponent form
    CHECK_EQ(precision(1e-6, 2), "0.0000010"); // 181: e = −6 is not
    CHECK_EQ(precision(-1.5, 1), "-2"); // 182
    CHECK_EQ(precision(99.99, 2), "1.0e+2"); // 183: the carry pushes e to p
    CHECK_EQ(precision(123.456, 6), "123.456"); // 184
    CHECK_EQ(precision(1, 1), "1"); // 185
    CHECK_EQ(precision(2.5, 1), "3"); // 186
    CHECK_EQ(precision(0.1, 21), "0.100000000000000005551"); // 187
    CHECK_EQ(precision(100, 2), "1.0e+2"); // 188
    CHECK_EQ(precision(100, 3), "100"); // 189
    CHECK_EQ(precision(123456, 21), "123456.000000000000000"); // 190
    CHECK_EQ(precision(-inf, 3), "-Infinity"); // 191
    CHECK_EQ(precision(0.5, 1), "0.5"); // 192
    CHECK_EQ(precision(1e100, 1), "1e+100"); // 193: p = 1 has no point

    // ---- parseInt (§19.2.5)
    CHECK_EQ(pint("42", 0), 42.0); // 194
    CHECK_EQ(pint("  -42abc", 0), -42.0); // 195
    CHECK_EQ(pint("0x1F", 0), 31.0); // 196
    CHECK_EQ(pint("0x1F", 16), 31.0); // 197
    CHECK_EQ(pint("0x1F", 10), 0.0); // 198: radix 10 reads the "0" and stops
    CHECK_EQ(pint("-0x1F", 0), -31.0); // 199: the sign comes off before the prefix
    CHECK_EQ(pint("ff", 16), 255.0); // 200
    CHECK(std::isnan(pint("", 0))); // 201
    CHECK(is_negative_zero(pint("-0", 0))); // 202
    CHECK(is_positive_zero(pint("0", 0))); // 203
    CHECK_EQ(pint("101", 2), 5.0); // 204
    CHECK_EQ(pint("z", 36), 35.0); // 205
    CHECK_EQ(pint("Z", 36), 35.0); // 206
    CHECK(std::isnan(pint("1", 1))); // 207
    CHECK(std::isnan(pint("1", 37))); // 208
    CHECK_EQ(pint("  \n12", 0), 12.0); // 209
    CHECK_EQ(pint("12.9", 0), 12.0); // 210
    CHECK_EQ(pint("+7", 0), 7.0); // 211
    CHECK(std::isnan(pint("-", 0))); // 212
    CHECK(std::isnan(pint("0x", 0))); // 213
    CHECK_EQ(pint("9007199254740993", 0), 9007199254740992.0); // 214: correctly rounded
    CHECK_EQ(pint("1e3", 0), 1.0); // 215
    CHECK_EQ(pint("0b11", 0), 0.0); // 216: only 0x is a prefix here
    CHECK_EQ(pint("11", 36), 37.0); // 217
    CHECK_EQ(pint("1" + std::string(400, '0'), 10), inf); // 218
    CHECK_EQ(pint("777", 8), 511.0); // 219
    CHECK_EQ(pint("12", 2), 1.0); // 220
    CHECK(std::isnan(pint("\xD9\xA1\xD9\xA2", 10))); // 221
    CHECK_EQ(pint("\xC2\xA0" "12", 0), 12.0); // 222: NBSP is StrWhiteSpace
    CHECK_EQ(pint("+0x10", 0), 16.0); // 223
    CHECK_EQ(pint("0X10", 0), 16.0); // 224
    CHECK(std::isnan(pint("ff", 0))); // 225
    CHECK(std::isnan(pint("Infinity", 0))); // 226
    CHECK_EQ(pint("zz", 36), 1295.0); // 227
    CHECK_EQ(pint("20000000000001", 16), 9007199254740992.0); // 228: hex ties to even
    CHECK_EQ(pint("2222222222222222222222222222222222222", 3), 450283905890997376.0); // 229: 3^37 − 1, correctly rounded
    CHECK_EQ(pint("1010", 4), 68.0); // 230

    // ---- parseFloat (§19.2.4)
    CHECK_EQ(pfloat("3.14abc"), 3.14); // 231
    CHECK_EQ(pfloat("  .5"), 0.5); // 232
    CHECK_EQ(pfloat("1e"), 1.0); // 233: the incomplete exponent is left behind
    CHECK_EQ(pfloat("1e+"), 1.0); // 234
    CHECK_EQ(pfloat("1e3x"), 1000.0); // 235
    CHECK_EQ(pfloat("-Infinityx"), -inf); // 236
    CHECK_EQ(pfloat("Infinity"), inf); // 237
    CHECK(std::isnan(pfloat("abc"))); // 238
    CHECK_EQ(pfloat("0x10"), 0.0); // 239: no hex
    CHECK(is_negative_zero(pfloat("-0"))); // 240
    CHECK(std::isnan(pfloat("."))); // 241
    CHECK_EQ(pfloat("1_000"), 1.0); // 242
    CHECK(std::isnan(pfloat("+-1"))); // 243
    CHECK_EQ(pfloat("1.5e-3"), 0.0015); // 244
    CHECK(std::isnan(pfloat(""))); // 245
    CHECK_EQ(pfloat("  -.5e1"), -5.0); // 246
    CHECK_EQ(pfloat("1.e3"), 1000.0); // 247
    CHECK(std::isnan(pfloat("Infinit"))); // 248
    CHECK(std::isnan(pfloat("infinity"))); // 249
    CHECK_EQ(pfloat("  +Infinity"), inf); // 250
    CHECK_EQ(pfloat("1e400"), inf); // 251
    CHECK(is_negative_zero(pfloat("-1e-400"))); // 252
    CHECK_EQ(pfloat("0.1.2"), 0.1); // 253

    // ---- WTF-8 both ways
    CHECK(js::utf16_from_utf8("abc") == u"abc"); // 254
    CHECK(js::utf16_from_utf8("\xF0\x9F\x98\x80") == u"\xD83D\xDE00"); // 255: U+1F600 becomes a pair
    CHECK(js::utf8_from_utf16(u"\xD83D\xDE00") == "\xF0\x9F\x98\x80"); // 256: and back to four bytes
    CHECK(js::utf8_from_utf16(u"\xD800") == "\xED\xA0\x80"); // 257: a lone high surrogate, three bytes
    CHECK(js::utf16_from_utf8("\xED\xA0\x80") == u"\xD800"); // 258: and back to one unit
    CHECK(js::utf8_from_utf16(u"\xDC00") == "\xED\xB0\x80"); // 259: a lone low surrogate
    CHECK(js::utf16_from_utf8("\xED\xB0\x80") == u"\xDC00"); // 260
    CHECK(js::utf16_from_utf8(js::utf8_from_utf16(u"a\xD800" u"b\xDFFF" u"c\xD83D\xDE00")) == u"a\xD800" u"b\xDFFF" u"c\xD83D\xDE00"); // 261
    CHECK(js::utf16_from_utf8("\xFF") == u"�"); // 262: an invalid byte
    CHECK(js::utf16_from_utf8("\xE2\x82") == u"�"); // 263: a truncated sequence is one U+FFFD
    CHECK(js::utf16_from_utf8("\xE2\x82" "x") == u"�x"); // 264
    CHECK(js::utf16_from_utf8("\xC0\x80") == u"��"); // 265: an overlong NUL is two bad bytes
    CHECK(js::utf16_from_utf8("\xE2\x82\xAC") == u"€"); // 266
    CHECK(js::utf16_from_utf8("\xF4\x90\x80\x80") == u"����"); // 267: past U+10FFFF
    CHECK(js::utf16_from_utf8("\xC3\xA9") == u"é"); // 268
    CHECK(js::utf8_from_utf16(u"é") == "\xC3\xA9"); // 269
    CHECK(js::utf8_from_utf16(u"\xD83D" "a") == "\xED\xA0\xBD" "a"); // 270: a high surrogate not followed by a low
    CHECK(js::utf8_from_utf16(u"\xDE00\xD83D") == "\xED\xB8\x80\xED\xA0\xBD"); // 271: low then high is two lone ones
    CHECK(js::utf16_from_utf8("\xE0\x80\x80") == u"���"); // 272: overlong three-byte form
    CHECK(js::utf16_from_utf8("\xF0\x9F\x98") == u"�"); // 273: truncated four-byte form
    CHECK(js::utf16_from_utf8("\x80") == u"�"); // 274: a stray continuation byte
    CHECK(js::utf8_from_utf16(u"￿") == "\xEF\xBF\xBF"); // 275
    CHECK(js::utf16_from_utf8("\xF4\x8F\xBF\xBF") == u"\xDBFF\xDFFF"); // 276: U+10FFFF

    // ---- append_code_point / code_point_at
    {
        std::u16string s;
        js::append_code_point(s, 0x41); // 277
        js::append_code_point(s, 0x1F600);
        js::append_code_point(s, 0xD800);
        js::append_code_point(s, 0x110000);
        CHECK(s == u"A\xD83D\xDE00\xD800�");
        std::size_t units = 0;
        CHECK_EQ(js::code_point_at(s, 0, &units), char32_t { 0x41 }); // 278
        CHECK_EQ(units, std::size_t { 1 });
        CHECK_EQ(js::code_point_at(s, 1, &units), char32_t { 0x1F600 }); // 279: the pair joins
        CHECK_EQ(units, std::size_t { 2 });
        CHECK_EQ(js::code_point_at(s, 2, &units), char32_t { 0xDE00 }); // 280: the low half alone is itself
        CHECK_EQ(units, std::size_t { 1 });
        CHECK_EQ(js::code_point_at(s, 3, &units), char32_t { 0xD800 }); // 281: a high surrogate before a non-low
        CHECK_EQ(units, std::size_t { 1 });
        CHECK_EQ(js::code_point_at(s, 5, &units), char32_t { 0 }); // 282: past the end
        CHECK_EQ(units, std::size_t { 0 });
        CHECK_EQ(js::code_point_at(u"\xD800", 0), char32_t { 0xD800 }); // 283: a high surrogate at the end
        CHECK_EQ(js::code_point_at(u"\xDC00\xD800", 0), char32_t { 0xDC00 }); // 284: low then high do not pair
    }

    // ---- whitespace and trimming
    CHECK(js::trim_string(u"\t x 　") == u"x"); // 285
    CHECK(js::trim_string(u"  x  ", true, false) == u"x  "); // 286
    CHECK(js::trim_string(u"  x  ", false, true) == u"  x"); // 287
    CHECK(js::trim_string(u"   ").empty()); // 288
    CHECK(js::trim_string(u"").empty()); // 289
    CHECK(!js::is_string_whitespace(0x200B)); // 290: ZERO WIDTH SPACE is Cf, not Zs
    CHECK(js::is_string_whitespace(0xFEFF)); // 291
    CHECK(js::is_string_whitespace(0x1680)); // 292
    CHECK(!js::is_string_whitespace(0x0085)); // 293: NEL is not a LineTerminator
    CHECK(js::is_string_whitespace(0x2028)); // 294
    CHECK(js::is_string_whitespace(0x2029)); // 295
    CHECK(js::is_string_whitespace(0x000B)); // 296
    CHECK(js::is_string_whitespace(0x202F)); // 297
    CHECK(js::is_string_whitespace(0x205F)); // 298
    CHECK(js::is_string_whitespace(0x2000)); // 299
    CHECK(js::is_string_whitespace(0x200A)); // 300
    CHECK(!js::is_string_whitespace(0x180E)); // 301: MONGOLIAN VOWEL SEPARATOR left Zs in Unicode 6.3
    CHECK(!js::is_string_whitespace(u'x')); // 302

    // ---- simple case mapping
    CHECK(js::to_upper(u"abc") == u"ABC"); // 303
    CHECK(js::to_lower(u"ÀÉ") == u"àé"); // 304
    CHECK(js::to_upper(u"ß") == u"ß"); // 305: ß has only a full mapping
    CHECK(js::to_lower(u"\xD801\xDC00") == u"\xD801\xDC28"); // 306: DESERET CAPITAL LONG I → small
    CHECK(js::to_upper(u"\xD801\xDC28") == u"\xD801\xDC00"); // 307
    CHECK(js::to_upper(u"\xD800x") == u"\xD800X"); // 308: a lone surrogate passes through
    CHECK(js::to_lower(u"Σ") == u"σ"); // 309: simple mapping, not the final form
    CHECK(js::to_upper(u"é") == u"É"); // 310
    CHECK(js::to_lower(u"ABC 123") == u"abc 123"); // 311
    CHECK(js::to_upper(u"").empty()); // 312

    return sashfold::test::report("js_strings");
}
