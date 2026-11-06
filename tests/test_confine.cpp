#include "Test.h"

#include "platform/Confine.h"

#include <string>

#if defined(__linux__)
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#endif

using namespace sashfold;

#if defined(__linux__)
namespace {

bool can_make(std::string const& path)
{
    int const fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0)
        return false;
    bool const wrote = write(fd, "x", 1) == 1;
    close(fd);
    return wrote;
}

bool refused(bool call_failed)
{
    return call_failed && errno == EACCES;
}

bool refused_to_make(std::string const& path)
{
    errno = 0;
    return refused(open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600) < 0);
}

// The writes kept to named folders: `inside` is one, `outside` is not, and
// each holds a file `old` made before. The exit code says which
// expectation failed; 0 is none.
int writes_are_kept(std::string const& inside, std::string const& outside, bool with_an_earlier_thread)
{
    int const held = open((outside + "/held").c_str(), O_WRONLY | O_CREAT, 0600);
    if (held < 0)
        return 30;
    int started[2];
    int verdict[2];
    if (pipe(started) != 0 || pipe(verdict) != 0)
        return 31;
    std::thread earlier;
    if (with_an_earlier_thread) {
        earlier = std::thread([&] {
            char go = 0;
            if (read(started[0], &go, 1) != 1)
                _exit(32);
            char const answer = refused_to_make(outside + "/from-the-earlier-thread") && can_make(inside + "/from-the-earlier-thread") ? 'y' : 'n';
            if (write(verdict[1], &answer, 1) != 1)
                _exit(33);
        });
    }
    std::string why;
    bool const kept = platform::keep_writes_to({ inside }, &why);
    if (with_an_earlier_thread) {
        // A kernel that cannot reach a thread already running must say so
        // and give nothing up, never cover one thread and not the other.
        if (!kept) {
            bool const still_free = can_make(outside + "/not-kept");
            if (write(started[1], "g", 1) != 1)
                return 34;
            earlier.join();
            return !why.empty() && still_free ? 0 : 35;
        }
        if (write(started[1], "g", 1) != 1)
            return 36;
        char answer = 0;
        if (read(verdict[0], &answer, 1) != 1 || answer != 'y')
            return 37;
        earlier.join();
    } else if (!kept) {
        return 38;
    }

    // Inside, everything a profile does: a file made, written whole beside
    // itself and renamed over, cut short, removed; a folder made and removed.
    if (!can_make(inside + "/new") || !can_make(inside + "/new.tmp"))
        return 40;
    if (rename((inside + "/new.tmp").c_str(), (inside + "/new").c_str()) != 0)
        return 41;
    if (truncate((inside + "/new").c_str(), 0) != 0 || unlink((inside + "/new").c_str()) != 0)
        return 42;
    if (mkdir((inside + "/folder").c_str(), 0700) != 0 || !can_make(inside + "/folder/deep")
        || rename((inside + "/folder/deep").c_str(), (inside + "/moved").c_str()) != 0 || rmdir((inside + "/folder").c_str()) != 0)
        return 43;

    // Outside, nothing changes: no file made, opened for writing, cut
    // short, removed or renamed; no folder made or removed; nothing moved
    // out, nothing moved in.
    if (!refused_to_make(outside + "/new"))
        return 50;
    errno = 0;
    if (!refused(open((outside + "/old").c_str(), O_WRONLY) < 0))
        return 51;
    errno = 0;
    if (!refused(open((outside + "/old").c_str(), O_RDWR) < 0))
        return 52;
    errno = 0;
    if (!refused(truncate((outside + "/old").c_str(), 0) != 0))
        return 53;
    errno = 0;
    if (!refused(unlink((outside + "/old").c_str()) != 0))
        return 54;
    errno = 0;
    if (!refused(rename((outside + "/old").c_str(), (outside + "/renamed").c_str()) != 0))
        return 55;
    errno = 0;
    if (!refused(mkdir((outside + "/folder").c_str(), 0700) != 0))
        return 56;
    errno = 0;
    if (!refused(rmdir((outside + "/empty").c_str()) != 0))
        return 57;
    errno = 0;
    if (!refused(symlink("/etc/passwd", (outside + "/link").c_str()) != 0))
        return 58;
    // Across the line the kernel answers "another file system" or "no
    // right", by its version; either way nothing moves.
    if (rename((inside + "/moved").c_str(), (outside + "/moved").c_str()) == 0)
        return 59;
    if (rename((outside + "/old").c_str(), (inside + "/taken").c_str()) == 0)
        return 60;
    if (link((outside + "/old").c_str(), (inside + "/linked").c_str()) == 0)
        return 61;

    // Reading is what it was.
    char byte = 0;
    int const reading = open((outside + "/old").c_str(), O_RDONLY);
    if (reading < 0 || read(reading, &byte, 1) != 1 || byte != 'x')
        return 62;
    close(reading);
    // What was open before goes on being written.
    if (write(held, "x", 1) != 1)
        return 63;
    // The device that swallows what it is given, a pipe, a thread.
    int const nothing = open("/dev/null", O_WRONLY);
    if (nothing < 0 || write(nothing, "x", 1) != 1)
        return 64;
    close(nothing);
    bool ran = false;
    std::thread later([&] { ran = refused_to_make(outside + "/from-a-later-thread"); });
    later.join();
    if (!ran)
        return 65;
    // A child made now is kept to the same folders.
    pid_t const grandchild = fork();
    if (grandchild == 0)
        _exit(refused_to_make(outside + "/from-a-child") && can_make(inside + "/from-a-child") ? 0 : 1);
    int inner = 0;
    waitpid(grandchild, &inner, 0);
    if (!WIFEXITED(inner) || WEXITSTATUS(inner) != 0)
        return 66;

    // Asking again narrows: a folder within stays, the rest of what was
    // kept goes.
    if (mkdir((inside + "/narrow").c_str(), 0700) != 0)
        return 70;
    if (!platform::keep_writes_to({ inside + "/narrow" }))
        return 71;
    if (!can_make(inside + "/narrow/new") || !refused_to_make(inside + "/after-narrowing"))
        return 72;
    // And asking for more than is held gives nothing back.
    if (!platform::keep_writes_to({ inside, outside }))
        return 73;
    if (!refused_to_make(outside + "/after-asking-for-more") || !refused_to_make(inside + "/after-asking-for-more"))
        return 74;
    return 0;
}

