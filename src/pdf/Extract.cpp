#include "pdf/Extract.h"

#include "pdf/Render.h"

#include <cmath>
#include <set>

namespace sashfold::pdf {

namespace {

constexpr int k_max_outline = 20000;
constexpr int k_max_tree_depth = 32;

// A name tree's value for a key (§7.9.6): its /Names pairs or its /Kids by /Limits.
Object name_tree_lookup(Document const& doc, Object const& node_in, std::string const& key, int depth)
{
    if (depth > k_max_tree_depth)
        return {};
    Dict const node = doc.resolve(node_in).as_dict();
    Array const& names = doc.resolve(node.get("Names")).as_array();
    for (std::size_t i = 0; i + 1 < names.size(); i += 2) {
        if (doc.resolve(names[i]).as_string() == key)
            return doc.resolve(names[i + 1]);
    }
    for (Object const& kid : doc.resolve(node.get("Kids")).as_array()) {
        Dict const kid_dict = doc.resolve(kid).as_dict();
        Array const& limits = doc.resolve(kid_dict.get("Limits")).as_array();
        if (limits.size() == 2) {
            std::string const& low = doc.resolve(limits[0]).as_string();
            std::string const& high = doc.resolve(limits[1]).as_string();
            if (key < low || key > high)
                continue;
        }
        Object found = name_tree_lookup(doc, kid, key, depth + 1);
        if (!found.is_null())
            return found;
    }
    return {};
}

} // namespace

std::string utf8_of(std::u32string const& text)
{
    std::string out;
    for (char32_t code : text) {
        if (code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF))
            code = 0xFFFD;
        if (code < 0x80) {
            out.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else if (code < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (code >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }
    return out;
}

std::vector<TextRun> page_text(Document const& doc, std::size_t index)
{
    std::vector<TextGlyph> const glyphs = page_glyphs(doc, index, 1);
    std::vector<TextRun> runs;
    std::u32string text;
    TextRun run;
    bool open = false;
    double end_x = 0; // where the run's last glyph ended, along the baseline direction
    double end_y = 0;
    double dir_x = 1;
    double dir_y = 0;
    auto const flush = [&](bool ends_line) {
        if (!open)
            return;
        // Trailing spaces are kept out of the box; a word space between runs is the run's last character.
        run.text = utf8_of(text);
        run.width = std::hypot(end_x - run.x, end_y - run.y);
        run.ends_line = ends_line;
        if (!run.text.empty() && run.width > 0 && run.height > 0)
            runs.push_back(run);
        text.clear();
        open = false;
    };
    for (TextGlyph const& glyph : glyphs) {
        svg::Point const& bl = glyph.quad[0];
        svg::Point const& br = glyph.quad[1];
        svg::Point const& tl = glyph.quad[3];
        double const gx = static_cast<double>(br.x) - static_cast<double>(bl.x);
        double const gy = static_cast<double>(br.y) - static_cast<double>(bl.y);
        double const advance = std::hypot(gx, gy);
        double const height = std::hypot(static_cast<double>(tl.x) - static_cast<double>(bl.x), static_cast<double>(tl.y) - static_cast<double>(bl.y));
        if (height <= 0 || glyph.text.empty())
            continue;
        double const ux = advance > 0 ? gx / advance : dir_x;
        double const uy = advance > 0 ? gy / advance : dir_y;
        // Ascent part only for the box: from the baseline to the top.
        if (open) {
            // The same run: same direction, on the baseline's line, close after the last glyph.
            double const dx = static_cast<double>(bl.x) - end_x;
            double const dy = static_cast<double>(bl.y) - end_y;
            double const along = dx * dir_x + dy * dir_y;
            double const across = -dx * dir_y + dy * dir_x;
            bool const same_direction = std::abs(ux * dir_x + uy * dir_y - 1) < 0.01;
            bool const same_line = std::abs(across) < run.height * 0.5 && same_direction;
            if (!same_line || along < -run.height * 0.5) {
                flush(true);
            } else if (along > run.height * 3) {
                // A wide gap on the same line: a separate run (columns, tables), joined by a space in copied text.
                text += U' ';
                flush(false);
            } else if (along > run.height * 0.15 && !text.empty() && text.back() != U' ' && glyph.text != U" ") {
                // A gap the size of a space with no space glyph: one is supplied.
                text += U' ';
            }
        }
        if (!open) {
            run = TextRun {};
            run.x = static_cast<double>(bl.x);
            run.y = static_cast<double>(bl.y);
            run.height = height;
            run.angle = std::atan2(uy, ux);
            dir_x = ux;
            dir_y = uy;
            open = true;
        }
        run.height = std::max(run.height, height);
        text += glyph.text;
        end_x = static_cast<double>(br.x);
        end_y = static_cast<double>(br.y);
    }
    flush(true);
    return runs;
}

std::optional<Destination> resolve_destination(Document const& doc, Object const& destination_in)
{
    Object destination = doc.resolve(destination_in);
    // A named destination: the catalog's /Dests dictionary or the /Names /Dests tree.
    for (int hop = 0; hop < 4 && (destination.is_name() || destination.is_string()); ++hop) {
        std::string const key = destination.is_name() ? destination.as_name() : destination.as_string();
        Object found = doc.resolve(doc.resolve(doc.catalog().get("Dests")).as_dict().get(key));
        if (found.is_null()) {
            Object const names = doc.resolve(doc.catalog().get("Names"));
            found = name_tree_lookup(doc, names.as_dict().get("Dests"), key, 0);
        }
        destination = found.is_dict() ? doc.resolve(found.as_dict().get("D")) : found;
    }
    Array const& array = destination.as_array();
    if (array.empty())
        return std::nullopt;
    Destination out;
    if (array[0].is_ref()) {
        std::optional<std::size_t> const page = doc.page_index(array[0].as_ref());
        if (!page)
            return std::nullopt;
        out.page = *page;
    } else if (array[0].is_number()) {
        // Remote-style page numbers appear in local destinations from some writers.
        std::int64_t const page = array[0].as_int(-1);
        if (page < 0 || static_cast<std::size_t>(page) >= doc.page_count())
            return std::nullopt;
        out.page = static_cast<std::size_t>(page);
    } else {
        return std::nullopt;
    }
    std::string const kind = array.size() > 1 ? doc.resolve(array[1]).as_name() : "";
    Page const& page = doc.page(out.page);
    canvas::Transform const to_device = page_to_device(page, 1);
    auto const number = [&](std::size_t i) -> std::optional<double> {
        if (i >= array.size())
            return std::nullopt;
        Object const v = doc.resolve(array[i]);
        if (!v.is_number())
            return std::nullopt;
        return v.as_number();
    };
    std::optional<double> left;
    std::optional<double> top;
    if (kind == "XYZ") {
        left = number(2);
        top = number(3);
    } else if (kind == "FitH" || kind == "FitBH") {
        top = number(2);
    } else if (kind == "FitV" || kind == "FitBV") {
        left = number(2);
    } else if (kind == "FitR") {
        left = number(2);
        top = number(5);
    }
    if (left || top) {
        svg::Point const p = to_device.apply(left.value_or(page.crop_box.x0), top.value_or(page.crop_box.y1));
        if (left)
            out.x = std::max(0.0, static_cast<double>(p.x));
        if (top)
            out.y = std::max(0.0, static_cast<double>(p.y));
    }
    return out;
}

std::vector<Link> page_links(Document const& doc, std::size_t index)
{
    std::vector<Link> links;
    if (index >= doc.page_count())
        return links;
    Page const& page = doc.page(index);
    canvas::Transform const to_device = page_to_device(page, 1);
    for (Object const& annotation_ref : doc.resolve(page.dict->get("Annots")).as_array()) {
        Dict const annotation = doc.resolve(annotation_ref).as_dict();
        if (doc.resolve(annotation.get("Subtype")).as_name() != "Link")
            continue;
        Array const& rect = doc.resolve(annotation.get("Rect")).as_array();
        if (rect.size() != 4)
            continue;
        svg::Point const a = to_device.apply(doc.resolve(rect[0]).as_number(), doc.resolve(rect[1]).as_number());
        svg::Point const b = to_device.apply(doc.resolve(rect[2]).as_number(), doc.resolve(rect[3]).as_number());
        Link link;
        link.x0 = static_cast<double>(std::min(a.x, b.x));
        link.y0 = static_cast<double>(std::min(a.y, b.y));
        link.x1 = static_cast<double>(std::max(a.x, b.x));
        link.y1 = static_cast<double>(std::max(a.y, b.y));
        Dict const action = doc.resolve(annotation.get("A")).as_dict();
        std::string const kind = doc.resolve(action.get("S")).as_name();
        if (kind == "URI") {
            link.url = doc.resolve(action.get("URI")).as_string();
        } else if (kind == "GoTo") {
            link.destination = resolve_destination(doc, action.get("D"));
        } else if (kind == "Named") {
            // The standard's named actions that move through the document.
            std::string const name = doc.resolve(action.get("N")).as_name();
            std::size_t target = index;
            if (name == "NextPage" && index + 1 < doc.page_count())
                target = index + 1;
            else if (name == "PrevPage" && index > 0)
                target = index - 1;
            else if (name == "FirstPage")
                target = 0;
            else if (name == "LastPage")
                target = doc.page_count() - 1;
            else
                continue;
            link.destination = Destination { target, std::nullopt, std::nullopt };
        } else if (annotation.has("Dest")) {
            link.destination = resolve_destination(doc, annotation.get("Dest"));
        }
        if (link.url.empty() && !link.destination)
            continue;
        links.push_back(std::move(link));
    }
    return links;
}

namespace {

void read_outline(Document const& doc, Object const& first, int depth, std::set<int>& seen, int& count, std::vector<OutlineItem>& out)
{
    if (depth > k_max_tree_depth)
        return;
    Object node_ref = first;
    while (node_ref.is_ref() && count < k_max_outline) {
        if (!seen.insert(node_ref.as_ref().num).second)
            break;
        ++count;
        Dict const node = doc.resolve(node_ref).as_dict();
        OutlineItem item;
        item.title = text_string_to_utf8(doc.resolve(node.get("Title")).as_string());
        if (node.has("Dest")) {
            item.destination = resolve_destination(doc, node.get("Dest"));
        } else {
            Dict const action = doc.resolve(node.get("A")).as_dict();
            std::string const kind = doc.resolve(action.get("S")).as_name();
            if (kind == "GoTo")
                item.destination = resolve_destination(doc, action.get("D"));
            else if (kind == "URI")
                item.url = doc.resolve(action.get("URI")).as_string();
        }
        item.open = doc.resolve(node.get("Count")).as_int(0) > 0;
        read_outline(doc, node.get("First"), depth + 1, seen, count, item.children);
        out.push_back(std::move(item));
        node_ref = node.get("Next");
    }
}

} // namespace

std::vector<OutlineItem> document_outline(Document const& doc)
{
    std::vector<OutlineItem> items;
    Dict const outlines = doc.resolve(doc.catalog().get("Outlines")).as_dict();
    std::set<int> seen;
    int count = 0;
    read_outline(doc, outlines.get("First"), 0, seen, count, items);
    return items;
}

}
