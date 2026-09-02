// The Sashfold 100 dashboard: reads the corpus and the report each row's
// render left beside its picture (sashfold --render --report), and writes
// the public page and a JSON summary. The page is hand-shaped HTML in the
// landing page's manner, with no scripts: Sashfold renders it too.
//
//   sashfold100 <corpus.txt> <renders-dir> [--html <file>] [--json <file>]
//
// The renders directory holds, per row, <id>.png, <id>-thumb.png and
// <id>.json; tools/sashfold100.sh fills it and then runs this.

#include "core/Json.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace sashfold;

namespace {

struct Category {
    std::string id;
    std::string title;
    std::string description;
};

struct Row {
    std::string id;
    std::string category;
    std::string rank;
    std::string url;
    std::vector<std::string> flags;
    std::string note;

    // From the render's report; `outcome` is "not run" when there is none.
    std::string outcome = "not run";
    int status = 0;
    int exit_code = 0;
    std::string error;
    std::string final_url;
    std::string title;
    std::string content_type;
    long bytes = 0;
    long page_height = 0;
    long characters = 0;
    long stylesheets = 0;
    long stylesheets_failed = 0;
    long images = 0;
    long images_failed = 0;
    long fonts = 0;
    long ms_fetch = 0;
    long ms_total = 0;
    std::string rendered;
    // What the page's stylesheets ask for that the engine does not do yet:
    // feature key → declarations.
    std::vector<std::pair<std::string, long>> asks;

    bool has_flag(std::string_view flag) const
    {
        return std::find(flags.begin(), flags.end(), flag) != flags.end();
    }
};

struct Corpus {
    std::string list_id;
    std::string list_date;
    std::string list_url;
    std::vector<Category> categories;
    std::vector<Row> rows;
};

std::vector<std::string> split_tabs(std::string const& line)
{
    std::vector<std::string> fields;
    std::string::size_type start = 0;
    while (true) {
        std::string::size_type const tab = line.find('\t', start);
        if (tab == std::string::npos) {
            fields.push_back(line.substr(start));
            return fields;
        }
        fields.push_back(line.substr(start, tab - start));
        start = tab + 1;
    }
}

std::optional<Corpus> read_corpus(std::filesystem::path const& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::cerr << "error: cannot read " << path.string() << "\n";
        return std::nullopt;
    }
    Corpus corpus;
    std::string line;
    int line_number = 0;
    while (std::getline(in, line)) {
        ++line_number;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty() || line[0] == '#')
            continue;
        std::vector<std::string> const fields = split_tabs(line);
        std::string const& kind = fields[0];
        if (kind == "list" && fields.size() == 4) {
            corpus.list_id = fields[1];
            corpus.list_date = fields[2];
            corpus.list_url = fields[3];
        } else if (kind == "category" && fields.size() == 4) {
            corpus.categories.push_back(Category { fields[1], fields[2], fields[3] });
        } else if (kind == "row" && fields.size() == 7) {
            Row row;
            row.id = fields[1];
            row.category = fields[2];
            row.rank = fields[3];
            row.url = fields[4];
            if (fields[5] != "-") {
                std::istringstream flags(fields[5]);
                std::string flag;
                while (std::getline(flags, flag, ','))
                    row.flags.push_back(flag);
            }
            row.note = fields[6];
            corpus.rows.push_back(std::move(row));
        } else {
            std::cerr << "error: " << path.string() << ":" << line_number << ": unreadable record\n";
            return std::nullopt;
        }
    }
    return corpus;
}

std::string string_of(JsonValue const& object, std::string_view key)
{
    JsonValue const* const value = object.get(key);
    return value && value->is_string() ? value->as_string() : std::string();
}

long number_of(JsonValue const& object, std::string_view key)
{
    JsonValue const* const value = object.get(key);
    return value && value->is_number() ? static_cast<long>(value->as_number()) : 0;
}

long nested_number(JsonValue const& object, std::string_view group, std::string_view key)
{
    JsonValue const* const inner = object.get(group);
    return inner && inner->is_object() ? number_of(*inner, key) : 0;
}

