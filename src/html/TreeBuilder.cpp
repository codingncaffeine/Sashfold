#include "html/TreeBuilder.h"

#include "core/Ascii.h"
#include "core/Unicode.h"
#include "html/Encoding.h"
#include "html/InputStream.h"

#include <algorithm>
#include <array>

namespace sashfold::html {

using dom::Attr;
using dom::Comment;
using dom::Document;
using dom::DocumentFragment;
using dom::DocumentType;
using dom::Element;
using dom::Node;
using dom::NodeType;
using dom::QuirksMode;
using dom::Text;

namespace {

template<std::size_t N>
bool one_of(std::string_view name, std::array<std::string_view, N> const& names)
{
    return std::find(names.begin(), names.end(), name) != names.end();
}

bool ascii_ci_starts_with(std::string_view text, std::string_view prefix)
{
    return text.size() >= prefix.size() && ascii_ci_equals(text.substr(0, prefix.size()), prefix);
}

std::string ascii_lowercase(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (char const c : text)
        out.push_back(static_cast<char>(to_ascii_lowercase(static_cast<unsigned char>(c))));
    return out;
}

constexpr std::array<std::string_view, 16> formatting_tags {
    "a", "b", "big", "code", "em", "font", "i", "nobr", "s", "small", "strike", "strong", "tt", "u"
};

constexpr std::array<std::string_view, 85> special_html_tags {
    "address", "applet", "area", "article", "aside", "base", "basefont", "bgsound", "blockquote",
    "body", "br", "button", "caption", "center", "col", "colgroup", "dd", "details", "dir", "div",
    "dl", "dt", "embed", "fieldset", "figcaption", "figure", "footer", "form", "frame", "frameset",
    "h1", "h2", "h3", "h4", "h5", "h6", "head", "header", "hgroup", "hr", "html", "iframe", "img",
    "input", "keygen", "li", "link", "listing", "main", "marquee", "menu", "meta", "nav", "noembed",
    "noframes", "noscript", "object", "ol", "p", "param", "plaintext", "pre", "script", "search",
    "section", "select", "source", "style", "summary", "table", "tbody", "td", "template",
    "textarea", "tfoot", "th", "thead", "title", "tr", "track", "ul", "wbr", "xmp", "isindex", "image"
};

constexpr std::array<std::string_view, 8> implied_end_tags { "dd", "dt", "li", "optgroup", "option", "p", "rb", "rp" };
constexpr std::array<std::string_view, 2> implied_end_tags_extra { "rt", "rtc" };
constexpr std::array<std::string_view, 8> thorough_extra { "caption", "colgroup", "tbody", "td", "tfoot", "th", "thead", "tr" };

constexpr std::array<std::string_view, 6> heading_tags { "h1", "h2", "h3", "h4", "h5", "h6" };

// InBody block starts that first close an open <p>.
constexpr std::array<std::string_view, 25> close_p_blocks {
    "address", "article", "aside", "blockquote", "center", "details", "dialog", "dir", "div", "dl",
    "fieldset", "figcaption", "figure", "footer", "header", "hgroup", "main", "menu", "nav", "ol",
    "p", "search", "section", "summary", "ul"
};

constexpr std::array<std::string_view, 28> end_tag_blocks {
    "address", "article", "aside", "blockquote", "button", "center", "details", "dialog", "dir",
    "div", "dl", "fieldset", "figcaption", "figure", "footer", "header", "hgroup", "listing",
    "main", "menu", "nav", "ol", "pre", "search", "section", "select", "summary", "ul"
};

// Foreign-content breakout start tags.
constexpr std::array<std::string_view, 44> foreign_breakout {
    "b", "big", "blockquote", "body", "br", "center", "code", "dd", "div", "dl", "dt", "em",
    "embed", "h1", "h2", "h3", "h4", "h5", "h6", "head", "hr", "i", "img", "li", "listing",
    "menu", "meta", "nobr", "ol", "p", "pre", "ruby", "s", "small", "span", "strong", "strike",
    "sub", "sup", "table", "tt", "u", "ul", "var"
};

struct NameFix {
    std::string_view from;
    std::string_view to;
};

constexpr std::array<NameFix, 36> svg_tag_fixes { {
    { "altglyph", "altGlyph" }, { "altglyphdef", "altGlyphDef" }, { "altglyphitem", "altGlyphItem" },
    { "animatecolor", "animateColor" }, { "animatemotion", "animateMotion" },
    { "animatetransform", "animateTransform" }, { "clippath", "clipPath" }, { "feblend", "feBlend" },
    { "fecolormatrix", "feColorMatrix" }, { "fecomponenttransfer", "feComponentTransfer" },
    { "fecomposite", "feComposite" }, { "feconvolvematrix", "feConvolveMatrix" },
    { "fediffuselighting", "feDiffuseLighting" }, { "fedisplacementmap", "feDisplacementMap" },
    { "fedistantlight", "feDistantLight" }, { "fedropshadow", "feDropShadow" }, { "feflood", "feFlood" },
    { "fefunca", "feFuncA" }, { "fefuncb", "feFuncB" }, { "fefuncg", "feFuncG" }, { "fefuncr", "feFuncR" },
    { "fegaussianblur", "feGaussianBlur" }, { "feimage", "feImage" }, { "femerge", "feMerge" },
    { "femergenode", "feMergeNode" }, { "femorphology", "feMorphology" }, { "feoffset", "feOffset" },
    { "fepointlight", "fePointLight" }, { "fespecularlighting", "feSpecularLighting" },
    { "fespotlight", "feSpotLight" }, { "fetile", "feTile" }, { "feturbulence", "feTurbulence" },
    { "foreignobject", "foreignObject" }, { "glyphref", "glyphRef" },
    { "lineargradient", "linearGradient" }, { "radialgradient", "radialGradient" },
} };
// textPath is handled below; the array above stays exactly at the table entries
// whose lowercase differs. (textpath -> textPath added separately to keep the
// array literal aligned with the spec table.)
constexpr NameFix svg_tag_fix_textpath { "textpath", "textPath" };

constexpr std::array<NameFix, 58> svg_attribute_fixes { {
    { "attributename", "attributeName" }, { "attributetype", "attributeType" },
    { "basefrequency", "baseFrequency" }, { "baseprofile", "baseProfile" }, { "calcmode", "calcMode" },
    { "clippathunits", "clipPathUnits" }, { "diffuseconstant", "diffuseConstant" },
    { "edgemode", "edgeMode" }, { "filterunits", "filterUnits" }, { "glyphref", "glyphRef" },
    { "gradienttransform", "gradientTransform" }, { "gradientunits", "gradientUnits" },
    { "kernelmatrix", "kernelMatrix" }, { "kernelunitlength", "kernelUnitLength" },
    { "keypoints", "keyPoints" }, { "keysplines", "keySplines" }, { "keytimes", "keyTimes" },
    { "lengthadjust", "lengthAdjust" }, { "limitingconeangle", "limitingConeAngle" },
    { "markerheight", "markerHeight" }, { "markerunits", "markerUnits" }, { "markerwidth", "markerWidth" },
    { "maskcontentunits", "maskContentUnits" }, { "maskunits", "maskUnits" }, { "numoctaves", "numOctaves" },
    { "pathlength", "pathLength" }, { "patterncontentunits", "patternContentUnits" },
    { "patterntransform", "patternTransform" }, { "patternunits", "patternUnits" },
    { "pointsatx", "pointsAtX" }, { "pointsaty", "pointsAtY" }, { "pointsatz", "pointsAtZ" },
    { "preservealpha", "preserveAlpha" }, { "preserveaspectratio", "preserveAspectRatio" },
    { "primitiveunits", "primitiveUnits" }, { "refx", "refX" }, { "refy", "refY" },
    { "repeatcount", "repeatCount" }, { "repeatdur", "repeatDur" },
    { "requiredextensions", "requiredExtensions" }, { "requiredfeatures", "requiredFeatures" },
    { "specularconstant", "specularConstant" }, { "specularexponent", "specularExponent" },
    { "spreadmethod", "spreadMethod" }, { "startoffset", "startOffset" },
    { "stddeviation", "stdDeviation" }, { "stitchtiles", "stitchTiles" },
    { "surfacescale", "surfaceScale" }, { "systemlanguage", "systemLanguage" },
    { "tablevalues", "tableValues" }, { "targetx", "targetX" }, { "targety", "targetY" },
    { "textlength", "textLength" }, { "viewbox", "viewBox" }, { "viewtarget", "viewTarget" },
    { "xchannelselector", "xChannelSelector" }, { "ychannelselector", "yChannelSelector" },
    { "zoomandpan", "zoomAndPan" },
} };

constexpr std::array<std::string_view, 55> quirky_public_prefixes { {
    "+//Silmaril//dtd html Pro v0r11 19970101//",
    "-//AS//DTD HTML 3.0 asWedit + extensions//",
    "-//AdvaSoft Ltd//DTD HTML 3.0 asWedit + extensions//",
    "-//IETF//DTD HTML 2.0 Level 1//",
    "-//IETF//DTD HTML 2.0 Level 2//",
    "-//IETF//DTD HTML 2.0 Strict Level 1//",
    "-//IETF//DTD HTML 2.0 Strict Level 2//",
    "-//IETF//DTD HTML 2.0 Strict//",
    "-//IETF//DTD HTML 2.0//",
    "-//IETF//DTD HTML 2.1E//",
    "-//IETF//DTD HTML 3.0//",
    "-//IETF//DTD HTML 3.2 Final//",
    "-//IETF//DTD HTML 3.2//",
    "-//IETF//DTD HTML 3//",
    "-//IETF//DTD HTML Level 0//",
    "-//IETF//DTD HTML Level 1//",
    "-//IETF//DTD HTML Level 2//",
    "-//IETF//DTD HTML Level 3//",
    "-//IETF//DTD HTML Strict Level 0//",
    "-//IETF//DTD HTML Strict Level 1//",
    "-//IETF//DTD HTML Strict Level 2//",
    "-//IETF//DTD HTML Strict Level 3//",
    "-//IETF//DTD HTML Strict//",
    "-//IETF//DTD HTML//",
    "-//Metrius//DTD Metrius Presentational//",
    "-//Microsoft//DTD Internet Explorer 2.0 HTML Strict//",
    "-//Microsoft//DTD Internet Explorer 2.0 HTML//",
    "-//Microsoft//DTD Internet Explorer 2.0 Tables//",
    "-//Microsoft//DTD Internet Explorer 3.0 HTML Strict//",
    "-//Microsoft//DTD Internet Explorer 3.0 HTML//",
    "-//Microsoft//DTD Internet Explorer 3.0 Tables//",
    "-//Netscape Comm. Corp.//DTD HTML//",
    "-//Netscape Comm. Corp.//DTD Strict HTML//",
    "-//O'Reilly and Associates//DTD HTML 2.0//",
    "-//O'Reilly and Associates//DTD HTML Extended 1.0//",
    "-//O'Reilly and Associates//DTD HTML Extended Relaxed 1.0//",
    "-//SQ//DTD HTML 2.0 HoTMetaL + extensions//",
    "-//SoftQuad Software//DTD HoTMetaL PRO 6.0::19990601::extensions to HTML 4.0//",
    "-//SoftQuad//DTD HoTMetaL PRO 4.0::19971010::extensions to HTML 4.0//",
    "-//Spyglass//DTD HTML 2.0 Extended//",
    "-//Sun Microsystems Corp.//DTD HotJava HTML//",
    "-//Sun Microsystems Corp.//DTD HotJava Strict HTML//",
    "-//W3C//DTD HTML 3 1995-03-24//",
    "-//W3C//DTD HTML 3.2 Draft//",
    "-//W3C//DTD HTML 3.2 Final//",
    "-//W3C//DTD HTML 3.2//",
    "-//W3C//DTD HTML 3.2S Draft//",
    "-//W3C//DTD HTML 4.0 Frameset//",
    "-//W3C//DTD HTML 4.0 Transitional//",
    "-//W3C//DTD HTML Experimental 19960712//",
    "-//W3C//DTD HTML Experimental 970421//",
    "-//W3C//DTD W3 HTML//",
    "-//W3O//DTD W3 HTML 3.0//",
    "-//WebTechs//DTD Mozilla HTML 2.0//",
    "-//WebTechs//DTD Mozilla HTML//",
} };

QuirksMode quirks_mode_for_doctype(Token const& token)
{
    std::string const name = token.doctype_name.value_or("");
    std::string const public_id = token.public_identifier.value_or("");
    std::string const system_id = token.system_identifier.value_or("");
    bool const has_system = token.system_identifier.has_value();

    if (token.force_quirks || name != "html")
        return QuirksMode::Yes;
    if (ascii_ci_equals(public_id, "-//W3O//DTD W3 HTML Strict 3.0//EN//")
        || ascii_ci_equals(public_id, "-/W3C/DTD HTML 4.0 Transitional/EN")
        || ascii_ci_equals(public_id, "HTML"))
        return QuirksMode::Yes;
    if (ascii_ci_equals(system_id, "http://www.ibm.com/data/dtd/v11/ibmxhtml1-transitional.dtd"))
        return QuirksMode::Yes;
    for (std::string_view prefix : quirky_public_prefixes) {
        if (ascii_ci_starts_with(public_id, prefix))
            return QuirksMode::Yes;
    }
    if (!has_system
        && (ascii_ci_starts_with(public_id, "-//W3C//DTD HTML 4.01 Frameset//")
            || ascii_ci_starts_with(public_id, "-//W3C//DTD HTML 4.01 Transitional//")))
        return QuirksMode::Yes;

    if (ascii_ci_starts_with(public_id, "-//W3C//DTD XHTML 1.0 Frameset//")
        || ascii_ci_starts_with(public_id, "-//W3C//DTD XHTML 1.0 Transitional//"))
        return QuirksMode::Limited;
    if (has_system
        && (ascii_ci_starts_with(public_id, "-//W3C//DTD HTML 4.01 Frameset//")
            || ascii_ci_starts_with(public_id, "-//W3C//DTD HTML 4.01 Transitional//")))
        return QuirksMode::Limited;

    return QuirksMode::No;
}

bool is_mathml_text_integration_point(Element const& element)
{
    return element.namespace_uri() == dom::ns::mathml
        && one_of(element.local_name(), std::array<std::string_view, 5> { "mi", "mo", "mn", "ms", "mtext" });
}

bool is_html_integration_point(Element const& element)
{
    if (element.namespace_uri() == dom::ns::svg)
        return one_of(element.local_name(), std::array<std::string_view, 3> { "foreignObject", "desc", "title" });
    if (element.is_mathml("annotation-xml")) {
        if (Attr const* encoding = element.find_attribute("encoding"))
            return ascii_ci_equals(encoding->value, "text/html")
                || ascii_ci_equals(encoding->value, "application/xhtml+xml");
    }
    return false;
}

bool is_special(Element const& element)
{
    if (element.is_html())
        return one_of(element.local_name(), special_html_tags);
    if (element.namespace_uri() == dom::ns::mathml)
        return one_of(element.local_name(),
            std::array<std::string_view, 6> { "mi", "mo", "mn", "ms", "mtext", "annotation-xml" });
    if (element.namespace_uri() == dom::ns::svg)
        return one_of(element.local_name(), std::array<std::string_view, 3> { "foreignObject", "desc", "title" });
    return false;
}

bool is_default_scope_terminator(Element const& element)
{
    if (element.is_html())
        return one_of(element.local_name(),
            std::array<std::string_view, 10> { "applet", "caption", "html", "table", "td", "th", "marquee", "object", "select", "template" });
    if (element.namespace_uri() == dom::ns::mathml)
        return one_of(element.local_name(),
            std::array<std::string_view, 6> { "mi", "mo", "mn", "ms", "mtext", "annotation-xml" });
    if (element.namespace_uri() == dom::ns::svg)
        return one_of(element.local_name(), std::array<std::string_view, 3> { "foreignObject", "desc", "title" });
    return false;
}

void adjust_svg_tag_name(std::string& name)
{
    for (NameFix const& fix : svg_tag_fixes) {
        if (name == fix.from) {
            name = fix.to;
            return;
        }
    }
    if (name == svg_tag_fix_textpath.from)
        name = svg_tag_fix_textpath.to;
}

Attr adjusted_foreign_attribute(Attribute const& attribute, bool mathml, bool svg)
{
    Attr out;
    out.local_name = attribute.name;
    out.value = attribute.value;

    if (mathml && attribute.name == "definitionurl") {
        out.local_name = "definitionURL";
        return out;
    }
    if (svg) {
        for (NameFix const& fix : svg_attribute_fixes) {
            if (attribute.name == fix.from) {
                out.local_name = fix.to;
                return out;
            }
        }
    }

    auto const set = [&](std::string_view prefix, std::string_view local, std::string_view namespace_uri) {
        out.prefix = std::string(prefix);
        out.local_name = std::string(local);
        out.namespace_uri = std::string(namespace_uri);
    };
    if (attribute.name.starts_with("xlink:")) {
        std::string_view const local = std::string_view(attribute.name).substr(6);
        if (one_of(local, std::array<std::string_view, 7> { "actuate", "arcrole", "href", "role", "show", "title", "type" }))
            set("xlink", local, dom::ns::xlink);
    } else if (attribute.name == "xml:lang" || attribute.name == "xml:space") {
        set("xml", std::string_view(attribute.name).substr(4), dom::ns::xml);
    } else if (attribute.name == "xmlns") {
        set("", "xmlns", dom::ns::xmlns);
    } else if (attribute.name == "xmlns:xlink") {
        set("xmlns", "xlink", dom::ns::xmlns);
    }
    return out;
}

}

// --- Construction and the pump -----------------------------------------------

TreeBuilder::TreeBuilder(Document& document)
    : m_document(document)
{
}

TreeBuilder::TreeBuilder(Document& document, Element& context)
    : m_document(document)
    , m_context(&context)
{
    Token html_token;
    html_token.type = Token::Type::StartTag;
    html_token.tag_name = "html";
    m_html_element = create_element_for_token(html_token, dom::ns::html);
    m_document.append_child(*m_html_element);
    m_stack.push_back(m_html_element);

    if (context.is_html("template"))
        m_template_modes.push_back(Mode::InTemplate);

    reset_insertion_mode();

    for (Element* ancestor = &context; ancestor; ) {
        if (ancestor->is_html("form")) {
            m_form_element = ancestor;
            break;
        }
        Node* parent = ancestor->parent();
        ancestor = (parent && parent->is_element()) ? static_cast<Element*>(parent) : nullptr;
    }
}

Tokenizer::State TreeBuilder::tokenizer_state_for_fragment_context(Element const& context)
{
    if (!context.is_html())
        return Tokenizer::State::Data;
    std::string_view const name = context.local_name();
    if (name == "title" || name == "textarea")
        return Tokenizer::State::RCDATA;
    if (name == "style" || name == "xmp" || name == "iframe" || name == "noembed" || name == "noframes")
        return Tokenizer::State::RAWTEXT;
    if (name == "script")
        return Tokenizer::State::ScriptData;
    if (name == "plaintext")
        return Tokenizer::State::PLAINTEXT;
    return Tokenizer::State::Data; // noscript with scripting off parses normally
}

void TreeBuilder::run(Tokenizer& tokenizer)
{
    m_tokenizer = &tokenizer;
    while (!m_done) {
        Element* adjusted = adjusted_current_node();
        tokenizer.set_in_foreign_content(adjusted && adjusted->namespace_uri() != dom::ns::html);
        std::optional<Token> token = tokenizer.next_token();
        if (!token)
            break;
        process(*token);
    }
}

void TreeBuilder::process(Token& token)
{
    if (m_ignore_next_linefeed) {
        m_ignore_next_linefeed = false;
        if (token.is_character() && token.code_point == U'\n')
            return;
    }
    while (dispatch(token)) { }
}

bool TreeBuilder::use_foreign_rules(Token const& token) const
{
    Element* adjusted = adjusted_current_node();
    if (!adjusted || adjusted->namespace_uri() == dom::ns::html)
        return false;
    if (token.is_eof())
        return false;
    if (is_mathml_text_integration_point(*adjusted)) {
        if (token.is_start_tag() && token.tag_name != "mglyph" && token.tag_name != "malignmark")
            return false;
        if (token.is_character())
            return false;
    }
    if (adjusted->is_mathml("annotation-xml") && token.is_start_tag() && token.tag_name == "svg")
        return false;
    if (is_html_integration_point(*adjusted) && (token.is_start_tag() || token.is_character()))
        return false;
    return true;
}

bool TreeBuilder::dispatch(Token& token)
{
    if (use_foreign_rules(token))
        return process_foreign(token);
    return process_mode(m_mode, token);
}

bool TreeBuilder::process_mode(Mode mode, Token& token)
{
    switch (mode) {
    case Mode::Initial: return mode_initial(token);
    case Mode::BeforeHtml: return mode_before_html(token);
    case Mode::BeforeHead: return mode_before_head(token);
    case Mode::InHead: return mode_in_head(token);
    case Mode::InHeadNoscript: return mode_in_head_noscript(token);
    case Mode::AfterHead: return mode_after_head(token);
    case Mode::InBody: return mode_in_body(token);
    case Mode::Text: return mode_text(token);
    case Mode::InTable: return mode_in_table(token);
    case Mode::InTableText: return mode_in_table_text(token);
    case Mode::InCaption: return mode_in_caption(token);
    case Mode::InColumnGroup: return mode_in_column_group(token);
    case Mode::InTableBody: return mode_in_table_body(token);
    case Mode::InRow: return mode_in_row(token);
    case Mode::InCell: return mode_in_cell(token);
    case Mode::InTemplate: return mode_in_template(token);
    case Mode::AfterBody: return mode_after_body(token);
    case Mode::InFrameset: return mode_in_frameset(token);
    case Mode::AfterFrameset: return mode_after_frameset(token);
    case Mode::AfterAfterBody: return mode_after_after_body(token);
    case Mode::AfterAfterFrameset: return mode_after_after_frameset(token);
    }
    return false;
}

// --- Foreign content ----------------------------------------------------------

bool TreeBuilder::process_foreign(Token& token)
{
    switch (token.type) {
    case Token::Type::Character:
        if (token.code_point == U'\0')
            insert_character(replacement_character);
        else
            insert_character(token.code_point);
        if (token.code_point != U'\0' && !is_tokenizer_whitespace(token.code_point))
            m_frameset_ok = false;
        return false;
    case Token::Type::Comment:
        insert_comment(token);
        return false;
    case Token::Type::Doctype:
        return false; // parse error, ignored
    case Token::Type::EndOfFile:
        return false; // handled by the HTML dispatcher
    case Token::Type::StartTag: {
        bool breakout = one_of(token.tag_name, foreign_breakout);
        if (!breakout && token.tag_name == "font") {
            for (Attribute const& attribute : token.attributes) {
                if (attribute.name == "color" || attribute.name == "face" || attribute.name == "size") {
                    breakout = true;
                    break;
                }
            }
        }
        if (breakout) {
            while (true) {
                Element* node = current_node();
                if (!node || node->is_html() || is_mathml_text_integration_point(*node) || is_html_integration_point(*node))
                    break;
                pop();
            }
            // "Reprocess ... in HTML content" — bypass the dispatcher: in the
            // fragment case the adjusted current node is still the foreign
            // context element, and re-dispatching would loop forever.
            return process_mode(m_mode, token);
        }

        Element* adjusted = adjusted_current_node();
        std::string_view const target_namespace = adjusted ? std::string_view(adjusted->namespace_uri()) : dom::ns::html;
        if (target_namespace == dom::ns::svg)
            adjust_svg_tag_name(token.tag_name);
        Element* element = insert_foreign_element(token, target_namespace);
        if (token.self_closing) {
            (void)element;
            pop(); // (an svg <script/> would execute here; scripting is off)
        }
        return false;
    }
    case Token::Type::EndTag: {
        Element* node = current_node();
        if (token.tag_name == "br" || token.tag_name == "p") {
            // Parse error: these end tags break out of foreign content, then
            // go straight to the HTML rules (re-dispatching could bounce back
            // here forever from an integration point).
            while (Element* current = current_node()) {
                if (current->is_html() || is_mathml_text_integration_point(*current)
                    || is_html_integration_point(*current))
                    break;
                pop();
            }
            return process_mode(m_mode, token);
        }
        if (token.tag_name == "script" && node && node->is_svg("script")) {
            pop();
            return false;
        }
        if (!node)
            return false;
        std::size_t index = m_stack.size() - 1;
        while (true) {
            if (ascii_lowercase(node->local_name()) == token.tag_name) {
                pop_until_popped(node);
                return false;
            }
            if (index == 0)
                return false;
            --index;
            node = m_stack[index];
            if (node->namespace_uri() == dom::ns::html)
                return process_mode(m_mode, token);
        }
    }
    }
    return false;
}

// --- Simple leading modes -----------------------------------------------------

bool TreeBuilder::mode_initial(Token& token)
{
    if (token.is_character() && is_tokenizer_whitespace(token.code_point))
        return false;
    if (token.type == Token::Type::Comment) {
        insert_comment(token, &m_document);
        return false;
    }
    if (token.type == Token::Type::Doctype) {
        DocumentType* doctype = m_document.create<DocumentType>();
        doctype->name = token.doctype_name.value_or("");
        doctype->public_identifier = token.public_identifier.value_or("");
        doctype->system_identifier = token.system_identifier.value_or("");
        m_document.append_child(*doctype);
        m_document.quirks_mode = quirks_mode_for_doctype(token);
        m_mode = Mode::BeforeHtml;
        return false;
    }
    m_document.quirks_mode = QuirksMode::Yes;
    m_mode = Mode::BeforeHtml;
    return true;
}

bool TreeBuilder::mode_before_html(Token& token)
{
    if (token.type == Token::Type::Doctype)
        return false;
    if (token.type == Token::Type::Comment) {
        insert_comment(token, &m_document);
        return false;
    }
    if (token.is_character() && is_tokenizer_whitespace(token.code_point))
        return false;
    if (token.is_start_tag() && token.tag_name == "html") {
        Element* element = create_element_for_token(token, dom::ns::html);
        m_document.append_child(*element);
        m_stack.push_back(element);
        m_html_element = element;
        m_mode = Mode::BeforeHead;
        return false;
    }
    if (token.is_end_tag()
        && !one_of(token.tag_name, std::array<std::string_view, 4> { "head", "body", "html", "br" }))
        return false;

    Token html_token;
    html_token.type = Token::Type::StartTag;
    html_token.tag_name = "html";
    Element* element = create_element_for_token(html_token, dom::ns::html);
    m_document.append_child(*element);
    m_stack.push_back(element);
    m_html_element = element;
    m_mode = Mode::BeforeHead;
    return true;
}

bool TreeBuilder::mode_before_head(Token& token)
{
    if (token.is_character() && is_tokenizer_whitespace(token.code_point))
        return false;
    if (token.type == Token::Type::Comment) {
        insert_comment(token);
        return false;
    }
    if (token.type == Token::Type::Doctype)
        return false;
    if (token.is_start_tag() && token.tag_name == "html")
        return mode_in_body(token);
    if (token.is_start_tag() && token.tag_name == "head") {
        m_head_element = insert_html_element(token);
        m_mode = Mode::InHead;
        return false;
    }
    if (token.is_end_tag()
        && !one_of(token.tag_name, std::array<std::string_view, 4> { "head", "body", "html", "br" }))
        return false;

    Token head_token;
    head_token.type = Token::Type::StartTag;
    head_token.tag_name = "head";
    m_head_element = insert_html_element(head_token);
    m_mode = Mode::InHead;
    return true;
}

bool TreeBuilder::mode_in_head(Token& token)
{
    if (token.is_character() && is_tokenizer_whitespace(token.code_point)) {
        insert_character(token.code_point);
        return false;
    }
    if (token.type == Token::Type::Comment) {
        insert_comment(token);
        return false;
    }
    if (token.type == Token::Type::Doctype)
        return false;
    if (token.is_start_tag()) {
        std::string_view const name = token.tag_name;
        if (name == "html")
            return mode_in_body(token);
        if (one_of(name, std::array<std::string_view, 5> { "base", "basefont", "bgsound", "link", "meta" })) {
            insert_html_element(token);
            pop();
            return false;
        }
        if (name == "title") {
            parse_generic_text(token, Tokenizer::State::RCDATA);
            return false;
        }
        if (name == "noscript") { // scripting is off
            insert_html_element(token);
            m_mode = Mode::InHeadNoscript;
            return false;
        }
        if (name == "noframes" || name == "style") {
            parse_generic_text(token, Tokenizer::State::RAWTEXT);
            return false;
        }
        if (name == "script") {
            insert_html_element(token);
            m_tokenizer->set_state(Tokenizer::State::ScriptData);
            m_original_mode = m_mode;
            m_mode = Mode::Text;
            return false;
        }
        if (name == "template") {
            insert_html_element(token);
            push_formatting_marker();
            m_frameset_ok = false;
            m_mode = Mode::InTemplate;
            m_template_modes.push_back(Mode::InTemplate);
            return false;
        }
        if (name == "head")
            return false; // parse error
    }
    if (token.is_end_tag()) {
        std::string_view const name = token.tag_name;
        if (name == "head") {
            pop();
            m_mode = Mode::AfterHead;
            return false;
        }
        if (name == "template") {
            if (!stack_has_template())
                return false; // parse error
            generate_implied_end_tags_thoroughly();
            pop_until_html_element_popped("template");
            clear_formatting_to_marker();
            if (!m_template_modes.empty())
                m_template_modes.pop_back();
            reset_insertion_mode();
            return false;
        }
        if (!(name == "body" || name == "html" || name == "br"))
            return false; // parse error
    }
    pop();
    m_mode = Mode::AfterHead;
    return true;
}

bool TreeBuilder::mode_in_head_noscript(Token& token)
{
    if (token.type == Token::Type::Doctype)
        return false;
    if (token.is_start_tag() && token.tag_name == "html")
        return mode_in_body(token);
    if (token.is_end_tag() && token.tag_name == "noscript") {
        pop();
        m_mode = Mode::InHead;
        return false;
    }
    bool const in_head_delegate = (token.is_character() && is_tokenizer_whitespace(token.code_point))
        || token.type == Token::Type::Comment
        || (token.is_start_tag()
            && one_of(token.tag_name,
                std::array<std::string_view, 6> { "basefont", "bgsound", "link", "meta", "noframes", "style" }));
    if (in_head_delegate)
        return mode_in_head(token);
    if ((token.is_start_tag() && (token.tag_name == "head" || token.tag_name == "noscript"))
        || (token.is_end_tag() && token.tag_name != "br"))
        return false; // parse error
    pop();
    m_mode = Mode::InHead;
    return true;
}

bool TreeBuilder::mode_after_head(Token& token)
{
    if (token.is_character() && is_tokenizer_whitespace(token.code_point)) {
        insert_character(token.code_point);
        return false;
    }
    if (token.type == Token::Type::Comment) {
        insert_comment(token);
        return false;
    }
    if (token.type == Token::Type::Doctype)
        return false;
    if (token.is_start_tag()) {
        std::string_view const name = token.tag_name;
        if (name == "html")
            return mode_in_body(token);
        if (name == "body") {
            insert_html_element(token);
            m_frameset_ok = false;
            m_mode = Mode::InBody;
            return false;
        }
        if (name == "frameset") {
            insert_html_element(token);
            m_mode = Mode::InFrameset;
            return false;
        }
        if (one_of(name,
                std::array<std::string_view, 10> { "base", "basefont", "bgsound", "link", "meta", "noframes", "script", "style", "template", "title" })) {
            // parse error: re-open head briefly
            m_stack.push_back(m_head_element);
            bool const again = mode_in_head(token);
            remove_from_stack(m_head_element);
            return again;
        }
        if (name == "head")
            return false; // parse error
    }
    if (token.is_end_tag()) {
        if (token.tag_name == "template")
            return mode_in_head(token);
        if (!(token.tag_name == "body" || token.tag_name == "html" || token.tag_name == "br"))
            return false; // parse error
    }
    Token body_token;
    body_token.type = Token::Type::StartTag;
    body_token.tag_name = "body";
    insert_html_element(body_token);
    m_mode = Mode::InBody;
    return true;
}

// --- InBody -------------------------------------------------------------------

bool TreeBuilder::mode_in_body(Token& token)
{
    switch (token.type) {
    case Token::Type::Character:
        if (token.code_point == U'\0')
            return false; // parse error
        reconstruct_formatting();
        insert_character(token.code_point);
        if (!is_tokenizer_whitespace(token.code_point))
            m_frameset_ok = false;
        return false;
    case Token::Type::Comment:
        insert_comment(token);
        return false;
    case Token::Type::Doctype:
        return false; // parse error
    case Token::Type::EndOfFile:
        if (!m_template_modes.empty())
            return mode_in_template(token);
        stop_parsing();
        return false;
    case Token::Type::StartTag:
        break;
    case Token::Type::EndTag:
        break;
    }

    std::string_view const name = token.tag_name;

    if (token.is_start_tag()) {
        if (name == "html") {
            if (stack_has_template())
                return false;
            merge_attributes_into(*m_stack.front(), token);
            return false;
        }
        if (one_of(name,
                std::array<std::string_view, 10> { "base", "basefont", "bgsound", "link", "meta", "noframes", "script", "style", "template", "title" }))
            return mode_in_head(token);
        if (name == "body") {
            if (m_stack.size() < 2 || !m_stack[1]->is_html("body") || stack_has_template())
                return false; // parse error / fragment
            m_frameset_ok = false;
            merge_attributes_into(*m_stack[1], token);
            return false;
        }
        if (name == "frameset") {
            if (m_stack.size() < 2 || !m_stack[1]->is_html("body"))
                return false;
            if (!m_frameset_ok)
                return false;
            m_stack[1]->remove();
            while (m_stack.size() > 1)
                pop();
            insert_html_element(token);
            m_mode = Mode::InFrameset;
            return false;
        }
        if (one_of(name, close_p_blocks)) {
            if (has_in_button_scope("p"))
                close_p_element();
            insert_html_element(token);
            return false;
        }
        if (one_of(name, heading_tags)) {
            if (has_in_button_scope("p"))
                close_p_element();
            if (Element* node = current_node(); node && node->is_html() && one_of(node->local_name(), heading_tags))
                pop(); // parse error
            insert_html_element(token);
            return false;
        }
        if (name == "pre" || name == "listing") {
            if (has_in_button_scope("p"))
                close_p_element();
            insert_html_element(token);
            m_ignore_next_linefeed = true;
            m_frameset_ok = false;
            return false;
        }
        if (name == "form") {
            if (m_form_element && !stack_has_template())
                return false; // parse error
            if (has_in_button_scope("p"))
                close_p_element();
            Element* element = insert_html_element(token);
            if (!stack_has_template())
                m_form_element = element;
            return false;
        }
        if (name == "li" || name == "dd" || name == "dt") {
            m_frameset_ok = false;
            bool const is_li = name == "li";
            for (std::size_t i = m_stack.size(); i-- > 0;) {
                Element* node = m_stack[i];
                bool const matches = is_li ? node->is_html("li")
                                           : (node->is_html("dd") || node->is_html("dt"));
                if (matches) {
                    generate_implied_end_tags(node->local_name());
                    pop_until_popped(node);
                    break;
                }
                if (is_special(*node) && !node->is_html("address") && !node->is_html("div") && !node->is_html("p"))
                    break;
            }
            if (has_in_button_scope("p"))
                close_p_element();
            insert_html_element(token);
            return false;
        }
        if (name == "plaintext") {
            if (has_in_button_scope("p"))
                close_p_element();
            insert_html_element(token);
            m_tokenizer->set_state(Tokenizer::State::PLAINTEXT);
            return false;
        }
        if (name == "button") {
            if (has_in_scope("button")) {
                generate_implied_end_tags();
                pop_until_html_element_popped("button");
            }
            reconstruct_formatting();
            insert_html_element(token);
            m_frameset_ok = false;
            return false;
        }
        if (name == "a") {
            if (int index = formatting_index_after_marker("a"); index >= 0) {
                Element* existing = m_formatting[static_cast<std::size_t>(index)].element;
                adoption_agency(token);
                for (std::size_t i = 0; i < m_formatting.size(); ++i) {
                    if (m_formatting[i].element == existing) {
                        m_formatting.erase(m_formatting.begin() + static_cast<std::ptrdiff_t>(i));
                        break;
                    }
                }
                remove_from_stack(existing);
            }
            reconstruct_formatting();
            push_formatting(insert_html_element(token), token);
            return false;
        }
        if (one_of(name,
                std::array<std::string_view, 12> { "b", "big", "code", "em", "font", "i", "s", "small", "strike", "strong", "tt", "u" })) {
            reconstruct_formatting();
            push_formatting(insert_html_element(token), token);
            return false;
        }
        if (name == "nobr") {
            reconstruct_formatting();
            if (has_in_scope("nobr")) {
                adoption_agency(token);
                reconstruct_formatting();
            }
            push_formatting(insert_html_element(token), token);
            return false;
        }
        if (name == "applet" || name == "marquee" || name == "object") {
            reconstruct_formatting();
            insert_html_element(token);
            push_formatting_marker();
            m_frameset_ok = false;
            return false;
        }
        if (name == "table") {
            if (m_document.quirks_mode != QuirksMode::Yes && has_in_button_scope("p"))
                close_p_element();
            insert_html_element(token);
            m_frameset_ok = false;
            m_mode = Mode::InTable;
            return false;
        }
        if (one_of(name, std::array<std::string_view, 6> { "area", "br", "embed", "img", "keygen", "wbr" })) {
            reconstruct_formatting();
            insert_html_element(token);
            pop();
            m_frameset_ok = false;
            return false;
        }
        if (name == "input") {
            if (m_context && m_context->is_html("select")) // fragment case
                return false; // parse error, ignored
            if (has_in_scope("select")) // parse error: input escapes the select
                pop_until_html_element_popped("select");
            reconstruct_formatting();
            insert_html_element(token);
            pop();
            Attribute const* type = nullptr;
            for (Attribute const& attribute : token.attributes) {
                if (attribute.name == "type")
                    type = &attribute;
            }
            if (!type || !ascii_ci_equals(type->value, "hidden"))
                m_frameset_ok = false;
            return false;
        }
        if (name == "param" || name == "source" || name == "track") {
            insert_html_element(token);
            pop();
            return false;
        }
        if (name == "hr") {
            if (has_in_button_scope("p"))
                close_p_element();
            if (has_in_scope("select"))
                generate_implied_end_tags();
            insert_html_element(token);
            pop();
            m_frameset_ok = false;
            return false;
        }
        if (name == "image") { // parse error, famously
            token.tag_name = "img";
            return true;
        }
        if (name == "textarea") {
            insert_html_element(token);
            m_ignore_next_linefeed = true;
            m_tokenizer->set_state(Tokenizer::State::RCDATA);
            m_original_mode = m_mode;
            m_frameset_ok = false;
            m_mode = Mode::Text;
            return false;
        }
        if (name == "xmp") {
            if (has_in_button_scope("p"))
                close_p_element();
            reconstruct_formatting();
            m_frameset_ok = false;
            parse_generic_text(token, Tokenizer::State::RAWTEXT);
            return false;
        }
        if (name == "iframe") {
            m_frameset_ok = false;
            parse_generic_text(token, Tokenizer::State::RAWTEXT);
            return false;
        }
        if (name == "noembed") {
            parse_generic_text(token, Tokenizer::State::RAWTEXT);
            return false;
        }
        if (name == "noscript") { // scripting off: ordinary element
            reconstruct_formatting();
            insert_html_element(token);
            return false;
        }
        if (name == "select") {
            if (m_context && m_context->is_html("select")) // fragment case
                return false; // parse error, ignored
            if (has_in_scope("select")) { // parse error: acts as </select>
                pop_until_html_element_popped("select");
                return false;
            }
            reconstruct_formatting();
            insert_html_element(token);
            m_frameset_ok = false;
            return false;
        }
        if (name == "option") {
            if (has_in_scope("select")) {
                generate_implied_end_tags("optgroup");
            } else if (Element* node = current_node(); node && node->is_html("option")) {
                pop();
            }
            reconstruct_formatting();
            insert_html_element(token);
            return false;
        }
        if (name == "optgroup") {
            if (has_in_scope("select")) {
                generate_implied_end_tags();
            } else if (Element* node = current_node(); node && node->is_html("option")) {
                pop();
            }
            reconstruct_formatting();
            insert_html_element(token);
            return false;
        }
        if (name == "rb" || name == "rtc") {
            if (has_in_scope("ruby"))
                generate_implied_end_tags();
            insert_html_element(token);
            return false;
        }
        if (name == "rp" || name == "rt") {
            if (has_in_scope("ruby"))
                generate_implied_end_tags("rtc");
            insert_html_element(token);
            return false;
        }
        if (name == "math") {
            reconstruct_formatting();
            insert_foreign_element(token, dom::ns::mathml);
            if (token.self_closing)
                pop();
            return false;
        }
        if (name == "svg") {
            reconstruct_formatting();
            insert_foreign_element(token, dom::ns::svg);
            if (token.self_closing)
                pop();
            return false;
        }
        if (one_of(name,
                std::array<std::string_view, 11> { "caption", "col", "colgroup", "frame", "head", "tbody", "td", "tfoot", "th", "thead", "tr" }))
            return false; // parse error, ignored
        reconstruct_formatting();
        insert_html_element(token);
        return false;
    }

    // End tags.
    if (name == "template")
        return mode_in_head(token);
    if (name == "body" || name == "html") {
        if (!has_in_scope("body"))
            return false; // parse error
        m_mode = Mode::AfterBody;
        return name == "html";
    }
    if (one_of(name, end_tag_blocks)) {
        if (!has_in_scope(name))
            return false; // parse error
        generate_implied_end_tags();
        pop_until_html_element_popped(name);
        return false;
    }
    if (name == "form") {
        if (!stack_has_template()) {
            Element* node = m_form_element;
            m_form_element = nullptr;
            if (!node || !has_in_scope(node))
                return false; // parse error
            generate_implied_end_tags();
            remove_from_stack(node);
            return false;
        }
        if (!has_in_scope("form"))
            return false;
        generate_implied_end_tags();
        pop_until_html_element_popped("form");
        return false;
    }
    if (name == "p") {
        if (!has_in_button_scope("p")) {
            Token p_token;
            p_token.type = Token::Type::StartTag;
            p_token.tag_name = "p";
            insert_html_element(p_token);
        }
        close_p_element();
        return false;
    }
    if (name == "li") {
        if (!has_in_list_item_scope("li"))
            return false;
        generate_implied_end_tags("li");
        pop_until_html_element_popped("li");
        return false;
    }
    if (name == "dd" || name == "dt") {
        if (!has_in_scope(name))
            return false;
        generate_implied_end_tags(name);
        pop_until_html_element_popped(name);
        return false;
    }
    if (one_of(name, heading_tags)) {
        if (!has_heading_in_scope())
            return false;
        generate_implied_end_tags();
        while (!m_stack.empty()) {
            Element* node = current_node();
            pop();
            if (node->is_html() && one_of(node->local_name(), heading_tags))
                break;
        }
        return false;
    }
    if (one_of(name, formatting_tags)) {
        adoption_agency(token);
        return false;
    }
    if (name == "applet" || name == "marquee" || name == "object") {
        if (!has_in_scope(name))
            return false;
        generate_implied_end_tags();
        pop_until_html_element_popped(name);
        clear_formatting_to_marker();
        return false;
    }
    if (name == "br") { // parse error: acts as <br>
        Token br_token;
        br_token.type = Token::Type::StartTag;
        br_token.tag_name = "br";
        reconstruct_formatting();
        insert_html_element(br_token);
        pop();
        m_frameset_ok = false;
        return false;
    }
    any_other_end_tag_in_body(token);
    return false;
}

bool TreeBuilder::any_other_end_tag_in_body(Token& token)
{
    for (std::size_t i = m_stack.size(); i-- > 0;) {
        Element* node = m_stack[i];
        if (node->is_html(token.tag_name)) {
            generate_implied_end_tags(token.tag_name);
            pop_until_popped(node);
            return false;
        }
        if (is_special(*node))
            return false; // parse error, ignored
    }
    return false;
}

bool TreeBuilder::mode_text(Token& token)
{
    if (token.is_character()) {
        insert_character(token.code_point);
        return false;
    }
    if (token.is_eof()) {
        pop();
        m_mode = m_original_mode;
        return true;
    }
    pop(); // any end tag (script would execute here; scripting is off)
    m_mode = m_original_mode;
    return false;
}

// --- Tables -------------------------------------------------------------------

bool TreeBuilder::mode_in_table(Token& token)
{
    Element* current = current_node();
    bool const table_context = current
        && (current->is_html("table") || current->is_html("tbody") || current->is_html("tfoot")
            || current->is_html("thead") || current->is_html("tr"));

    if (token.is_character() && table_context) {
        m_pending_table_characters.clear();
        m_original_mode = m_mode;
        m_mode = Mode::InTableText;
        return true;
    }
    if (token.type == Token::Type::Comment) {
        insert_comment(token);
        return false;
    }
    if (token.type == Token::Type::Doctype)
        return false;
    if (token.is_start_tag()) {
        std::string_view const name = token.tag_name;
        if (name == "caption") {
            clear_stack_to_table_context();
            push_formatting_marker();
            insert_html_element(token);
            m_mode = Mode::InCaption;
            return false;
        }
        if (name == "colgroup") {
            clear_stack_to_table_context();
            insert_html_element(token);
            m_mode = Mode::InColumnGroup;
            return false;
        }
        if (name == "col") {
            clear_stack_to_table_context();
            Token colgroup;
            colgroup.type = Token::Type::StartTag;
            colgroup.tag_name = "colgroup";
            insert_html_element(colgroup);
            m_mode = Mode::InColumnGroup;
            return true;
        }
        if (name == "tbody" || name == "tfoot" || name == "thead") {
            clear_stack_to_table_context();
            insert_html_element(token);
            m_mode = Mode::InTableBody;
            return false;
        }
        if (name == "td" || name == "th" || name == "tr") {
            clear_stack_to_table_context();
            Token tbody;
            tbody.type = Token::Type::StartTag;
            tbody.tag_name = "tbody";
            insert_html_element(tbody);
            m_mode = Mode::InTableBody;
            return true;
        }
        if (name == "table") { // parse error
            if (!has_in_table_scope("table"))
                return false;
            pop_until_html_element_popped("table");
            reset_insertion_mode();
            return true;
        }
        if (name == "style" || name == "script" || name == "template")
            return mode_in_head(token);
        if (name == "input") {
            Attribute const* type = nullptr;
            for (Attribute const& attribute : token.attributes) {
                if (attribute.name == "type")
                    type = &attribute;
            }
            if (type && ascii_ci_equals(type->value, "hidden")) {
                insert_html_element(token);
                pop();
                return false;
            }
        }
        if (name == "form") { // parse error
            if (stack_has_template() || m_form_element)
                return false;
            m_form_element = insert_html_element(token);
            pop();
            return false;
        }
    }
    if (token.is_end_tag()) {
        std::string_view const name = token.tag_name;
        if (name == "table") {
            if (!has_in_table_scope("table"))
                return false;
            pop_until_html_element_popped("table");
            reset_insertion_mode();
            return false;
        }
        if (one_of(name,
                std::array<std::string_view, 9> { "body", "caption", "col", "colgroup", "html", "tbody", "td", "tfoot", "th" })
            || name == "thead" || name == "tr")
            return false; // parse error
        if (name == "template")
            return mode_in_head(token);
    }
    if (token.is_eof())
        return mode_in_body(token);

    // Anything else: parse error, process per InBody with foster parenting.
    m_foster_parenting = true;
    bool const again = mode_in_body(token);
    m_foster_parenting = false;
    return again;
}

bool TreeBuilder::mode_in_table_text(Token& token)
{
    if (token.is_character()) {
        if (token.code_point == U'\0')
            return false; // parse error
        m_pending_table_characters.push_back(token.code_point);
        return false;
    }

    bool all_whitespace = true;
    for (char32_t const c : m_pending_table_characters) {
        if (!is_tokenizer_whitespace(c)) {
            all_whitespace = false;
            break;
        }
    }
    if (all_whitespace) {
        for (char32_t const c : m_pending_table_characters)
            insert_character(c);
    } else {
        m_foster_parenting = true;
        for (char32_t const c : m_pending_table_characters) {
            reconstruct_formatting();
            insert_character(c);
            if (!is_tokenizer_whitespace(c))
                m_frameset_ok = false;
        }
        m_foster_parenting = false;
    }
    m_pending_table_characters.clear();
    m_mode = m_original_mode;
    return true;
}

bool TreeBuilder::mode_in_caption(Token& token)
{
    auto const close_caption = [&]() -> bool {
        if (!has_in_table_scope("caption"))
            return false;
        generate_implied_end_tags();
        pop_until_html_element_popped("caption");
        clear_formatting_to_marker();
        m_mode = Mode::InTable;
        return true;
    };

    if (token.is_end_tag() && token.tag_name == "caption") {
        close_caption();
        return false;
    }
    if ((token.is_start_tag()
            && one_of(token.tag_name,
                std::array<std::string_view, 9> { "caption", "col", "colgroup", "tbody", "td", "tfoot", "th", "thead", "tr" }))
        || (token.is_end_tag() && token.tag_name == "table")) {
        if (!close_caption())
            return false;
        return true;
    }
    if (token.is_end_tag()
        && one_of(token.tag_name,
            std::array<std::string_view, 9> { "body", "col", "colgroup", "html", "tbody", "td", "tfoot", "th", "thead" }))
        return false;
    if (token.is_end_tag() && token.tag_name == "tr")
        return false;
    return mode_in_body(token);
}

bool TreeBuilder::mode_in_column_group(Token& token)
{
    if (token.is_character() && is_tokenizer_whitespace(token.code_point)) {
        insert_character(token.code_point);
        return false;
    }
    if (token.type == Token::Type::Comment) {
        insert_comment(token);
        return false;
    }
    if (token.type == Token::Type::Doctype)
        return false;
    if (token.is_start_tag()) {
        if (token.tag_name == "html")
            return mode_in_body(token);
        if (token.tag_name == "col") {
            insert_html_element(token);
            pop();
            return false;
        }
        if (token.tag_name == "template")
            return mode_in_head(token);
    }
    if (token.is_end_tag()) {
        if (token.tag_name == "colgroup") {
            Element* current = current_node();
            if (!current || !current->is_html("colgroup"))
                return false; // parse error
            pop();
            m_mode = Mode::InTable;
            return false;
        }
        if (token.tag_name == "col")
            return false; // parse error
        if (token.tag_name == "template")
            return mode_in_head(token);
    }
    if (token.is_eof())
        return mode_in_body(token);

    Element* current = current_node();
    if (!current || !current->is_html("colgroup"))
        return false; // parse error
    pop();
    m_mode = Mode::InTable;
    return true;
}

bool TreeBuilder::mode_in_table_body(Token& token)
{
    if (token.is_start_tag() && token.tag_name == "tr") {
        clear_stack_to_table_body_context();
        insert_html_element(token);
        m_mode = Mode::InRow;
        return false;
    }
    if (token.is_start_tag() && (token.tag_name == "th" || token.tag_name == "td")) {
        clear_stack_to_table_body_context();
        Token tr_token;
        tr_token.type = Token::Type::StartTag;
        tr_token.tag_name = "tr";
        insert_html_element(tr_token);
        m_mode = Mode::InRow;
        return true;
    }
    if (token.is_end_tag()
        && one_of(token.tag_name, std::array<std::string_view, 3> { "tbody", "tfoot", "thead" })) {
        if (!has_in_table_scope(token.tag_name))
            return false;
        clear_stack_to_table_body_context();
        pop();
        m_mode = Mode::InTable;
        return false;
    }
    if ((token.is_start_tag()
            && one_of(token.tag_name,
                std::array<std::string_view, 5> { "caption", "col", "colgroup", "tbody", "tfoot" }))
        || (token.is_start_tag() && token.tag_name == "thead")
        || (token.is_end_tag() && token.tag_name == "table")) {
        if (!has_in_table_scope("tbody") && !has_in_table_scope("thead") && !has_in_table_scope("tfoot"))
            return false;
        clear_stack_to_table_body_context();
        pop();
        m_mode = Mode::InTable;
        return true;
    }
    if (token.is_end_tag()
        && one_of(token.tag_name,
            std::array<std::string_view, 7> { "body", "caption", "col", "colgroup", "html", "td", "th" }))
        return false;
    if (token.is_end_tag() && token.tag_name == "tr" && !has_in_table_scope("tr"))
        return false;
    return mode_in_table(token);
}

bool TreeBuilder::mode_in_row(Token& token)
{
    if (token.is_start_tag() && (token.tag_name == "th" || token.tag_name == "td")) {
        clear_stack_to_table_row_context();
        insert_html_element(token);
        m_mode = Mode::InCell;
        push_formatting_marker();
        return false;
    }
    if (token.is_end_tag() && token.tag_name == "tr") {
        if (!has_in_table_scope("tr"))
            return false;
        clear_stack_to_table_row_context();
        pop();
        m_mode = Mode::InTableBody;
        return false;
    }
    if ((token.is_start_tag()
            && one_of(token.tag_name,
                std::array<std::string_view, 7> { "caption", "col", "colgroup", "tbody", "tfoot", "thead", "tr" }))
        || (token.is_end_tag() && token.tag_name == "table")) {
        if (!has_in_table_scope("tr"))
            return false;
        clear_stack_to_table_row_context();
        pop();
        m_mode = Mode::InTableBody;
        return true;
    }
    if (token.is_end_tag()
        && one_of(token.tag_name, std::array<std::string_view, 3> { "tbody", "tfoot", "thead" })) {
        if (!has_in_table_scope(token.tag_name))
            return false;
        if (!has_in_table_scope("tr"))
            return false;
        clear_stack_to_table_row_context();
        pop();
        m_mode = Mode::InTableBody;
        return true;
    }
    if (token.is_end_tag()
        && one_of(token.tag_name,
            std::array<std::string_view, 6> { "body", "caption", "col", "colgroup", "html", "td" }))
        return false;
    if (token.is_end_tag() && token.tag_name == "th")
        return false;
    return mode_in_table(token);
}

bool TreeBuilder::mode_in_cell(Token& token)
{
    if (token.is_end_tag() && (token.tag_name == "td" || token.tag_name == "th")) {
        if (!has_in_table_scope(token.tag_name))
            return false;
        generate_implied_end_tags();
        pop_until_html_element_popped(token.tag_name);
        clear_formatting_to_marker();
        m_mode = Mode::InRow;
        return false;
    }
    if (token.is_start_tag()
        && one_of(token.tag_name,
            std::array<std::string_view, 9> { "caption", "col", "colgroup", "tbody", "td", "tfoot", "th", "thead", "tr" })) {
        if (!has_in_table_scope("td") && !has_in_table_scope("th"))
            return false;
        close_cell();
        return true;
    }
    if (token.is_end_tag()
        && one_of(token.tag_name,
            std::array<std::string_view, 5> { "body", "caption", "col", "colgroup", "html" }))
        return false;
    if (token.is_end_tag()
        && one_of(token.tag_name,
            std::array<std::string_view, 5> { "table", "tbody", "tfoot", "thead", "tr" })) {
        if (!has_in_table_scope(token.tag_name))
            return false;
        close_cell();
        return true;
    }
    return mode_in_body(token);
}

void TreeBuilder::close_cell()
{
    generate_implied_end_tags();
    while (!m_stack.empty()) {
        Element* node = current_node();
        pop();
        if (node->is_html("td") || node->is_html("th"))
            break;
    }
    clear_formatting_to_marker();
    m_mode = Mode::InRow;
}

// --- Template -----------------------------------------------------------------

bool TreeBuilder::mode_in_template(Token& token)
{
    if (token.is_character() || token.type == Token::Type::Comment || token.type == Token::Type::Doctype)
        return mode_in_body(token);
    if (token.is_start_tag()) {
        std::string_view const name = token.tag_name;
        if (one_of(name,
                std::array<std::string_view, 10> { "base", "basefont", "bgsound", "link", "meta", "noframes", "script", "style", "template", "title" }))
            return mode_in_head(token);
        if (one_of(name, std::array<std::string_view, 5> { "caption", "colgroup", "tbody", "tfoot", "thead" })) {
            m_template_modes.back() = Mode::InTable;
            m_mode = Mode::InTable;
            return true;
        }
        if (name == "col") {
            m_template_modes.back() = Mode::InColumnGroup;
            m_mode = Mode::InColumnGroup;
            return true;
        }
        if (name == "tr") {
            m_template_modes.back() = Mode::InTableBody;
            m_mode = Mode::InTableBody;
            return true;
        }
        if (name == "td" || name == "th") {
            m_template_modes.back() = Mode::InRow;
            m_mode = Mode::InRow;
            return true;
        }
        m_template_modes.back() = Mode::InBody;
        m_mode = Mode::InBody;
        return true;
    }
    if (token.is_end_tag()) {
        if (token.tag_name == "template")
            return mode_in_head(token);
        return false; // parse error
    }
    // EOF
    if (!stack_has_template()) {
        stop_parsing();
        return false;
    }
    pop_until_html_element_popped("template");
    clear_formatting_to_marker();
    if (!m_template_modes.empty())
        m_template_modes.pop_back();
    reset_insertion_mode();
    return true;
}

// --- Trailing modes -----------------------------------------------------------

bool TreeBuilder::mode_after_body(Token& token)
{
    if (token.is_character() && is_tokenizer_whitespace(token.code_point))
        return mode_in_body(token);
    if (token.type == Token::Type::Comment) {
        insert_comment(token, m_stack.front());
        return false;
    }
    if (token.type == Token::Type::Doctype)
        return false;
    if (token.is_start_tag() && token.tag_name == "html")
        return mode_in_body(token);
    if (token.is_end_tag() && token.tag_name == "html") {
        if (m_context)
            return false; // fragment: parse error
        m_mode = Mode::AfterAfterBody;
        return false;
    }
    if (token.is_eof()) {
        stop_parsing();
        return false;
    }
    m_mode = Mode::InBody;
    return true;
}

bool TreeBuilder::mode_in_frameset(Token& token)
{
    if (token.is_character() && is_tokenizer_whitespace(token.code_point)) {
        insert_character(token.code_point);
        return false;
    }
    if (token.type == Token::Type::Comment) {
        insert_comment(token);
        return false;
    }
    if (token.type == Token::Type::Doctype)
        return false;
    if (token.is_start_tag()) {
        if (token.tag_name == "html")
            return mode_in_body(token);
        if (token.tag_name == "frameset") {
            insert_html_element(token);
            return false;
        }
        if (token.tag_name == "frame") {
            insert_html_element(token);
            pop();
            return false;
        }
        if (token.tag_name == "noframes")
            return mode_in_head(token);
    }
    if (token.is_end_tag() && token.tag_name == "frameset") {
        if (Element* current = current_node(); current && current->is_html("html"))
            return false; // fragment: parse error
        pop();
        if (!m_context) {
            if (Element* current = current_node(); current && !current->is_html("frameset"))
                m_mode = Mode::AfterFrameset;
        }
        return false;
    }
    if (token.is_eof()) {
        stop_parsing();
        return false;
    }
    return false; // parse error
}

bool TreeBuilder::mode_after_frameset(Token& token)
{
    if (token.is_character() && is_tokenizer_whitespace(token.code_point)) {
        insert_character(token.code_point);
        return false;
    }
    if (token.type == Token::Type::Comment) {
        insert_comment(token);
        return false;
    }
    if (token.type == Token::Type::Doctype)
        return false;
    if (token.is_start_tag() && token.tag_name == "html")
        return mode_in_body(token);
    if (token.is_end_tag() && token.tag_name == "html") {
        m_mode = Mode::AfterAfterFrameset;
        return false;
    }
    if (token.is_start_tag() && token.tag_name == "noframes")
        return mode_in_head(token);
    if (token.is_eof()) {
        stop_parsing();
        return false;
    }
    return false;
}

bool TreeBuilder::mode_after_after_body(Token& token)
{
    if (token.type == Token::Type::Comment) {
        insert_comment(token, &m_document);
        return false;
    }
    if (token.type == Token::Type::Doctype
        || (token.is_character() && is_tokenizer_whitespace(token.code_point))
        || (token.is_start_tag() && token.tag_name == "html"))
        return mode_in_body(token);
    if (token.is_eof()) {
        stop_parsing();
        return false;
    }
    m_mode = Mode::InBody;
    return true;
}

bool TreeBuilder::mode_after_after_frameset(Token& token)
{
    if (token.type == Token::Type::Comment) {
        insert_comment(token, &m_document);
        return false;
    }
    if (token.type == Token::Type::Doctype
        || (token.is_character() && is_tokenizer_whitespace(token.code_point))
        || (token.is_start_tag() && token.tag_name == "html"))
        return mode_in_body(token);
    if (token.is_start_tag() && token.tag_name == "noframes")
        return mode_in_head(token);
    if (token.is_eof()) {
        stop_parsing();
        return false;
    }
    return false;
}

// --- Stack and scopes ---------------------------------------------------------

Element* TreeBuilder::current_node() const
{
    return m_stack.empty() ? nullptr : m_stack.back();
}

Element* TreeBuilder::adjusted_current_node() const
{
    if (m_context && m_stack.size() == 1)
        return m_context;
    return current_node();
}

Element* TreeBuilder::node_above(Element* element) const
{
    for (std::size_t i = m_stack.size(); i-- > 0;) {
        if (m_stack[i] == element)
            return i == 0 ? nullptr : m_stack[i - 1];
    }
    return nullptr;
}

void TreeBuilder::pop()
{
    if (m_stack.empty())
        return;
    Element* node = m_stack.back();
    m_stack.pop_back();
    // "When an option element is popped off the stack of open elements of an
    // HTML parser ..., run maybe clone an option into selectedcontent."
    if (node->is_html("option"))
        maybe_clone_option_into_selectedcontent(*node);
}

void TreeBuilder::pop_until_html_element_popped(std::string_view name)
{
    while (!m_stack.empty()) {
        Element* node = m_stack.back();
        pop();
        if (node->is_html(name))
            return;
    }
}

void TreeBuilder::pop_until_popped(Element* element)
{
    while (!m_stack.empty()) {
        Element* node = m_stack.back();
        pop();
        if (node == element)
            return;
    }
}

void TreeBuilder::stop_parsing()
{
    while (!m_stack.empty())
        pop();
    m_done = true;
}

// The customizable-select machinery (the select element section of the spec),
// reduced to what the parser needs. Selectedness is computed on demand from
// the tree — equivalent, at pop time, to having run the select's ask-for-a-
// reset after every option insertion. The selectedcontent internal disabled
// flag (only settable through DOM mutations outside the parser) is omitted.
static Element* nearest_ancestor_select(Element& option)
{
    for (Node* node = option.parent(); node; node = node->parent()) {
        if (!node->is_element())
            return nullptr;
        Element* ancestor = static_cast<Element*>(node);
        if (ancestor->is_html("datalist") || ancestor->is_html("hr") || ancestor->is_html("option"))
            return nullptr;
        if (ancestor->is_html("optgroup")) {
            // A second optgroup between the option and the select breaks the
            // association; the walk from the option sees it further out.
            for (Node* outer = ancestor->parent(); outer; outer = outer->parent()) {
                if (!outer->is_element())
                    return nullptr;
                Element* outer_element = static_cast<Element*>(outer);
                if (outer_element->is_html("optgroup"))
                    return nullptr;
                if (outer_element->is_html("select"))
                    return outer_element;
                if (outer_element->is_html("datalist") || outer_element->is_html("hr")
                    || outer_element->is_html("option"))
                    return nullptr;
            }
            return nullptr;
        }
        if (ancestor->is_html("select"))
            return ancestor;
    }
    return nullptr;
}

static void collect_options_of(Element& select, Node& node, std::vector<Element*>& options)
{
    for (Node* child : node.children()) {
        if (child->is_element()) {
            Element* element = static_cast<Element*>(child);
            if (element->is_html("option") && nearest_ancestor_select(*element) == &select)
                options.push_back(element);
            if (element->is_html("select"))
                continue; // options inside a nested select are its own
        }
        collect_options_of(select, *child, options);
    }
}

static bool option_is_disabled(Element& option)
{
    if (option.has_attribute("disabled"))
        return true;
    for (Node* node = option.parent(); node && node->is_element(); node = node->parent()) {
        Element* ancestor = static_cast<Element*>(node);
        if (ancestor->is_html("select"))
            break;
        if (ancestor->is_html("optgroup") && ancestor->has_attribute("disabled"))
            return true;
    }
    return false;
}

static Element* first_selectedcontent_descendant(Node& node)
{
    for (Node* child : node.children()) {
        if (child->is_element() && static_cast<Element*>(child)->is_html("selectedcontent"))
            return static_cast<Element*>(child);
        if (Element* found = first_selectedcontent_descendant(*child))
            return found;
    }
    return nullptr;
}

void TreeBuilder::maybe_clone_option_into_selectedcontent(Element& option)
{
    Element* select = nearest_ancestor_select(option);
    if (!select || select->has_attribute("multiple"))
        return;

    // The option's selectedness: the last option with a selected attribute
    // wins; with none, a display-size-1 select defaults to the first enabled
    // option (ask for a reset).
    std::vector<Element*> options;
    collect_options_of(*select, *select, options);
    Element* selected = nullptr;
    for (Element* candidate : options) {
        if (candidate->has_attribute("selected"))
            selected = candidate;
    }
    if (!selected) {
        bool display_size_is_one = true;
        if (dom::Attr const* size = select->find_attribute("size")) {
            std::string_view value = size->value;
            if (!value.empty() && value != "1")
                display_size_is_one = false;
        }
        if (display_size_is_one) {
            for (Element* candidate : options) {
                if (!option_is_disabled(*candidate)) {
                    selected = candidate;
                    break;
                }
            }
        }
    }
    if (selected != &option)
        return;

    Element* selectedcontent = first_selectedcontent_descendant(*select);
    if (!selectedcontent)
        return;

    // Clone an option into a selectedcontent: replace all with clones of the
    // option's children.
    for (Node* child : std::vector<Node*>(selectedcontent->children()))
        child->remove();
    for (Node* child : std::vector<Node*>(option.children()))
        selectedcontent->append_child(*dom::clone_subtree(*child, m_document));
}

void TreeBuilder::remove_from_stack(Element* element)
{
    m_stack.erase(std::remove(m_stack.begin(), m_stack.end(), element), m_stack.end());
}

bool TreeBuilder::stack_contains(Element* element) const
{
    return std::find(m_stack.begin(), m_stack.end(), element) != m_stack.end();
}

bool TreeBuilder::stack_has_template() const
{
    for (Element* node : m_stack) {
        if (node->is_html("template"))
            return true;
    }
    return false;
}

void TreeBuilder::clear_stack_to_table_context()
{
    while (Element* node = current_node()) {
        if (node->is_html("table") || node->is_html("template") || node->is_html("html"))
            return;
        pop();
    }
}

void TreeBuilder::clear_stack_to_table_body_context()
{
    while (Element* node = current_node()) {
        if (node->is_html("tbody") || node->is_html("tfoot") || node->is_html("thead")
            || node->is_html("template") || node->is_html("html"))
            return;
        pop();
    }
}

void TreeBuilder::clear_stack_to_table_row_context()
{
    while (Element* node = current_node()) {
        if (node->is_html("tr") || node->is_html("template") || node->is_html("html"))
            return;
        pop();
    }
}

bool TreeBuilder::has_in_scope(std::string_view name) const
{
    for (std::size_t i = m_stack.size(); i-- > 0;) {
        Element* node = m_stack[i];
        if (node->is_html(name))
            return true;
        if (is_default_scope_terminator(*node))
            return false;
    }
    return false;
}

bool TreeBuilder::has_in_scope(Element* element) const
{
    for (std::size_t i = m_stack.size(); i-- > 0;) {
        Element* node = m_stack[i];
        if (node == element)
            return true;
        if (is_default_scope_terminator(*node))
            return false;
    }
    return false;
}

bool TreeBuilder::has_in_list_item_scope(std::string_view name) const
{
    for (std::size_t i = m_stack.size(); i-- > 0;) {
        Element* node = m_stack[i];
        if (node->is_html(name))
            return true;
        if (is_default_scope_terminator(*node) || node->is_html("ol") || node->is_html("ul"))
            return false;
    }
    return false;
}

bool TreeBuilder::has_in_button_scope(std::string_view name) const
{
    for (std::size_t i = m_stack.size(); i-- > 0;) {
        Element* node = m_stack[i];
        if (node->is_html(name))
            return true;
        if (is_default_scope_terminator(*node) || node->is_html("button"))
            return false;
    }
    return false;
}

bool TreeBuilder::has_in_table_scope(std::string_view name) const
{
    for (std::size_t i = m_stack.size(); i-- > 0;) {
        Element* node = m_stack[i];
        if (node->is_html(name))
            return true;
        if (node->is_html("html") || node->is_html("table") || node->is_html("template"))
            return false;
    }
    return false;
}

bool TreeBuilder::has_heading_in_scope() const
{
    for (std::size_t i = m_stack.size(); i-- > 0;) {
        Element* node = m_stack[i];
        if (node->is_html() && one_of(node->local_name(), heading_tags))
            return true;
        if (is_default_scope_terminator(*node))
            return false;
    }
    return false;
}

void TreeBuilder::generate_implied_end_tags(std::string_view except)
{
    while (Element* node = current_node()) {
        if (!node->is_html())
            return;
        std::string_view const name = node->local_name();
        if (name == except)
            return;
        if (one_of(name, implied_end_tags) || one_of(name, implied_end_tags_extra)) {
            pop();
            continue;
        }
        return;
    }
}

void TreeBuilder::generate_implied_end_tags_thoroughly()
{
    while (Element* node = current_node()) {
        if (!node->is_html())
            return;
        std::string_view const name = node->local_name();
        if (one_of(name, implied_end_tags) || one_of(name, implied_end_tags_extra) || one_of(name, thorough_extra)) {
            pop();
            continue;
        }
        return;
    }
}

// --- Insertion ----------------------------------------------------------------

TreeBuilder::InsertionLocation TreeBuilder::appropriate_place(Node* override_target) const
{
    Node* target = override_target ? override_target : current_node();
    if (!target)
        return { &m_document, nullptr };

    InsertionLocation location { target, nullptr };

    bool const foster_eligible = m_foster_parenting && target->is_element()
        && (static_cast<Element*>(target)->is_html("table") || static_cast<Element*>(target)->is_html("tbody")
            || static_cast<Element*>(target)->is_html("tfoot") || static_cast<Element*>(target)->is_html("thead")
            || static_cast<Element*>(target)->is_html("tr"));
    if (foster_eligible) {
        Element* last_template = nullptr;
        std::size_t template_index = 0;
        Element* last_table = nullptr;
        std::size_t table_index = 0;
        for (std::size_t i = m_stack.size(); i-- > 0;) {
            if (!last_template && m_stack[i]->is_html("template")) {
                last_template = m_stack[i];
                template_index = i;
            }
            if (!last_table && m_stack[i]->is_html("table")) {
                last_table = m_stack[i];
                table_index = i;
            }
        }
        if (last_template && (!last_table || template_index > table_index)) {
            location = { last_template->template_content(), nullptr };
        } else if (!last_table) {
            location = { m_stack.front(), nullptr };
        } else if (last_table->parent()) {
            location = { last_table->parent(), last_table };
        } else {
            location = { m_stack[table_index - 1], nullptr };
        }
    }

    if (location.parent && location.parent->is_element()) {
        Element* parent_element = static_cast<Element*>(location.parent);
        if (parent_element->is_html("template") && parent_element->template_content())
            location = { parent_element->template_content(), nullptr };
    }
    return location;
}

Element* TreeBuilder::create_element_for_token(Token const& token, std::string_view namespace_uri)
{
    Element* element = m_document.create<Element>(std::string(namespace_uri), token.tag_name);
    for (Attribute const& attribute : token.attributes) {
        Attr attr;
        attr.local_name = attribute.name;
        attr.value = attribute.value;
        element->attributes().push_back(std::move(attr));
    }
    if (element->is_html("template"))
        element->set_template_content(m_document.create<DocumentFragment>());
    return element;
}

Element* TreeBuilder::insert_html_element(Token const& token)
{
    return insert_foreign_element(token, dom::ns::html);
}

Element* TreeBuilder::insert_foreign_element(Token const& token, std::string_view namespace_uri)
{
    InsertionLocation const location = appropriate_place();
    Element* element = m_document.create<Element>(std::string(namespace_uri), token.tag_name);
    bool const svg = namespace_uri == dom::ns::svg;
    bool const mathml = namespace_uri == dom::ns::mathml;
    for (Attribute const& attribute : token.attributes) {
        if (svg || mathml) {
            element->attributes().push_back(adjusted_foreign_attribute(attribute, mathml, svg));
        } else {
            Attr attr;
            attr.local_name = attribute.name;
            attr.value = attribute.value;
            element->attributes().push_back(std::move(attr));
        }
    }
    if (element->is_html("template"))
        element->set_template_content(m_document.create<DocumentFragment>());
    if (location.parent)
        location.parent->insert_before(*element, location.before);
    m_stack.push_back(element);
    return element;
}

void TreeBuilder::insert_character(char32_t code_point)
{
    InsertionLocation const location = appropriate_place();
    if (!location.parent || location.parent->type() == NodeType::Document)
        return;

    Node* previous = nullptr;
    if (location.before) {
        auto const& siblings = location.parent->children();
        auto const it = std::find(siblings.begin(), siblings.end(), location.before);
        if (it != siblings.begin())
            previous = *(it - 1);
    } else {
        previous = location.parent->last_child();
    }

    if (previous && previous->is_text()) {
        append_utf8(static_cast<Text*>(previous)->data, code_point);
        return;
    }
    Text* text = m_document.create<Text>();
    append_utf8(text->data, code_point);
    location.parent->insert_before(*text, location.before);
}

void TreeBuilder::insert_comment(Token const& token, Node* explicit_parent)
{
    Comment* comment = m_document.create<Comment>();
    comment->data = token.data;
    if (explicit_parent) {
        explicit_parent->append_child(*comment);
        return;
    }
    InsertionLocation const location = appropriate_place();
    if (location.parent)
        location.parent->insert_before(*comment, location.before);
}

void TreeBuilder::merge_attributes_into(Element& element, Token const& token)
{
    for (Attribute const& attribute : token.attributes) {
        if (!element.find_attribute(attribute.name)) {
            Attr attr;
            attr.local_name = attribute.name;
            attr.value = attribute.value;
            element.attributes().push_back(std::move(attr));
        }
    }
}

void TreeBuilder::close_p_element()
{
    generate_implied_end_tags("p");
    pop_until_html_element_popped("p");
}

void TreeBuilder::parse_generic_text(Token const& token, Tokenizer::State state)
{
    insert_html_element(token);
    m_tokenizer->set_state(state);
    m_original_mode = m_mode;
    m_mode = Mode::Text;
}

// --- Active formatting elements ----------------------------------------------

void TreeBuilder::push_formatting(Element* element, Token const& token)
{
    // Noah's Ark: at most three identical entries since the last marker.
    int identical = 0;
    int earliest = -1;
    for (std::size_t i = m_formatting.size(); i-- > 0;) {
        FormattingEntry const& entry = m_formatting[i];
        if (!entry.element)
            break;
        if (entry.element->local_name() != element->local_name()
            || entry.element->namespace_uri() != element->namespace_uri())
            continue;
        if (entry.element->attributes().size() != element->attributes().size())
            continue;
        bool same = true;
        for (Attr const& attribute : element->attributes()) {
            Attr const* other = entry.element->find_attribute(attribute.local_name);
            if (!other || other->value != attribute.value) {
                same = false;
                break;
            }
        }
        if (same) {
            ++identical;
            earliest = static_cast<int>(i);
        }
    }
    if (identical >= 3 && earliest >= 0)
        m_formatting.erase(m_formatting.begin() + earliest);

    FormattingEntry entry;
    entry.element = element;
    entry.token = token;
    m_formatting.push_back(std::move(entry));
}

void TreeBuilder::push_formatting_marker()
{
    m_formatting.push_back(FormattingEntry {});
}

void TreeBuilder::clear_formatting_to_marker()
{
    while (!m_formatting.empty()) {
        bool const was_marker = m_formatting.back().element == nullptr;
        m_formatting.pop_back();
        if (was_marker)
            return;
    }
}

int TreeBuilder::formatting_index_after_marker(std::string_view name) const
{
    for (std::size_t i = m_formatting.size(); i-- > 0;) {
        FormattingEntry const& entry = m_formatting[i];
        if (!entry.element)
            return -1;
        if (entry.element->is_html(name))
            return static_cast<int>(i);
    }
    return -1;
}

void TreeBuilder::reconstruct_formatting()
{
    if (m_formatting.empty())
        return;
    if (!m_formatting.back().element || stack_contains(m_formatting.back().element))
        return;

    std::size_t index = m_formatting.size() - 1;
    while (index > 0) {
        FormattingEntry const& entry = m_formatting[index - 1];
        if (!entry.element || stack_contains(entry.element))
            break;
        --index;
    }
    for (; index < m_formatting.size(); ++index) {
        FormattingEntry& entry = m_formatting[index];
        Element* element = insert_html_element(entry.token);
        entry.element = element;
    }
}

// --- The adoption agency ------------------------------------------------------

void TreeBuilder::adoption_agency(Token& token)
{
    std::string_view const subject = token.tag_name;

    if (Element* current = current_node();
        current && current->is_html(subject) && formatting_index_after_marker(subject) < 0
        && [&] {
               for (FormattingEntry const& entry : m_formatting) {
                   if (entry.element == current)
                       return false;
               }
               return true;
           }()) {
        pop();
        return;
    }

    for (int outer = 0; outer < 8; ++outer) {
        int const formatting_index = formatting_index_after_marker(subject);
        if (formatting_index < 0) {
            any_other_end_tag_in_body(token);
            return;
        }
        Element* formatting_element = m_formatting[static_cast<std::size_t>(formatting_index)].element;

        if (!stack_contains(formatting_element)) {
            m_formatting.erase(m_formatting.begin() + formatting_index);
            return; // parse error
        }
        if (!has_in_scope(formatting_element))
            return; // parse error

        // Furthest block: lowest special element below the formatting element.
        std::size_t formatting_stack_index = 0;
        for (std::size_t i = 0; i < m_stack.size(); ++i) {
            if (m_stack[i] == formatting_element)
                formatting_stack_index = i;
        }
        Element* furthest_block = nullptr;
        std::size_t furthest_index = 0;
        for (std::size_t i = formatting_stack_index + 1; i < m_stack.size(); ++i) {
            if (is_special(*m_stack[i])) {
                furthest_block = m_stack[i];
                furthest_index = i;
                break;
            }
        }
        if (!furthest_block) {
            pop_until_popped(formatting_element);
            m_formatting.erase(m_formatting.begin() + formatting_index);
            return;
        }

        Element* common_ancestor = m_stack[formatting_stack_index - 1];
        std::size_t bookmark = static_cast<std::size_t>(formatting_index);

        Element* node = furthest_block;
        Element* last_node = furthest_block;
        std::size_t node_index = furthest_index;

        for (int inner = 1;; ++inner) {
            --node_index;
            node = m_stack[node_index];
            if (node == formatting_element)
                break;

            int node_formatting_index = -1;
            for (std::size_t i = 0; i < m_formatting.size(); ++i) {
                if (m_formatting[i].element == node)
                    node_formatting_index = static_cast<int>(i);
            }
            if (inner > 3 && node_formatting_index >= 0) {
                m_formatting.erase(m_formatting.begin() + node_formatting_index);
                if (static_cast<std::size_t>(node_formatting_index) < bookmark)
                    --bookmark;
                node_formatting_index = -1;
            }
            if (node_formatting_index < 0) {
                // Not in the list: drop from the stack and continue upward.
                m_stack.erase(m_stack.begin() + static_cast<std::ptrdiff_t>(node_index));
                if (node_index < furthest_index)
                    --furthest_index;
                continue;
            }

            // Clone the node and swap the clone into both structures.
            Token const& node_token = m_formatting[static_cast<std::size_t>(node_formatting_index)].token;
            Element* clone = create_element_for_token(node_token, dom::ns::html);
            m_formatting[static_cast<std::size_t>(node_formatting_index)].element = clone;
            m_stack[node_index] = clone;
            node = clone;

            if (last_node == furthest_block)
                bookmark = static_cast<std::size_t>(node_formatting_index) + 1;

            node->append_child(*last_node);
            last_node = node;
        }

        InsertionLocation const location = appropriate_place(common_ancestor);
        if (location.parent)
            location.parent->insert_before(*last_node, location.before);

        Token const formatting_token = m_formatting[formatting_index_after_marker(subject) >= 0
                ? static_cast<std::size_t>(formatting_index_after_marker(subject))
                : static_cast<std::size_t>(formatting_index)]
                                           .token;
        Element* replacement = create_element_for_token(formatting_token, dom::ns::html);
        while (!furthest_block->children().empty())
            replacement->append_child(*furthest_block->children().front());
        furthest_block->append_child(*replacement);

        for (std::size_t i = 0; i < m_formatting.size(); ++i) {
            if (m_formatting[i].element == formatting_element) {
                FormattingEntry entry = std::move(m_formatting[i]);
                entry.element = replacement;
                m_formatting.erase(m_formatting.begin() + static_cast<std::ptrdiff_t>(i));
                if (i < bookmark)
                    --bookmark;
                if (bookmark > m_formatting.size())
                    bookmark = m_formatting.size();
                m_formatting.insert(m_formatting.begin() + static_cast<std::ptrdiff_t>(bookmark), std::move(entry));
                break;
            }
        }
        remove_from_stack(formatting_element);
        for (std::size_t i = 0; i < m_stack.size(); ++i) {
            if (m_stack[i] == furthest_block) {
                m_stack.insert(m_stack.begin() + static_cast<std::ptrdiff_t>(i) + 1, replacement);
                break;
            }
        }
    }
}

// --- Mode resets and wrappers -------------------------------------------------

void TreeBuilder::reset_insertion_mode()
{
    for (std::size_t i = m_stack.size(); i-- > 0;) {
        bool const last = i == 0;
        Element* node = m_stack[i];
        if (last && m_context)
            node = m_context;

        if ((node->is_html("td") || node->is_html("th")) && !last) {
            m_mode = Mode::InCell;
            return;
        }
        if (node->is_html("tr")) {
            m_mode = Mode::InRow;
            return;
        }
        if (node->is_html("tbody") || node->is_html("thead") || node->is_html("tfoot")) {
            m_mode = Mode::InTableBody;
            return;
        }
        if (node->is_html("caption")) {
            m_mode = Mode::InCaption;
            return;
        }
        if (node->is_html("colgroup")) {
            m_mode = Mode::InColumnGroup;
            return;
        }
        if (node->is_html("table")) {
            m_mode = Mode::InTable;
            return;
        }
        if (node->is_html("template")) {
            m_mode = m_template_modes.empty() ? Mode::InBody : m_template_modes.back();
            return;
        }
        if (node->is_html("head") && !last) {
            m_mode = Mode::InHead;
            return;
        }
        if (node->is_html("body")) {
            m_mode = Mode::InBody;
            return;
        }
        if (node->is_html("frameset")) {
            m_mode = Mode::InFrameset;
            return;
        }
        if (node->is_html("html")) {
            m_mode = m_head_element ? Mode::AfterHead : Mode::BeforeHead;
            return;
        }
        if (last) {
            m_mode = Mode::InBody;
            return;
        }
    }
    m_mode = Mode::InBody;
}

std::unique_ptr<Document> parse_document(std::string_view utf8)
{
    return parse_document(decode_utf8(utf8));
}

std::unique_ptr<Document> parse_document_bytes(std::string_view bytes)
{
    return parse_document(decode_document_bytes(bytes));
}

std::unique_ptr<Document> parse_document(std::u32string code_points)
{
    auto document = std::make_unique<Document>();
    TreeBuilder builder(*document);
    Tokenizer tokenizer(InputStream(std::move(code_points)), Tokenizer::State::Data);
    builder.run(tokenizer);
    return document;
}

FragmentParseResult parse_fragment(std::u32string code_points, std::string_view context_namespace,
    std::string_view context_local_name)
{
    FragmentParseResult result;
    result.document = std::make_unique<Document>();
    Element* context = result.document->create<Element>(std::string(context_namespace), std::string(context_local_name));
    TreeBuilder builder(*result.document, *context);
    Tokenizer tokenizer(InputStream(std::move(code_points)),
        TreeBuilder::tokenizer_state_for_fragment_context(*context));
    // No last-start-tag seeding: "if no start tag has been emitted from this
    // tokenizer, then no end tag token is appropriate" — a fragment's context
    // element was never emitted, so e.g. "</script>" inside a script-context
    // fragment stays character data.
    builder.run(tokenizer);
    result.root = builder.root_html_element();
    return result;
}

}
