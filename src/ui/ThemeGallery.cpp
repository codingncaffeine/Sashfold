#include "ui/ThemeGallery.h"

#include "core/Json.h"
#include "net/Url.h"
#include "ui/InternalPages.h"

#include <algorithm>
#include <cstdint>

namespace sashfold::ui {

namespace {

// A form's value as it arrives in a query: + is a space, %XX a byte.
std::string form_decoded(std::string_view text)
{
    auto const hex = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '+') {
            out += ' ';
        } else if (text[i] == '%' && i + 2 < text.size() && hex(text[i + 1]) >= 0 && hex(text[i + 2]) >= 0) {
            out += static_cast<char>(hex(text[i + 1]) * 16 + hex(text[i + 2]));
            i += 2;
        } else {
            out += text[i];
        }
    }
    return out;
}

// And the other way, for a query of ours: everything but letters, digits
// and - . _ ~ as %XX.
std::string query_encoded(std::string_view text)
{
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string out;
    for (char const c : text) {
        auto const byte = static_cast<unsigned char>(c);
        bool const plain = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9')
            || byte == '-' || byte == '.' || byte == '_' || byte == '~';
        if (plain) {
            out += c;
        } else {
            out += '%';
            out += digits[byte >> 4];
            out += digits[byte & 15];
        }
    }
    return out;
}

std::string trimmed(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
        text.remove_suffix(1);
    return std::string(text);
}

std::string_view valid_sort(std::string_view sort)
{
    return sort == "rating" || sort == "created" ? sort : std::string_view("users");
}

// An add-on's name is a string, or one string per locale.
std::string localized(JsonValue const* value)
{
    if (!value)
        return {};
    if (value->is_string())
        return value->as_string();
    if (!value->is_object())
        return {};
    if (JsonValue const* const english = value->get("en-US"); english && english->is_string())
        return english->as_string();
    for (JsonValue::Member const& member : value->as_object()) {
        if (member.second.is_string())
            return member.second.as_string();
    }
    return {};
}

std::string resolved(std::string const& address, std::optional<net::Url> const& base)
{
    if (address.empty())
        return {};
    std::optional<net::Url> const url = net::parse_url(address, base ? &*base : nullptr);
    return url ? url->serialize() : std::string();
}

std::string thousands(long count)
{
    std::string digits = std::to_string(std::max(0L, count));
    for (std::size_t at = digits.size(); at > 3; at -= 3)
        digits.insert(at - 3, ",");
    return digits;
}