// Fills a row from its report; a row without one stays "not run".
void read_report(Row& row, std::filesystem::path const& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return;
    std::stringstream buffer;
    buffer << in.rdbuf();
    std::optional<JsonValue> const report = JsonValue::parse(buffer.str());
    if (!report || !report->is_object()) {
        row.outcome = "unreadable report";
        return;
    }
    row.outcome = string_of(*report, "outcome");
    if (row.outcome.empty())
        row.outcome = "unreadable report";
    row.status = static_cast<int>(number_of(*report, "status"));
    row.exit_code = static_cast<int>(number_of(*report, "exit"));
    row.error = string_of(*report, "error");
    row.final_url = string_of(*report, "url");
    row.title = string_of(*report, "title");
    row.content_type = string_of(*report, "content_type");
    row.bytes = number_of(*report, "bytes");
    row.page_height = number_of(*report, "page_height");
    row.characters = nested_number(*report, "text", "characters");
    row.stylesheets = nested_number(*report, "stylesheets", "count");
    row.stylesheets_failed = nested_number(*report, "stylesheets", "failed");
    row.images = nested_number(*report, "images", "count");
    row.images_failed = nested_number(*report, "images", "failed");
    row.fonts = number_of(*report, "fonts");
    row.ms_fetch = nested_number(*report, "ms", "fetch");
    row.ms_total = nested_number(*report, "ms", "total");
    row.rendered = string_of(*report, "rendered");
    if (JsonValue const* const asks = report->get("asks"); asks && asks->is_object()) {
        for (JsonValue::Member const& member : asks->as_object()) {
            if (member.second.is_number() && member.second.as_number() > 0)
                row.asks.emplace_back(member.first, static_cast<long>(member.second.as_number()));
        }
        std::sort(row.asks.begin(), row.asks.end(),
            [](auto const& a, auto const& b) { return a.second > b.second; });
    }
}

// The features the census counts, with the words the page uses for them.
struct FeatureName {
    std::string_view key;
    std::string_view label;
};

constexpr FeatureName feature_names[] = {
    { "display-contents", "display: contents" },
    { "transforms", "rotations, scales and skews (translations are drawn)" },
    { "animations", "animations and transitions" },
    { "border-radius", "rounded corners" },
    { "shadows", "shadows" },
    { "effects", "filters, clipping paths, masks" },
    { "text-properties", "letter-spacing, text-transform, text-overflow" },
    { "multi-column", "multi-column layout" },
    { "sizing", "object-fit and aspect-ratio" },
    { "outline", "outlines" },
    { "direction", "writing direction" },
    { "counters", "counters" },
    { "web-fonts", "web fonts in WOFF or WOFF2" },
    { "at-rules", "@supports, @layer, @container, @keyframes, @scope" },
};

std::string feature_label(std::string const& key)
{
    for (FeatureName const& name : feature_names) {
        if (name.key == key)
            return std::string(name.label);
    }
    return key;
}

// Across the corpus: how many pages ask for each feature, and how many
// declarations in all — the ranking of what to write next.
struct FeatureTotal {
    std::string key;
    long pages = 0;
    long declarations = 0;
};

std::vector<FeatureTotal> feature_totals(std::vector<Row> const& rows)
{
    std::map<std::string, FeatureTotal> totals;
    for (Row const& row : rows) {
        for (auto const& [key, count] : row.asks) {
            FeatureTotal& total = totals[key];
            total.key = key;
            ++total.pages;
            total.declarations += count;
        }
    }
    std::vector<FeatureTotal> list;
    for (auto const& [key, total] : totals)
        list.push_back(total);
    std::sort(list.begin(), list.end(), [](FeatureTotal const& a, FeatureTotal const& b) {
        return a.pages != b.pages ? a.pages > b.pages : a.declarations > b.declarations;
    });
    return list;
}

std::string html_escaped(std::string_view text)
{
    std::string out;
    for (char const c : text) {
        switch (c) {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += "&quot;";
            break;
        default:
            out += c;
        }
    }
    return out;
}

std::string json_escaped(std::string_view text)
{
    std::string out;
    for (unsigned char const c : text) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20) {
                char buffer[8];
                std::snprintf(buffer, sizeof buffer, "\\u%04x", c);
                out += buffer;
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    return out;
}

