#include "JsTest.h"

#include "bindings/Realm.h"
#include "core/Base64.h"
#include "core/Bitmap.h"
#include "core/Png.h"
#include "dom/Dom.h"
#include "html/TreeBuilder.h"
#include "paint/Canvas2D.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The canvas element and its 2D context, driven through script the way a
// page drives them and read back through getImageData, the encoders and the
// pictures the host is handed. Every realm runs under heap stress, so a
// context, a gradient or a path that is not rooted fails at once.

using namespace sashfold;

namespace {

struct Page {
    std::unique_ptr<dom::Document> document = std::make_unique<dom::Document>();
    std::unique_ptr<bindings::Realm> realm;
    std::string console;
    std::map<std::string, net::FetchResponse> responses;

    explicit Page(std::string_view html, std::function<void(bindings::HostHooks&)> const& host = {})
    {
        bindings::HostHooks hooks;
        if (host)
            host(hooks);
        hooks.console = [this](std::string_view level, std::string_view message) {
            console += std::string(level) + ":" + std::string(message) + "|";
        };
        hooks.now = [] { return 1000.0; };
        hooks.fetch_resource = [this](net::Url const& target, net::ResourceRequest const&, net::RequestGuard const&) -> net::FetchResult {
            auto const it = responses.find(target.serialize());
            if (it == responses.end())
                return { std::nullopt, "no such resource" };
            net::FetchResponse response = it->second;
            response.final_url = target;
            return { std::move(response), "" };
        };
        realm = std::make_unique<bindings::Realm>(*document, *net::parse_url("https://example.test/page.html"), std::move(hooks));
        realm->interpreter().heap().set_stress(true);
        m_html = std::string(html);
    }
    void load()
    {
        html::parse_document_bytes_into(*document, m_html, realm.get());
        realm->document_parsed();
    }
    test::JsRun eval(std::string_view source)
    {
        js::Outcome const outcome = realm->run(source, "<test>");
        test::JsRun run;
        run.ok = outcome.ok;
        run.value = outcome.value;
        if (!outcome.ok)
            run.thrown = realm->interpreter().describe(outcome.value);
        return run;
    }
    double number(std::string_view source)
    {
        test::JsRun const run = eval(source);
        if (!run.ok || !run.value.is_number()) {
            test::fail((run.ok ? "not a number: " : "threw " + run.thrown + " evaluating: ") + std::string(source), __FILE__, __LINE__);
            return 0;
        }
        return run.value.as_number();
    }
    std::string string(std::string_view source)
    {
        test::JsRun const run = eval(source);
        if (!run.ok || !run.value.is_string()) {
            test::fail((run.ok ? "not a string: " : "threw " + run.thrown + " evaluating: ") + std::string(source), __FILE__, __LINE__);
            return "";
        }
        return run.value.as_string()->to_utf8();
    }
    bool boolean(std::string_view source)
    {
        test::JsRun const run = eval(source);
        if (!run.ok || !run.value.is_boolean()) {
            test::fail((run.ok ? "not a boolean: " : "threw " + run.thrown + " evaluating: ") + std::string(source), __FILE__, __LINE__);
            return false;
        }
        return run.value.as_boolean();
    }
    std::string throws(std::string_view source) { return test::eval_throws(realm->interpreter(), source); }

private:
    std::string m_html;
};

// A page with one canvas, `c`, and its context, `x`, and a helper that reads
// one pixel back as "r,g,b,a".
std::unique_ptr<Page> canvas_page(std::string_view size = "width=100 height=100")
{
    auto page = std::make_unique<Page>("<!DOCTYPE html><canvas id=c " + std::string(size) + "></canvas>");
    page->load();
    page->eval("var c = document.getElementById('c'); var x = c.getContext('2d');"
               "function px(X, Y) { return Array.from(x.getImageData(X, Y, 1, 1).data).join(','); }");
    return page;
}

// Whether a pixel "r,g,b,a" is within `slack` of the one expected.
bool near(std::string const& actual, int r, int g, int b, int a, int slack = 2)
{
    int got[4] = { -1, -1, -1, -1 };
    std::size_t at = 0;
    for (int& value : got) {
        std::size_t const end = actual.find(',', at);
        std::string const part = actual.substr(at, end == std::string::npos ? std::string::npos : end - at);
        if (part.empty())
            return false;
        value = std::stoi(part);
        at = end == std::string::npos ? actual.size() : end + 1;
    }
    int const want[4] = { r, g, b, a };
    for (int i = 0; i < 4; ++i) {
        if (got[i] < want[i] - slack || got[i] > want[i] + slack)
            return false;
    }
    return true;
}

std::string png_data_url(Bitmap const& bitmap)
{
    std::vector<std::uint8_t> const png = encode_png(bitmap);
    return "data:image/png;base64," + base64_encode(std::span<std::uint8_t const>(png));
}

void test_context_is_made_once_and_only_for_2d()
{
    auto page = canvas_page();
    CHECK(page->boolean("c.getContext('2d') === x"));
    CHECK(page->boolean("x.canvas === c"));
    CHECK(page->boolean("x instanceof CanvasRenderingContext2D"));
    CHECK(page->boolean("c.getContext('webgl') === null"));
    CHECK(page->boolean("document.createElement('canvas').getContext('bitmaprenderer') === null"));
    // An id this engine makes no context for leaves the canvas free.
    CHECK(page->boolean("var o = document.createElement('canvas'); o.getContext('webgl') === null && o.getContext('2d') !== null"));
    CHECK(page->boolean("o.getContext('webgl') === null"));
    CHECK_EQ(page->string("JSON.stringify(x.getContextAttributes())"),
        "{\"alpha\":true,\"colorSpace\":\"srgb\",\"colorType\":\"unorm8\",\"desynchronized\":false,\"willReadFrequently\":false}");
    CHECK(!page->boolean("x.isContextLost()"));
    CHECK_EQ(page->string("var d = document.createElement('canvas'); d.width + 'x' + d.height"), "300x150");
    CHECK_EQ(page->throws("c.getContext()"), "TypeError: Failed to execute 'getContext' on 'HTMLCanvasElement': 1 argument required, but only 0 present.");
}

void test_fill_rect_and_the_fill_style()
{
    auto page = canvas_page();
    CHECK_EQ(page->string("x.fillStyle"), "#000000");
    page->eval("x.fillStyle = 'red'; x.fillRect(10, 10, 20, 20);");
    CHECK_EQ(page->string("px(10, 10)"), "255,0,0,255");
    CHECK_EQ(page->string("px(29, 29)"), "255,0,0,255");
    CHECK_EQ(page->string("px(30, 30)"), "0,0,0,0");
    CHECK_EQ(page->string("px(9, 15)"), "0,0,0,0");
    CHECK_EQ(page->string("x.fillStyle"), "#ff0000");
    // A string that is not a color is ignored, the style kept; so is one CSS
    // Color 4 refuses, a legacy rgb() mixing its separators.
    page->eval("x.fillStyle = 'not a color'; x.fillStyle = 'rgb(0, 255 0)'; x.fillStyle = 'rgba(0, 0, 255, ';");
    CHECK_EQ(page->string("x.fillStyle"), "#ff0000");
    page->eval("x.fillStyle = 'rgb(0 0 255 / 50%)';");
    CHECK_EQ(page->string("x.fillStyle"), "rgba(0, 0, 255, 0.5)");
    page->eval("x.fillRect(50, 50, 10, 10);");
    CHECK_EQ(page->string("px(55, 55)"), "0,0,255,128");
    page->eval("x.clearRect(0, 0, 100, 100);");
    CHECK_EQ(page->string("px(15, 15)"), "0,0,0,0");
}

void test_strokes_end_with_their_caps()
{
    auto page = canvas_page();
    auto const line_with = [&](std::string const& cap) {
        page->eval("x.clearRect(0, 0, 100, 100); x.strokeStyle = '#0f0'; x.lineWidth = 10; x.lineCap = '" + cap
            + "'; x.beginPath(); x.moveTo(20, 50); x.lineTo(80, 50); x.stroke();");
    };
    line_with("butt");
    CHECK_EQ(page->string("px(50, 50)"), "0,255,0,255");
    CHECK_EQ(page->string("px(50, 45)"), "0,255,0,255");
    CHECK_EQ(page->string("px(50, 44)"), "0,0,0,0");
    CHECK_EQ(page->string("px(16, 50)"), "0,0,0,0");
    line_with("square");
    CHECK_EQ(page->string("px(16, 50)"), "0,255,0,255");
    CHECK_EQ(page->string("px(15, 46)"), "0,255,0,255");
    CHECK_EQ(page->string("px(14, 50)"), "0,0,0,0");
    line_with("round");
    CHECK_EQ(page->string("px(16, 50)"), "0,255,0,255");
    CHECK_EQ(page->string("px(15, 45)"), "0,0,0,0");
    CHECK_EQ(page->string("x.lineCap"), "round");
    page->eval("x.lineCap = 'nonsense';");
    CHECK_EQ(page->string("x.lineCap"), "round");
}

void test_dashes_leave_gaps()
{
    auto page = canvas_page();
    page->eval("x.setLineDash([10, 5, 3]);");
    CHECK_EQ(page->string("x.getLineDash().join()"), "10,5,3,10,5,3");
    page->eval("x.setLineDash([10, -1]);");
    CHECK_EQ(page->string("x.getLineDash().join()"), "10,5,3,10,5,3");
    page->eval("x.setLineDash([10, 10]); x.lineWidth = 4; x.strokeStyle = 'blue';"
               "x.beginPath(); x.moveTo(0, 20); x.lineTo(100, 20); x.stroke();");
    CHECK_EQ(page->string("px(5, 20)"), "0,0,255,255");
    CHECK_EQ(page->string("px(15, 20)"), "0,0,0,0");
    CHECK_EQ(page->string("px(25, 20)"), "0,0,255,255");
    page->eval("x.lineDashOffset = 10; x.clearRect(0, 0, 100, 100); x.stroke();");
    CHECK_EQ(page->string("px(5, 20)"), "0,0,0,0");
    CHECK_EQ(page->string("px(15, 20)"), "0,0,255,255");
}

void test_a_linear_gradient_runs_between_its_stops()
{
    auto page = canvas_page();
    page->eval("var g = x.createLinearGradient(0, 0, 100, 0); g.addColorStop(0, '#f00'); g.addColorStop(1, '#00f');"
               "x.fillStyle = g; x.fillRect(0, 0, 100, 10);");
    CHECK(page->boolean("x.fillStyle === g"));
    CHECK(near(page->string("px(0, 5)"), 254, 0, 1, 255, 2));
    CHECK(near(page->string("px(50, 5)"), 126, 0, 129, 255, 3));
    CHECK(near(page->string("px(99, 5)"), 1, 0, 254, 255, 2));
    CHECK_EQ(page->throws("g.addColorStop(1.5, 'red')"), "IndexSizeError: The provided value (1.5) is outside the range (0.0, 1.0).");
    CHECK_EQ(page->throws("g.addColorStop(0.5, 'nonsense')"), "SyntaxError: The value provided ('nonsense') could not be parsed as a color.");
    // A radial gradient: the inner color at its center.
    page->eval("var r = x.createRadialGradient(50, 60, 0, 50, 60, 30); r.addColorStop(0, '#0f0'); r.addColorStop(1, '#000');"
               "x.fillStyle = r; x.fillRect(0, 20, 100, 80);");
    CHECK(near(page->string("px(50, 60)"), 0, 255, 0, 255, 12));
    CHECK(near(page->string("px(95, 95)"), 0, 0, 0, 255, 2));
}

void test_a_clip_holds_until_restore()
{
    auto page = canvas_page();
    page->eval("x.save(); x.beginPath(); x.rect(10, 10, 20, 20); x.clip(); x.fillStyle = 'red'; x.fillRect(0, 0, 100, 100);");
    CHECK_EQ(page->string("px(15, 15)"), "255,0,0,255");
    CHECK_EQ(page->string("px(35, 15)"), "0,0,0,0");
    page->eval("x.restore(); x.fillStyle = 'blue'; x.fillRect(40, 40, 10, 10);");
    CHECK_EQ(page->string("px(45, 45)"), "0,0,255,255");
    // The even-odd rule leaves a hole where two rectangles overlap.
    page->eval("x.clearRect(0, 0, 100, 100); x.beginPath(); x.rect(0, 0, 60, 60); x.rect(20, 20, 20, 20); x.fill('evenodd');");
    CHECK_EQ(page->string("px(10, 10)"), "0,0,255,255");
    CHECK_EQ(page->string("px(30, 30)"), "0,0,0,0");
    CHECK(page->boolean("x.isPointInPath(10, 10)"));
    CHECK(!page->boolean("x.isPointInPath(30, 30, 'evenodd')"));
    CHECK(page->boolean("x.isPointInPath(30, 30, 'nonzero')"));
    CHECK(page->boolean("x.isPointInStroke(0, 30)"));
    CHECK(!page->boolean("x.isPointInStroke(10, 30)"));
}

void test_transforms_move_what_is_drawn()
{
    auto page = canvas_page();
    page->eval("x.translate(50, 50); x.rotate(Math.PI / 2); x.fillStyle = '#0f0'; x.fillRect(0, 0, 20, 10);");
    CHECK_EQ(page->string("px(45, 60)"), "0,255,0,255");
    CHECK_EQ(page->string("px(55, 60)"), "0,0,0,0");
    CHECK_EQ(page->string("px(45, 45)"), "0,0,0,0");
    CHECK(page->boolean("var m = x.getTransform(); m instanceof DOMMatrix && Math.abs(m.a) < 1e-12 && m.b === 1 && m.e === 50 && m.f === 50"));
    page->eval("x.setTransform(2, 0, 0, 2, 0, 0); x.fillStyle = 'red'; x.fillRect(1, 1, 1, 1);");
    CHECK_EQ(page->string("px(2, 2)"), "255,0,0,255");
    CHECK_EQ(page->string("px(3, 3)"), "255,0,0,255");
    CHECK_EQ(page->string("px(4, 4)"), "0,0,0,0");
    page->eval("x.setTransform({ a: 1, d: 1, e: 5 }); var t = x.getTransform();");
    CHECK_EQ(page->string("[t.a, t.b, t.c, t.d, t.e, t.f].join()"), "1,0,0,1,5,0");
    page->eval("x.resetTransform();");
    CHECK(page->boolean("x.getTransform().isIdentity"));
    // A DOMMatrix of its own: parsed, multiplied, inverted.
    CHECK_EQ(page->string("new DOMMatrix('matrix(1, 2, 3, 4, 5, 6)').toString()"), "matrix(1, 2, 3, 4, 5, 6)");
    CHECK_EQ(page->string("new DOMMatrix([2, 0, 0, 2, 10, 20]).inverse().toString()"), "matrix(0.5, 0, 0, 0.5, -5, -10)");
    CHECK_EQ(page->string("var p = new DOMMatrix().translate(10, 0).scale(2).transformPoint({ x: 1, y: 1 }); p.x + ',' + p.y"), "12,2");
}

void test_composite_operations()
{
    auto page = canvas_page();
    auto const composite = [&](std::string const& op) {
        page->eval("x.globalCompositeOperation = 'source-over'; x.clearRect(0, 0, 100, 100); x.fillStyle = '#f00'; x.fillRect(0, 0, 20, 20);"
                   "x.globalCompositeOperation = '" + op + "'; x.fillStyle = '#00f'; x.fillRect(10, 0, 20, 20);");
    };
    composite("source-over");
    CHECK_EQ(page->string("px(15, 5)"), "0,0,255,255");
    composite("destination-over");
    CHECK_EQ(page->string("px(15, 5)"), "255,0,0,255");
    CHECK_EQ(page->string("px(25, 5)"), "0,0,255,255");
    composite("source-in");
    CHECK_EQ(page->string("px(15, 5)"), "0,0,255,255");
    CHECK_EQ(page->string("px(5, 5)"), "0,0,0,0");
    CHECK_EQ(page->string("px(25, 5)"), "0,0,0,0");
    composite("destination-out");
    CHECK_EQ(page->string("px(5, 5)"), "255,0,0,255");
    CHECK_EQ(page->string("px(15, 5)"), "0,0,0,0");
    composite("xor");
    CHECK_EQ(page->string("px(5, 5)"), "255,0,0,255");
    CHECK_EQ(page->string("px(15, 5)"), "0,0,0,0");
    CHECK_EQ(page->string("px(25, 5)"), "0,0,255,255");
    composite("copy");
    CHECK_EQ(page->string("px(5, 5)"), "0,0,0,0");
    CHECK_EQ(page->string("px(15, 5)"), "0,0,255,255");
    composite("lighter");
    CHECK_EQ(page->string("px(15, 5)"), "255,0,255,255");
    composite("multiply");
    CHECK_EQ(page->string("px(15, 5)"), "0,0,0,255");
    composite("screen");
    CHECK_EQ(page->string("px(15, 5)"), "255,0,255,255");
    CHECK_EQ(page->string("x.globalCompositeOperation"), "screen");
    page->eval("x.globalCompositeOperation = 'plus-darker';");
    CHECK_EQ(page->string("x.globalCompositeOperation"), "screen");
    // Global alpha scales what is drawn.
    page->eval("x.globalCompositeOperation = 'source-over'; x.clearRect(0, 0, 100, 100); x.globalAlpha = 0.5; x.fillStyle = '#0f0'; x.fillRect(0, 0, 10, 10);");
    CHECK_EQ(page->string("px(5, 5)"), "0,255,0,128");
    page->eval("x.globalAlpha = 2;");
    CHECK_EQ(page->number("x.globalAlpha"), 0.5);
}

void test_a_shadow_falls_beside_the_shape()
{
    auto page = canvas_page();
    page->eval("x.shadowColor = '#00f'; x.shadowOffsetX = 30; x.fillStyle = '#f00'; x.fillRect(10, 10, 20, 20);");
    CHECK_EQ(page->string("px(15, 15)"), "255,0,0,255");
    CHECK_EQ(page->string("px(45, 15)"), "0,0,255,255");
    CHECK_EQ(page->string("px(65, 15)"), "0,0,0,0");
    // A blurred one spreads past the shape's edge and fades.
    page->eval("x.clearRect(0, 0, 100, 100); x.shadowOffsetX = 0; x.shadowOffsetY = 50; x.shadowBlur = 8; x.fillRect(10, 10, 20, 20);");
    CHECK(page->boolean("var e = x.getImageData(20, 81, 1, 1).data; e[2] > 0 && e[3] > 0 && e[3] < 255"));
    CHECK(page->boolean("var h = x.getImageData(20, 70, 1, 1).data[3]; h > 240 && h <= 255"));
}

void test_text_is_measured_and_drawn()
{
    auto page = canvas_page("width=200 height=60");
    CHECK_EQ(page->string("x.font"), "10px sans-serif");
    page->eval("x.font = 'bold 20px serif'; var m = x.measureText('Hello'); var n = x.measureText('HelloHello');");
    CHECK_EQ(page->string("x.font"), "bold 20px serif");
    CHECK(page->boolean("m instanceof TextMetrics && m.width > 20 && m.width < 100"));
    CHECK(page->boolean("Math.abs(n.width - 2 * m.width) < 2"));
    CHECK(page->boolean("['width', 'actualBoundingBoxLeft', 'actualBoundingBoxRight', 'fontBoundingBoxAscent', 'fontBoundingBoxDescent',"
                        " 'actualBoundingBoxAscent', 'actualBoundingBoxDescent', 'emHeightAscent', 'emHeightDescent', 'hangingBaseline',"
                        " 'alphabeticBaseline', 'ideographicBaseline'].every(k => typeof m[k] === 'number' && isFinite(m[k]))"));
    CHECK(page->boolean("m.fontBoundingBoxAscent > 10 && m.actualBoundingBoxAscent > 8 && m.alphabeticBaseline === 0"));
    CHECK(page->boolean("Math.abs(m.emHeightAscent + m.emHeightDescent - 20) < 0.01"));
    CHECK_EQ(page->number("x.measureText('').width"), 0);
    // A font the parser refuses is ignored.
    page->eval("x.font = '20px'; x.font = 'bold';");
    CHECK_EQ(page->string("x.font"), "bold 20px serif");
    // Drawn text covers pixels near its baseline; centred, it sits either side
    // of the point; squeezed to a maximum width, it stays inside it.
    page->eval("x.fillStyle = '#000'; x.fillText('Hello', 10, 40);");
    CHECK(page->boolean("x.getImageData(10, 20, 70, 21).data.some((v, i) => i % 4 === 3 && v > 128)"));
    CHECK(page->boolean("x.getImageData(0, 0, 200, 15).data.every((v, i) => i % 4 !== 3 || v === 0)"));
    page->eval("x.clearRect(0, 0, 200, 60); x.textAlign = 'center'; x.fillText('Hello', 100, 40, 20);");
    CHECK(page->boolean("x.getImageData(0, 0, 88, 60).data.every((v, i) => i % 4 !== 3 || v === 0)"));
    CHECK(page->boolean("x.getImageData(112, 0, 88, 60).data.every((v, i) => i % 4 !== 3 || v === 0)"));
    CHECK(page->boolean("x.getImageData(88, 20, 24, 21).data.some((v, i) => i % 4 === 3 && v > 0)"));
}

void test_draw_image_scales_its_source()
{
    // Two pixels, red and blue, as a data: URL an <img> names.
    Bitmap picture(2, 1);
    picture.set_pixel(0, 0, Color::rgb(255, 0, 0));
    picture.set_pixel(1, 0, Color::rgb(0, 0, 255));
    auto page = std::make_unique<Page>("<!DOCTYPE html><canvas id=c width=100 height=100></canvas><img id=i src='" + png_data_url(picture) + "'>");
    page->load();
    page->eval("var c = document.getElementById('c'); var x = c.getContext('2d'); var img = document.getElementById('i');"
               "function px(X, Y) { return Array.from(x.getImageData(X, Y, 1, 1).data).join(','); }");
    page->eval("x.imageSmoothingEnabled = false; x.drawImage(img, 0, 0, 40, 20);");
    CHECK_EQ(page->string("px(5, 5)"), "255,0,0,255");
    CHECK_EQ(page->string("px(19, 19)"), "255,0,0,255");
    CHECK_EQ(page->string("px(20, 5)"), "0,0,255,255");
    CHECK_EQ(page->string("px(39, 19)"), "0,0,255,255");
    CHECK_EQ(page->string("px(40, 5)"), "0,0,0,0");
    // The nine-argument form: the blue half alone, stretched.
    page->eval("x.drawImage(img, 1, 0, 1, 1, 50, 50, 10, 10);");
    CHECK_EQ(page->string("px(55, 55)"), "0,0,255,255");
    // Another canvas as the source, scaled up threefold.
    page->eval("var s = document.createElement('canvas'); s.width = 2; s.height = 2; var sx = s.getContext('2d');"
               "sx.fillStyle = '#0f0'; sx.fillRect(0, 0, 1, 2); x.drawImage(s, 0, 60, 6, 6);");
    CHECK_EQ(page->string("px(2, 62)"), "0,255,0,255");
    CHECK_EQ(page->string("px(4, 62)"), "0,0,0,0");
    CHECK_EQ(page->throws("x.drawImage(img, 0, 0, 1)"), "TypeError: Failed to execute 'drawImage' on 'CanvasRenderingContext2D': Valid arities are: [3, 5, 9], but 4 arguments provided.");
    CHECK_EQ(page->throws("x.drawImage({}, 0, 0)").substr(0, 9), "TypeError");
    page->eval("s.width = 0;");
    CHECK_EQ(page->throws("x.drawImage(s, 0, 0)"), "InvalidStateError: The canvas has no pixels.");
}

void test_a_picture_made_by_script_loads_in_a_task()
{
    Bitmap picture(1, 1);
    picture.set_pixel(0, 0, Color::rgb(0, 128, 0));
    auto page = canvas_page();
    page->eval("var log = []; var i = new Image(); i.onload = () => { log.push('load'); x.drawImage(i, 0, 0, 10, 10); };"
               "i.onerror = () => log.push('error'); i.src = '" + png_data_url(picture) + "';"
               "var b = new Image(); b.onload = () => log.push('b load'); b.onerror = () => log.push('b error');"
               "b.src = 'https://example.test/missing.png';");
    // Never inside the script that set the source.
    CHECK_EQ(page->string("log.join()"), "");
    while (page->realm->run_pending()) {
    }
    CHECK_EQ(page->string("log.join()"), "load,b error");
    CHECK_EQ(page->string("px(5, 5)"), "0,128,0,255");
    CHECK_EQ(page->throws("x.drawImage(b, 0, 0)"), "InvalidStateError: The image is broken.");
    // A source set twice before its task runs is heard of once, for the last.
    page->eval("log = []; i.src = 'https://example.test/missing.png'; i.src = '" + png_data_url(picture) + "';");
    while (page->realm->run_pending()) {
    }
    CHECK_EQ(page->string("log.join()"), "load");
}

void test_image_data_round_trips()
{
    auto page = canvas_page();
    CHECK_EQ(page->string("var d = x.createImageData(2, 2); d.width + 'x' + d.height + ':' + d.data.length + ':' + d.data.join('')"),
        "2x2:16:0000000000000000");
    CHECK_EQ(page->string("d.colorSpace"), "srgb");
    page->eval("d.data.set([255, 0, 0, 255, 100, 50, 200, 128, 0, 0, 0, 0, 1, 2, 3, 255]); x.putImageData(d, 10, 10);");
    CHECK_EQ(page->string("px(10, 10)"), "255,0,0,255");
    CHECK(near(page->string("px(11, 10)"), 100, 50, 200, 128, 1));
    CHECK_EQ(page->string("px(12, 10)"), "0,0,0,0");
    CHECK_EQ(page->string("px(11, 11)"), "1,2,3,255");
    // The dirty rectangle puts only the part it names.
    page->eval("x.clearRect(0, 0, 100, 100); x.putImageData(d, 50, 50, 1, 1, 1, 1);");
    CHECK_EQ(page->string("px(50, 50)"), "0,0,0,0");
    CHECK_EQ(page->string("px(51, 51)"), "1,2,3,255");
    // Out of the bitmap reads transparent black; no width is an error.
    CHECK_EQ(page->string("Array.from(x.getImageData(-1, -1, 1, 1).data).join()"), "0,0,0,0");
    CHECK_EQ(page->throws("x.getImageData(0, 0, 0, 1)"), "IndexSizeError: The source width and height must not be zero.");
    CHECK_EQ(page->string("var e = new ImageData(new Uint8ClampedArray(8), 1); e.width + 'x' + e.height"), "1x2");
    CHECK_EQ(page->throws("new ImageData(new Uint8ClampedArray(8), 3)"), "IndexSizeError: The input data length is not a multiple of (4 * width).");
    CHECK_EQ(page->throws("new ImageData(0, 1)"), "IndexSizeError: The source width or height is zero.");
    // ImageData clones as its data does.
    CHECK_EQ(page->string("var k = structuredClone(d); (k instanceof ImageData) + ':' + k.data[4] + ':' + (k.data !== d.data)"), "true:100:true");
}

void test_path_errors_and_path2d()
{
    auto page = canvas_page();
    CHECK_EQ(page->throws("x.arc(0, 0, -1, 0, 1)"), "IndexSizeError: The radius provided is negative.");
    CHECK_EQ(page->throws("x.roundRect(0, 0, 10, 10, -1)"), "RangeError: Failed to execute 'roundRect': A radius provided is negative.");
    CHECK_EQ(page->throws("x.roundRect(0, 0, 10, 10, [1, 2, 3, 4, 5])"),
        "RangeError: Failed to execute 'roundRect': 5 radii provided. Between one and four radii are necessary.");
    CHECK_EQ(page->throws("x.arcTo(0, 0, 1, 1, -2)"), "IndexSizeError: The radius provided is negative.");
    page->eval("var p = new Path2D('M10 10 h 20 v 20 h -20 z'); x.fillStyle = '#f0f'; x.fill(p);");
    CHECK_EQ(page->string("px(20, 20)"), "255,0,255,255");
    CHECK_EQ(page->string("px(35, 20)"), "0,0,0,0");
    CHECK(page->boolean("x.isPointInPath(p, 15, 15) && !x.isPointInPath(p, 50, 50)"));
    page->eval("var q = new Path2D(); q.addPath(p, new DOMMatrix().translate(40, 0)); x.fill(q);");
    CHECK_EQ(page->string("px(60, 20)"), "255,0,255,255");
    // A full circle, filled: its center covered, its bounding box's corner not.
    page->eval("x.beginPath(); x.arc(50, 70, 10, 0, 2 * Math.PI); x.fillStyle = '#00f'; x.fill();");
    CHECK_EQ(page->string("px(50, 70)"), "0,0,255,255");
    CHECK_EQ(page->string("px(41, 61)"), "0,0,0,0");
}

void test_setting_the_size_resets_the_bitmap_and_the_state()
{
    auto page = canvas_page();
    page->eval("x.fillStyle = 'red'; x.translate(5, 5); x.fillRect(0, 0, 10, 10);");
    CHECK_EQ(page->string("px(6, 6)"), "255,0,0,255");
    page->eval("c.width = 50;");
    CHECK_EQ(page->number("c.width"), 50);
    CHECK_EQ(page->string("c.getAttribute('width')"), "50");
    CHECK_EQ(page->string("px(6, 6)"), "0,0,0,0");
    CHECK_EQ(page->string("x.fillStyle"), "#000000");
    CHECK(page->boolean("x.getTransform().isIdentity"));
    CHECK_EQ(page->number("x.getImageData(0, 0, 100, 100).width"), 100);
    // reset() does the same without a new size.
    page->eval("x.fillStyle = 'blue'; x.fillRect(0, 0, 5, 5); x.reset();");
    CHECK_EQ(page->string("px(1, 1)"), "0,0,0,0");
    CHECK_EQ(page->string("x.fillStyle"), "#000000");
}

void test_the_host_is_handed_the_picture()
{
    auto page = canvas_page("width=20 height=10");
    page->eval("x.fillStyle = '#00ff00'; x.fillRect(0, 0, 20, 10);");
    std::vector<bindings::VideoFrame> const first = page->realm->video_frames();
    CHECK_EQ(first.size(), 1u);
    if (first.size() != 1)
        return;
    dom::Attr const* const id = first[0].element->find_attribute("id");
    CHECK(id && id->value == "c");
    CHECK_EQ(first[0].bitmap->width(), 20);
    CHECK_EQ(first[0].bitmap->height(), 10);
    CHECK(first[0].bitmap->pixel(5, 5) == Color::rgb(0, 255, 0));
    // Drawing again changes the picture the host holds, in place.
    page->eval("x.fillStyle = '#0000ff'; x.fillRect(0, 0, 10, 10);");
    std::vector<bindings::VideoFrame> const later = page->realm->video_frames();
    CHECK_EQ(later.size(), 1u);
    if (later.size() != 1)
        return;
    CHECK(later[0].frames != first[0].frames);
    CHECK(later[0].bitmap->pixel(5, 5) == Color::rgb(0, 0, 255));
    CHECK(later[0].bitmap->pixel(15, 5) == Color::rgb(0, 255, 0));
    // A canvas taken out of the document is no longer painted.
    page->eval("c.remove();");
    CHECK(page->realm->video_frames().empty());
}

void test_exports_and_the_origin_clean_flag()
{
    Bitmap picture(1, 1);
    picture.set_pixel(0, 0, Color::rgb(0, 0, 255));
    std::vector<std::uint8_t> const png = encode_png(picture);
    auto page = std::make_unique<Page>("<!DOCTYPE html><canvas id=c width=4 height=3></canvas><img id=far src='https://elsewhere.test/p.png'>");
    net::FetchResponse response;
    response.status = 200;
    response.status_text = "OK";
    response.headers.push_back(net::Header { "Content-Type", "image/png" });
    response.body = png;
    page->responses["https://elsewhere.test/p.png"] = response;
    page->load();
    page->eval("var c = document.getElementById('c'); var x = c.getContext('2d'); x.fillStyle = '#f00'; x.fillRect(0, 0, 4, 3);");
    std::string const url = page->string("c.toDataURL()");
    CHECK(url.starts_with("data:image/png;base64,"));
    std::optional<std::vector<std::uint8_t>> const bytes = base64_decode(url.substr(22));
    std::optional<Bitmap> const decoded = bytes ? decode_png(*bytes) : std::nullopt;
    CHECK(decoded.has_value());
    if (decoded) {
        CHECK_EQ(decoded->width(), 4);
        CHECK_EQ(decoded->height(), 3);
        CHECK(decoded->pixel(3, 2) == Color::rgb(255, 0, 0));
    }
    CHECK(page->string("c.toDataURL('image/webp', 0.5)").starts_with("data:image/png;base64,"));
    // toBlob answers through a task, with a PNG.
    page->eval("var blobbed = 'no'; c.toBlob(b => { blobbed = b.type + ':' + (b.size > 0); });");
    CHECK_EQ(page->string("blobbed"), "no");
    page->realm->run_pending();
    CHECK_EQ(page->string("blobbed"), "image/png:true");
    // A picture from another origin taints the canvas for good.
    CHECK(page->eval("x.drawImage(document.getElementById('far'), 0, 0);").ok);
    CHECK_EQ(page->throws("x.getImageData(0, 0, 1, 1)"), "SecurityError: The canvas has been tainted by cross-origin data.");
    CHECK_EQ(page->throws("c.toDataURL()"), "SecurityError: Tainted canvases may not be exported.");
    CHECK_EQ(page->throws("c.toBlob(function () {})"), "SecurityError: Tainted canvases may not be exported.");
}

// A picture the host fetched is the page's own only where its bytes came
// from: a same-origin src that redirected elsewhere taints, and a
// crossorigin picture another origin allowed by CORS does not.
void test_a_host_picture_is_judged_by_where_it_came_from()
{
    Bitmap blue(1, 1);
    blue.set_pixel(0, 0, Color::rgb(0, 0, 255));
    auto const bitmap = std::make_shared<Bitmap const>(blue);
    auto const picture = [&](std::string_view from, std::string allow_origin = "", bool credentials = false) {
        bindings::HostPicture held;
        held.bitmap = bitmap;
        if (!from.empty())
            held.from = net::parse_url(from);
        held.allow_origin = std::move(allow_origin);
        held.allow_credentials = credentials;
        return held;
    };
    std::map<std::string, bindings::HostPicture> const held {
        { "own", picture("https://example.test/own.png") },
        { "moved", picture("https://elsewhere.test/moved.png") },
        { "unknown", picture("") },
        { "anon", picture("https://elsewhere.test/a.png", "*") },
        { "credstar", picture("https://elsewhere.test/b.png", "*", true) },
        { "credexact", picture("https://elsewhere.test/c.png", "https://example.test", true) },
        { "credless", picture("https://elsewhere.test/d.png", "https://example.test") },
        { "plain", picture("https://elsewhere.test/e.png", "*") },
        { "wrong", picture("https://elsewhere.test/f.png", "https://other.test") },
    };
    auto page = std::make_unique<Page>(R"HTML(<!DOCTYPE html>
<img id=own src='https://example.test/own.png'>
<img id=moved src='https://example.test/moved.png'>
<img id=unknown src='https://example.test/unknown.png'>
<img id=anon crossorigin src='https://elsewhere.test/a.png'>
<img id=credstar crossorigin=use-credentials src='https://elsewhere.test/b.png'>
<img id=credexact crossorigin=USE-CREDENTIALS src='https://elsewhere.test/c.png'>
<img id=credless crossorigin=use-credentials src='https://elsewhere.test/d.png'>
<img id=plain src='https://elsewhere.test/e.png'>
<img id=wrong crossorigin=anonymous src='https://elsewhere.test/f.png'>)HTML",
        [&held](bindings::HostHooks& hooks) {
            hooks.image_state = [](dom::Element const&) { return bindings::ImageState::Available; };
            hooks.image_picture = [&held](dom::Element const& image) -> bindings::HostPicture {
                dom::Attr const* const id = image.find_attribute("id");
                auto const it = id ? held.find(id->value) : held.end();
                return it == held.end() ? bindings::HostPicture {} : it->second;
            };
        });
    page->load();
    page->eval("function judged(id) { var k = document.createElement('canvas').getContext('2d');"
               "  k.drawImage(document.getElementById(id), 0, 0);"
               "  try { return 'clean ' + k.getImageData(0, 0, 1, 1).data.join(','); } catch (e) { return e.name; } }");
    CHECK_EQ(page->string("judged('own')"), "clean 0,0,255,255");
    CHECK_EQ(page->string("judged('moved')"), "SecurityError");
    CHECK_EQ(page->string("judged('unknown')"), "SecurityError");
    CHECK_EQ(page->string("judged('anon')"), "clean 0,0,255,255");
    CHECK_EQ(page->string("judged('credstar')"), "SecurityError");
    CHECK_EQ(page->string("judged('credexact')"), "clean 0,0,255,255");
    CHECK_EQ(page->string("judged('credless')"), "SecurityError");
    CHECK_EQ(page->string("judged('plain')"), "SecurityError");
    CHECK_EQ(page->string("judged('wrong')"), "SecurityError");
}

