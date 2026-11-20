// PDF form scripts (pdf/FormScript.h): the AF functions and util against
// the examples of Adobe's JavaScript for Acrobat API Reference and forms
// documentation, table by table; the order form, formats, filters,
// validation, reset, show/hide and a calculation loop of forms-js.pdf
// (tools/pdf-make-form-tests.py) through the requests the viewer sends;
// the values saved with what the scripts made; and a hostile document's
// reach for the page, the network, unending time and memory, each of
// which must fail inside the sandbox.

#include "Test.h"

#include "core/Json.h"
#include "pdf/Document.h"
#include "pdf/Form.h"
#include "pdf/FormScript.h"
#include "pdf/Writer.h"

#include <chrono>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace sashfold;

namespace {

using Bytes = std::vector<std::uint8_t>;

Bytes read_file(std::string const& path)
{
    std::ifstream in(path, std::ios::binary);
    return Bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string quoted(std::string const& text)
{
    std::string out = "\"";
    for (char const c : text) {
        if (c == '\n') {
            out += "\\n";
            continue;
        }
        if (c == '"' || c == '\\')
            out.push_back('\\');
        out.push_back(c);
    }
    return out + "\"";
}

struct Form {
    std::shared_ptr<pdf::Document const> doc;
    std::unique_ptr<pdf::FormSandbox> sandbox;
    pdf::FormValues values;

    JsonValue ask(std::string const& json)
    {
        std::string const answer = sandbox->request(json);
        pdf::apply_script_changes(*doc, answer, values);
        std::optional<JsonValue> parsed = JsonValue::parse(answer);
        CHECK(parsed.has_value());
        return parsed ? *parsed : JsonValue();
    }
    std::string eval(std::string const& source)
    {
        JsonValue const answer = ask("{\"type\":\"eval\",\"source\":" + quoted(source) + "}");
        JsonValue const* result = answer.get("result");
        return result && result->is_string() ? result->as_string() : std::string("(no result)");
    }
    JsonValue commit(std::string const& field, std::string const& value)
    {
        return ask("{\"type\":\"commit\",\"field\":" + quoted(field) + ",\"value\":" + quoted(value) + ",\"commitKey\":2}");
    }
};

std::optional<Form> open_form(std::string const& path, pdf::FormSandbox::Limits limits = {})
{
    pdf::Document::Status status {};
    auto doc = pdf::Document::open(read_file(path), "", status);
    CHECK(doc != nullptr);
    if (!doc)
        return std::nullopt;
    Form form;
    form.doc = doc;
    form.sandbox = std::make_unique<pdf::FormSandbox>(pdf::form_script_model(*doc, {}, "test.pdf"), limits);
    return form;
}

std::string change_of(JsonValue const& answer, std::string const& field, std::string const& key)
{
    JsonValue const* changes = answer.get("changes");
    JsonValue const* entry = changes ? changes->get(field) : nullptr;
    JsonValue const* value = entry ? entry->get(key) : nullptr;
    if (!value)
        return "(none)";
    if (value->is_string())
        return value->as_string();
    if (value->is_bool())
        return value->as_bool() ? "true" : "false";
    return "(other)";
}

// The AF functions and util, row by row: an expression and what it gives.
void test_af_tables(Form& form)
{
    // T: a format's text (and its color where it sets one); K: a
    // keystroke's verdict and change; V: a validation's verdict.
    form.eval(R"(
        function T(value, call) {
            var target = { name: "t", textColor: null };
            event = { value: value, target: target, rc: true, willCommit: false, change: "", selStart: 0, selEnd: 0 };
            call();
            return String(event.value) + (target.textColor && target.textColor[0] !== "G" ? "|" + target.textColor.join(",") : "");
        }
        function K(value, change, start, end, commit, call) {
            event = { value: value, target: { name: "t" }, rc: true, willCommit: commit, change: change, selStart: start, selEnd: end };
            call();
            return String(event.rc) + "|" + event.change;
        }
        "ok")");
    struct Row {
        char const* expression;
        char const* expected;
    };
    Row const rows[] = {
        // util.printf (the API Reference's example: n = Math.PI * 100).
        { "util.printf('Decimal format: %d', Math.PI * 100)", "Decimal format: 314" },
        { "util.printf('Hex format: %x', Math.PI * 100)", "Hex format: 13A" },
        { "util.printf('Float format: %.2f', Math.PI * 100)", "Float format: 314.16" },
        { "util.printf('String format: %s', Math.PI * 100)", "String format: 314.159265358979" },
        { "util.printf('%,0.2f', 1234567.891)", "1,234,567.89" },
        { "util.printf('%,1.2f', 1234567.891)", "1234567.89" },
        { "util.printf('%,2.2f', 1234567.891)", "1.234.567,89" },
        { "util.printf('%,3.2f', 1234567.891)", "1234567,89" },
        { "util.printf('%+d|% d|%05d|%5s|%%', 7, 7, -42, 'ab')", "+7| 7|-0042|   ab|%" },
        // util.printd and util.scand.
        { "util.printd('mm/dd/yyyy', new Date(2024, 2, 5))", "03/05/2024" },
        { "util.printd('mmm d, yyyy', new Date(2024, 0, 5))", "Jan 5, 2024" },
        { "util.printd('dddd, mmmm dd, yyyy', new Date(2024, 2, 1))", "Friday, March 01, 2024" },
        { "util.printd('h:MM tt', new Date(2024, 0, 5, 13, 7))", "1:07 pm" },
        { "util.printd('HH:MM:ss', new Date(2024, 0, 5, 9, 7, 3))", "09:07:03" },
        { "util.printd(0, new Date(2024, 0, 5, 13, 7, 0))", "D:20240105130700" },
        { "util.printd(1, new Date(2024, 0, 5, 13, 7, 0))", "2024.01.05 13:07:00" },
        { "util.printd(2, new Date(2024, 0, 5, 13, 7, 0))", "1/5/24 1:07:00 pm" },
        { "util.printd('yy-mm-dd', util.scand('mm/dd/yyyy', '03/15/2024'))", "24-03-15" },
        { "String(util.scand('mm/dd/yyyy', '02/30/2024'))", "null" },
        { "util.printd('mmmm d', util.scand('mmm d, yyyy', 'Dec 25, 2023'))", "December 25" },
        // util.printx (the reference's example) and its case marks.
        { "util.printx('9 (999) 999-9999', 'aaa14159697489zzz')", "1 (415) 969-7489" },
        { "util.printx('>AAAA', 'ab-cd')", "ABCD" },
        { "util.printx('<X? X*', 'A1-B2rest')", "a1 b2rest" },
        // AFNumber_Format: the four separator styles, the negative styles, currency.
        { "T('1234.5', function () { AFNumber_Format(2, 0, 0, 0, '$', true); })", "$1,234.50" },
        { "T(-1234.5, function () { AFNumber_Format(2, 1, 0, 0, '', false); })", "-1234.50" },
        { "T('1234.5', function () { AFNumber_Format(2, 2, 0, 0, ' €', false); })", "1.234,50 €" },
        { "T('1234.5', function () { AFNumber_Format(2, 3, 0, 0, '', false); })", "1234,50" },
        { "T('1234.5', function () { AFNumber_Format(2, 4, 0, 0, '', false); })", "1'234.50" },
        { "T('-5', function () { AFNumber_Format(2, 0, 1, 0, '', false); })", "5.00|RGB,1,0,0" },
        { "T('-5', function () { AFNumber_Format(2, 0, 2, 0, '', false); })", "(5.00)" },
        { "T('-5', function () { AFNumber_Format(2, 0, 3, 0, '$', true); })", "($5.00)|RGB,1,0,0" },
        { "T('-5', function () { AFNumber_Format(2, 0, 0, 0, '$', true); })", "-$5.00" },
        { "T('1234.5', function () { AFNumber_Format(0, 0, 0, 0, '', false); })", "1,235" },
        { "T('1,5', function () { AFNumber_Format(1, 0, 0, 0, '', false); })", "1.5" },
        { "T('abc', function () { AFNumber_Format(2, 0, 0, 0, '', false); })", "" },
        { "T('-0.001', function () { AFNumber_Format(2, 0, 0, 0, '', false); })", "0.00" },
        // AFPercent_Format.
        { "T('0.1234', function () { AFPercent_Format(2, 0); })", "12.34%" },
        { "T('0.5', function () { AFPercent_Format(0, 0); })", "50%" },
        { "T('12.3456', function () { AFPercent_Format(1, 2); })", "1.234,6%" },
        { "T('0.1234', function () { AFPercent_Format(2, 0, true); })", "%12.34" },
        // AFDate_Format(Ex) and AFTime_Format(Ex).
        { "T('3/5/2024', function () { AFDate_FormatEx('mm/dd/yyyy'); })", "03/05/2024" },
        { "T('5.3.2024', function () { AFDate_FormatEx('dd.mm.yyyy'); })", "05.03.2024" },
        { "T('March 5, 2024', function () { AFDate_Format(10); })", "Mar 5, 2024" },
        { "T('3/5/24', function () { AFDate_Format(2); })", "03/05/24" },
        { "T('2024-03-05', function () { AFDate_FormatEx('yyyy-mm-dd'); })", "2024-03-05" },
        { "T('5 Mar 2024', function () { AFDate_FormatEx('dd-mmm-yyyy'); })", "05-Mar-2024" },
        { "T('not a date', function () { AFDate_FormatEx('mm/dd/yyyy'); })", "" },
        { "T('1:05 pm', function () { AFTime_Format(0); })", "13:05" },
        { "T('13:05', function () { AFTime_Format(1); })", "1:05 pm" },
        { "T('13:05:09', function () { AFTime_Format(3); })", "1:05:09 pm" },
        { "T('9:5', function () { AFTime_FormatEx('HH:MM'); })", "09:05" },
        // AFSpecial_Format: zip, zip+4, phone, social security.
        { "T('123456789', function () { AFSpecial_Format(0); })", "12345" },
        { "T('123456789', function () { AFSpecial_Format(1); })", "12345-6789" },
        { "T('5551234567', function () { AFSpecial_Format(2); })", "(555) 123-4567" },
        { "T('1234567', function () { AFSpecial_Format(2); })", "123-4567" },
        { "T('123456789', function () { AFSpecial_Format(3); })", "123-45-6789" },
        // AFSimple and the helpers.
        { "[AFSimple('SUM', 2, 3), AFSimple('PRD', 2, 3), AFSimple('AVG', 2, 3), AFSimple('MIN', 2, 3), AFSimple('MAX', 2, 3)].join()", "5,6,2.5,2,3" },
        { "[AFMakeNumber('1,5'), AFMakeNumber(' 12 '), AFMakeNumber('abc'), AFMakeNumber(7)].join('|')", "1.5|12||7" },
        { "AFMergeChange({ value: 'abcd', change: 'X', selStart: 1, selEnd: 3, willCommit: false })", "aXd" },
        { "AFMergeChange({ value: 'abcd', change: 'X', selStart: 1, selEnd: 3, willCommit: true })", "abcd" },
        { "AFExtractNums('123-45-6789').join('|')", "123|45|6789" },
        { "String(AFExtractNums('none'))", "null" },
        { "util.printd('mm/dd/yyyy', AFParseDateEx('12/31/99', 'mm/dd/yy'))", "12/31/1999" },
        { "util.printd('mm/dd/yyyy', AFParseDateEx('31.12.2023', 'dd.mm.yyyy'))", "12/31/2023" },
        { "util.printd('HH:MM', AFParseDateEx('2:30 pm', 'h:MM tt'))", "14:30" },
        // Keystrokes: numbers in the style's separators, dates, masks.
        { "K('12', '3', 2, 2, false, function () { AFNumber_Keystroke(2, 0, 0, 0, '', true); })", "true|3" },
        { "K('12', 'a', 2, 2, false, function () { AFNumber_Keystroke(2, 0, 0, 0, '', true); })", "false|a" },
        { "K('12', ',', 2, 2, false, function () { AFNumber_Keystroke(2, 2, 0, 0, '', true); })", "true|," },
        { "K('12', ',', 2, 2, false, function () { AFNumber_Keystroke(2, 0, 0, 0, '', true); })", "false|," },
        { "K('1x', '', 0, 0, true, function () { AFNumber_Keystroke(2, 0, 0, 0, '', true); })", "false|" },
        { "K('-1.5', '', 0, 0, true, function () { AFNumber_Keystroke(2, 0, 0, 0, '', true); })", "true|" },
        { "K('13/45/2024', '', 0, 0, true, function () { AFDate_KeystrokeEx('mm/dd/yyyy'); })", "false|" },
        { "K('12/24/2024', '', 0, 0, true, function () { AFDate_KeystrokeEx('mm/dd/yyyy'); })", "true|" },
        { "K('123', '4', 3, 3, false, function () { AFSpecial_KeystrokeEx('999-9999'); })", "true|-4" },
        { "K('123', 'x', 3, 3, false, function () { AFSpecial_KeystrokeEx('999-9999'); })", "false|x" },
        { "K('123-45', '', 0, 0, true, function () { AFSpecial_KeystrokeEx('999-9999'); })", "false|" },
        { "K('1234', '5', 4, 4, false, function () { AFSpecial_Keystroke(0); })", "true|5" },
        { "K('12345', '6', 5, 5, false, function () { AFSpecial_Keystroke(0); })", "false|6" },
        // AFRange_Validate: inside, outside, either bound alone.
        { "K('5', '', 0, 0, true, function () { AFRange_Validate(true, 0, true, 10); })", "true|" },
        { "K('11', '', 0, 0, true, function () { AFRange_Validate(true, 0, true, 10); })", "false|" },
        { "K('-1', '', 0, 0, true, function () { AFRange_Validate(true, 0, false, 0); })", "false|" },
        { "K('', '', 0, 0, true, function () { AFRange_Validate(true, 0, true, 10); })", "true|" },
        // color.
        { "color.convert(['RGB', 1, 0, 0], 'G').map(function (v) { return typeof v === 'number' ? v.toFixed(2) : v; }).join()", "G,0.30" },
        { "color.convert(color.cyan, 'RGB').join()", "RGB,0,1,1" },
        { "[color.equal(color.red, ['RGB', 1, 0, 0]), color.equal(color.red, color.blue), color.equal(['G', 1], color.white)].join()", "true,false,true" },
        // The constants.
        { "[display.visible, display.hidden, display.noPrint, display.noView, border.s, style.ch].join()", "0,1,2,3,solid,check" },
    };
    int failures = 0;
    for (Row const& row : rows) {
        std::string const got = form.eval(row.expression);
        if (got != row.expected) {
            ++failures;
            std::printf("AF row: %s\n  got      %s\n  expected %s\n", row.expression, got.c_str(), row.expected);
        }
        CHECK_EQ(got, std::string(row.expected));
    }
    std::printf("AF table: %zu rows, %d differ\n", std::size(rows), failures);
    // The alert a range refusal raises, as the forms documentation words it.
    JsonValue const refused = form.ask("{\"type\":\"eval\",\"source\":\"K('11', '', 0, 0, true, function () { AFRange_Validate(true, 0, true, 10); })\"}");
    JsonValue const* alerts = refused.get("alerts");
    CHECK(alerts && alerts->is_array() && alerts->as_array().size() == 1
        && alerts->as_array()[0].as_string() == "Invalid value: must be greater than or equal to 0 and less than or equal to 10.");
}

void test_order_form(std::string const& fixtures)
{
    std::optional<Form> made = open_form(fixtures + "/forms/forms-js.pdf");
    if (!made)
        return;
    Form& form = *made;
    test_af_tables(form);

    // Open: the document script, then the open action that uses it; the
    // formatted fields shown formatted.
    JsonValue open = form.ask("{\"type\":\"open\"}");
    CHECK_EQ(change_of(open, "status", "value"), std::string("opened yes"));
    CHECK_EQ(change_of(open, "rate", "display"), std::string("8.0%"));

    // The order: quantity x price per row, the subtotal, the tax at the
    // rate, the total, each formatted, in /CO order.
    form.commit("qty1", "3");
    form.commit("price1", "19.99");
    form.commit("qty2", "2");
    JsonValue const last = form.commit("price2", "5");
    CHECK_EQ(change_of(last, "line2", "display"), std::string("$10.00"));
    CHECK_EQ(change_of(last, "subtotal", "display"), std::string("$69.97"));
    CHECK_EQ(change_of(last, "tax", "display"), std::string("5,60 \xE2\x82\xAC"));
    CHECK_EQ(change_of(last, "total", "display"), std::string("$75.57"));
    CHECK_EQ(change_of(last, "total", "value"), std::string("75.5676"));
    CHECK_EQ(form.eval("this.getField('line1').valueAsString"), std::string("59.97"));
    CHECK_EQ(form.eval("typeof this.getField('subtotal').value"), std::string("number"));

    // Formats with their keystroke checks at commit.
    CHECK_EQ(change_of(form.commit("date", "5.3.2024"), "date", "display"), std::string("05.03.2024"));
    JsonValue const bad_date = form.commit("date", "31.02.2024");
    CHECK(bad_date.get("rc") && !bad_date.get("rc")->as_bool());
    CHECK(bad_date.get("alerts") && bad_date.get("alerts")->as_array().size() == 1);
    CHECK_EQ(form.eval("this.getField('date').value"), std::string("5.3.2024")); // refused: the value stays
    CHECK_EQ(change_of(form.commit("time", "14:05"), "time", "display"), std::string("2:05 pm"));
    CHECK_EQ(change_of(form.commit("phone", "5551234567"), "phone", "display"), std::string("(555) 123-4567"));
    CHECK_EQ(change_of(form.commit("euro", "1234.5"), "euro", "display"), std::string("1234,50"));

    // Keystroke filters: digits only; upper case.
    JsonValue const letter = form.ask("{\"type\":\"keystroke\",\"field\":\"digits\",\"value\":\"12\",\"change\":\"a\",\"selStart\":2,\"selEnd\":2}");
    CHECK(letter.get("rc") && !letter.get("rc")->as_bool());
    JsonValue const digit = form.ask("{\"type\":\"keystroke\",\"field\":\"digits\",\"value\":\"12\",\"change\":\"3\",\"selStart\":2,\"selEnd\":2}");
    CHECK(digit.get("rc") && digit.get("rc")->as_bool());
    JsonValue const upper = form.ask("{\"type\":\"keystroke\",\"field\":\"upper\",\"value\":\"\",\"change\":\"abc\",\"selStart\":0,\"selEnd\":0}");
    CHECK(upper.get("change") && upper.get("change")->as_string() == "ABC");

    // A validated range: refused with its alert, the value kept.
    form.commit("percent", "50");
    JsonValue const over = form.commit("percent", "150");
    CHECK(over.get("rc") && !over.get("rc")->as_bool());
    CHECK(over.get("alerts") && over.get("alerts")->as_array().size() == 1
        && over.get("alerts")->as_array()[0].as_string() == "Invalid value: must be greater than or equal to 0 and less than or equal to 100.");
    CHECK_EQ(form.eval("this.getField('percent').valueAsString"), std::string("50"));

    // The check box shows the hidden field, and hides it again.
    CHECK_EQ(form.eval("this.getField('extra').display"), std::string("1"));
    JsonValue const shown = form.ask("{\"type\":\"mouse\",\"field\":\"show\",\"widget\":0,\"action\":\"U\",\"value\":\"Yes\"}");
    CHECK_EQ(change_of(shown, "extra", "hidden"), std::string("false"));
    JsonValue const hidden = form.ask("{\"type\":\"mouse\",\"field\":\"show\",\"widget\":0,\"action\":\"U\",\"value\":\"Off\"}");
    CHECK_EQ(change_of(hidden, "extra", "hidden"), std::string("true"));

    // Two fields calculated from each other: each commit runs one pass.
    auto const started = std::chrono::steady_clock::now();
    form.commit("qty2", "4");
    JsonValue const loop = form.commit("qty2", "5");
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(2));
    CHECK(!loop.get("terminated"));
    double const a = std::stod(form.eval("this.getField('loopA').valueAsString"));
    double const b = std::stod(form.eval("this.getField('loopB').valueAsString"));
    CHECK_EQ(b, a + 1);

    // Reset: the fields it names (a name, a reference) back to their
    // defaults, and the calculations after them.
    form.commit("digits", "123");
    form.commit("upper", "AB");
    JsonValue const reset = form.ask("{\"type\":\"mouse\",\"field\":\"reset\",\"widget\":0,\"action\":\"U\"}");
    CHECK_EQ(change_of(reset, "digits", "value"), std::string(""));
    CHECK_EQ(change_of(reset, "upper", "value"), std::string(""));
    CHECK_EQ(change_of(reset, "qty1", "value"), std::string(""));
    CHECK_EQ(change_of(reset, "line1", "display"), std::string("$0.00"));
    CHECK_EQ(change_of(reset, "subtotal", "display"), std::string("$25.00"));

    // console.println and a script's error reach the console; the viewer goes on.
    JsonValue const printed = form.ask("{\"type\":\"eval\",\"source\":\"console.println('hello'); 1\"}");
    CHECK(printed.get("console") && printed.get("console")->as_array().size() == 1 && printed.get("console")->as_array()[0].as_string() == "hello");
    form.eval("this.getField('qty1').setAction('Keystroke', 'undefinedFunction();')");
    JsonValue const broken = form.ask("{\"type\":\"keystroke\",\"field\":\"qty1\",\"value\":\"\",\"change\":\"1\",\"selStart\":0,\"selEnd\":0}");
    CHECK(broken.get("console") && broken.get("console")->as_array().size() == 1
        && broken.get("console")->as_array()[0].as_string().find("ReferenceError") != std::string::npos);
    CHECK(broken.get("rc") && broken.get("rc")->as_bool());

    // app.setTimeOut: due now, run by the timer request.
    JsonValue const armed = form.ask("{\"type\":\"eval\",\"source\":\"app.setTimeOut('this.getField(\\\\'status\\\\').value = \\\\'tick\\\\'', 0); 1\"}");
    CHECK(armed.get("timer") && armed.get("timer")->is_number());
    JsonValue const ticked = form.ask("{\"type\":\"timer\"}");
    CHECK_EQ(change_of(ticked, "status", "value"), std::string("tick"));
    CHECK(ticked.get("timer") && ticked.get("timer")->is_null());

    // print() asks the viewer to print.
    JsonValue const print = form.ask("{\"type\":\"eval\",\"source\":\"print(); 1\"}");
    CHECK(print.get("print") && print.get("print")->as_bool());

    // Saved: /V holds the value, the appearance the formatted text, and
    // the hidden field its flag.
    form.ask("{\"type\":\"mouse\",\"field\":\"show\",\"widget\":0,\"action\":\"U\",\"value\":\"Yes\"}");
    form.commit("price1", "2.5");
    Bytes const saved = pdf::save_with_form_values(*form.doc, form.values);
    pdf::Document::Status status {};
    auto const reopened = pdf::Document::open(saved, "", status);
    CHECK(reopened != nullptr);
    if (reopened) {
        bool total_seen = false, extra_seen = false;
        for (pdf::FormField const& field : pdf::page_fields(*reopened, 0)) {
            if (field.name == "total") {
                total_seen = true;
                CHECK(field.value.text == "27");
                pdf::Dict const widget = reopened->resolve(pdf::Object::ref(field.widget)).as_dict();
                pdf::Stream const* ap = reopened->resolve(reopened->resolve(widget.get("AP")).as_dict().get("N")).as_stream();
                CHECK(ap != nullptr);
                if (ap) {
                    Bytes const content = reopened->decode(*ap);
                    CHECK(std::string(content.begin(), content.end()).find("($27.00)") != std::string::npos);
                }
            }
            if (field.name == "extra") {
                extra_seen = true;
                CHECK(!field.hidden());
            }
        }
        CHECK(total_seen && extra_seen);
    }
}

// What a hostile document's scripts reach for, and that the viewer's side
// of the sandbox goes on answering.
void test_hostile(std::string const& fixtures)
{
    pdf::FormSandbox::Limits limits;
    limits.milliseconds = 500;
    limits.heap_bytes = 48u * 1024u * 1024u;
    std::optional<Form> made = open_form(fixtures + "/forms/forms-js-hostile.pdf", limits);
    if (!made)
        return;
    Form& form = *made;
    JsonValue const open = form.ask("{\"type\":\"open\"}");
    std::string const probe = change_of(open, "probe", "value");
    std::printf("hostile probes: %s\n", probe.c_str());
    for (char const* name : { "fetch!ReferenceError", "XMLHttpRequest!ReferenceError", "document!ReferenceError", "window!ReferenceError",
             "escape=undefined", "setTimeout!ReferenceError", "localStorage!ReferenceError", "launchURL=undefined" })
        CHECK(probe.find(name) != std::string::npos);
    // The endless loop: cut by the time limit, the realm still there.
    auto started = std::chrono::steady_clock::now();
    JsonValue const loop = form.ask("{\"type\":\"mouse\",\"field\":\"loop\",\"widget\":0,\"action\":\"U\"}");
    auto const loop_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    std::printf("endless loop answered in %lld ms\n", static_cast<long long>(loop_ms));
    CHECK(loop.get("terminated") && loop.get("terminated")->as_bool());
    CHECK(loop_ms < 1500);
    JsonValue const alive = form.ask("{\"type\":\"mouse\",\"field\":\"alive\",\"widget\":0,\"action\":\"U\"}");
    CHECK_EQ(change_of(alive, "probe", "value"), std::string("alive"));
    // The memory bomb: the heap's ceiling ends it and every script of the
    // document after it; requests still answer, at once.
    started = std::chrono::steady_clock::now();
    JsonValue const bomb = form.ask("{\"type\":\"mouse\",\"field\":\"bomb\",\"widget\":0,\"action\":\"U\"}");
    auto const bomb_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    std::printf("memory bomb answered in %lld ms: %s\n", static_cast<long long>(bomb_ms), bomb.get("dead") ? "dead" : "terminated");
    CHECK((bomb.get("dead") && bomb.get("dead")->as_bool()) || (bomb.get("terminated") && bomb.get("terminated")->as_bool()));
    started = std::chrono::steady_clock::now();
    JsonValue const after = form.ask("{\"type\":\"mouse\",\"field\":\"alive\",\"widget\":0,\"action\":\"U\"}");
    CHECK(after.is_object());
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(2));
}

}

int main(int argc, char** argv)
{
    if (argc > 1) {
        test_order_form(argv[1]);
        test_hostile(argv[1]);
    } else {
        CHECK(!"the fixtures folder is the first argument");
    }
    return test::report("test_pdf_scripts");
}