// The host of a URL, for the card's heading: between the scheme and the
// next slash, without a leading www.
std::string host_of(std::string const& url)
{
    std::string::size_type start = url.find("://");
    start = start == std::string::npos ? 0 : start + 3;
    std::string::size_type const end = url.find('/', start);
    std::string host = url.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (host.starts_with("www."))
        host.erase(0, 4);
    return host;
}

std::string seconds(long ms)
{
    char buffer[32];
    if (ms < 1000)
        std::snprintf(buffer, sizeof buffer, "%ld ms", ms);
    else
        std::snprintf(buffer, sizeof buffer, "%.1f s", static_cast<double>(ms) / 1000.0);
    return buffer;
}

std::string shortened(std::string const& text, std::size_t limit)
{
    if (text.size() <= limit)
        return text;
    // Cut at a character boundary: back up over UTF-8 continuation bytes.
    std::size_t cut = limit;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80)
        --cut;
    return text.substr(0, cut) + "…";
}

// The verdict on a row: a class for the pill, its text, and whether the
// row counts as loaded (a page with text on it), refused (a bad
// certificate turned away), or failed.
struct Verdict {
    std::string kind; // ok, refused, warn, bad, muted
    std::string label;
};

Verdict verdict_of(Row const& row)
{
    if (row.has_flag("refuse") && row.outcome != "certificate-error" && row.outcome != "not run") {
        // A bad certificate that was not turned away: the page loading is
        // the wrong answer, whatever it looks like.
        if (row.outcome == "document" || row.outcome == "text")
            return { "bad", "not refused" };
    }
    if (row.outcome == "document" || row.outcome == "text") {
        std::string const status = std::to_string(row.status);
        if (row.status != 200)
            return { "warn", status };
        if (row.characters == 0)
            return { "warn", "200, no text" };
        return { "ok", row.outcome == "text" ? "200, plain text" : "200" };
    }
    if (row.outcome == "certificate-error")
        return { row.has_flag("refuse") ? "refused" : "bad", "certificate refused" };
    if (row.outcome == "unsupported")
        return { "warn", "not a page" };
    if (row.outcome == "error")
        return { "bad", "unreachable" };
    if (row.outcome == "timeout")
        return { "bad", "timed out" };
    if (row.outcome == "crashed")
        return { "bad", "crashed" };
    return { "muted", row.outcome };
}

bool is_loaded(Row const& row)
{
    Verdict const verdict = verdict_of(row);
    return verdict.kind == "ok";
}

std::string now_utc()
{
    std::time_t const now = std::time(nullptr);
    char buffer[32] = {};
    if (std::strftime(buffer, sizeof buffer, "%Y-%m-%d %H:%M UTC", std::gmtime(&now)) == 0)
        return "";
    return buffer;
}

// The newest render among the rows ("2026-09-02T14:12:46Z"), as Arizona's
// date and time — Arizona keeps UTC−7 the whole year — so a reader there
// sees at once how fresh the pictures are. Empty when no row was rendered.
std::string latest_render_arizona(std::vector<Row> const& rows)
{
    std::string latest;
    for (Row const& row : rows) {
        if (row.rendered.size() == 20 && row.rendered > latest)
            latest = row.rendered;
    }
    if (latest.empty())
        return {};
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (std::sscanf(latest.c_str(), "%4d-%2d-%2dT%2d:%2d:%2dZ", &year, &month, &day, &hour, &minute, &second) != 6)
        return {};
    // Days since the civil epoch, then back, seven hours earlier.
    auto const days_from_civil = [](int y, int m, int d) -> long {
        y -= m <= 2;
        long const era = (y >= 0 ? y : y - 399) / 400;
        long const yoe = y - era * 400;
        long const doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
        long const doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        return era * 146097 + doe - 719468;
    };
    auto const civil_from_days = [](long z, int& y, int& m, int& d) {
        z += 719468;
        long const era = (z >= 0 ? z : z - 146096) / 146097;
        long const doe = z - era * 146097;
        long const yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        long const doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        long const mp = (5 * doy + 2) / 153;
        d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
        m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
        y = static_cast<int>(yoe + era * 400 + (m <= 2));
    };
    long seconds = days_from_civil(year, month, day) * 86400L + hour * 3600L + minute * 60L + second;
    seconds -= 7L * 3600L;
    long days = seconds / 86400L;
    long rest = seconds % 86400L;
    if (rest < 0) {
        rest += 86400L;
        --days;
    }
    int ay = 0;
    int am = 0;
    int ad = 0;
    civil_from_days(days, ay, am, ad);
    char buffer[64];
    std::snprintf(buffer, sizeof buffer, "%04d-%02d-%02d %02d:%02d", ay, am, ad, static_cast<int>(rest / 3600),
        static_cast<int>(rest % 3600 / 60));
    return buffer;
}

