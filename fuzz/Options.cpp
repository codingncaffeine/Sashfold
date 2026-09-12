// AddressSanitizer's defaults for every harness in this directory.
//
// libFuzzer's runtime on some toolchains (Arch's clang 22, for one) carries
// a bundled C++ standard library whose weak operator new and operator
// delete survive into the link. They resolve ahead of the sanitizer's own
// and reach malloc and free directly, so a picture allocated through the
// one operator the bundle lacks — operator new (nothrow), which the
// standard library's stable_sort uses for its buffer — is freed through
// the bundle's delete and reads to the sanitizer as "operator new vs
// free" in code that called the right operator all along (the object file
// says so). That one check cannot be sound under libFuzzer here, so the
// harnesses run without it; the sanitizer lane, which has no libFuzzer in
// the link, keeps it. Every other check stays on.

extern "C" char const* __asan_default_options()
{
    return "alloc_dealloc_mismatch=0";
}
