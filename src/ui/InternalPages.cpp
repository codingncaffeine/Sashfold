#include "ui/InternalPages.h"

namespace sashfold::ui {

namespace {

// The style every internal page shares — the M2 property set only, so it
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
    body += "<p>M3: it's a browser now. Pages load over HTTP and HTTPS through the engine's own "
            "fetch pipeline with a cookie jar that blocks third-party cookies by default and a "
            "session cache. Themes are data: every color and size of this window comes from "
            "<code>themes/default.json</code>.</p>";
    body += "<h2>Pages</h2>";
    body += "<p><a href=\"about:blank\">about:blank</a> &middot; view-source: in front of any "
            "URL shows its source.</p>";
    body += "<p class=muted><a href=\"https://sashfold.com/\">sashfold.com</a> &middot; "
            "<a href=\"https://github.com/codingncaffeine/Sashfold\">source on GitHub</a></p>";
    return wrap("About Sashfold", body);
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
    std::string body = "<h1>Sashfold can't show this yet</h1>";
    body += "<div class=box><p>The server sent <b>" + html_escape(content_type) + "</b> ("
        + std::to_string(byte_count) + " bytes). This version renders HTML and plain text; "
          "images, downloads, and other kinds of content arrive in later milestones. Nothing "
          "was saved to disk.</p>";
    body += "<p class=url>" + html_escape(url) + "</p></div>";
    return wrap("Unsupported content", body);
}

}