struct Totals {
    long rows = 0;
    long rendered = 0; // a picture exists
    long loaded = 0; // answered 200 with text on the page
    long refusals_expected = 0;
    long refused = 0;
    long median_ms = 0; // over the loaded rows, request to pixels
};

Totals totals_of(std::vector<Row> const& rows)
{
    Totals totals;
    std::vector<long> times;
    for (Row const& row : rows) {
        ++totals.rows;
        if (row.outcome == "document" || row.outcome == "text" || row.outcome == "unsupported"
            || row.outcome == "certificate-error" || row.outcome == "error")
            ++totals.rendered;
        if (is_loaded(row)) {
            ++totals.loaded;
            times.push_back(row.ms_total);
        }
        if (row.has_flag("refuse")) {
            ++totals.refusals_expected;
            if (row.outcome == "certificate-error")
                ++totals.refused;
        }
    }
    if (!times.empty()) {
        std::sort(times.begin(), times.end());
        totals.median_ms = times[times.size() / 2];
    }
    return totals;
}

void write_card(std::ostream& out, Row const& row)
{
    Verdict const verdict = verdict_of(row);
    bool const greyed = row.has_flag("rtl");
    std::string const host = host_of(row.final_url.empty() ? row.url : row.final_url);
    bool const has_picture = row.outcome != "not run" && row.outcome != "timeout"
        && row.outcome != "crashed" && row.outcome != "unreadable report";
    out << "<div class=\"card" << (greyed ? " greyed" : "") << "\">\n";
    if (has_picture) {
        out << "  <a href=\"" << html_escaped(row.id) << ".png\"><img src=\"" << html_escaped(row.id)
            << "-thumb.png\" width=\"320\" height=\"240\" loading=\"lazy\" alt=\"" << html_escaped(host)
            << " rendered by Sashfold\"></a>\n";
    } else {
        out << "  <div class=\"nopic\">no picture</div>\n";
    }
    out << "  <p class=\"site\"><a href=\"" << html_escaped(row.url) << "\">" << html_escaped(host)
        << "</a> <span class=\"rank\">#" << html_escaped(row.rank) << "</span> <span class=\"pill "
        << verdict.kind << "\">" << html_escaped(verdict.label) << "</span></p>\n";
    if (!row.title.empty() && row.outcome != "certificate-error" && row.outcome != "error")
        out << "  <p class=\"title\">" << html_escaped(shortened(row.title, 80)) << "</p>\n";
    out << "  <p class=\"note\">" << html_escaped(row.note);
    if (greyed)
        out << " — right-to-left text is not written yet";
    out << "</p>\n";
    if (row.has_flag("refuse") && (row.outcome == "document" || row.outcome == "text")) {
        out << "  <p class=\"facts\">the connection was accepted and the page loaded (" << html_escaped(seconds(row.ms_total))
            << " to pixels); it should have been refused</p>\n";
    } else if (row.outcome == "document" || row.outcome == "text") {
        out << "  <p class=\"facts\">" << html_escaped(seconds(row.ms_total)) << " to pixels · "
            << row.page_height << " px tall · " << row.stylesheets << " sheet" << (row.stylesheets == 1 ? "" : "s");
        if (row.stylesheets_failed > 0)
            out << " (" << row.stylesheets_failed << " failed)";
        out << " · " << row.images << " picture" << (row.images == 1 ? "" : "s");
        if (row.images_failed > 0)
            out << " (" << row.images_failed << " failed)";
        if (row.fonts > 0)
            out << " · " << row.fonts << " font" << (row.fonts == 1 ? "" : "s");
        out << "</p>\n";
        if (!row.asks.empty()) {
            out << "  <p class=\"asks\">asks for ";
            std::size_t shown = 0;
            for (auto const& [key, count] : row.asks) {
                if (shown == 3)
                    break;
                out << (shown ? ", " : "") << html_escaped(feature_label(key)) << " ×" << count;
                ++shown;
            }
            if (row.asks.size() > 3)
                out << ", and " << (row.asks.size() - 3) << " more";
            out << "</p>\n";
        }
    } else if (row.outcome == "certificate-error") {
        out << "  <p class=\"facts\">" << html_escaped(row.error) << "</p>\n";
    } else if (row.outcome == "error") {
        out << "  <p class=\"facts\">" << html_escaped(row.error) << "</p>\n";
    } else if (row.outcome == "unsupported") {
        out << "  <p class=\"facts\">" << html_escaped(row.content_type) << ", " << row.bytes
            << " bytes; the window would offer it as a download</p>\n";
    } else if (row.outcome == "timeout") {
        out << "  <p class=\"facts\">the render did not finish within the time limit</p>\n";
    } else if (row.outcome == "crashed") {
        out << "  <p class=\"facts\">the render exited with status " << row.exit_code << "</p>\n";
    }
    out << "</div>\n";
}

