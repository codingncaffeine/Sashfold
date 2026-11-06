#include "platform/Confine.h"

// seccomp(2) with a filter of our own: classic BPF over the system call's
// number, refused calls answered EPERM as if the user had no right to them
// (a caller that checks its errors goes on; nothing is killed). The numbers
// are the kernel's for the machine this was compiled for, and a call made
// through another machine's table — 32-bit calls on x86-64, the x32 table —
// is refused whatever it is, since the filter could not tell what it was.

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

namespace sashfold::platform {

namespace {

#if defined(__x86_64__)
constexpr unsigned this_machine = AUDIT_ARCH_X86_64;
#elif defined(__aarch64__)
constexpr unsigned this_machine = AUDIT_ARCH_AARCH64;
#else
constexpr unsigned this_machine = 0;
#endif

// What a browser never calls. Each is named only where this machine's
// kernel has it.
constexpr long refused_calls[] = {
    // Starting a program.
    SYS_execve,
#ifdef SYS_execveat
    SYS_execveat,
#endif
    // Reaching into another process.
    SYS_ptrace,
#ifdef SYS_process_vm_readv
    SYS_process_vm_readv,
#endif
#ifdef SYS_process_vm_writev
    SYS_process_vm_writev,
#endif
#ifdef SYS_kcmp
    SYS_kcmp,
#endif
    // Changing what the file system or the process's view of the machine is.
    SYS_mount,
    SYS_umount2,
    SYS_pivot_root,
    SYS_chroot,
    SYS_unshare,
#ifdef SYS_setns
    SYS_setns,
#endif
#ifdef SYS_open_by_handle_at
    SYS_open_by_handle_at,
#endif
    // The kernel itself.
    SYS_init_module,
    SYS_delete_module,
#ifdef SYS_finit_module
    SYS_finit_module,
#endif
    SYS_kexec_load,
#ifdef SYS_kexec_file_load
    SYS_kexec_file_load,
#endif
    SYS_reboot,
    SYS_swapon,
    SYS_swapoff,
    SYS_acct,
#ifdef SYS_bpf
    SYS_bpf,
#endif
#ifdef SYS_perf_event_open
    SYS_perf_event_open,
#endif
#ifdef SYS_userfaultfd
    SYS_userfaultfd,
#endif
    // The kernel's key store.
    SYS_add_key,
    SYS_request_key,
    SYS_keyctl,
};

// A sanitizer's leak check stops the world by tracing the process as it
// ends: such a build keeps its calls.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
constexpr bool sanitizer_build = true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(memory_sanitizer)
constexpr bool sanitizer_build = true;
#else
constexpr bool sanitizer_build = false;
#endif
#else
constexpr bool sanitizer_build = false;
#endif

bool said(std::string* why_not, std::string reason)
{
    if (why_not != nullptr)
        *why_not = std::move(reason);
    return false;
}

}

bool forbid_new_programs(std::string* why_not)
{
    if (sanitizer_build)
        return said(why_not, "a sanitizer build keeps the calls its own checks make");
    if (this_machine == 0)
        return said(why_not, "no filter is written for this machine's system calls");

    constexpr std::uint32_t refuse = SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA);
    std::vector<sock_filter> program;
    auto const statement = [&program](std::uint16_t code, std::uint32_t k) { program.push_back(sock_filter { code, 0, 0, k }); };
    auto const jump = [&program](std::uint16_t code, std::uint32_t k, std::uint8_t if_true, std::uint8_t if_false) {
        program.push_back(sock_filter { code, if_true, if_false, k });
    };
    // Another machine's call table: refused.
    statement(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, arch));
    jump(BPF_JMP | BPF_JEQ | BPF_K, this_machine, 1, 0);
    statement(BPF_RET | BPF_K, refuse);
    statement(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr));
#if defined(__x86_64__)
    // The x32 table shares the machine and numbers its calls from 2^30.
    jump(BPF_JMP | BPF_JGE | BPF_K, 0x40000000u, 0, 1);
    statement(BPF_RET | BPF_K, refuse);
#endif
    for (long const call : refused_calls) {
        jump(BPF_JMP | BPF_JEQ | BPF_K, static_cast<std::uint32_t>(call), 0, 1);
        statement(BPF_RET | BPF_K, refuse);
    }
    statement(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);

    // Without this the kernel takes a filter only from root; with it no
    // program started from here could gain rights — and none can start.
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
        return said(why_not, std::string("the kernel refused no-new-privileges: ") + std::strerror(errno));
    sock_fprog const filter { static_cast<unsigned short>(program.size()), program.data() };
    // Every thread there is, at once: one left out could do what the rest
    // may not.
    long const outcome = syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_TSYNC, &filter);
    if (outcome != 0)
        return said(why_not, outcome > 0 ? std::string("a thread could not be given the filter")
                                         : std::string("the kernel refused the filter: ") + std::strerror(errno));
    return true;
}

