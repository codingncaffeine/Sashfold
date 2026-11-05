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
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
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

}
