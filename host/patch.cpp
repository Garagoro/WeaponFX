#include "patch.h"
#include "log.h"

#include <Windows.h>
#include <TlHelp32.h>
#include <cstring>

namespace Wfx {

bool ReadMem(uintptr_t at, void* out, size_t len) {
    __try {
        memcpy(out, (const void*)at, len);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uintptr_t FindUnique(const char* module, const short* sig, size_t len, int* count) {
    if (count) *count = 0;
    HMODULE m = GetModuleHandleA(module);
    if (!m || !sig || !len) return 0;
    const auto* dos = (const IMAGE_DOS_HEADER*)m;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    const auto* nt = (const IMAGE_NT_HEADERS*)((uintptr_t)m + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    const uintptr_t start = (uintptr_t)m + nt->OptionalHeader.BaseOfCode;
    const uintptr_t end = start + nt->OptionalHeader.SizeOfCode;
    if (end <= start + len) return 0;
    uintptr_t first = 0;
    int n = 0;
    for (uintptr_t a = start; a <= end - len; ++a) {
        const uint8_t* b = (const uint8_t*)a;
        size_t i = 0;
        for (; i < len; ++i)
            if (sig[i] >= 0 && b[i] != (uint8_t)sig[i]) break;
        if (i != len) continue;
        if (!n) first = a;
        // Two is enough to know it is not unique; counting the rest costs a
        // full pass for nothing.
        if (++n > 1) break;
    }
    if (count) *count = n;
    return n == 1 ? first : 0;
}

namespace {

bool PatchBytes(uintptr_t fn, const uint8_t* bytes, size_t len) {
    DWORD old = 0;
    if (!VirtualProtect((void*)fn, len, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy((void*)fn, bytes, len);
    VirtualProtect((void*)fn, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)fn, len);
    return true;
}

} // namespace

bool WriteCodeSafely(uintptr_t fn, const uint8_t* bytes, size_t len) {
    if (!fn || !bytes || !len) return false;
    const DWORD self = GetCurrentThreadId();
    const DWORD pid = GetCurrentProcessId();
    // Fixed storage: nothing between the first SuspendThread and the last
    // ResumeThread may allocate - a parked thread can hold the heap lock.
    constexpr size_t kMaxThreads = 512;
    HANDLE held[kMaxThreads];
    HANDLE parked[kMaxThreads];
    size_t nOpen = 0;
    // The snapshot walks every thread on the machine (tens of ms), so it is
    // refreshed every few attempts rather than every one; a thread born in
    // between is caught by the next refresh.
    constexpr int kAttempts = 40;
    constexpr int kResnapshotEvery = 8;
    for (int attempt = 0; attempt < kAttempts; ++attempt) {
        if (attempt % kResnapshotEvery == 0) {
            for (size_t i = 0; i < nOpen; ++i) CloseHandle(held[i]);
            nOpen = 0;
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (snap == INVALID_HANDLE_VALUE) return false;
            THREADENTRY32 te = {};
            te.dwSize = sizeof(te);
            if (Thread32First(snap, &te)) {
                do {
                    if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
                    if (nOpen >= kMaxThreads) break;
                    if (HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID))
                        held[nOpen++] = t;
                } while (Thread32Next(snap, &te));
            }
            CloseHandle(snap);
        }
        size_t nParked = 0;
        bool blocked = false;
        for (size_t i = 0; i < nOpen; ++i) {
            if (SuspendThread(held[i]) == (DWORD)-1) continue;
            parked[nParked++] = held[i];
            CONTEXT ctx = {};
            ctx.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(held[i], &ctx) && ctx.Rip >= fn && ctx.Rip < fn + len) blocked = true;
        }
        const bool ok = !blocked && PatchBytes(fn, bytes, len);
        for (size_t i = 0; i < nParked; ++i) ResumeThread(parked[i]);
        if (!blocked) {
            for (size_t i = 0; i < nOpen; ++i) CloseHandle(held[i]);
            return ok;
        }
        Sleep(2);
    }
    for (size_t i = 0; i < nOpen; ++i) CloseHandle(held[i]);
    return false;
}

bool Install(Detour& d, const char* name, uintptr_t fn, const short* expect, size_t len, int callAt, void* target) {
    if (d.on) return true;
    d.name = name;
    if (!fn || len < 14 || len > sizeof(d.saved) || !target) return false;
    uint8_t cur[sizeof(d.saved)] = {};
    if (!ReadMem(fn, cur, len)) return false;
    for (size_t i = 0; i < len; ++i) {
        if (expect && expect[i] >= 0 && cur[i] != (uint8_t)expect[i]) {
            Log("patch: %s at %p is not the known shape (byte %zu is %02X, want %02X) - not patched", name, (void*)fn,
                i, cur[i], (unsigned)expect[i]);
            return false;
        }
    }
    if (callAt >= 0 && ((size_t)callAt + 5 > len || cur[callAt] != 0xE8)) return false;
    if (!d.tramp) {
        d.tramp = (uint8_t*)VirtualAlloc(nullptr, 0x80, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!d.tramp) return false;
    }
    size_t o = 0;
    if (callAt >= 0) {
        int32_t rel = 0;
        memcpy(&rel, cur + callAt + 1, sizeof(rel));
        const uint64_t callee = (uint64_t)(fn + callAt + 5 + (int64_t)rel);
        memcpy(d.tramp, cur, (size_t)callAt);
        o = (size_t)callAt;
        d.tramp[o++] = 0x48; d.tramp[o++] = 0xB8;                  // mov rax, imm64
        memcpy(d.tramp + o, &callee, 8); o += 8;
        d.tramp[o++] = 0xFF; d.tramp[o++] = 0xD0;                  // call rax
        const size_t tail = len - ((size_t)callAt + 5);            // bytes after the call, replayed as is
        memcpy(d.tramp + o, cur + callAt + 5, tail); o += tail;
    } else {
        memcpy(d.tramp, cur, len);
        o = len;
    }
    d.tramp[o++] = 0xFF; d.tramp[o++] = 0x25;                      // jmp [rip+0]
    *(uint32_t*)(d.tramp + o) = 0; o += 4;
    *(uint64_t*)(d.tramp + o) = (uint64_t)(fn + len);
    FlushInstructionCache(GetCurrentProcess(), d.tramp, 0x80);

    uint8_t patch[sizeof(d.saved)];
    memset(patch, 0x90, sizeof(patch));
    patch[0] = 0xFF; patch[1] = 0x25;                              // jmp [rip+0]: 14 bytes, no register touched
    *(uint32_t*)(patch + 2) = 0;
    *(uint64_t*)(patch + 6) = (uint64_t)target;
    memcpy(d.saved, cur, len);
    d.fn = fn;
    d.len = len;
    d.orig = d.tramp;
    if (!WriteCodeSafely(fn, patch, len)) {
        Log("patch: %s at %p could not be written safely", name, (void*)fn);
        return false;
    }
    d.on = true;
    Log("patch: %s at %p hooked", name, (void*)fn);
    return true;
}

bool Remove(Detour& d) {
    if (!d.on) return true;
    if (!WriteCodeSafely(d.fn, d.saved, d.len)) {
        Log("patch: %s at %p could NOT be restored", d.name ? d.name : "?", (void*)d.fn);
        return false;
    }
    d.on = false;
    return true;
}

} // namespace Wfx
