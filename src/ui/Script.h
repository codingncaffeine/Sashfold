#pragma once

// --script replay: deterministic event injection into the OS-free
// shell. It is at once the shell-test harness — the same script runs on
// every OS in CI and its chrome screenshots are byte-identical — and the
// proof of the agent-legibility claim: a browser you can drive, and read
// back, from a text file.
//
// One command per line; # starts a comment. Relative paths resolve against
// the script's own directory.
//
//   open <url|path>      navigate (a bare path becomes a file: URL)   tick
//   navigate <typed>     address-bar semantics (https-first for bare hosts)
//   click <x> <y>        left click at window coordinates
//   middle-click <x> <y>
//   click-text <text>    click the center of the first run containing text
//   move <x> <y>         hover
//   wheel <notches>      scroll the content (positive rolls up)
//   type <text>          type into whatever has focus
//   key <chord>          e.g. Enter, Escape, ctrl+l, alt+left, ctrl+shift+tab
//   back | forward | reload | new-tab | close-tab [n] | select-tab <n>
//   resize <w> <h>
//   screenshot <path>    write the frame as PNG
//   assert-golden <png>  frame bytes equal the file (--update-goldens blesses)
//   assert-url <url>     assert-address <text>   assert-title <text>
//   assert-text <text>   assert-no-text <text>   assert-status <text>
//   assert-tabs <n>      assert-scroll <n>       assert-scrolled
//   assert-pixel <x> <y> <#rrggbb>
//   assert-focus address|page
//   focus <name>         focus the first form control with that name
//   assert-value <name> <text>   the control's current value
//   assert-focused <name>        the focused control's name ("" = none)
//   drag <x1> <y1> <x2> <y2>     press, move, release the left button
//   select-text <text>   select the first run's occurrence of text
//   assert-selection <text>      the selected text (one line)
//   echo <text>

#include "ui/Browser.h"

#include <iosfwd>
#include <string>

namespace sashfold::ui {

struct ScriptResult {
    int commands = 0;
    int failures = 0;
    bool ok() const { return failures == 0; }
};

ScriptResult run_script(Browser& browser, std::string const& path, bool update_goldens,
    std::ostream& out);

}