int exit_code_of(pid_t child)
{
    int status = 0;
    waitpid(child, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// The folders the write tests use, under the folder the test runs in and
// emptied first: `inside` and `outside`, each with a file `old`, and
// `outside` with an empty folder.
void write_tests(std::string& why)
{
    namespace fs = std::filesystem;
    std::error_code error;
    fs::path const root = fs::absolute("confine-scratch", error);
    fs::remove_all(root, error);
    std::string const inside = (root / "inside").string();
    std::string const outside = (root / "outside").string();
    fs::create_directories(root / "inside", error);
    fs::create_directories(root / "outside" / "empty", error);
    CHECK(can_make(inside + "/old"));
    CHECK(can_make(outside + "/old"));

    // Whether this kernel has the rule at all.
    {
        pid_t const probe = fork();
        if (probe == 0)
            _exit(platform::keep_writes_to({ inside }) ? 0 : 7);
        if (exit_code_of(probe) == 7) {
            CHECK(!platform::keep_writes_to({ inside }, &why));
            CHECK(!why.empty());
            std::printf("test_confine: writes are not kept here (%s); that half is not tested\n", why.c_str());
            fs::remove_all(root, error);
            return;
        }
    }
    // Control: with no rule a file is made outside.
    {
        pid_t const child = fork();
        if (child == 0)
            _exit(can_make(outside + "/control") ? 0 : 1);
        CHECK_EQ(exit_code_of(child), 0);
        CHECK(fs::exists(outside + "/control"));
        fs::remove(outside + "/control", error);
    }
    // A name that is not there: said, and nothing given up.
    {
        pid_t const child = fork();
        if (child == 0) {
            std::string reason;
            if (platform::keep_writes_to({ inside, outside + "/missing" }, &reason) || reason.find("missing") == std::string::npos)
                _exit(1);
            _exit(can_make(outside + "/after-a-refusal") ? 0 : 2);
        }
        CHECK_EQ(exit_code_of(child), 0);
        fs::remove(outside + "/after-a-refusal", error);
    }
    // A file named and not a folder: it is written, and nothing is made
    // beside it.
    {
        pid_t const child = fork();
        if (child == 0) {
            if (!platform::keep_writes_to({ outside + "/old" }))
                _exit(1);
            int const fd = open((outside + "/old").c_str(), O_WRONLY | O_TRUNC);
            if (fd < 0 || write(fd, "xy", 2) != 2)
                _exit(2);
            _exit(refused_to_make(outside + "/beside") && refused_to_make(inside + "/elsewhere") ? 0 : 3);
        }
        CHECK_EQ(exit_code_of(child), 0);
    }
    {
        pid_t const child = fork();
        if (child == 0)
            _exit(writes_are_kept(inside, outside, false));
        CHECK_EQ(exit_code_of(child), 0);
        // Seen from outside the child: what it was refused is not there,
        // what it was allowed is.
        CHECK(!fs::exists(outside + "/new"));
        CHECK(!fs::exists(outside + "/from-a-child"));
        CHECK(fs::exists(outside + "/old"));
        CHECK(fs::exists(outside + "/empty"));
        CHECK(fs::exists(inside + "/from-a-child"));
        CHECK(fs::exists(inside + "/narrow/new"));
    }
    fs::remove_all(root / "inside", error);
    fs::create_directories(root / "inside", error);
    {
        pid_t const child = fork();
        if (child == 0)
            _exit(writes_are_kept(inside, outside, true));
        CHECK_EQ(exit_code_of(child), 0);
        CHECK(!fs::exists(outside + "/from-the-earlier-thread"));
    }
    fs::remove_all(root, error);
}

}
#endif

int main()
{
#if defined(__linux__)
    std::string why;
    write_tests(why);
    // A sanitizer build keeps its calls, and says so; nothing to test there.
    {
        pid_t const probe = fork();
        if (probe == 0)
            _exit(platform::forbid_new_programs() ? 0 : 7);
        int status = 0;
        waitpid(probe, &status, 0);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 7) {
            CHECK(!platform::forbid_new_programs(&why));
            CHECK(!why.empty());
            std::printf("test_confine: no filter here (%s); the rest is not tested\n", why.c_str());
            return sashfold::test::report("confine");
        }
    }

    // In a child, so that the test's own process keeps its rights. The
    // child's exit code says which expectation failed: 0 is none.
    // Control first: without the filter a program starts.
    {
        pid_t const child = fork();
        if (child == 0) {
            char* const arguments[] = { const_cast<char*>("/bin/sh"), const_cast<char*>("-c"), const_cast<char*>("exit 42"), nullptr };
            execv("/bin/sh", arguments);
            _exit(1);
        }
        int status = 0;
        waitpid(child, &status, 0);
        CHECK(WIFEXITED(status));
        CHECK_EQ(WEXITSTATUS(status), 42);
    }
    {
        pid_t const child = fork();
        if (child == 0) {
            // A second thread is running before the filter goes on: it must
            // be covered as this one is.
            int started[2];
            int verdict[2];
            if (pipe(started) != 0 || pipe(verdict) != 0)
                _exit(2);
            std::thread earlier([&] {
                char go = 0;
                if (read(started[0], &go, 1) != 1)
                    _exit(3);
                char* const arguments[] = { const_cast<char*>("/bin/sh"), const_cast<char*>("-c"), const_cast<char*>("exit 42"), nullptr };
                execv("/bin/sh", arguments);
                char const refused = errno == EPERM ? 'y' : 'n';
                if (write(verdict[1], &refused, 1) != 1)
                    _exit(4);
            });
            if (!platform::forbid_new_programs())
                _exit(10);
            // Starting a program is refused, as a right the user lacks.
            char* const arguments[] = { const_cast<char*>("/bin/sh"), const_cast<char*>("-c"), const_cast<char*>("exit 42"), nullptr };
            execv("/bin/sh", arguments);
            if (errno != EPERM)
                _exit(11);
            // So is tracing.
            errno = 0;
            if (ptrace(PTRACE_TRACEME, 0, nullptr, nullptr) != -1 || errno != EPERM)
                _exit(12);
            // The thread that was already running is refused too.
            if (write(started[1], "g", 1) != 1)
                _exit(13);
            char refused = 0;
            if (read(verdict[0], &refused, 1) != 1 || refused != 'y')
                _exit(14);
            earlier.join();
            // What the browser does goes on: a thread made now, a file
            // opened, memory made executable (the script engine's machine
            // code), a pipe.
            bool ran = false;
            std::thread later([&ran] { ran = true; });
            later.join();
            if (!ran)
                _exit(15);
            int const file = open("/dev/null", O_WRONLY);
            if (file < 0 || write(file, "x", 1) != 1)
                _exit(16);
            close(file);
            void* const code = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (code == MAP_FAILED || mprotect(code, 4096, PROT_READ | PROT_EXEC) != 0)
                _exit(17);
            // Asking twice is harmless.
            if (!platform::forbid_new_programs())
                _exit(18);
            // A child made now inherits the filter: it cannot start a
            // program either.
            pid_t const grandchild = fork();
            if (grandchild == 0) {
                execv("/bin/sh", arguments);
                _exit(errno == EPERM ? 0 : 1);
            }
            int inner = 0;
            waitpid(grandchild, &inner, 0);
            if (!WIFEXITED(inner) || WEXITSTATUS(inner) != 0)
                _exit(19);
            _exit(0);
        }
        int status = 0;
        waitpid(child, &status, 0);
        CHECK(WIFEXITED(status));
        CHECK_EQ(WIFEXITED(status) ? WEXITSTATUS(status) : -1, 0);
    }
#else
    std::string why;
    CHECK(!platform::forbid_new_programs(&why));
    CHECK(!why.empty());
    why.clear();
    CHECK(!platform::keep_writes_to({}, &why));
    CHECK(!why.empty());
#endif
    return sashfold::test::report("confine");
}