void write_html(std::filesystem::path const& path, Corpus const& corpus, Totals const& totals,
    std::string const& stamp)
{
    std::ofstream out(path, std::ios::binary);
    out << "<!doctype html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n"
           "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
           "<title>Sashfold — The Sashfold 100</title>\n"
           "<meta name=\"description\" content=\"A hundred pages of the web, rendered every night by the Sashfold browser engine, with the picture and the numbers of each.\">\n"
           "<link rel=\"icon\" type=\"image/png\" href=\"../icon-160.png\">\n"
           "<link rel=\"canonical\" href=\"https://sashfold.com/sashfold100/\">\n"
           "<style>\n"
           "  :root { --bg: #f7f9fc; --panel: #ffffff; --ink: #16233c; --muted: #4e5d78; --navy: #16305e;\n"
           "          --blue: #1e63c4; --sky: #37b6e8; --accent: #f49c3c; --line: #d8e1ee; --code-bg: #eef3fa;\n"
           "          --ok: #1f8a4c; --ok-bg: #e3f5ea; --warn: #9a6200; --warn-bg: #fff2d6; --bad: #b3261e; --bad-bg: #fde7e5; }\n"
           "  @media (prefers-color-scheme: dark) {\n"
           "    :root { --bg: #0d1524; --panel: #131e33; --ink: #e6edf7; --muted: #9fb0c9; --navy: #a8c4ef;\n"
           "            --blue: #5b9bea; --sky: #4cc3ef; --accent: #f5aa55; --line: #24334f; --code-bg: #1a2740;\n"
           "            --ok: #7fd49c; --ok-bg: #163524; --warn: #f0c060; --warn-bg: #3a2d10; --bad: #f08a83; --bad-bg: #3d1a17; }\n"
           "  }\n"
           "  * { box-sizing: border-box; }\n"
           "  body { margin: 0; background: var(--bg); color: var(--ink);\n"
           "         font: 17px/1.65 system-ui, \"Segoe UI\", Roboto, Helvetica, Arial, sans-serif; }\n"
           "  main { max-width: 1100px; margin: 0 auto; padding: 0 20px 64px; }\n"
           "  a { color: var(--blue); }\n"
           "  a:hover { color: var(--sky); }\n"
           "  h1, h2 { color: var(--navy); line-height: 1.2; }\n"
           "  h1 { margin-top: 48px; }\n"
           "  h2 { margin-top: 2.4em; font-size: 1.4rem; }\n"
           "  code { font-family: ui-monospace, \"Cascadia Code\", Consolas, monospace; background: var(--code-bg);\n"
           "         padding: 1px 6px; border-radius: 4px; font-size: 0.92em; }\n"
           "  .numbers { display: flex; flex-wrap: wrap; gap: 14px; margin: 20px 0; }\n"
           "  .numbers .stat { flex: 1 1 180px; background: var(--panel); border: 1px solid var(--line);\n"
           "                   border-radius: 10px; padding: 14px 16px; }\n"
           "  .stat .big { font-size: 1.5rem; font-weight: 700; color: var(--blue); display: block; }\n"
           "  .stat .what { color: var(--muted); font-size: 0.92rem; }\n"
           "  p.lens { color: var(--muted); margin: 0 0 12px; }\n"
           "  h2 .stamp { font-size: 0.95rem; font-weight: 500; color: var(--muted); margin-left: 12px; }\n"
           "  .cards { display: flex; flex-wrap: wrap; gap: 14px; }\n"
           "  .card { flex: 0 0 342px; background: var(--panel); border: 1px solid var(--line); border-radius: 10px;\n"
           "          padding: 10px 10px 4px; }\n"
           "  .card img { display: block; width: 320px; height: 240px; border: 1px solid var(--line); border-radius: 6px;\n"
           "              background: #fff; }\n"
           "  .card .nopic { width: 320px; height: 240px; border: 1px dashed var(--line); border-radius: 6px;\n"
           "                 color: var(--muted); text-align: center; line-height: 240px; }\n"
           "  .card p { margin: 6px 0; line-height: 1.4; }\n"
           "  .card .site { font-weight: 650; }\n"
           "  .card .site a { color: var(--navy); text-decoration: none; }\n"
           "  .card .rank { color: var(--muted); font-weight: 400; font-size: 0.9rem; }\n"
           "  .card .title { font-size: 0.95rem; }\n"
           "  .card .note, .card .facts, .card .asks { color: var(--muted); font-size: 0.86rem; }\n"
           "  table { border-collapse: collapse; width: 100%; margin: 20px 0; }\n"
           "  th, td { text-align: left; padding: 8px 10px; border-bottom: 1px solid var(--line); }\n"
           "  th { color: var(--muted); font-weight: 600; font-size: 0.92rem; }\n"
           "  td.num { text-align: right; font-variant-numeric: tabular-nums; white-space: nowrap; }\n"
           "  .bar { background: var(--code-bg); border-radius: 4px; height: 10px; width: 100%; min-width: 120px; }\n"
           "  .bar span { display: block; height: 10px; border-radius: 4px; background: var(--accent); }\n"
           "  .card.greyed img, .card.greyed .title { opacity: 0.45; }\n"
           "  .pill { font-size: 0.8rem; font-weight: 600; padding: 1px 8px; border-radius: 10px; white-space: nowrap; }\n"
           "  .pill.ok { color: var(--ok); background: var(--ok-bg); }\n"
           "  .pill.refused { color: var(--blue); background: var(--code-bg); }\n"
           "  .pill.warn { color: var(--warn); background: var(--warn-bg); }\n"
           "  .pill.bad { color: var(--bad); background: var(--bad-bg); }\n"
           "  .pill.muted { color: var(--muted); background: var(--code-bg); }\n"
           "  p.muted { color: var(--muted); font-size: 0.95rem; }\n"
           "  footer { margin-top: 64px; padding-top: 20px; border-top: 1px solid var(--line); color: var(--muted);\n"
           "           font-size: 0.92rem; text-align: center; }\n"
           "</style>\n</head>\n<body>\n<main>\n"
           "<p><a href=\"../\">← sashfold.com</a></p>\n"
           "<h1>The Sashfold 100</h1>\n"
           "<p>A hundred pages of the web, fetched and rendered every night by Sashfold itself — its own HTTP, TLS policy, "
           "HTML and CSS parsers, layout, fonts and image decoders — at a 1024×768 viewport, with no scripts run because "
           "none can be yet. Each card links to the full picture (cut at 2400 px) and says what the load cost. "
           "The sites come from the <a href=\""
        << html_escaped(corpus.list_url) << "\">Tranco list " << html_escaped(corpus.list_id) << "</a> of "
        << html_escaped(corpus.list_date)
        << ", the research ranking of the web's most visited domains; the number after each site is its rank there. "
           "Nothing here is retouched: a page that arrives blank without scripts is shown blank.</p>\n";
    out << "<div class=\"numbers\">\n"
        << "  <div class=\"stat\"><span class=\"big\">" << totals.loaded << " / " << totals.rows
        << "</span><span class=\"what\">pages answered 200 and rendered with text</span></div>\n"
        << "  <div class=\"stat\"><span class=\"big\">" << html_escaped(seconds(totals.median_ms))
        << "</span><span class=\"what\">median time from request to pixels, over those</span></div>\n"
        << "  <div class=\"stat\"><span class=\"big\">" << totals.refused << " / " << totals.refusals_expected
        << "</span><span class=\"what\">bad certificates refused</span></div>\n"
        << "  <div class=\"stat\"><span class=\"big\">" << html_escaped(stamp)
        << "</span><span class=\"what\">last rendered, on a Windows runner</span></div>\n"
        << "</div>\n";
    std::vector<FeatureTotal> const asked = feature_totals(corpus.rows);
    if (!asked.empty()) {
        long pages_with_report = 0;
        for (Row const& row : corpus.rows)
            if (row.outcome == "document")
                ++pages_with_report;
        out << "<h2>What the hundred ask for that is not written yet</h2>\n"
               "<p class=\"lens\">Every page's stylesheets are read for the features Sashfold does not do yet, "
               "counted as declarations; a row's card names its three biggest asks. This is the honest reason a "
               "page looks wrong, and the order the engine work goes in: by pages asking.</p>\n"
               "<table>\n<thead><tr><th>Feature</th> <th></th> <th>Pages asking</th> <th>Declarations</th></tr></thead>\n<tbody>\n";
        // Spaces between the cells: a table renders as one, and an engine
        // without tables (this one, today) keeps the numbers apart.
        for (FeatureTotal const& total : asked) {
            int const width = pages_with_report == 0 ? 0 : static_cast<int>(100 * total.pages / pages_with_report);
            out << "<tr><td>" << html_escaped(feature_label(total.key)) << "</td> <td><div class=\"bar\"><span style=\"width: "
                << width << "%\"></span></div></td> <td class=\"num\">" << total.pages << " pages</td> <td class=\"num\">"
                << total.declarations << " declarations</td></tr>\n";
        }
        out << "</tbody>\n</table>\n";
    }
    std::string const arizona = latest_render_arizona(corpus.rows);
    bool first_category = true;
    for (Category const& category : corpus.categories) {
        out << "<h2>" << html_escaped(category.title);
        // The freshness of the pictures, by the first heading — Arizona's
        // clock, for the reader who compares them from there.
        if (first_category && !arizona.empty())
            out << " <span class=\"stamp\">pictures rendered " << html_escaped(arizona) << " Arizona time</span>";
        first_category = false;
        out << "</h2>\n<p class=\"lens\">" << html_escaped(category.description) << "</p>\n<div class=\"cards\">\n";
        for (Row const& row : corpus.rows)
            if (row.category == category.id)
                write_card(out, row);
        out << "</div>\n";
    }
    out << "<p class=\"muted\">How to read a card: the pill is the HTTP status when a page arrived (200 with text on the page "
           "counts as loaded; 200 with no text is a page that draws itself with scripts), <em>certificate refused</em> when the "
           "connection was turned away — the right answer for the broken certificates, a wrong one anywhere else — and "
           "<em>unreachable</em>, <em>timed out</em> or <em>crashed</em> when nothing rendered. The time is the whole of it: "
           "request, stylesheets, pictures, parse, style, layout and paint, one page per process, from a GitHub Actions runner. "
           "Rows greyed out use right-to-left scripts, which the engine does not shape yet. "
           "The corpus is <code>tests/sashfold100/corpus.txt</code> in the repository; <code>tools/sashfold100.sh</code> renders it.</p>\n"
           "<footer><p>BSD-2 licensed · <a href=\"https://github.com/codingncaffeine/Sashfold\">github.com/codingncaffeine/Sashfold</a></p>\n"
           "<p>This page is generated HTML with no scripts, no trackers, and no external requests.</p></footer>\n"
           "</main>\n</body>\n</html>\n";
}

