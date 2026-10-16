#pragma once

// Code patching for WeaponFX: signature scans, 14-byte absolute-jump detours
// with a trampoline, and writes made while every other thread is parked off
// the bytes being changed.
//
// A detour only goes over a prologue whose bytes the caller recognised: the
// caller passes what it expects, and a mismatch is a refusal with a log line,
// never a patch over unknown code.

#include <cstddef>
#include <cstdint>

namespace Wfx {

// Pattern over a module's code section; -1 is a wildcard. Zero unless it
// matches exactly once (*count says how many: 0, 1, or 2 for "more").
uintptr_t FindUnique(const char* module, const short* sig, size_t len, int* count);

// Guarded reads: false instead of a fault.
bool ReadMem(uintptr_t at, void* out, size_t len);

// Writes `len` bytes at `fn` with every other thread suspended and none of
// them executing inside [fn, fn+len). Retries a few dozen times; false when a
// thread kept standing there or the write failed.
bool WriteCodeSafely(uintptr_t fn, const uint8_t* bytes, size_t len);

struct Detour {
    const char* name = nullptr;
    uintptr_t fn = 0;
    uint8_t saved[32] = {};
    size_t len = 0;            // bytes overwritten at fn (>= 14)
    uint8_t* tramp = nullptr;  // replay + jump back; reused, never freed
    void* orig = nullptr;      // call this for the original
    bool on = false;
};

// Detour `fn` to `target`. The first `len` bytes must equal `expect` (a
// wildcard -1 in `expect` matches any byte) and are replayed in the
// trampoline as they are - so they must hold no RIP-relative operand, except
// a rel32 call at offset `callAt` (>= 0), which is replayed as an absolute
// call through rax (the call's own return register).
bool Install(Detour& d, const char* name, uintptr_t fn, const short* expect, size_t len, int callAt, void* target);
// Puts the original bytes back (safely). False leaves it installed.
bool Remove(Detour& d);

} // namespace Wfx