constexpr std::string_view gallery_style = R"(<style>
.notice { background-color: #eef6ee; border: 1px solid #b7d8b7; padding: 12px 16px; margin: 0 0 20px 0 }
.own a, .own b { display: inline-block; margin: 0 8px 8px 0; padding: 6px 12px; border: 1px solid #d9dbe0; background-color: #ffffff; text-decoration: none }
.own b { border-color: #1f5fbf; color: #1f5fbf }
.sorts a { margin: 0 12px 0 0 }
.card { display: inline-block; vertical-align: top; width: 300px; margin: 0 16px 20px 0; background-color: #ffffff; border: 1px solid #d9dbe0 }
.card img { display: block; width: 300px; height: 46px; background-color: #e9eaee }
.card .words { padding: 10px 12px 12px 12px }
.card .name { font-size: 15px; margin: 0 0 2px 0 }
.card .by { font-size: 13px; color: #5d6470; margin: 0 0 8px 0 }
.puton { display: inline-block; padding: 5px 14px; background-color: #1f5fbf; color: #ffffff; text-decoration: none }
input[type=text] { width: 420px; padding: 6px 8px; font-size: 15px }
input[type=submit] { padding: 6px 14px; font-size: 15px }
.pages a, .pages b { margin: 0 10px 0 0 }
</style>)";

}

GalleryQuery gallery_query_of(std::optional<std::string> const& url_query)
{
    GalleryQuery query;
    if (!url_query)
        return query;
    std::string_view rest = *url_query;
    while (!rest.empty()) {
        std::size_t const amp = std::min(rest.find('&'), rest.size());
        std::string_view const pair = rest.substr(0, amp);
        rest.remove_prefix(std::min(amp + 1, rest.size()));
        std::size_t const equals = std::min(pair.find('='), pair.size());
        std::string const name = form_decoded(pair.substr(0, equals));
        std::string const value = form_decoded(equals < pair.size() ? pair.substr(equals + 1) : std::string_view());
        if (name == "q") {
            query.words = trimmed(value);
        } else if (name == "sort") {
            query.sort = std::string(valid_sort(value));
        } else if (name == "page" || name == "use") {
            long number = 0;
            bool digits = !value.empty() && value.size() <= 6;
            for (char const c : value) {
                digits = digits && c >= '0' && c <= '9';
                number = number * 10 + (c - '0');
            }
            if (!digits)
                continue;
            if (name == "page")
                query.page = static_cast<int>(std::clamp(number, 1L, 9999L));
            else
                query.use = static_cast<std::size_t>(number);
        } else if (name == "crx") {
            query.crx = trimmed(value);
        }
    }
    return query;
}

std::string gallery_address(GalleryQuery const& query)
{
    std::string address = "about:themes";
    char separator = '?';
    auto const add = [&](std::string const& member) {
        address += separator;
        address += member;
        separator = '&';
    };
    if (valid_sort(query.sort) != "users")
        add("sort=" + std::string(valid_sort(query.sort)));
    if (!query.words.empty())
        add("q=" + query_encoded(query.words));
    if (query.page > 1)
        add("page=" + std::to_string(query.page));
    return address;
}

std::string amo_search_url(std::string_view api, GalleryQuery const& query)
{
    std::string url(api);
    url += "?type=statictheme&app=firefox&sort=";
    url += valid_sort(query.sort);
    url += "&page_size=24&page=" + std::to_string(std::max(1, query.page));
    if (!query.words.empty())
        url += "&q=" + query_encoded(query.words);
    return url;
}

std::optional<GalleryAnswer> parse_amo_search(std::string_view json, std::string_view source)
{
    std::optional<JsonValue> const parsed = JsonValue::parse(json);
    JsonValue const* const results = parsed ? parsed->get("results") : nullptr;
    if (!results || !results->is_array())
        return std::nullopt;
    std::optional<net::Url> const base = net::parse_url(source);
    GalleryAnswer answer;
    if (JsonValue const* const pages = parsed->get("page_count"); pages && pages->is_number())
        answer.pages = static_cast<int>(std::clamp(pages->as_number(), 1.0, 9999.0));
    for (JsonValue const& result : results->as_array()) {
        if (!result.is_object())
            continue;
        GalleryTheme theme;
        theme.name = localized(result.get("name"));
        if (JsonValue const* const authors = result.get("authors"); authors && authors->is_array() && !authors->as_array().empty())
            theme.author = localized(authors->as_array().front().get("name"));
        if (JsonValue const* const previews = result.get("previews"); previews && previews->is_array() && !previews->as_array().empty()) {
            JsonValue const& first = previews->as_array().front();
            if (JsonValue const* const image = first.get("image_url"); image && image->is_string())
                theme.preview_url = resolved(image->as_string(), base);
        }
        if (JsonValue const* const version = result.get("current_version")) {
            // One file a version since the API's fifth; a list of them before.
            JsonValue const* file = version->get("file");
            if (!file) {
                if (JsonValue const* const files = version->get("files"); files && files->is_array() && !files->as_array().empty())
                    file = &files->as_array().front();
            }
            if (JsonValue const* const url = file ? file->get("url") : nullptr; url && url->is_string())
                theme.file_url = resolved(url->as_string(), base);
        }
        if (JsonValue const* const users = result.get("average_daily_users"); users && users->is_number())
            theme.users = static_cast<long>(std::clamp(users->as_number(), 0.0, 1.0e12));
        if (!theme.name.empty() && !theme.file_url.empty())
            answer.themes.push_back(std::move(theme));
    }
    return answer;
}

std::optional<std::string> chrome_theme_id(std::string_view text)
{
    // The id is the one run of exactly thirty-two letters a to p: in a store
    // address it is a whole segment of the path.
    std::size_t at = 0;
    while (at < text.size()) {
        std::size_t end = at;
        while (end < text.size() && text[end] >= 'a' && text[end] <= 'p')
            ++end;
        bool const bounded_before = at == 0 || !((text[at - 1] >= 'a' && text[at - 1] <= 'z') || (text[at - 1] >= 'A' && text[at - 1] <= 'Z'));
        bool const bounded_after = end == text.size() || !((text[end] >= 'a' && text[end] <= 'z') || (text[end] >= 'A' && text[end] <= 'Z'));
        if (end - at == 32 && bounded_before && bounded_after)
            return std::string(text.substr(at, 32));
        at = end > at ? end : at + 1;
    }
    return std::nullopt;
}

std::string chrome_crx_url(std::string_view id)
{
    // The store's own update service, asked for one item the way a browser
    // asks for its extensions: it answers with a redirect to the .crx.
    return "https://clients2.google.com/service/update2/crx?response=redirect&prodversion=131.0.0.0"
           "&acceptformat=crx2,crx3&x=id%3D"
        + std::string(id) + "%26installsource%3Dondemand%26uc";
}

std::string themes_page(ThemesPage const& page)
{
    std::string body = "<h1>Themes</h1>";
    if (!page.notice.empty())
        body += "<p class=notice>" + html_escape(page.notice) + "</p>";

    body += "<h2>Yours</h2><p class=own>";
    for (std::size_t i = 0; i < page.own.size(); ++i) {
        ThemesPage::Own const& own = page.own[i];
        if (own.current)
            body += "<b>" + html_escape(own.name) + " \xe2\x9c\x93</b>";
        else
            body += "<a href=\"about:themes?use=" + std::to_string(i) + "\">" + html_escape(own.name) + "</a>";
    }
    body += "</p>";
    if (page.own.empty())
        body += "<p class=muted>None yet.</p>";

    body += "<h2>Firefox themes</h2>";
    if (!page.can_adopt)
        body += "<p class=muted>This window has no profile folder to keep a theme in, so themes here can be looked at but not put on.</p>";
    body += "<form action=\"about:themes\"><p><input type=text name=q value=\"" + html_escape(page.query.words)
        + "\"> <input type=submit value=\"Search themes\"></p></form>";
    std::string const words = page.query.words.empty() ? std::string() : "&q=" + query_encoded(page.query.words);
    body += "<p class=sorts>";
    static constexpr std::pair<std::string_view, std::string_view> sorts[]
        = { { "users", "Most used" }, { "rating", "Top rated" }, { "created", "Newest" } };
    for (auto const& [sort, label] : sorts) {
        if (page.query.sort == sort)
            body += "<b>" + std::string(label) + "</b> &nbsp; ";
        else
            body += "<a href=\"about:themes?sort=" + std::string(sort) + words + "\">" + std::string(label) + "</a>";
    }
    body += "</p>";
    if (!page.gallery) {
        body += "<div class=warn><p>The gallery could not be read"
            + (page.gallery_error.empty() ? std::string(".") : ": " + html_escape(page.gallery_error) + ".")
            + "</p></div>";
    } else if (page.gallery->themes.empty()) {
        body += "<p class=muted>No themes by those words.</p>";
    } else {
        body += "<div>";
        for (GalleryTheme const& theme : page.gallery->themes) {
            body += "<div class=card>";
            if (!theme.preview_url.empty())
                body += "<img src=\"" + html_escape(theme.preview_url) + "\" alt=\"\">";
            body += "<div class=words><p class=name>" + html_escape(theme.name) + "</p><p class=by>"
                + (theme.author.empty() ? std::string() : "by " + html_escape(theme.author) + " &middot; ")
                + thousands(theme.users) + " users</p><a class=puton href=\"" + html_escape(theme.file_url)
                + "\">Put on</a></div></div>";
        }
        body += "</div><p class=pages>";
        std::string const sorted = "about:themes?sort=" + page.query.sort + words + "&page=";
        if (page.query.page > 1)
            body += "<a href=\"" + sorted + std::to_string(page.query.page - 1) + "\">&larr; Earlier</a>";
        body += "<b>Page " + std::to_string(page.query.page) + " of " + std::to_string(page.gallery->pages) + "</b>";
        if (page.query.page < page.gallery->pages)
            body += "<a href=\"" + sorted + std::to_string(page.query.page + 1) + "\">More &rarr;</a>";
        body += "</p>";
    }
    body += "<p class=muted>Read from the public catalogue of <a href=\"https://addons.mozilla.org/en-US/firefox/themes/\">"
            "addons.mozilla.org</a> when this page is opened, and at no other time.</p>";

    body += "<h2>Chrome themes</h2>";
    body += "<p>The Chrome Web Store publishes no catalogue to read, so its themes cannot be listed here. "
            "Find one in <a href=\"https://chromewebstore.google.com/category/themes\">the store</a>, and paste "
            "its address (or its id) here:</p>";
    body += "<form action=\"about:themes\"><p><input type=text name=crx value=\"\"> "
            "<input type=submit value=\"Put on\"></p></form>";
    return internal_page("Themes", body, gallery_style);
}

}
