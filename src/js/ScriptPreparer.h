#pragma once

// Scripts parsed off the page's thread, as V8 parses every script it can on
// a background thread: the host offers a script's bytes the moment they
// arrive, under the script's address, and once the page's heap is attached
// each offered script is decoded and parsed on a thread of a small pool, its
// names interned through Heap::atom_from_any_thread. When the page comes to
// run that address it takes the parsed program instead of parsing — only
// when the bytes it runs are the bytes that were offered, so a response the
// page's own request got differently is parsed afresh. A parse that failed
// is not handed over: the page parses again and reports the error as it
// always has.
//
// Shared between the host's loader threads (offer) and the page's thread
// (attach, take, detach). The heap must outlive the attachment: detach()
// waits for the parses running and drops the rest, and the interpreter
// detaches before its heap goes.

#include "js/Ast.h"
#include "js/Parser.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace sashfold::js {

class Heap;

class ScriptPreparer {
public:
    ScriptPreparer();
    ~ScriptPreparer();
    ScriptPreparer(ScriptPreparer const&) = delete;
    ScriptPreparer& operator=(ScriptPreparer const&) = delete;

    // Any thread: a classic script's bytes (UTF-8) under its address. The
    // first offer of an address stands; a byte-order mark is not source.
    void offer(std::string name, std::string utf8);
    // The page's thread: the heap the programs are parsed for and the
    // options its own parse uses; what was offered before starts now.
    // False when it is attached already, or was detached: one heap only.
    bool attach(Heap&, ParseOptions);
    // The page's thread: running parses are waited for, the others dropped,
    // and nothing offered later is parsed.
    void detach();

    struct Taken {
        std::unique_ptr<Program> program;
        std::size_t code_units = 0; // the source's length in UTF-16
        double parse_ms = 0; // decoding and parsing, on the pool's thread
        double waited_ms = 0; // what the page's thread waited for it
    };
    // The page's thread: the program parsed from exactly these bytes (a
    // byte-order mark aside) under this name, waited for while it is still
    // parsing; nothing when none was offered, the bytes differ, the parse
    // failed, or it is not attached to this heap.
    std::optional<Taken> take(Heap&, std::string const& name, std::string_view utf8);

    struct Shared;

private:
    std::shared_ptr<Shared> m_shared;
};

}
