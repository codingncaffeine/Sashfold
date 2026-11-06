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

}
