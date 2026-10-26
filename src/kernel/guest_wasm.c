/*
 * Guest address space for the WebAssembly build.
 *
 * Natively the guest's 4 GB are laid out 1:1 at a host offset, with the RAM
 * mirrors, the contiguous window and the tiled aperture as extra views of the
 * same pages. wasm32 has one linear memory, no fixed placement and no
 * aliasing, so every guest access goes through g_xbox_pagemap instead (see
 * XBOX_PTR in recomp_types.h): one entry per guest MB holding the host delta,
 * host = va + delta (mod 2^32).
 *
 *   0x00000000  RAM (map size, 64 MB retail), plus mirrors every map size
 *               up to 1 + XBOX_NUM_MIRRORS map sizes (same pages)
 *   0x80000000  contiguous window (XBOX_CONTIG_SIZE)
 *   0xF0000000  tiled aperture: the contiguous window again
 *   0xFD000000  NV2A registers, 16 MB of plain memory
 *   0xFE800000  MCPX registers, 8 MB (device ranges are serviced by
 *               xbox_mmio_read/write before the page map is consulted)
 *   0xFF000000  flash, 1 MB
 *   elsewhere   one shared scratch MB: reads garbage, never traps
 */
#if defined(__EMSCRIPTEN__)
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kernel.h"
#include "xbox_memory_layout.h"

#define MB (1u << 20)

uint32_t g_xbox_pagemap[4096];

static uint8_t *s_ram, *s_contig, *s_nv2a, *s_mcpx, *s_flash, *s_scratch;
static size_t s_ram_size;
static uint32_t s_contig_base, s_contig_size;

static uint8_t *block(size_t bytes)
{
    void *p = aligned_alloc(MB, bytes);
    if (p)
        memset(p, 0, bytes);
    return (uint8_t *)p;
}

static void map(uint32_t va, uint32_t bytes, uint8_t *host)
{
    uint32_t mb;
    for (mb = va >> 20; mb < ((va + (bytes - 1)) >> 20) + 1 && mb < 4096; mb++)
        g_xbox_pagemap[mb] = (uint32_t)(uintptr_t)host - va;
}

/* Allocates every block and fills the page map. Returns the RAM block, the
 * host address of guest 0. */
void *xbox_WasmMapGuest(size_t ram_size, unsigned mirrors,
                        uint32_t contig_base, uint32_t contig_size,
                        uint32_t tiled_base, uint32_t tiled_size)
{
    uint32_t mb, m;

    if (s_ram)
        return s_ram;
    s_ram_size = ram_size;
    s_contig_base = contig_base;
    s_contig_size = contig_size;
    s_ram     = block(ram_size);
    s_contig  = block(contig_size);
    s_nv2a    = block(16 * MB);
    s_mcpx    = block(8 * MB);
    s_flash   = block(MB);
    s_scratch = block(MB);
    if (!s_ram || !s_contig || !s_nv2a || !s_mcpx || !s_flash || !s_scratch) {
        fprintf(stderr, "xbox_WasmMapGuest: out of memory\n");
        return NULL;
    }
    for (mb = 0; mb < 4096; mb++)
        g_xbox_pagemap[mb] = (uint32_t)(uintptr_t)s_scratch - (mb << 20);
    for (m = 1; m <= mirrors; m++) {
        uint64_t lo = (uint64_t)m * ram_size;
        if (lo + ram_size > contig_base)
            break;
        map((uint32_t)lo, (uint32_t)ram_size, s_ram);
    }
    map(0, (uint32_t)ram_size, s_ram);
    map(contig_base, contig_size, s_contig);
    map(tiled_base, tiled_size < contig_size ? tiled_size : contig_size, s_contig);
    map(0xFD000000u, 16 * MB, s_nv2a);
    map(0xFE800000u, 8 * MB, s_mcpx);
    map(0xFF000000u, MB, s_flash);
    fprintf(stderr, "  [WASM] guest memory: RAM %u MB at %p (+%u mirrors), contiguous"
            " %u MB at %p, NV2A %p, MCPX %p, flash %p\n",
            (unsigned)(ram_size / MB), (void *)s_ram, m - 1,
            contig_size / MB, (void *)s_contig, (void *)s_nv2a,
            (void *)s_mcpx, (void *)s_flash);
    return s_ram;
}

uint32_t xbox_HostToGuest(const void *host)
{
    const uint8_t *h = (const uint8_t *)host;
    if (s_ram && h >= s_ram && h < s_ram + s_ram_size)
        return (uint32_t)(h - s_ram);
    if (s_contig && h >= s_contig && h < s_contig + s_contig_size)
        return s_contig_base + (uint32_t)(h - s_contig);
    return 0;
}

/* Host address of a mapped aperture, by its guest base. */
void *xbox_WasmBlock(uint32_t guest_base)
{
    return (void *)(uintptr_t)(guest_base + g_xbox_pagemap[guest_base >> 20]);
}
#endif
