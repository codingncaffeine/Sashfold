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
// The second piece keeps what it writes to its own folders: the profile, the
// downloads folder and whatever a run was told to write on its command
// line. Everything else the user owns — their documents, their shell's
// start-up files, another program's settings — can be read as before and
// no longer changed, made, renamed or removed.
//
// What neither does: the process can still read every file the user can,
// and reach the network. Those need the split into processes.

#include <string>
#include <vector>

namespace sashfold::platform {

// Gives the rights up. False, with the reason in `why_not` when one is
// asked for, where the system has no such filter, the kernel refuses it,
// or the build is one whose own tools need those calls (a sanitizer's leak
// check traces the process as it ends).
bool forbid_new_programs(std::string* why_not = nullptr);

// Keeps the process's writes, on every thread and for good, to the folders
// named (a file may be named too) and to the few devices a browser writes
// to: the graphics card's, and the one that swallows what it is given.
// Each name must be there already. Writing to a file opened before this is
// not undone, and neither are sockets and pipes: those are not files.
//
// False, with the reason, where the system has no such rule, the kernel is
// too old to have it (Linux before 5.13) or refuses, or a name cannot be
// opened; nothing is given up then. Asking again can only narrow what the
// first asking left.
bool keep_writes_to(std::vector<std::string> const& folders, std::string* why_not = nullptr);

}
