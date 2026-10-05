#ifndef PCSX2F_GUEST_MODULE_H
#define PCSX2F_GUEST_MODULE_H
#include <stdint.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

#define PCSX2F_MODULE_MAGIC 0x4D503250u /* P2PM */
#define PCSX2F_MODULE_VERSION 1u
#define PCSX2F_HOST_VERSION 1u

/* ELF32/LE/EM_MIPS/ET_REL, -G0 -mno-abicalls -fno-pic. No standalone CRT.
 * The loader relocates this descriptor; all addresses are guest addresses.
 * Reserved fields and unsupported flags must be zero. */
struct PCSX2FModuleDescriptor
{
    uint32_t magic, version, size, flags;
#ifdef __mips__
    void (*entry)(const void* context);
#else
    uint32_t entry;
#endif
    uint32_t stack_size, heap_size, reserved;
};

struct PCSX2FModuleContext
{
    uint32_t size, version, game_gp, module_base;
    uint32_t heap_begin, heap_end, reserved[2];
};

#if defined(__mips__)
#define PCSX2F_MODULE(entry_fn, stack_bytes, heap_bytes) \
    const struct PCSX2FModuleDescriptor PCSX2FModule \
    __attribute__((used, aligned(4))) = {PCSX2F_MODULE_MAGIC, PCSX2F_MODULE_VERSION, \
        sizeof(struct PCSX2FModuleDescriptor), 0, entry_fn, stack_bytes, heap_bytes, 0}
#endif

/* Host ABI. Registration/writes are permitted only inside the CPU-thread ELF
 * initialization callback. Pointer fields below are HOST function pointers. */
#ifndef __mips__
struct PCSX2FGuestHostV1
{
    uint32_t size, version, arena_begin, arena_end;
    uint64_t generation;
    bool (*write)(uint32_t address, const void* bytes, uint32_t size);
    bool (*queue)(uint32_t entry, uint32_t stack_top, uint32_t gp, uint32_t context);
    bool (*commit)();
    void (*abort)();
    void (*warn)(const char* message);
};
#endif
#endif
