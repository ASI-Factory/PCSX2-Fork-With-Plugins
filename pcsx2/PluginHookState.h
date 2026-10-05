#pragma once
#include <cstdint>
#include <cstring>
#include <initializer_list>

// Optional mid-hook state. No host pointers enter guest memory. The VU image is
// opaque and source-bound; it cannot be moved between emulator builds. Keeping
// it on the guest stack also makes nested callbacks independent.
namespace PluginHookState
{
struct VULayout
{
    uint32_t size, prefix, tail, cycle, vi, index;
    uint32_t fmacread, fmacwrite, fmaccount, ialuread, ialuwrite, ialucount;
};
inline constexpr uint32_t VUBytes = 2048;
inline constexpr uint32_t Magic = 0x31554850; // PHU1

inline bool Transfer(uint8_t* image, uint32_t flags, bool restore,
    uint8_t* accumulator, uint8_t* vu, VULayout layout, void (*finish)())
{
    if (!image || !flags || (flags & ~3u) || !accumulator) return false;
    const uint32_t tail_size = layout.size - layout.tail;
    if ((flags & 2) && (!vu || !finish || layout.tail > layout.size || !layout.prefix ||
        layout.prefix > layout.tail || 16ull + layout.prefix + tail_size > VUBytes ||
        layout.vi + 32ull * 16 > layout.prefix || layout.cycle + 8ull > layout.prefix ||
        layout.index + 4ull > layout.prefix)) return false;
    uint8_t* snapshot = image + 8;
    auto read = [](const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; };
    if (restore && (flags & 2))
    {
        if (read(snapshot) != Magic || read(snapshot + 4) != layout.prefix ||
            read(snapshot + 8) != layout.tail || read(snapshot + 12) != layout.size) return false;
        // Emulator pipeline cursors must stay bounded even if a callback
        // accidentally overwrites its opaque save area.
        for (auto offset : {layout.fmacread, layout.fmacwrite, layout.fmaccount,
                            layout.ialuread, layout.ialuwrite, layout.ialucount})
        {
            if (offset < layout.tail || offset + 4ull > layout.size) return false;
            const uint32_t value = read(snapshot + 16 + layout.prefix + offset - layout.tail);
            if (value > ((offset == layout.fmaccount || offset == layout.ialucount) ? 4u : 3u)) return false;
        }
    }
    if (flags & 2)
    {
        // Borrow VU0 only at an idle microprogram boundary, just like an
        // interlocked COP2 instruction. VU1 and VIF/micro memories are not saved.
        finish();
        if (read(vu + layout.vi + 29 * 16) & 1) return false;
        if (!restore)
        {
            const uint32_t header[] = {Magic, layout.prefix, layout.tail, layout.size};
            std::memcpy(snapshot, header, 16);
            std::memcpy(snapshot + 16, vu, layout.prefix);
            std::memcpy(snapshot + 16 + layout.prefix, vu + layout.tail, tail_size);
        }
        else
        {
            // VPU_STAT/FBRST/CMSAR1 also control VU1. Preserve their live
            // values, and never roll back VU time or its host-owned identity.
            const uint32_t control[] = {read(vu + layout.vi + 28 * 16),
                read(vu + layout.vi + 29 * 16), read(vu + layout.vi + 31 * 16), read(vu + layout.index)};
            uint64_t cycle; std::memcpy(&cycle, vu + layout.cycle, 8);
            std::memcpy(vu, snapshot + 16, layout.prefix);
            std::memcpy(vu + layout.tail, snapshot + 16 + layout.prefix, tail_size);
            for (unsigned i = 0; i < 3; ++i)
                std::memcpy(vu + layout.vi + (i == 0 ? 28 : i == 1 ? 29 : 31) * 16, control + i, 4);
            std::memcpy(vu + layout.index, control + 3, 4);
            std::memcpy(vu + layout.cycle, &cycle, 8);
        }
    }
    if (flags & 1)
    {
        if (restore) std::memcpy(accumulator, image, 8);
        else std::memcpy(image, accumulator, 8); // Raw ACC and its overflow flag.
    }
    return true;
}
}
