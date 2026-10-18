/* host64 from the Linux kit (build_posix.py host64 --kit) only. zig's compiler_rt defines memcpy, memcmp and
 * the rest weak in the executable, and -rdynamic exports them, so the libraries host64 loads (radv, PipeWire)
 * bind to them ahead of glibc's. Their memcmp and bcmp compare one byte at a time, and radv compares 720 bytes
 * on every pipeline bind. A hidden reference makes the linker keep each definition local to host64, and the
 * libraries then use glibc's. (zig cc leaves a version script out of an executable's link.) Without
 * compiler_rt's definitions the link fails rather than binding these to nothing, so no other build uses this. */
#include <stddef.h>

#define HIDDEN __attribute__((visibility("hidden")))
extern void* memcpy(void*, const void*, size_t) HIDDEN;
extern void* memmove(void*, const void*, size_t) HIDDEN;
extern void* memset(void*, int, size_t) HIDDEN;
extern int memcmp(const void*, const void*, size_t) HIDDEN;
extern int bcmp(const void*, const void*, size_t) HIDDEN;
extern void* __memcpy_chk(void*, const void*, size_t, size_t) HIDDEN;
extern void* __memmove_chk(void*, const void*, size_t, size_t) HIDDEN;
extern void* __memset_chk(void*, int, size_t, size_t) HIDDEN;

/* the references that carry the visibility into the link */
__attribute__((used)) static void* const hidden_mem[] = {
    (void*)memcpy, (void*)memmove,      (void*)memset,       (void*)memcmp,
    (void*)bcmp,   (void*)__memcpy_chk, (void*)__memmove_chk, (void*)__memset_chk};