void write_json(std::filesystem::path const& path, Corpus const& corpus, Totals const& totals,
    std::string const& stamp)
{
    std::ofstream out(path, std::ios::binary);
    out << "{\n  \"list\": { \"id\": \"" << json_escaped(corpus.list_id) << "\", \"date\": \""
        << json_escaped(corpus.list_date) << "\", \"url\": \"" << json_escaped(corpus.list_url) << "\" },\n"
        << "  \"rendered\": \"" << json_escaped(stamp) << "\",\n"
        << "  \"rows\": " << totals.rows << ",\n  \"loaded\": " << totals.loaded << ",\n  \"refused\": "
        << totals.refused << ",\n  \"refusals_expected\": " << totals.refusals_expected
        << ",\n  \"median_ms\": " << totals.median_ms << ",\n  \"asks\": [\n";
    std::vector<FeatureTotal> const asked = feature_totals(corpus.rows);
    for (std::size_t i = 0; i < asked.size(); ++i) {
        out << "    { \"feature\": \"" << json_escaped(asked[i].key) << "\", \"pages\": " << asked[i].pages
            << ", \"declarations\": " << asked[i].declarations << " }" << (i + 1 < asked.size() ? "," : "") << "\n";
    }
    out << "  ],\n  \"pages\": [\n";
    for (std::size_t i = 0; i < corpus.rows.size(); ++i) {
        Row const& row = corpus.rows[i];
        Verdict const verdict = verdict_of(row);
        out << "    { \"id\": \"" << json_escaped(row.id) << "\", \"category\": \"" << json_escaped(row.category)
            << "\", \"rank\": \"" << json_escaped(row.rank) << "\", \"url\": \"" << json_escaped(row.url)
            << "\", \"outcome\": \"" << json_escaped(row.outcome) << "\", \"verdict\": \"" << json_escaped(verdict.label)
            << "\", \"status\": " << row.status << ", \"final_url\": \"" << json_escaped(row.final_url)
            << "\", \"title\": \"" << json_escaped(row.title) << "\", \"ms_total\": " << row.ms_total
            << ", \"page_height\": " << row.page_height << ", \"characters\": " << row.characters
            << ", \"stylesheets\": " << row.stylesheets << ", \"images\": " << row.images << " }"
            << (i + 1 < corpus.rows.size() ? "," : "") << "\n";
    }
    out << "  ]\n}\n";
}

} // namespace

