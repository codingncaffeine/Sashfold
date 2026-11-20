#include "platform/Confine.h"

// Windows' half of platform/Confine.h, as far as one process can go.
//
// The right to start a program is given up with a job object: a job that
// holds this process alone and allows it no second one. Every way of
// starting a program — CreateProcess, ShellExecute, WinExec, the C
// library's spawn — makes the new process inside its maker's job, and the
// job refuses it. The job has no name and its handle is closed once the
// process is in it, so nothing in the process can reach the job again to
// widen it, and a process cannot leave its job (breaking away is not
// allowed either). A job inside the job a launcher or a build runner put us
// in is fine from Windows 8 on.
//
// The limit is then tried, once: this program started again, suspended. A
// system that holds the job to its limit refuses that; one that does not
// (Wine keeps a job's limit without enforcing it) gets the suspended process
// ended before it has run, and the call answers that nothing was given up,
// so that the answer is what the system does and not what it was asked.
//
// What is not here. The calls Linux's filter also refuses (tracing or
// reading another process and the rest) and the rule that keeps writes to
// the profile and the downloads folder both need a token with fewer rights
// than the user's, and a window made with such a token can no longer take
// a file dragged from the desktop or show the system's file dialog: on
// Windows they wait for the split into processes, where the engine gets
// that token and the window keeps the user's.

#include <mutex>
#include <string>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace sashfold::platform {

namespace {

std::string last_error(char const* what)
{
    return std::string(what) + " failed (error " + std::to_string(GetLastError()) + ")";
}

// Whether a program can still be started: this one, suspended, and ended
// before it runs if it was made.
bool starts_a_program()
{
    wchar_t self[32768];
    DWORD const length = GetModuleFileNameW(nullptr, self, 32768);
    if (length == 0 || length >= 32768)
        return false;
    STARTUPINFOW startup {};
    startup.cb = sizeof startup;
    PROCESS_INFORMATION started {};
    if (!CreateProcessW(self, nullptr, nullptr, nullptr, FALSE, CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr, &startup, &started))
        return false;
    TerminateProcess(started.hProcess, 0);
    CloseHandle(started.hThread);
    CloseHandle(started.hProcess);
    return true;
}

}

bool forbid_new_programs(std::string* why_not)
{
    static std::mutex guard;
    static bool given_up = false;
    std::lock_guard<std::mutex> const lock(guard);
    if (given_up)
        return true;
    HANDLE const job = CreateJobObjectW(nullptr, nullptr);
    if (job == nullptr) {
        if (why_not != nullptr)
            *why_not = last_error("CreateJobObject");
        return false;
    }
    JOBOBJECT_BASIC_LIMIT_INFORMATION limits {};
    limits.LimitFlags = JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
    limits.ActiveProcessLimit = 1;
    bool ok = SetInformationJobObject(job, JobObjectBasicLimitInformation, &limits, sizeof limits) != 0;
    if (!ok && why_not != nullptr)
        *why_not = last_error("SetInformationJobObject");
    if (ok) {
        ok = AssignProcessToJobObject(job, GetCurrentProcess()) != 0;
        if (!ok && why_not != nullptr)
            *why_not = last_error("AssignProcessToJobObject");
    }
    // Closed either way: a job with the process in it lives as long as the
    // process does, and without a handle or a name nothing can change it.
    CloseHandle(job);
    if (ok && starts_a_program()) {
        ok = false;
        if (why_not != nullptr)
            *why_not = "this system does not hold a job to its limit of one process";
    }
    given_up = ok;
    return ok;
}

bool keep_writes_to(std::vector<std::string> const&, std::string* why_not)
{
    if (why_not != nullptr)
        *why_not = "not written for this system yet: it needs the split into processes";
    return false;
}

}
