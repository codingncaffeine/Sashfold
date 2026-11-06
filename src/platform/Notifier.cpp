#include "platform/Notifier.h"

namespace sashfold::platform {

std::string escape_notice_markup(std::string_view text)
{
    std::string escaped;
    escaped.reserve(text.size());
    for (char const c : text) {
        switch (c) {
        case '&': escaped += "&amp;"; break;
        case '<': escaped += "&lt;"; break;
        case '>': escaped += "&gt;"; break;
        default: escaped += c; break;
        }
    }
    return escaped;
}

namespace {

// Text as XML 1.0 holds it, in an element or an attribute: the five
// characters that mean something escaped, and the C0 controls XML cannot
// hold at all (a tab and the line ends excepted) left out.
std::string xml_text(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (char const c : text) {
        unsigned char const byte = static_cast<unsigned char>(c);
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        case '\'': out += "&apos;"; break;
        default:
            if (byte >= 0x20 || c == '\t' || c == '\n' || c == '\r')
                out += c;
            break;
        }
    }
    return out;
}

}

std::string toast_xml(Notice const& notice, std::string_view picture_uri)
{
    std::string xml = notice.stays ? "<toast duration=\"long\">" : "<toast>";
    xml += "<visual><binding template=\"ToastGeneric\">";
    xml += "<text>" + xml_text(notice.title.empty() ? notice.site : notice.title) + "</text>";
    if (!notice.body.empty())
        xml += "<text>" + xml_text(notice.body) + "</text>";
    // Who is speaking, on the line Windows keeps for it.
    if (!notice.site.empty())
        xml += "<text placement=\"attribution\">" + xml_text(notice.site) + "</text>";
    if (!picture_uri.empty())
        xml += "<image placement=\"appLogoOverride\" src=\"" + xml_text(picture_uri) + "\"/>";
    xml += "</binding></visual>";
    if (notice.silent)
        xml += "<audio silent=\"true\"/>";
    xml += "</toast>";
    return xml;
}

}
