#include "Test.h"

#include "css/StyleResolver.h"
#include "css/Stylesheets.h"
#include "dom/Dom.h"
#include "html/TreeBuilder.h"
#include "layout/Controls.h"
#include "layout/Layout.h"
#include "net/Url.h"
#include "text/Face.h"
#include "text/FontManager.h"
#include "ui/Forms.h"

#include <cmath>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// Forms without scripts: the kinds of controls and what they carry, a
// form's data set and its urlencoding, where a GET submission lands, and
// the boxes layout gives controls — Sashfold Mono at the controls' 13.333px
// advances 8.333 px per glyph on a 16 px line.

using namespace sashfold;

namespace {

std::unique_ptr<dom::Document> parse(std::string_view html)
{
    return html::parse_document(html);
}

dom::Element const* by_id(dom::Document const& document, std::string_view id)
{
    return ui::element_by_id(document, id);
}

dom::Element const* named(dom::Document const& document, std::string_view name)
{
    return ui::control_named(document, name);
}

layout::Fragment const* find_box(layout::Fragment const& fragment, std::string_view id)
{
    if (fragment.element) {
        if (dom::Attr const* attribute = fragment.element->find_attribute("id");
            attribute && attribute->value == id)
            return &fragment;
    }
    for (layout::Fragment const& child : fragment.children) {
        if (layout::Fragment const* found = find_box(child, id))
            return found;
    }
    return nullptr;
}

bool near(float actual, float expected)
{
    return std::fabs(actual - expected) < 0.05f;
}

} // namespace

