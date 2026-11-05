#pragma once

// What the process gives up for good once it has started, so that a page
// that takes the engine over cannot use it: the first piece of the sandbox
// (the split into processes with rights of their own is the rest).
//
// The browser never starts another program — nothing in it calls exec or
// spawn on any system — so the right to is given up outright, and with it
// the calls a browser has no use for and an intruder does: tracing or
// reading another process, mounting, loading kernel code, changing
// namespaces, the kernel's key store. Every thread is covered, those made
// later included, and nothing can give the rights back.
//
// What this does not do: the process can still open and write the files
// the user can. Keeping it to its own folders is the next piece.

#include <string>

namespace sashfold::platform {

// Gives the rights up. False, with the reason in `why_not` when one is
// asked for, where the system has no such filter, the kernel refuses it,
// or the build is one whose own tools need those calls (a sanitizer's leak
// check traces the process as it ends).
bool forbid_new_programs(std::string* why_not = nullptr);

}
