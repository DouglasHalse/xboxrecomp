/* Guest VA -> host pointer, for host code that reads or writes guest memory
 * (device models, bridges, diagnostics).
 *
 * Natively the guest is laid out 1:1 at a host offset, so this is offset + va
 * for every mapped range (RAM, its mirrors, the contiguous window, the
 * apertures). The web build has no such layout: a page map with 1 MB
 * granularity gives each guest MB its host delta (guest_wasm.c). Pointer
 * arithmetic on the result is fine within one mapped range. */
#ifndef XBOXRECOMP_GUEST_PTR_H
#define XBOXRECOMP_GUEST_PTR_H
#include <stddef.h>
#include <stdint.h>

#if defined(__EMSCRIPTEN__)
extern uint32_t g_xbox_pagemap[4096];
#define XBOX_HOST(va) ((uint8_t *)(uintptr_t)((uint32_t)(va) + g_xbox_pagemap[(uint32_t)(va) >> 20]))
#else
ptrdiff_t xbox_GetMemoryOffset(void);
#define XBOX_HOST(va) ((uint8_t *)xbox_GetMemoryOffset() + (uint32_t)(va))
#endif

void *xbox_GuestToHost(uint32_t va);
/* The inverse, for diagnostics (a guest object the host holds a pointer to).
 * On the web build it knows RAM and the contiguous window; else 0. */
uint32_t xbox_HostToGuest(const void *host);

#endif