int main()
{
    text::FontManager::instance().set_system_fonts(false);
    using layout::ControlKind;

    // --- Kinds ------------------------------------------------------------------
    {
        auto const document = parse(R"(<input id=t><input id=p type=PASSWORD><input id=c type=checkbox>
<input id=r type=radio><input id=s type=submit><input id=i type=image><input id=b type=button>
<input id=x type=reset><input id=h type=hidden><input id=f type=file><input id=u type=range>
<button id=bt>Go</button><button id=bb type=button>Plain</button><select id=sel></select>
<textarea id=ta></textarea><div id=d></div>)");
        auto const kind = [&](std::string_view id) { return layout::control_kind(*by_id(*document, id)); };
        CHECK(kind("t") == ControlKind::Text);
        CHECK(kind("p") == ControlKind::Password);
        CHECK(kind("c") == ControlKind::Checkbox);
        CHECK(kind("r") == ControlKind::Radio);
        CHECK(kind("s") == ControlKind::Submit);
        CHECK(kind("i") == ControlKind::Submit);
        CHECK(kind("b") == ControlKind::Button);
        CHECK(kind("x") == ControlKind::Button);
        CHECK(kind("h") == ControlKind::Hidden);
        CHECK(kind("f") == ControlKind::File);
        CHECK(kind("u") == ControlKind::Text); // an unknown type behaves as text
        CHECK(kind("bt") == ControlKind::Submit);
        CHECK(kind("bb") == ControlKind::Button);
        CHECK(kind("sel") == ControlKind::Select);
        CHECK(kind("ta") == ControlKind::TextArea);
        CHECK(layout::is_control(*by_id(*document, "t")));
        CHECK(!layout::is_control(*by_id(*document, "h")));
        CHECK(!layout::is_control(*by_id(*document, "d")));
        CHECK(layout::is_text_kind(ControlKind::Password));
        CHECK(!layout::is_text_kind(ControlKind::Select));
    }

    // --- Values, checkedness, captions ----------------------------------------
    {
        auto const document = parse(R"(<input id=a name=a value=x><input id=c type=checkbox name=c>
<input id=cv type=checkbox value=yes><textarea id=t>hello
there</textarea>
<select id=s><option>one<option value=2 selected>two<optgroup><option label=Three>three</optgroup></select>
<input id=sub type=submit><input id=sub2 type=submit value=Send><input id=rst type=reset>
<button id=bt> Go  now </button><input id=pw type=password value=secret>)");
        CHECK_EQ(layout::control_value(*by_id(*document, "a"), nullptr), "x");
        CHECK_EQ(layout::control_value(*by_id(*document, "c"), nullptr), "on");
        CHECK_EQ(layout::control_value(*by_id(*document, "cv"), nullptr), "yes");
        CHECK_EQ(layout::control_value(*by_id(*document, "t"), nullptr), "hello\nthere");
        CHECK_EQ(layout::control_value(*by_id(*document, "s"), nullptr), "2");
        CHECK_EQ(layout::control_caption(*by_id(*document, "s"), nullptr), "two");
        CHECK_EQ(layout::control_caption(*by_id(*document, "sub"), nullptr), "Submit");
        CHECK_EQ(layout::control_caption(*by_id(*document, "sub2"), nullptr), "Send");
        CHECK_EQ(layout::control_caption(*by_id(*document, "rst"), nullptr), "Reset");
        // A <button> has no caption: its children are laid out instead.
        CHECK_EQ(layout::control_caption(*by_id(*document, "bt"), nullptr), "");
        CHECK(!layout::control_checked(*by_id(*document, "c"), nullptr));
        layout::SelectOptions const options = layout::select_options(*by_id(*document, "s"), nullptr);
        CHECK_EQ(options.labels.size(), std::size_t { 3 });
        CHECK(options.labels.size() == 3 && options.labels[2] == "Three" && options.values[2] == "three");
        CHECK_EQ(options.selected, std::size_t { 1 });

        // The live state wins over the markup.
        layout::ControlStates states;
        states.states[by_id(*document, "a")].value = "typed";
        states.states[by_id(*document, "c")].checked = true;
        states.states[by_id(*document, "s")].value = "three";
        CHECK_EQ(layout::control_value(*by_id(*document, "a"), &states), "typed");
        CHECK(layout::control_checked(*by_id(*document, "c"), &states));
        CHECK_EQ(layout::control_caption(*by_id(*document, "s"), &states), "Three");
        CHECK_EQ(layout::select_options(*by_id(*document, "s"), &states).selected, std::size_t { 2 });
    }

    // --- Form ownership, the data set, urlencoding -----------------------------
    {
        auto const document = parse(R"(<form id=f action=search>
<input name=q value="hello world"><input type=checkbox name=c checked><input type=checkbox name=d>
<input type=radio name=r value=1 checked><input type=radio name=r value=2>
<select name=s><option>one<option selected>two</select>
<textarea name=t>line1
line2</textarea><input type=hidden name=h value=v><input name=dis disabled value=x>
<input name=empty><input type=file name=up>
<input type=submit name=go value=Go><input type=submit name=other><button name=b value=bv>B</button>
</form><input id=outside form=f name=o value=1><input id=loose name=l><form id=g method=post><input name=p></form>)");
        dom::Element const* const form = by_id(*document, "f");
        CHECK(form != nullptr);
        CHECK(ui::form_owner(*named(*document, "q"), *document) == form);
        CHECK(ui::form_owner(*by_id(*document, "outside"), *document) == form);
        CHECK(ui::form_owner(*by_id(*document, "loose"), *document) == nullptr);
        dom::Element const* const go = named(*document, "go");
        CHECK(ui::default_submitter(*form) == go);

        std::vector<ui::FormField> const fields = ui::form_data_set(*form, go, nullptr);
        std::vector<std::string> pairs;
        for (ui::FormField const& field : fields)
            pairs.push_back(field.name + "=" + field.value);
        CHECK((pairs == std::vector<std::string> { "q=hello world", "c=on", "r=1", "s=two",
                   "t=line1\r\nline2", "h=v", "empty=", "go=Go" }));
        std::vector<ui::FormField> const without = ui::form_data_set(*form, nullptr, nullptr);
        CHECK_EQ(without.size(), std::size_t { 7 }); // no button without a submitter
        std::vector<ui::FormField> const by_button
            = ui::form_data_set(*form, named(*document, "b"), nullptr);
        CHECK(!by_button.empty() && by_button.back().name == "b" && by_button.back().value == "bv");
        std::vector<ui::FormField> const bare
            = ui::form_data_set(*form, named(*document, "other"), nullptr);
        CHECK(!bare.empty() && bare.back().value == "Submit"); // an unlabeled submit sends Submit

        CHECK_EQ(ui::urlencode_form({ { "q", "hello world" } }), "q=hello+world");
        CHECK_EQ(ui::urlencode_form({ { "n", "a&b=c/\xC3\xA9" }, { "k*-._", "" } }),
            "n=a%26b%3Dc%2F%C3%A9&k*-._=");
        CHECK_EQ(ui::urlencode_form({}), "");

        net::Url const page = *net::parse_url("https://example.org/dir/page.html?old=1#frag");
        std::optional<net::Url> const url = ui::get_submission_url(*form, go, nullptr, page);
        CHECK(url && url->serialize()
                == "https://example.org/dir/search?q=hello+world&c=on&r=1&s=two&t=line1%0D%0Aline2&h=v&empty=&go=Go");
        // No action: the page's own URL, its query replaced and its fragment dropped.
        auto const plain = parse(R"(<form><input name=a value=1></form>)");
        dom::Element const* const plain_form = ui::form_owner(*named(*plain, "a"), *plain);
        std::optional<net::Url> const own = ui::get_submission_url(*plain_form, nullptr, nullptr, page);
        CHECK(own && own->serialize() == "https://example.org/dir/page.html?a=1");
        // A posting form is not submitted this way.
        dom::Element const* const posting = by_id(*document, "g");
        CHECK(!ui::get_submission_url(*posting, nullptr, nullptr, page).has_value());
        // formaction and formmethod on the submitter win.
        auto const override_doc = parse(R"(<form method=post action=a><input name=x value=1><input id=s type=submit formmethod=get formaction=/b></form>)");
        dom::Element const* const override_form = ui::form_owner(*named(*override_doc, "x"), *override_doc);
        std::optional<net::Url> const overridden
            = ui::get_submission_url(*override_form, by_id(*override_doc, "s"), nullptr, page);
        CHECK(overridden && overridden->serialize() == "https://example.org/b?x=1");

        // What submitting asks for. A GET: the same address, nothing posted.
        {
            std::optional<ui::FormSubmission> const asked = ui::form_submission(*plain_form, nullptr, nullptr, page);
            CHECK(asked && !asked->post && asked->body.empty() && asked->content_type.empty());
            CHECK(asked && asked->url.serialize() == "https://example.org/dir/page.html?a=1");
        }
        // A POST: the action as written — its own query kept, the fragment
        // dropped — and the data set as the body, urlencoded unless the form
        // says otherwise.
        {
            auto const doc = parse(R"(<form method=POST action="/take?kept=1#top"><input name=who value="A & B"><input name=n value="line1
line2"><input id=s type=submit name=go value=Send></form>)");
            dom::Element const* const post_form = ui::form_owner(*named(*doc, "who"), *doc);
            std::optional<ui::FormSubmission> const asked = ui::form_submission(*post_form, by_id(*doc, "s"), nullptr, page);
            CHECK(asked && asked->post);
            CHECK(asked && asked->url.serialize() == "https://example.org/take?kept=1");
            CHECK(asked && asked->content_type == "application/x-www-form-urlencoded");
            CHECK(asked && std::string(asked->body.begin(), asked->body.end()) == "who=A+%26+B&n=line1%0D%0Aline2&go=Send");
        }
        // multipart/form-data: a part a field, between lines of a boundary
        // that nothing in the form holds, the same for the same form.
        {
            auto const doc = parse(R"(<form method=post enctype="multipart/form-data" action=up><input name='the "name"' value=plain><input name=t value=two></form>)");
            dom::Element const* const multi = ui::form_owner(*named(*doc, "t"), *doc);
            std::optional<ui::FormSubmission> const asked = ui::form_submission(*multi, nullptr, nullptr, page);
            CHECK(asked && asked->post && asked->url.serialize() == "https://example.org/dir/up");
            std::string const type = asked ? asked->content_type : std::string();
            CHECK(type.starts_with("multipart/form-data; boundary=----SashfoldFormBoundary"));
            std::string const boundary = type.substr(type.find('=') + 1);
            std::string const body = asked ? std::string(asked->body.begin(), asked->body.end()) : std::string();
            CHECK_EQ(body,
                "--" + boundary + "\r\nContent-Disposition: form-data; name=\"the %22name%22\"\r\n\r\nplain\r\n--" + boundary
                    + "\r\nContent-Disposition: form-data; name=\"t\"\r\n\r\ntwo\r\n--" + boundary + "--\r\n");
            std::optional<ui::FormSubmission> const again = ui::form_submission(*multi, nullptr, nullptr, page);
            CHECK(again && again->content_type == type);
            // A field that holds the boundary gets a longer one.
            std::vector<ui::FormField> const awkward { { "a", "has " + ui::multipart_boundary_for({ { "a", "x" } }) + " in it" } };
            std::string const other = ui::multipart_boundary_for(awkward);
            CHECK(awkward[0].value.find(other) == std::string::npos);
            CHECK_EQ(ui::multipart_form({}, "B"), std::string("--B--\r\n"));
        }
        // text/plain, by the submitter's formenctype; a formmethod of post on
        // a form that says get; and what is no method at all is get.
        {
            auto const doc = parse(R"(<form action=t><input name=k value="v w"><input id=p type=submit formmethod=post formenctype=text/plain><input id=odd type=submit formmethod=purple></form>)");
            dom::Element const* const form_t = ui::form_owner(*named(*doc, "k"), *doc);
            std::optional<ui::FormSubmission> const plain_text = ui::form_submission(*form_t, by_id(*doc, "p"), nullptr, page);
            CHECK(plain_text && plain_text->post && plain_text->content_type == "text/plain");
            CHECK(plain_text && std::string(plain_text->body.begin(), plain_text->body.end()) == "k=v w\r\n");
            std::optional<ui::FormSubmission> const odd = ui::form_submission(*form_t, by_id(*doc, "odd"), nullptr, page);
            CHECK(odd && !odd->post && odd->url.serialize() == "https://example.org/dir/t?k=v+w");
        }
        // A form that names a dialog is nobody's navigation.
        {
            auto const doc = parse(R"(<form method=dialog><input name=k value=v></form>)");
            CHECK(!ui::form_submission(*ui::form_owner(*named(*doc, "k"), *doc), nullptr, nullptr, page).has_value());
        }

        std::vector<dom::Element const*> const focusable = ui::focusable_controls(*document);
        CHECK_EQ(focusable.size(), std::size_t { 15 }); // every drawn control but the disabled one
        CHECK(named(*document, "h") != nullptr); // hidden ones are still addressable by name
    }

    // --- The boxes layout gives controls ----------------------------------------
    {
        constexpr std::string_view html = R"HTML(<!doctype html><html><head><style>
  body { margin: 0; font-family: "Sashfold Mono"; font-size: 16px; line-height: 20px }
</style></head><body><p><input id="q" name="q"> <input id="c" type="checkbox" checked> <button id="go">Go</button> <select id="s"><option>one<option>two</select> <input id="pw" type="password" value="abc"></p>
<input id="block" style="display:block;width:100px">
<textarea id="ta" rows="3" cols="10">a
b</textarea></body></html>)HTML";
        auto const document = parse(html);
        css::StyleMap const styles = css::resolve_styles(*document);
        layout::LayoutResult const page = layout::layout_document(*document, styles, 800);
        layout::Fragment const* q = find_box(page.root, "q");
        layout::Fragment const* c = find_box(page.root, "c");
        layout::Fragment const* go = find_box(page.root, "go");
        layout::Fragment const* s = find_box(page.root, "s");
        layout::Fragment const* pw = find_box(page.root, "pw");
        layout::Fragment const* block = find_box(page.root, "block");
        layout::Fragment const* ta = find_box(page.root, "ta");
        CHECK(q != nullptr);
        CHECK(c != nullptr);
        CHECK(go != nullptr);
        CHECK(s != nullptr);
        CHECK(pw != nullptr);
        CHECK(block != nullptr);
        CHECK(ta != nullptr);
        if (q && c && go && s && pw && block && ta) {
            float const glyph = 13.333f * 0.625f;
            // A control's line is the face's normal line at its size: ascent,
            // descent and line gap of Sashfold Mono (38/32 of the size).
            text::FaceMetrics const metrics = text::builtin_face().metrics(13.333f);
            float const line = metrics.ascent + metrics.descent + metrics.line_gap;
            CHECK(q->control && q->control->kind == ControlKind::Text);
            CHECK(near(q->width, 20 * glyph + 6)); // size=20 glyphs and the edges
            CHECK(near(q->height, line + 6)); // one line and the edges
            CHECK(q->runs.empty()); // nothing typed yet
            CHECK(c->control && c->control->kind == ControlKind::Checkbox && c->control->checked);
            CHECK_EQ(c->width, 13.0f);
            CHECK_EQ(c->height, 13.0f);
            CHECK(c->x > q->x + q->width); // after the field, with its margins
            CHECK(go->control && go->control->kind == ControlKind::Submit);
            // "Go", then the built-in sheet's 6px of padding and 2px border
            // each side, the text laid out as the button's own line.
            CHECK(near(go->width, 2 * glyph + 16));
            CHECK(go->runs.size() == 1 && go->runs[0].text == U"Go");
            CHECK(near(s->width, 3 * glyph + 26)); // the widest option, the arrow, the edges
            CHECK(s->runs.size() == 1 && s->runs[0].text == U"one"); // the first option shows
            CHECK(pw->runs.size() == 1 && pw->runs[0].text == U"•••");
            CHECK_EQ(block->width, 100.0f); // block-level, sized by CSS
            CHECK(near(block->height, line + 6));
            CHECK(near(ta->height, 3 * line + 6));
            CHECK_EQ(ta->runs.size(), std::size_t { 2 }); // one run per line
        }

        // A focused field with a typed value shows it, with the caret after it.
        layout::ControlStates states;
        states.focused = by_id(*document, "q");
        states.states[states.focused].value = "hi";
        states.states[states.focused].caret = 1;
        layout::LayoutResult const live = layout::layout_document(*document, styles, 800, nullptr, &states);
        layout::Fragment const* focused = find_box(live.root, "q");
        if (CHECK(focused && focused->control)) {
            CHECK(focused->control->focused);
            CHECK(focused->runs.size() == 1 && focused->runs[0].text == U"hi");
            CHECK(focused->control->caret_x.has_value());
            if (focused->control->caret_x)
                CHECK(near(*focused->control->caret_x, focused->x + 4 + 13.333f * 0.625f)); // after "h"
        }
    }

    // --- The same controls on a display of two device px per CSS px ----------
    // The styles resolve at the scale (the control face is 13.333 CSS px, so
    // 26.667 device px) and the edges layout owns double with them.
    {
        constexpr std::string_view html = R"HTML(<!doctype html><html><head><style>
  body { margin: 0; font-family: "Sashfold Mono"; font-size: 16px; line-height: 20px }
</style></head><body><p><input id="q" name="q"> <input id="c" type="checkbox"> <button id="go">Go</button></p></body></html>)HTML";
        auto const document = parse(html);
        std::vector<css::SheetSource> const sheets = css::collect_stylesheets(*document, nullptr, {});
        css::StyleMap const styles = css::resolve_styles(*document, sheets, css::MediaContext { 800, 600, 2 });
        layout::LayoutResult const page = layout::layout_document(*document, styles, 800, nullptr, nullptr, 0, 2);
        layout::Fragment const* q = find_box(page.root, "q");
        layout::Fragment const* c = find_box(page.root, "c");
        layout::Fragment const* go = find_box(page.root, "go");
        CHECK(q != nullptr);
        CHECK(c != nullptr);
        CHECK(go != nullptr);
        if (q && c && go) {
            float const glyph = 26.667f * 0.625f;
            text::FaceMetrics const metrics = text::builtin_face().metrics(26.667f);
            float const line = metrics.ascent + metrics.descent + metrics.line_gap;
            CHECK(near(q->width, 20 * glyph + 12));
            CHECK(near(q->height, line + 12));
            CHECK(near(c->width, 26));
            CHECK(near(c->height, 26));
            CHECK(near(go->width, 2 * glyph + 32)); // "Go", then 6px of padding and a 2px border each side, doubled
        }
    }

    // --- A <button> lays out what it holds ----------------------------------------
    // Its children are boxes inside its own: an inline SVG icon is laid out
    // in the button's line, the line centred across by the built-in sheet's
    // text-align and down by button layout (HTML §15.5.2). With no strut
    // (font-size 0) the line is the icon alone, so the icon's centre is the
    // button's. The button is still the control it acts as, and its face is
    // drawn only when the page gave it no background or border of its own.
    {
        constexpr std::string_view html = R"HTML(<!doctype html><html><head><style>
  body { margin: 0; font-family: "Sashfold Mono"; font-size: 16px; line-height: 20px }
  #icon { width: 100px; height: 60px; font-size: 0 }
  #own { background: #eef; border: 1px solid #334 }
  #bare { appearance: none }
  #inked { color: #c00 }
</style></head><body><p><button id="icon"><svg id="glyph" width="20" height="20"><rect width="20" height="20" fill="currentcolor"/></svg></button>
<button id="plain">Go</button> <button id="own">Own</button> <button id="bare">Bare</button> <button id="inked">Ink</button>
<span id="inline-wrap">a<button id="inline" style="display: inline">x<br>y</button>b</span></p></body></html>)HTML";
        auto const document = parse(html);
        std::vector<css::SheetSource> const sheets = css::collect_stylesheets(*document, nullptr, {});
        css::StyleMap const styles = css::resolve_styles(*document, sheets, css::MediaContext { 800, 600, 1 });
        layout::LayoutResult const page = layout::layout_document(*document, styles, 800);
        layout::Fragment const* icon = find_box(page.root, "icon");
        layout::Fragment const* glyph = find_box(page.root, "glyph");
        if (CHECK(icon != nullptr) && CHECK(glyph != nullptr)) {
            CHECK(icon->control && icon->control->contents && icon->control->kind == ControlKind::Submit);
            CHECK(near(icon->width, 100));
            CHECK(near(icon->height, 60));
            // The icon is the button's own child box, not a picture of it.
            bool const child = find_box(*icon, "glyph") == glyph;
            CHECK(child);
            CHECK(glyph->x >= icon->x && glyph->x + glyph->width <= icon->x + icon->width);
            CHECK(glyph->y >= icon->y && glyph->y + glyph->height <= icon->y + icon->height);
            CHECK(near(glyph->width, 20));
            CHECK(near(glyph->x + glyph->width / 2, icon->x + icon->width / 2));
            CHECK(near(glyph->y + glyph->height / 2, icon->y + icon->height / 2));
        }
        // A button of text alone lays its text out as a line of its own; the
        // built-in face stays unless the page decorated the button itself.
        layout::Fragment const* plain = find_box(page.root, "plain");
        layout::Fragment const* own = find_box(page.root, "own");
        layout::Fragment const* bare = find_box(page.root, "bare");
        layout::Fragment const* inked = find_box(page.root, "inked");
        if (CHECK(plain && own && bare && inked)) {
            CHECK(plain->runs.size() == 1 && plain->runs[0].text == U"Go");
            CHECK(plain->control && plain->control->themed);
            CHECK(inked->control && inked->control->themed);
            CHECK(own->control && !own->control->themed);
            CHECK(bare->control && !bare->control->themed);
        }
        // An inline button is an atomic box all the same: its two lines stay
        // inside it rather than breaking the line around it.
        layout::Fragment const* inline_button = find_box(page.root, "inline");
        if (CHECK(inline_button != nullptr)) {
            CHECK(inline_button->control && inline_button->control->contents);
            CHECK_EQ(inline_button->runs.size(), std::size_t { 2 });
        }
    }

    return sashfold::test::report("forms");
}
