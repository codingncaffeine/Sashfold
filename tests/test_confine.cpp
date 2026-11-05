#include "Test.h"

#include "platform/Confine.h"

#include <string>

#if defined(__linux__)
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#endif

using namespace sashfold;

int main()
{
#if defined(__linux__)
    std::string why;
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
#endif
    return sashfold::test::report("confine");
}