// Landlock: a set of rules the process takes on for itself, which name the
// kinds of access it gives up everywhere and the folders under which it
// keeps them. Only the kinds that change the file system are named, so
// reading stays what it was. The numbers and layouts below are the
// kernel's own (linux/landlock.h), spelled here so that a machine whose
// headers are older than its kernel builds the same browser.
namespace {

constexpr long call_create_ruleset = 444;
constexpr long call_add_rule = 445;
constexpr long call_restrict_self = 446;

constexpr std::uint32_t ask_version = 1u << 0;
constexpr std::uint32_t rule_path_beneath = 1;
constexpr std::uint32_t every_thread = 1u << 3; // Linux 7.0 on

constexpr std::uint64_t write_file = 1ull << 1;
constexpr std::uint64_t remove_dir = 1ull << 4;
constexpr std::uint64_t remove_file = 1ull << 5;
constexpr std::uint64_t make_char = 1ull << 6;
constexpr std::uint64_t make_dir = 1ull << 7;
constexpr std::uint64_t make_reg = 1ull << 8;
constexpr std::uint64_t make_sock = 1ull << 9;
constexpr std::uint64_t make_fifo = 1ull << 10;
constexpr std::uint64_t make_block = 1ull << 11;
constexpr std::uint64_t make_sym = 1ull << 12;
constexpr std::uint64_t refer = 1ull << 13; // the second version on: renaming into another folder
constexpr std::uint64_t truncate = 1ull << 14; // the third on

struct RulesetAttributes {
    std::uint64_t handled_access_fs;
};

struct [[gnu::packed]] PathBeneath {
    std::uint64_t allowed_access;
    std::int32_t parent_fd;
};

// The devices a browser opens for writing. A device is written to in order
// to be spoken to, and these are the ones spoken to: the graphics card
// (pictures of a video decoded on it), and the three that hold nothing.
constexpr char const* written_devices[] = { "/dev/null", "/dev/zero", "/dev/full", "/dev/dri" };
// A graphics driver that keeps device files of its own beside those.
constexpr char const* written_device_families[] = { "nvidia", "mali" };

std::size_t threads_running()
{
    std::size_t count = 0;
    if (DIR* const tasks = opendir("/proc/self/task")) {
        while (dirent const* const entry = readdir(tasks))
            count += entry->d_name[0] != '.' ? 1 : 0;
        closedir(tasks);
    }
    return count;
}

}

bool keep_writes_to(std::vector<std::string> const& folders, std::string* why_not)
{
    long const version = syscall(call_create_ruleset, nullptr, 0, ask_version);
    if (version < 1)
        return said(why_not, std::string("this kernel has no rule for it (Landlock): ") + std::strerror(errno));
    std::uint64_t kinds = write_file | remove_dir | remove_file | make_char | make_dir | make_reg | make_sock | make_fifo
        | make_block | make_sym;
    if (version >= 2)
        kinds |= refer;
    if (version >= 3)
        kinds |= truncate;
    // What can be asked of something that is not a folder.
    std::uint64_t const kinds_of_a_file = kinds & (write_file | truncate);

    RulesetAttributes const attributes { kinds };
    int const ruleset = static_cast<int>(syscall(call_create_ruleset, &attributes, sizeof attributes, 0));
    if (ruleset < 0)
        return said(why_not, std::string("the kernel refused the rules: ") + std::strerror(errno));
    struct Closed {
        int fd;
        ~Closed() { close(fd); }
    } const closed { ruleset };

    auto const allow = [&](char const* path, bool whole_folder) {
        int const fd = open(path, O_PATH | O_CLOEXEC);
        if (fd < 0)
            return false;
        struct stat what {};
        bool const folder = fstat(fd, &what) == 0 && S_ISDIR(what.st_mode);
        PathBeneath const rule { folder && whole_folder ? kinds : kinds_of_a_file, fd };
        bool const added = syscall(call_add_rule, ruleset, rule_path_beneath, &rule, 0) == 0;
        close(fd);
        return added;
    };
    for (std::string const& folder : folders) {
        if (!allow(folder.c_str(), true))
            return said(why_not, folder + " cannot be kept writable: " + std::strerror(errno));
    }
    // Devices are opened and written, never made or removed; one this
    // machine lacks is no loss.
    for (char const* const device : written_devices)
        allow(device, false);
    if (DIR* const devices = opendir("/dev")) {
        while (dirent const* const entry = readdir(devices)) {
            for (char const* const family : written_device_families) {
                if (std::strncmp(entry->d_name, family, std::strlen(family)) == 0)
                    allow((std::string("/dev/") + entry->d_name).c_str(), false);
            }
        }
        closedir(devices);
    }

    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
        return said(why_not, std::string("the kernel refused no-new-privileges: ") + std::strerror(errno));
    // Every thread at once where the kernel can. Where it cannot, the rules
    // go on the asking thread and on those it makes afterwards — which is
    // all of them only when it is the one thread there is.
    if (syscall(call_restrict_self, ruleset, every_thread) == 0)
        return true;
    if (errno != EINVAL)
        return said(why_not, std::string("the kernel refused to take the rules on: ") + std::strerror(errno));
    if (threads_running() != 1)
        return said(why_not, "this kernel gives the rules to one thread at a time, and others are already running");
    if (syscall(call_restrict_self, ruleset, 0) != 0)
        return said(why_not, std::string("the kernel refused to take the rules on: ") + std::strerror(errno));
    return true;
}

}