// lineWidth, miterLimit, lineDashOffset and the dashes are unrestricted
// doubles: each reads back as the value it was set to, through save and
// restore too.
void test_line_settings_read_back_as_set()
{
    auto page = canvas_page();
    page->eval("x.lineWidth = 0.3; x.miterLimit = 0.3; x.lineDashOffset = 0.3; x.setLineDash([0.3, 0.1, 0.7]);");
    CHECK(page->boolean("x.lineWidth === 0.3"));
    CHECK(page->boolean("x.miterLimit === 0.3"));
    CHECK(page->boolean("x.lineDashOffset === 0.3"));
    CHECK_EQ(page->string("x.getLineDash().join()"), "0.3,0.1,0.7,0.3,0.1,0.7");
    page->eval("x.save(); x.lineWidth = 5; x.setLineDash([]); x.lineWidth = -1; x.restore();");
    CHECK(page->boolean("x.lineWidth === 0.3"));
    CHECK_EQ(page->string("x.getLineDash().join()"), "0.3,0.1,0.7,0.3,0.1,0.7");
    // A dash still gaps the stroke it is set on.
    page->eval("x.reset(); x.lineWidth = 4; x.setLineDash([10.5, 10.5]); x.strokeStyle = '#000';"
               "x.beginPath(); x.moveTo(0, 50); x.lineTo(100, 50); x.stroke();");
    CHECK(near(page->string("px(5, 50)"), 0, 0, 0, 255));
    CHECK(near(page->string("px(15, 50)"), 0, 0, 0, 0));
}

