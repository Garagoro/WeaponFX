#include "engine.h"
#include "log.h"
#include "material.h"
#include "patch.h"

#include <Windows.h>
#include <atomic>
#include <cstring>

namespace Wfx {
namespace Engine {
namespace {

// scenesystem!RenderObjects(?, ?, items, count, ?, passContext, ?). Its first
// 15 bytes (mov rax,rsp; two stores; push rbx/rsi/r15) have no RIP-relative
// operand, so they replay from the trampoline as they are.
constexpr short kSigRenderObjects[] = {
    0x48, 0x8B, 0xC4, 0x48, 0x89, 0x50, 0x10, 0x48, 0x89, 0x48, 0x08, 0x53, 0x56, 0x41, 0x57,
    0x48, 0x81, 0xEC, -1, -1, -1, -1,
    0x49, 0x63, 0xF1, 0x49, 0x8B, 0xD8, 0x4C, 0x8B, 0xFA, 0x83, 0xFE, 0x01 };
constexpr size_t kPatchLen = 15;   // 14 for the jump, ending mid `push r15`, so one more

// Engine layouts (2026-10 builds). Every read is guarded; a field that moved
// reads as "not the weapon", and the call goes through untouched.
constexpr size_t kItemSobj = 0x18;      // scene object (one per model)
constexpr size_t kItemMat = 0x20;       // CMaterial2*
constexpr size_t kItemMat2 = 0x28;      // the resolved variant the draw takes its render state from
constexpr size_t kItemColor = 0x50;     // RGBA8 tint
constexpr size_t kMatParamBlock = 0x20;
constexpr size_t kParamShaderName = 0x30;
constexpr size_t kPassLflags = 0x48;    // pass context -> layer flags
constexpr size_t kPassViewBits = 0x480; // pass context -> view flags
constexpr uint64_t kViewIsViewmodel = 0x100;
constexpr uint32_t kLflagDepth = 0x1000000;
constexpr size_t kDefaultStride = 0x70;
constexpr int kMaxItems = 4096;         // a corrupt count is a refusal, not a walk off the array
constexpr char kWeaponShader[] = "csgo_weapon.vfx";
constexpr uint32_t kMarkerColor = 0xFFFFFFFFu;

using RenderObjectsFn = void*(__fastcall*)(void*, void*, void*, int, void*, void*, void*);

Detour g_Det;
size_t g_Stride = kDefaultStride;
std::atomic<int> g_InHook{0};
std::atomic<int> g_Mode{(int)Mode::Replace};
std::atomic<unsigned> g_Calls{0}, g_WeaponItems{0}, g_Stripped{0}, g_NoRoom{0};

// Scratch for the rewritten item lists. RenderObjects runs on several
// scenesystem workers at once; a slot is claimed per call and released when
// the original returns (it consumes the list before returning).
constexpr int kScratchSlots = 8;
constexpr size_t kScratchBytes = 0x40000;
uint8_t* g_Scratch[kScratchSlots] = {};
std::atomic<uint32_t> g_ScratchBusy{0};

int ClaimScratch() {
    for (int i = 0; i < kScratchSlots; ++i) {
        const uint32_t bit = 1u << i;
        uint32_t cur = g_ScratchBusy.load(std::memory_order_relaxed);
        while (!(cur & bit)) {
            if (g_ScratchBusy.compare_exchange_weak(cur, cur | bit, std::memory_order_acquire)) {
                if (!g_Scratch[i])
                    g_Scratch[i] = (uint8_t*)VirtualAlloc(nullptr, kScratchBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
                if (g_Scratch[i]) return i;
                g_ScratchBusy.fetch_and(~bit, std::memory_order_release);
                return -1;
            }
        }
    }
    return -1;
}
void ReleaseScratch(int i) { g_ScratchBusy.fetch_and(~(1u << i), std::memory_order_release); }

bool ReadPtr(uintptr_t at, void** out) { return ReadMem(at, out, sizeof(*out)); }

// Is this material's shader csgo_weapon? Cached by material pointer (direct
// mapped, the low bit carries the verdict), so the string is read once per
// material, not per item per pass.
constexpr int kCacheSize = 512;
std::atomic<uintptr_t> g_ShaderCache[kCacheSize];

bool IsWeaponMaterial(void* mat) {
    if (!mat) return false;
    const uintptr_t key = (uintptr_t)mat;
    std::atomic<uintptr_t>& slot = g_ShaderCache[(key >> 4) % kCacheSize];
    const uintptr_t c = slot.load(std::memory_order_relaxed);
    if ((c & ~(uintptr_t)1) == key) return (c & 1) != 0;
    void* block = nullptr;
    void* name = nullptr;
    char buf[sizeof(kWeaponShader)] = {};
    bool weapon = false;
    if (ReadPtr(key + kMatParamBlock, &block) && block && ReadPtr((uintptr_t)block + kParamShaderName, &name) && name &&
        ReadMem((uintptr_t)name, buf, sizeof(buf)))
        weapon = memcmp(buf, kWeaponShader, sizeof(kWeaponShader)) == 0;   // the terminator too
    slot.store(key | (weapon ? 1 : 0), std::memory_order_relaxed);
    return weapon;
}

bool IsWeaponItem(uintptr_t item) {
    void* mat = nullptr;
    return ReadPtr(item + kItemMat, &mat) && IsWeaponMaterial(mat);
}

// --- the model path ---------------------------------------------------------
// scene object -> +o1 -> (+o2 ->) "....vmdl". Found by scanning the first
// weapon's scene object and remembered (2026-10: +0x110 -> +0xB8); a read
// that stops giving a .vmdl rescans, at most once a second.
uint32_t g_ModelO1 = 0;
int g_ModelO2 = -2;   // -2 not found yet, -1 the string is at +o1 itself
ULONGLONG g_ModelScanAt = 0;

char g_Model[260] = {};
std::atomic<unsigned> g_ModelSeq{0};      // odd while g_Model is written
std::atomic<unsigned> g_ModelSerial{0};
SRWLOCK g_ModelWrite = SRWLOCK_INIT;      // one writer at a time (several workers)

bool ReadVmdl(uintptr_t at, char* out, size_t cap) {
    void* s = nullptr;
    if (!ReadPtr(at, &s) || (uintptr_t)s < 0x10000) return false;
    if (!ReadMem((uintptr_t)s, out, cap - 1)) {
        // The string may end before the page does; read it byte by byte.
        size_t i = 0;
        for (; i + 1 < cap; ++i)
            if (!ReadMem((uintptr_t)s + i, out + i, 1) || !out[i]) break;
        out[i] = 0;
    }
    out[cap - 1] = 0;
    const size_t n = strnlen(out, cap);
    for (size_t i = 0; i < n; ++i)
        if (out[i] < 0x20 || out[i] > 0x7E) return false;
    return n > 5 && _stricmp(out + n - 5, ".vmdl") == 0;
}

bool ModelAt(void* sobj, uint32_t o1, int o2, char* out, size_t cap) {
    if (o2 < 0) return ReadVmdl((uintptr_t)sobj + o1, out, cap);
    void* p = nullptr;
    return ReadPtr((uintptr_t)sobj + o1, &p) && (uintptr_t)p >= 0x10000 && ReadVmdl((uintptr_t)p + (uint32_t)o2, out, cap);
}

bool FindModel(void* sobj, char* out, size_t cap) {
    if (g_ModelO2 != -2 && ModelAt(sobj, g_ModelO1, g_ModelO2, out, cap)) return true;
    const ULONGLONG now = GetTickCount64();
    if (now < g_ModelScanAt) return false;
    g_ModelScanAt = now + 1000;
    for (uint32_t a = 0; a < 0x400; a += 8) {
        if (ModelAt(sobj, a, -1, out, cap)) { g_ModelO1 = a; g_ModelO2 = -1; return true; }
        for (uint32_t b = 0; b < 0x100; b += 8)
            if (ModelAt(sobj, a, (int)b, out, cap)) {
                g_ModelO1 = a;
                g_ModelO2 = (int)b;
                Log("engine: model path at scene object +%#x -> +%#x (\"%s\")", a, b, out);
                return true;
            }
    }
    return false;
}

void NoteModel(uintptr_t item) {
    void* sobj = nullptr;
    char path[sizeof(g_Model)];
    if (!ReadPtr(item + kItemSobj, &sobj) || !sobj) return;
    AcquireSRWLockExclusive(&g_ModelWrite);
    if (FindModel(sobj, path, sizeof(path)) && strcmp(path, g_Model) != 0) {
        g_ModelSeq.fetch_add(1, std::memory_order_acq_rel);
        strcpy_s(g_Model, path);
        g_ModelSeq.fetch_add(1, std::memory_order_acq_rel);
        g_ModelSerial.fetch_add(1, std::memory_order_relaxed);
    }
    ReleaseSRWLockExclusive(&g_ModelWrite);
}

// Every distinct model among the weapon items, logged once: the weapon itself,
// its attachments, and anything else that draws with the weapon shader in the
// viewmodel pass (a muzzle flash, if it is a model). Scene objects are
// remembered by pointer, so a model is read once per object, not per frame.
constexpr int kSeenObjects = 64;
void* g_SeenObj[kSeenObjects] = {};
int g_SeenCount = 0;

void NoteItemModels(uintptr_t base, int count) {
    if (g_ModelO2 == -2 || g_SeenCount >= kSeenObjects) return;
    AcquireSRWLockExclusive(&g_ModelWrite);
    for (int i = 0; i < count && g_SeenCount < kSeenObjects; ++i) {
        const uintptr_t e = base + (size_t)i * g_Stride;
        void* sobj = nullptr;
        if (!IsWeaponItem(e) || !ReadPtr(e + kItemSobj, &sobj) || !sobj) continue;
        bool seen = false;
        for (int k = 0; k < g_SeenCount && !seen; ++k) seen = g_SeenObj[k] == sobj;
        if (seen) continue;
        g_SeenObj[g_SeenCount++] = sobj;
        char path[260];
        void* mat = nullptr;
        ReadPtr(e + kItemMat, &mat);
        Log("engine: weapon item %d/%d model %s (scene object %p, material %p)", i, count,
            ModelAt(sobj, g_ModelO1, g_ModelO2, path, sizeof(path)) ? path : "?", sobj, mat);
    }
    ReleaseSRWLockExclusive(&g_ModelWrite);
}

// --- the passes ---------------------------------------------------------------

// The viewmodel's depth pass without the weapon's items. True when it issued
// the call itself (or skipped it: nothing but the weapon was in it).
bool StripWeapon(void* p1, void* p2, void* items, int count, void* p5, void* L, void* p7, void** result) {
    const uintptr_t base = (uintptr_t)items;
    int weapon = 0;
    for (int i = 0; i < count; ++i) weapon += IsWeaponItem(base + (size_t)i * g_Stride) ? 1 : 0;
    if (!weapon) return false;
    g_Stripped.fetch_add((unsigned)weapon, std::memory_order_relaxed);
    if (weapon == count) { *result = nullptr; return true; }
    if ((size_t)count * g_Stride > kScratchBytes) { g_NoRoom.fetch_add(1); return false; }
    const int slot = ClaimScratch();
    if (slot < 0) { g_NoRoom.fetch_add(1); return false; }
    uint8_t* out = g_Scratch[slot];
    size_t n = 0;
    for (int i = 0; i < count; ++i) {
        const uintptr_t e = base + (size_t)i * g_Stride;
        if (IsWeaponItem(e)) continue;
        if (!ReadMem(e, out + n * g_Stride, g_Stride)) { ReleaseScratch(slot); return false; }
        ++n;
    }
    *result = ((RenderObjectsFn)g_Det.orig)(p1, p2, out, (int)n, p5, L, p7);
    ReleaseScratch(slot);
    return true;
}

// The viewmodel's colour pass with the marker on the weapon: instead of its
// items (replace), or as a second call with copies of them (overlay).
bool MarkWeapon(void* p1, void* p2, void* items, int count, void* p5, void* L, void* p7, void* marker, bool replace,
                void** result) {
    const uintptr_t base = (uintptr_t)items;
    int weapon = 0;
    int firstWeapon = -1;
    for (int i = 0; i < count; ++i)
        if (IsWeaponItem(base + (size_t)i * g_Stride)) { ++weapon; if (firstWeapon < 0) firstWeapon = i; }
    if (!weapon) return false;
    NoteModel(base + (size_t)firstWeapon * g_Stride);
    NoteItemModels(base, count);
    if (!marker) return false;   // off: the model is still followed, for the next weapon's settings
    g_WeaponItems.fetch_add((unsigned)weapon, std::memory_order_relaxed);
    const size_t outItems = replace ? (size_t)count : (size_t)weapon;
    if (outItems * g_Stride > kScratchBytes) { g_NoRoom.fetch_add(1); return false; }
    const int slot = ClaimScratch();
    if (slot < 0) { g_NoRoom.fetch_add(1); return false; }
    uint8_t* out = g_Scratch[slot];
    size_t n = 0;
    for (int i = 0; i < count; ++i) {
        const uintptr_t e = base + (size_t)i * g_Stride;
        const bool isWeapon = IsWeaponItem(e);
        if (!replace && !isWeapon) continue;
        uint8_t* dst = out + n * g_Stride;
        if (!ReadMem(e, dst, g_Stride)) { ReleaseScratch(slot); return false; }
        if (isWeapon) {
            // Both material slots: +0x28 is the variant the draw takes its
            // render state from; the weapon's own would draw the marker with
            // the weapon's depth and blend.
            *(void**)(dst + kItemMat) = marker;
            *(void**)(dst + kItemMat2) = marker;
            *(uint32_t*)(dst + kItemColor) = kMarkerColor;
        }
        ++n;
    }
    const RenderObjectsFn orig = (RenderObjectsFn)g_Det.orig;
    if (replace) {
        *result = orig(p1, p2, out, (int)n, p5, L, p7);
    } else {
        *result = orig(p1, p2, items, count, p5, L, p7);   // the skin first, as the engine handed it over
        orig(p1, p2, out, (int)n, p5, L, p7);
    }
    ReleaseScratch(slot);
    return true;
}

void* __fastcall HkRenderObjects(void* p1, void* p2, void* items, int count, void* p5, void* L, void* p7) {
    struct Guard {
        Guard() { g_InHook.fetch_add(1, std::memory_order_acquire); }
        ~Guard() { g_InHook.fetch_sub(1, std::memory_order_release); }
    } guard;
    const RenderObjectsFn orig = (RenderObjectsFn)g_Det.orig;
    const Mode mode = (Mode)g_Mode.load(std::memory_order_relaxed);
    void* marker = mode == Mode::Off ? nullptr : Material::MarkerIfReady();
    uint64_t view = 0;
    uint32_t lflags = 0;
    if (!items || !L || count <= 0 || count > kMaxItems || !ReadMem((uintptr_t)L + kPassViewBits, &view, 8) ||
        !(view & kViewIsViewmodel) || !ReadMem((uintptr_t)L + kPassLflags, &lflags, 4))
        return orig(p1, p2, items, count, p5, L, p7);
    g_Calls.fetch_add(1, std::memory_order_relaxed);
    void* r = nullptr;
    if (lflags & kLflagDepth) {
        if (marker && StripWeapon(p1, p2, items, count, p5, L, p7, &r)) return r;
    } else if (MarkWeapon(p1, p2, items, count, p5, L, p7, marker, mode == Mode::Replace, &r)) {
        return r;
    }
    return orig(p1, p2, items, count, p5, L, p7);
}

// The item size, read back out of the function: the walk advances its pointer
// with `add r64, imm8`. Majority vote over the first 0x600 bytes; the default
// stands when nothing votes.
size_t DeriveStride(uintptr_t fn) {
    unsigned tally[0x100] = {};
    uint8_t code[0x604] = {};
    if (!ReadMem(fn, code, sizeof(code))) return 0;
    for (size_t o = 0; o < 0x600; ++o) {
        if ((code[o] == 0x48 || code[o] == 0x49) && code[o + 1] == 0x83 && code[o + 2] >= 0xC0 && code[o + 2] <= 0xC7) {
            const uint8_t imm = code[o + 3];
            if (imm >= 0x40 && imm <= 0xC0 && imm % 8 == 0) ++tally[imm];
        }
    }
    unsigned best = 0, votes = 0;
    for (unsigned i = 0; i < 0x100; ++i)
        if (tally[i] > votes) { votes = tally[i]; best = i; }
    return votes ? best : 0;
}

} // namespace

bool Install() {
    if (g_Det.on) return true;
    int hits = 0;
    const uintptr_t fn = FindUnique("scenesystem.dll", kSigRenderObjects,
                                    sizeof(kSigRenderObjects) / sizeof(kSigRenderObjects[0]), &hits);
    if (!fn) {
        // Zero hits is a game update - or another module's detour already over
        // the prologue (another tool hooking it): then this one stays out.
        Log("engine: RenderObjects signature matched %d times (need 1) - not hooked", hits);
        return false;
    }
    if (const size_t s = DeriveStride(fn)) g_Stride = s;
    Log("engine: RenderObjects %p, item stride %#zx", (void*)fn, g_Stride);
    if (!Material::Resolve()) return false;
    return Wfx::Install(g_Det, "RenderObjects", fn, kSigRenderObjects, kPatchLen, -1, (void*)&HkRenderObjects);
}

void Remove() {
    Wfx::Remove(g_Det);
    for (int i = 0; i < 500 && g_InHook.load(std::memory_order_acquire) > 0; ++i) Sleep(1);
}

void SetMode(Mode m) { g_Mode.store((int)m, std::memory_order_relaxed); }
Mode GetMode() { return (Mode)g_Mode.load(std::memory_order_relaxed); }

void WeaponModel(char* out, unsigned cap, unsigned* serial) {
    if (!out || !cap) return;
    for (int tries = 0; tries < 100; ++tries) {
        const unsigned s0 = g_ModelSeq.load(std::memory_order_acquire);
        if (s0 & 1) { YieldProcessor(); continue; }
        strncpy_s(out, cap, g_Model, _TRUNCATE);
        if (g_ModelSeq.load(std::memory_order_acquire) == s0) {
            if (serial) *serial = g_ModelSerial.load(std::memory_order_relaxed);
            return;
        }
    }
    out[0] = 0;
}

void Stats(unsigned* calls, unsigned* weaponItems, unsigned* stripped, unsigned* noRoom) {
    if (calls) *calls = g_Calls.load();
    if (weaponItems) *weaponItems = g_WeaponItems.load();
    if (stripped) *stripped = g_Stripped.load();
    if (noRoom) *noRoom = g_NoRoom.load();
}

} // namespace Engine
} // namespace Wfx