int main(int argc, char** argv)
{
    std::vector<std::string> const args(argv + 1, argv + argc);
    std::string corpus_path;
    std::string renders;
    std::string html_path;
    std::string json_path;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--html" && i + 1 < args.size())
            html_path = args[++i];
        else if (args[i] == "--json" && i + 1 < args.size())
            json_path = args[++i];
        else if (corpus_path.empty())
            corpus_path = args[i];
        else if (renders.empty())
            renders = args[i];
        else {
            std::cerr << "usage: sashfold100 <corpus.txt> <renders-dir> [--html <file>] [--json <file>]\n";
            return 2;
        }
    }
    if (corpus_path.empty() || renders.empty()) {
        std::cerr << "usage: sashfold100 <corpus.txt> <renders-dir> [--html <file>] [--json <file>]\n";
        return 2;
    }
    std::optional<Corpus> corpus = read_corpus(corpus_path);
    if (!corpus)
        return 1;
    std::filesystem::path const dir(renders);
    for (Row& row : corpus->rows)
        read_report(row, dir / (row.id + ".json"));
    Totals const totals = totals_of(corpus->rows);
    std::string const stamp = now_utc();
    write_html(html_path.empty() ? dir / "index.html" : std::filesystem::path(html_path), *corpus, totals, stamp);
    write_json(json_path.empty() ? dir / "sashfold100.json" : std::filesystem::path(json_path), *corpus, totals, stamp);
    std::cout << "sashfold100: " << totals.loaded << " / " << totals.rows << " loaded, " << totals.refused << " / "
              << totals.refusals_expected << " bad certificates refused, median " << seconds(totals.median_ms)
              << " to pixels\n";
    for (Row const& row : corpus->rows) {
        Verdict const verdict = verdict_of(row);
        if (row.outcome != "not run" && (verdict.kind == "bad" || verdict.kind == "muted"))
            std::cout << "  " << row.id << ": " << verdict.label << (row.error.empty() ? "" : " — " + row.error) << "\n";
    }
    return 0;
}
