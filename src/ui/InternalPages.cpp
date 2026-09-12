#include "ui/InternalPages.h"

namespace sashfold::ui {

namespace {

// The style every internal page shares — the supported property set only, so it
// renders exactly on today's engine. Content colors are not themed: these
// are pages, not chrome.
constexpr std::string_view page_style = R"(<style>
html { background-color: #f7f7f5 }
body { margin: 0; padding: 48px 56px; color: #1d1f24; font-size: 16px; line-height: 1.5 }
h1 { font-size: 28px; margin: 0 0 16px 0 }
h2 { font-size: 18px; margin: 28px 0 8px 0 }
p { margin: 0 0 12px 0 }
.url { color: #5d6470; font-size: 14px; word-break: break-all }
.box { background-color: #ffffff; border: 1px solid #d9dbe0; padding: 20px 24px; margin: 0 0 20px 0 }
.warn { background-color: #fbeeed; border: 1px solid #e8b4ae; padding: 20px 24px; margin: 0 0 20px 0 }
.muted { color: #5d6470 }
pre { font-size: 13px; line-height: 1.4; white-space: pre-wrap; margin: 0 }
a { color: #1f5fbf }
</style>)";

std::string wrap(std::string_view title, std::string_view body)
{
    std::string page = "<!doctype html><html><head><meta charset=utf-8><title>";
    page += html_escape(title);
    page += "</title>";
    page += page_style;
    page += "</head><body>";
    page += body;
    page += "</body></html>";
    return page;
}

std::string bytes_as_text(std::vector<std::uint8_t> const& bytes)
{
    return std::string(bytes.begin(), bytes.end());
}

} // namespace

std::string html_escape(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (char const c : text) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        default: out += c; break;
        }
    }
    return out;
}

std::string error_page(std::string_view heading, std::string_view detail, std::string_view url)
{
    std::string body = "<h1>" + html_escape(heading) + "</h1>";
    body += "<div class=box><p>" + html_escape(detail) + "</p>";
    body += "<p class=url>" + html_escape(url) + "</p></div>";
    body += "<p class=muted>Check the address, then try again with the reload button or F5.</p>";
    return wrap(heading, body);
}

std::string certificate_error_page(std::string_view host, std::string_view url)
{
    std::string body = "<h1>Sashfold stopped this connection</h1>";
    body += "<div class=warn><p><b>" + html_escape(host)
        + "</b> presented a certificate that did not check out, so Sashfold could not verify "
          "that you are talking to the real site. Someone could be intercepting the connection, "
          "or the site is misconfigured.</p>";
    body += "<p>Nothing was sent. There is no way to continue past this page in this version, "
            "on purpose.</p>";
    body += "<p class=url>" + html_escape(url) + "</p></div>";
    body += "<p class=muted>Expired, self-signed, wrong-host, and untrusted-root certificates all "
            "land here; if this is your own site, fix the certificate rather than the browser.</p>";
    return wrap("Connection not secure", body);
}

std::string about_sashfold_page()
{
    std::string body = "<h1>Sashfold</h1>";
    body += "<p>Version " + std::string(version_string) + "</p>";
    body += "<div class=box><p>A web browser engine written from scratch, every byte, for "
            "Windows, Linux, and macOS. HTML, CSS, layout, text, fonts, images, networking, "
            "TLS on Linux, and the JavaScript engine are all written in this repository; the "
            "operating system's own interfaces and the language runtime are the only things "
            "beneath it.</p>";
    body += "<p>No telemetry. No sponsored tiles. No default-search auction. No account. No cloud "
            "AI. No self-updater.</p></div>";
    body += "<h2>Where things stand</h2>";
    body += "<p>Pages load over HTTP and HTTPS through the engine's own fetch pipeline, with a "
            "cookie jar that blocks third-party cookies by default and a session cache. Themes "
            "are data: every color and size of this window comes from "
            "<code>themes/default.json</code>.</p>";
    body += "<h2>Pages</h2>";
    body += "<p><a href=\"about:blank\">about:blank</a> &middot; view-source: in front of any "
            "URL shows its source.</p>";
    body += "<p class=muted><a href=\"https://sashfold.com/\">sashfold.com</a> &middot; "
            "<a href=\"https://github.com/codingncaffeine/Sashfold\">source on GitHub</a></p>";
    return wrap("About Sashfold", body);
}

std::string blocked_page(std::string_view url, std::string_view list, std::string_view rule, bool nefarious)
{
    std::string body = nefarious ? "<h1>Sashfold kept you off this site</h1>" : "<h1>Sashfold blocked this page</h1>";
    body += "<p class=url>" + html_escape(url) + "</p>";
    if (nefarious) {
        body += "<div class=warn><p>A list of phishing and malware sites on this computer names it: <b>"
            + html_escape(list) + "</b>, the line <code>" + html_escape(rule)
            + "</code>. Sashfold consulted no service to decide this; the list is a file of yours, "
              "read here, and nothing about this visit left the machine.</p></div>";
        body += "<p>If the list is wrong, remove the line from the file in the blocklists folder's "
                "<code>nefarious</code> directory and try again. There is no way through from this page.</p>";
    } else {
        body += "<div class=box><p>A filter list on this computer blocks it as a page: <b>" + html_escape(list)
            + "</b>, the rule <code>" + html_escape(rule) + "</code>.</p></div>";
        body += "<p>Filter lists live in the blocklists folder's <code>filters</code> directory; edit or "
                "remove the list and try again.</p>";
    }
    return wrap(nefarious ? "Sashfold kept you off this site" : "Sashfold blocked this page", body);
}

namespace {

std::string hex(Color color)
{
    static constexpr char digits[] = "0123456789abcdef";
    std::string out = "#";
    for (std::uint8_t const value : { color.r, color.g, color.b }) {
        out += digits[value >> 4];
        out += digits[value & 15];
    }
    return out;
}

// A JavaScript string literal that is also safe inside a <script> element.
std::string js_string(std::string_view text)
{
    std::string out = "\"";
    for (unsigned char const c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c == '<') {
            out += "\\x3c";
        } else if (c == '>') {
            out += "\\x3e";
        } else if (c == '&') {
            out += "\\x26";
        } else if (c < 0x20) {
            static constexpr char digits[] = "0123456789abcdef";
            out += "\\u00";
            out += digits[c >> 4];
            out += digits[c & 15];
        } else {
            out += static_cast<char>(c);
        }
    }
    out += '"';
    return out;
}

std::string greeting_for(int hour)
{
    if (hour < 5)
        return "Good night";
    if (hour < 12)
        return "Good morning";
    if (hour < 18)
        return "Good afternoon";
    if (hour < 22)
        return "Good evening";
    return "Good night";
}

std::string two_digits(int value)
{
    return (value < 10 ? "0" : "") + std::to_string(value);
}

} // namespace

std::string new_tab_page(NewTabPage const& page)
{
    std::string html = "<!doctype html><html><head><meta charset=utf-8><title>New Tab</title><style>\n";
    html += "html, body { margin: 0; height: 100% }\n";
    html += "body { background-color: " + hex(page.background) + "; background-image: linear-gradient(160deg, "
        + hex(page.background) + ", " + hex(page.background_end) + "); color: " + hex(page.text)
        + "; font-family: sans-serif; overflow: hidden }\n";
    html += ".bg { position: absolute; left: 0; top: 0; right: 0; bottom: 0; background-size: cover; "
            "background-position: center; background-repeat: no-repeat }\n";
    html += "#b1 { opacity: 0 }\n";
    html += ".scrim { position: absolute; left: 0; top: 0; right: 0; bottom: 0; background-color: rgba(0, 0, 0, 0.25) }\n";
    html += ".center { position: absolute; left: 0; top: 0; right: 0; bottom: 0; display: flex; flex-direction: column; "
            "align-items: center; justify-content: center; text-align: center }\n";
    html += ".clock { font-size: 96px; font-weight: bold; letter-spacing: 2px; line-height: 1 }\n";
    html += ".greeting { font-size: 28px; margin-top: 18px }\n";
    html += ".date { font-size: 16px; color: " + hex(page.text_muted) + "; margin-top: 8px }\n";
    html += "</style></head><body data-hour=\"" + std::to_string(page.hour) + "\" data-minute=\""
        + std::to_string(page.minute) + "\" data-next-minute-ms=\"" + std::to_string(page.next_minute_ms) + "\">";
    if (!page.pictures.empty()) {
        // Two picture layers — the next one fades in over the one showing —
        // and a scrim so the words read over any picture.
        html += "<div class=bg id=b0 style=\"background-image: url(&quot;" + html_escape(page.pictures.front())
            + "&quot;)\"></div><div class=bg id=b1></div><div class=scrim></div>";
    }
    html += "<div class=center><div class=clock id=clock>" + two_digits(page.hour) + ":" + two_digits(page.minute)
        + "</div><div class=greeting id=greeting>" + greeting_for(page.hour) + "</div><div class=date>"
        + html_escape(page.date) + "</div></div>";
    // The page keeps its own time from the minute it opened, on the page's
    // timers: no Date, so the replay's virtual clock drives it too.
    html += "<script>(function () {\n"
            "var body = document.body, hour = Number(body.getAttribute('data-hour')), minute = Number(body.getAttribute('data-minute'));\n"
            "var clock = document.getElementById('clock'), greeting = document.getElementById('greeting');\n"
            "function pad(n) { return (n < 10 ? '0' : '') + n; }\n"
            "function greet(h) { return h < 5 ? 'Good night' : h < 12 ? 'Good morning' : h < 18 ? 'Good afternoon' : h < 22 ? 'Good evening' : 'Good night'; }\n"
            "function tick() { minute += 1; if (minute === 60) { minute = 0; hour = (hour + 1) % 24; } "
            "clock.textContent = pad(hour) + ':' + pad(minute); greeting.textContent = greet(hour); setTimeout(tick, 60000); }\n"
            "setTimeout(tick, Number(body.getAttribute('data-next-minute-ms')));\n";
    if (page.pictures.size() > 1 && page.rotate_ms > 0) {
        html += "var pictures = [";
        for (std::size_t i = 0; i < page.pictures.size(); ++i)
            html += (i == 0 ? "" : ", ") + js_string(page.pictures[i]);
        html += "], next = 1, under = document.getElementById('b0'), over = document.getElementById('b1');\n"
                "function css(url, opacity) { return 'background-image: url(\"' + url + '\"); opacity: ' + opacity; }\n"
                // Twenty-five steps of forty milliseconds: a second's crossfade
                // on the top layer, then the bottom layer takes the picture
                // over and the top one clears for the next.
                "function rotate() { var url = pictures[next]; next = (next + 1) % pictures.length; var step = 0;\n"
                "  function fade() { step += 1; over.setAttribute('style', css(url, step / 25));\n"
                "    if (step < 25) { setTimeout(fade, 40); } else { under.setAttribute('style', css(url, 1)); "
                "over.setAttribute('style', css(url, 0)); setTimeout(rotate, "
            + std::to_string(page.rotate_ms) + "); } }\n"
                                               "  setTimeout(fade, 40); }\n"
                                               "setTimeout(rotate, "
            + std::to_string(page.rotate_ms) + ");\n";
    }
    html += "})();</script></body></html>";
    return html;
}

std::string source_page(std::string_view url, std::vector<std::uint8_t> const& bytes)
{
    std::string body = "<p class=url>view-source: " + html_escape(url) + "</p>";
    body += "<div class=box><pre>" + html_escape(bytes_as_text(bytes)) + "</pre></div>";
    return wrap("view-source:" + std::string(url), body);
}

std::string text_page(std::string_view title, std::vector<std::uint8_t> const& bytes)
{
    return wrap(title, "<pre>" + html_escape(bytes_as_text(bytes)) + "</pre>");
}

std::string unsupported_content_page(std::string_view url, std::string_view content_type,
    std::size_t byte_count)
{
    std::string body = "<h1>Sashfold can't show this</h1>";
    body += "<div class=box><p>The server sent <b>" + html_escape(content_type) + "</b> ("
        + std::to_string(byte_count) + " bytes). This version renders HTML and plain text, and "
          "there is no downloads folder to save anything else to, so nothing was written to "
          "disk.</p>";
    body += "<p class=url>" + html_escape(url) + "</p></div>";
    return wrap("Unsupported content", body);
}

std::string download_page(std::string_view file_name, std::string_view path,
    std::size_t byte_count, std::string_view content_type, bool marked)
{
    std::string body = "<h1>Downloaded</h1>";
    body += "<div class=box><p><b>" + html_escape(file_name) + "</b>, " + std::to_string(byte_count)
        + " bytes of " + html_escape(content_type) + "</p>";
    body += "<p class=url>" + html_escape(path) + "</p>";
    if (marked)
        body += "<p class=muted>Marked as downloaded from the Internet, so the system treats it "
                "with the usual caution.</p>";
    body += "</div><p class=muted>Sashfold never opens what it downloads. Open it yourself, "
            "from the folder, when you mean to.</p>";
    return wrap("Downloaded " + std::string(file_name), body);
}

}
