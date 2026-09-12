#pragma once

// Content blocking: filter lists in the Adblock Plus syntax that EasyList
// is written in, hosts files, and lists of hosts or URLs one per line —
// read from the user's own files and matched against every request the
// shell makes; and the lists' cosmetic rules (`##selector`, with the
// domains they are for and the `#@#` exceptions that lift them), which
// hide elements of a page through a stylesheet the shell adds to it. The
// procedural and scriptlet forms (`#?#`, `#$#`, `#%#`, `+js`) are counted
// and left. Data, not code: a list is a file, updated on the user's
// schedule, and no lookup ever leaves the machine. The same engine,
// pointed at a second folder of lists, keeps the shell away from sites
// named as phishing or malware: those block navigations too.

#include "net/Url.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace sashfold::net {

// What a request is for, as the rules' type options name it.
enum class ResourceKind : std::uint16_t {
    Document = 1 << 0, // a navigation
    Subdocument = 1 << 1,
    Script = 1 << 2,
    Stylesheet = 1 << 3,
    Image = 1 << 4,
    Font = 1 << 5,
    Xhr = 1 << 6, // fetch() and XMLHttpRequest
    Media = 1 << 7,
    Other = 1 << 8,
};

struct FilterRequest {
    Url const* url = nullptr;
    Url const* first_party = nullptr; // the page the request is for; null for a navigation
    ResourceKind kind = ResourceKind::Other;
};

// The part of a host a site is known by — the last two labels, or three
// under a two-letter country code's public second level (co.uk, com.au);
// an IP address whole. What "third-party" is measured against.
std::string registrable_domain(std::string_view host);

// One list. `everything` marks a list of sites to keep away from: its rules
// apply to navigations as well as to what pages request, whatever their
// type options say.
class FilterList {
public:
    struct Counts {
        std::size_t rules = 0; // blocking rules kept
        std::size_t exceptions = 0; // @@ rules kept
        std::size_t cosmetic = 0; // ## and #@# rules kept
        std::size_t unsupported = 0; // regular expressions, options and cosmetic forms this engine does not have
    };

    // An element-hiding rule: a selector, the domains it is for (none: every
    // site) and against, and whether it lifts a hide rather than adding one.
    struct Cosmetic {
        std::string selector;
        std::vector<std::string> domains_in;
        std::vector<std::string> domains_out;
        bool exception = false;
    };

    static FilterList parse(std::string_view text, std::string name, bool everything = false);

    std::string const& name() const { return m_name; }
    Counts const& counts() const { return m_counts; }
    bool everything() const { return m_everything; }

    enum class Verdict { None, Block, Allow };
    struct Decision {
        Verdict verdict = Verdict::None;
        std::string_view rule; // the line, as written
        bool important = false;
    };
    // The rule that decides the request: a block, an exception (which
    // beats a block unless the block is important), or nothing.
    Decision decide(FilterRequest const& request) const;

    std::size_t size() const { return m_rules.size(); }

    // The cosmetic rules that apply on a page at `host`: the hides into
    // `hide`, the exceptions into `lift`, each a selector.
    void cosmetic_for(std::string_view host, std::vector<std::string>& hide, std::vector<std::string>& lift) const;
    std::vector<Cosmetic> const& cosmetic_rules() const { return m_cosmetic; }

private:
    struct Rule {
        std::string text; // as written
        std::string pattern; // anchors stripped; lowercased unless match_case
        bool domain_anchor = false;
        bool start_anchor = false;
        bool end_anchor = false;
        bool match_case = false;
        bool exception = false;
        bool important = false;
        std::uint16_t types = 0; // ResourceKind bits
        int party = -1; // -1 any, 0 first-party only, 1 third-party only
        std::vector<std::string> domains_in;
        std::vector<std::string> domains_out;
    };

    bool add_rule(std::string_view line);
    bool add_cosmetic_rule(std::string_view line);
    void index(std::uint32_t id);
    bool matches(Rule const& rule, FilterRequest const& request, std::string const& url_text,
        std::size_t host_at, std::size_t host_end) const;

    std::string m_name;
    bool m_everything = false;
    Counts m_counts;
    std::vector<Rule> m_rules;
    std::vector<Cosmetic> m_cosmetic;
    // Every rule is filed under one token of its pattern that any URL it
    // matches must contain whole, so a request tries only the rules whose
    // token it carries; rules with no such token are tried every time.
    std::unordered_map<std::string, std::vector<std::uint32_t>> m_by_token;
    std::vector<std::uint32_t> m_untokened;
};

// The session's lists: the filter lists, and the lists of sites to keep
// away from. An exception in any filter list lifts a block from any other,
// as the lists are meant to compose; nothing lifts a nefarious-site block.
class Blocklists {
public:
    // Reads `<directory>/filters/*.txt` as filter lists and
    // `<directory>/nefarious/*.txt` as sites to keep away from, in name
    // order; a file that cannot be read is named in `problems`.
    void load_directory(std::string const& directory, std::vector<std::string>* problems = nullptr);
    void add(FilterList list);

    struct Block {
        std::string rule;
        std::string list;
        bool nefarious = false;
    };
    // The rule that blocks the request and the list it came from, or
    // nullopt when the request may go.
    std::optional<Block> blocks(FilterRequest const& request) const;

    // The selectors to hide on a page at `host`: every list's hides for it,
    // less any selector a list's exception lifts there. In the order the
    // lists and their rules were read, each selector once.
    std::vector<std::string> hidden_selectors(std::string_view host) const;
    std::size_t cosmetic_count() const;

    bool empty() const { return m_lists.empty(); }
    std::size_t list_count() const { return m_lists.size(); }
    std::size_t rule_count() const;

private:
    std::vector<FilterList> m_lists;
};

}