// rotate(), arc() and ellipse() turn by the engine's own sine and cosine,
// which agree with the C library's (correctly rounded, or nearly, on every
// argument) to within an ulp for every finite angle, however large.
void test_turns_use_the_engines_own_sine()
{
    // How far apart two doubles are, in units of the last place of b.
    auto ulps = [](double a, double b) {
        double const unit = std::nextafter(std::abs(b), std::numeric_limits<double>::infinity()) - std::abs(b);
        return std::abs(a - b) / unit;
    };
    // Never against the machine's own sine and cosine: the C library of
    // one lane is not within a unit of the true value even for small
    // angles, and one that reduces by the x87 instruction returns the
    // angle itself past 2^63. Every angle is held instead to values two
    // independent implementations agree on to the last bit, or within one
    // unit of each other: glibc 2.42 and V8's fdlibm (Node 26), 2026-09-24.
    struct Known {
        double angle;
        double sine;
        double cosine;
    };
    // Across the small sweep, each multiple of pi / 2 as a double (a hair
    // from the true multiple), and a few angles scaled up to 2^20.
    Known const known_small[] = {
        { -14.84, -0.76301401371370148, -0.64638194194803078 },
        { -13.9125, -0.97486836054898596, 0.22278168596303513 },
        { -12.985000000000001, -0.40650857763260145, 0.91364696481251406 },
        { -12.057500000000001, 0.48719127063948175, 0.87329529130339834 },
        { -11.130000000000001, 0.99097846133948275, 0.13402122653233414 },
        { -10.202500000000001, 0.70165816275628545, -0.71251373505180515 },
        { -9.2750000000000004, -0.14921858282224229, -0.98880423468982048 },
        { -8.3475000000000001, -0.88067156674143032, -0.47372733880703405 },
        { -7.4199999999999999, -0.90729872201718398, 0.42048665736974894 },
        { -6.4925000000000006, -0.20778959933967578, 0.97817354411487589 },
        { -5.5650000000000004, 0.65801929054841013, 0.75300107122511262 },
        { -4.6375000000000002, 0.99719713063411397, -0.074818999292223576 },
        { -3.71, 0.53829050829001768, -0.8427593539586935 },
        { -2.7825000000000002, -0.35142490575860402, -0.93621607314367139 },
        { -1.855, -0.95988524156762967, -0.28039315794193914 },
        { -0.92749999999999999, -0.80012285242390724, 0.59983616182173483 },
        { 0, 0, 1 },
        { 0.92749999999999999, 0.80012285242390724, 0.59983616182173483 },
        { 1.855, 0.95988524156762967, -0.28039315794193914 },
        { 2.7825000000000002, 0.35142490575860402, -0.93621607314367139 },
        { 3.71, -0.53829050829001768, -0.8427593539586935 },
        { 4.6375000000000002, -0.99719713063411397, -0.074818999292223576 },
        { 5.5650000000000004, -0.65801929054841013, 0.75300107122511262 },
        { 6.4925000000000006, 0.20778959933967578, 0.97817354411487589 },
        { 7.4199999999999999, 0.90729872201718398, 0.42048665736974894 },
        { 8.3475000000000001, 0.88067156674143032, -0.47372733880703405 },
        { 9.2750000000000004, 0.14921858282224229, -0.98880423468982048 },
        { 10.202500000000001, -0.70165816275628545, -0.71251373505180515 },
        { 11.130000000000001, -0.99097846133948275, 0.13402122653233414 },
        { 12.057500000000001, -0.48719127063948175, 0.87329529130339834 },
        { 12.985000000000001, 0.40650857763260145, 0.91364696481251406 },
        { 13.9125, 0.97486836054898596, 0.22278168596303513 },
        { 14.84, 0.76301401371370148, -0.64638194194803078 },
        { 1.5707963267948966, 1, 6.123233995736766e-17 },
        { 3.1415926535897931, 1.2246467991473532e-16, -1 },
        { 4.7123889803846897, -1, -1.8369701987210297e-16 },
        { 6.2831853071795862, -2.4492935982947064e-16, 1 },
        { 7.8539816339744828, 1, 3.0616169978683831e-16 },
        { 9.4247779607693793, 3.6739403974420594e-16, -1 },
        { 10.995574287564276, -1, -4.2862637970157361e-16 },
        { 12.566370614359172, -4.8985871965894128e-16, 1 },
        { 14.137166941154069, 1, 5.5109105961630896e-16 },
        { 15.707963267948966, 6.1232339957367663e-16, -1 },
        { 17.27875959474386, -1, -2.4499125789312946e-15 },
        { 18.849555921538759, -7.3478807948841188e-16, 1 },
        { 20.420352248333657, 1, -9.8033641995447082e-16 },
        { 21.991148575128552, 8.5725275940314722e-16, -1 },
        { 23.561944901923447, -1, -2.6948419387607653e-15 },
        { 25.132741228718345, -9.7971743931788257e-16, 1 },
        { 21.920000000000002, 0.07108856321879993, -0.99747000765912086 },
        { 350.72000000000003, -0.90795462848051112, 0.41906848201793584 },
        { 5611.5200000000004, 0.59359662560518034, 0.8047627265661248 },
        { 89784.320000000007, -0.67690747104167892, -0.73606811889115176 },
        { 1436549.1200000001, -0.62061101212684566, 0.78411859538394579 },
    };
    double worst = 0;
    for (Known const& k : known_small) {
        canvas::SineCosine const turned = canvas::sine_cosine(k.angle);
        worst = std::max(worst, ulps(turned.sine, k.sine));
        worst = std::max(worst, ulps(turned.cosine, k.cosine));
    }
    CHECK(worst <= 1);
    // Large angles, where an angle reduced by a rounded 2 pi drifts: the
    // error grows with the number of turns taken off. The last is the
    // double nearest a multiple of pi / 2 of them all (Kahan and McDonald).
    Known const known[] = {
        { 100.0, -0.50636564110975879, 0.86231887228768389 },
        { 1e6, -0.34999350217129294, 0.93675212753314474 },
        { 1e9, 0.54584344944869956, 0.83788718136390239 },
        { 123456789.0, 0.99011475180203545, 0.14025968153390964 },
        { 134217727.9, -0.8240740108670469, 0.5664821485391206 },
        { 134217728.0, -0.76340322880198075, 0.64592221687654516 },
        { 1e12, -0.61123870237688949, 0.79144630185289022 },
        { 1e15, 0.85827279317023586, -0.51319373778697031 },
        { 1e22, -0.85220084976718879, 0.52321478539513899 },
        { 1e100, -0.38063773100502868, 0.92472423875193377 },
        { 1e300, -0.81788191211590855, -0.57538611195754907 },
        { std::numeric_limits<double>::max(), 0.004961954789184062, -0.99998768942655991 },
        { -1e9, -0.54584344944869956, 0.83788718136390239 },
        { -1e300, 0.81788191211590855, -0.57538611195754907 },
        { std::ldexp(6381956970095103.0, 797), 1.0, -4.6871659242546277e-19 },
    };
    double worst_known = 0;
    for (Known const& k : known) {
        canvas::SineCosine const turned = canvas::sine_cosine(k.angle);
        worst_known = std::max(worst_known, ulps(turned.sine, k.sine));
        worst_known = std::max(worst_known, ulps(turned.cosine, k.cosine));
    }
    CHECK(worst_known <= 1);
    canvas::SineCosine const quarter = canvas::sine_cosine(3.14159265358979323846 / 2);
    CHECK(quarter.sine == 1 && quarter.cosine == 6.123233995736766e-17);
    CHECK(std::isnan(canvas::sine_cosine(std::numeric_limits<double>::infinity()).sine));
    auto page = canvas_page();
    // As in browsers: the double Math.PI / 2 falls short of a quarter turn,
    // and its cosine is that shortfall.
    CHECK_EQ(page->string("x.rotate(Math.PI / 2); var m = x.getTransform(); [m.a, m.b, m.e].join()"), "6.123233995736766e-17,1,0");
    // A quarter turn about the origin carries (10, -60) to (60, 10).
    page->eval("x.fillStyle = '#0f0'; x.fillRect(5, -65, 10, 10);");
    CHECK(near(page->string("px(60, 10)"), 0, 255, 0, 255));
    // rotate() and a DOMMatrix's rotate() in a transform list take the
    // engine's sine to the bit (at 90 radians it and glibc's cosine part in
    // the last place).
    canvas::SineCosine const ninety = canvas::sine_cosine(90);
    std::array<char, 32> digits {};
    auto const written = std::to_chars(digits.data(), digits.data() + digits.size(), ninety.cosine);
    CHECK_EQ(page->string("x.resetTransform(); x.rotate(90); String(x.getTransform().a)"), std::string(digits.data(), written.ptr));
    CHECK_EQ(page->string("var odd = 0; for (var d = -720; d <= 720; d += 7) { x.resetTransform(); x.rotate(d * Math.PI / 180);"
                          " var mine = x.getTransform(), other = new DOMMatrix('rotate(' + d + 'deg)');"
                          " if (mine.a !== other.a || mine.b !== other.b) ++odd; } x.resetTransform(); String(odd)"),
        "0");
    // A whole circle drawn by arc() closes on itself.
    page->eval("x.reset(); x.fillStyle = '#00f'; x.beginPath(); x.arc(50, 50, 30, 0, 2 * Math.PI); x.fill();");
    CHECK(near(page->string("px(50, 50)"), 0, 0, 255, 255));
    CHECK(near(page->string("px(78, 50)"), 0, 0, 255, 255));
    CHECK(near(page->string("px(50, 22)"), 0, 0, 255, 255));
    CHECK(near(page->string("px(90, 50)"), 0, 0, 0, 0));
}

}

int main()
{
    test_context_is_made_once_and_only_for_2d();
    test_fill_rect_and_the_fill_style();
    test_strokes_end_with_their_caps();
    test_dashes_leave_gaps();
    test_a_linear_gradient_runs_between_its_stops();
    test_a_clip_holds_until_restore();
    test_transforms_move_what_is_drawn();
    test_composite_operations();
    test_a_shadow_falls_beside_the_shape();
    test_text_is_measured_and_drawn();
    test_draw_image_scales_its_source();
    test_a_picture_made_by_script_loads_in_a_task();
    test_image_data_round_trips();
    test_path_errors_and_path2d();
    test_setting_the_size_resets_the_bitmap_and_the_state();
    test_the_host_is_handed_the_picture();
    test_exports_and_the_origin_clean_flag();
    test_a_host_picture_is_judged_by_where_it_came_from();
    test_line_settings_read_back_as_set();
    test_turns_use_the_engines_own_sine();
    return sashfold::test::report("canvas");
}
