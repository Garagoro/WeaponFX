#include "engine.h"
#include "log.h"
#include "material.h"
#include "patch.h"
#include "../core/weapon_fx.h"

#include <Windows.h>
#include <atomic>
#include <cstdio>
#include <cstring>

namespace Wfx {
namespace Engine {
namespace {

using WeaponFx::kTargets;
using WeaponFx::kWeapon;
using WeaponFx::kGloves;

// scenesystem!RenderObjects(?, ?, items, count, ?, passContext, ?). Its first
// 15 bytes (mov rax,rsp; two stores; push rbx/rsi/r15) have no RIP-relative
// operand, so they replay from the trampoline as they are.
constexpr short kSigRenderObjects[] = {
    0x48, 0x8B, 0xC4, 0x48, 0x89, 0x50, 0x10, 0x48, 0x89, 0x48, 0x08, 0x53, 0x56, 0x41, 0x57,
    0x48, 0x81, 0xEC, -1, -1, -1, -1,
    0x49, 0x63, 0xF1, 0x49, 0x8B, 0xD8, 0x4C, 0x8B, 0xFA, 0x83, 0xFE, 0x01 };
constexpr size_t kPatchLen = 15;   // 14 for the jump, ending mid `push r15`, so one more

// Engine layouts (2026-10 builds). Every read is guarded; a field that moved
// reads as "not ours", and the call goes through untouched.
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
constexpr char kCharacterShader[] = "csgo_character.vfx";
// The gloves' materials and models live under .../arms/glove_<name>/ (base
// game, 2026-10). Arms and sleeves are csgo_character as well, in other
// folders. One that moved out of this folder is a glove no longer treated as
// one - the effect is gone from it, nothing else changes.
constexpr char kGloveFolder[] = "/arms/glove_";
constexpr uint32_t kMarkerColor = 0xFFFFFFFFu;

Detour g_Det;
bool g_Attached = false;
size_t g_Stride = kDefaultStride;
std::atomic<int> g_InHook{0};
std::atomic<int> g_Mode[kTargets] = {};   // Mode, per target (Off = 0)
std::atomic<unsigned> g_Calls{0}, g_WeaponItems{0}, g_GloveItems{0}, g_Stripped{0}, g_NoRoom{0};

// Scratch for the rewritten item lists. RenderObjects runs on several
// scenesystem workers at once; a slot is claimed per call and released when
// the original returns (it consumes the list before returning). An overlay
// call needs two lists at once, so it claims two.
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
void ReleaseScratch(int i) {
    if (i >= 0) g_ScratchBusy.fetch_and(~(1u << i), std::memory_order_release);
}

bool ReadPtr(uintptr_t at, void** out) { return ReadMem(at, out, sizeof(*out)); }

// The material's shader, as a class: 1 csgo_weapon, 2 csgo_character, 0 other.
// Cached by material pointer (direct mapped, the low two bits carry the class;
// materials are heap objects, 16-aligned), so the string is read once per
// material, not per item per pass.
constexpr int kCacheSize = 512;
constexpr uintptr_t kClassBits = 3;
enum ShaderClass : uintptr_t { kOther = 0, kWeaponShaderClass = 1, kCharacterShaderClass = 2 };
std::atomic<uintptr_t> g_ShaderCache[kCacheSize];

// Cache slot of a pointer. Multiplicative, not the low bits: materials come
// 2 KB-aligned, and the arms, sleeve and glove of one model all fell into one
// slot by their low bits - each evicted the next, every item missed, every
// pass (2026-10-17, measured).
unsigned SlotOf(uintptr_t p, unsigned size) {
    return (unsigned)(((uint64_t)p * 0x9E3779B97F4A7C15ull) >> 40) % size;
}

uintptr_t ShaderClassOf(void* mat) {
    if (!mat) return kOther;
    const uintptr_t key = (uintptr_t)mat;
    std::atomic<uintptr_t>& slot = g_ShaderCache[SlotOf(key, kCacheSize)];
    const uintptr_t c = slot.load(std::memory_order_relaxed);
    if ((c & ~kClassBits) == key) return c & kClassBits;
    void* block = nullptr;
    void* name = nullptr;
    char buf[sizeof(kCharacterShader)] = {};
    uintptr_t cls = kOther;
    if (ReadPtr(key + kMatParamBlock, &block) && block && ReadPtr((uintptr_t)block + kParamShaderName, &name) && name &&
        ReadMem((uintptr_t)name, buf, sizeof(kWeaponShader))) {
        if (memcmp(buf, kWeaponShader, sizeof(kWeaponShader)) == 0) cls = kWeaponShaderClass;   // the terminator too
        else if (ReadMem((uintptr_t)name, buf, sizeof(kCharacterShader)) &&
                 memcmp(buf, kCharacterShader, sizeof(kCharacterShader)) == 0)
            cls = kCharacterShaderClass;
    }
    if (!(key & kClassBits)) slot.store(key | cls, std::memory_order_relaxed);
    return cls;
}

// --- the model path ---------------------------------------------------------
// scene object -> +o1 -> (+o2 ->) "....vmdl". Found by scanning the first
// scene object asked about and remembered (2026-10: +0x110 -> +0xB8); a read
// that stops giving a .vmdl rescans, at most once a second.
uint32_t g_ModelO1 = 0;
std::atomic<int> g_ModelO2{-2};   // -2 not found yet, -1 the string is at +o1 itself
ULONGLONG g_ModelScanAt = 0;
SRWLOCK g_ScanLock = SRWLOCK_INIT;   // the offsets: one scanner at a time (several workers)

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
    const int o2 = g_ModelO2.load(std::memory_order_acquire);
    if (o2 != -2 && ModelAt(sobj, g_ModelO1, o2, out, cap)) return true;
    AcquireSRWLockExclusive(&g_ScanLock);
    const ULONGLONG now = GetTickCount64();
    bool found = false;
    if (now >= g_ModelScanAt) {
        g_ModelScanAt = now + 1000;
        for (uint32_t a = 0; a < 0x400 && !found; a += 8) {
            if (ModelAt(sobj, a, -1, out, cap)) {
                g_ModelO1 = a;
                g_ModelO2.store(-1, std::memory_order_release);
                found = true;
                break;
            }
            for (uint32_t b = 0; b < 0x100; b += 8)
                if (ModelAt(sobj, a, (int)b, out, cap)) {
                    g_ModelO1 = a;
                    g_ModelO2.store((int)b, std::memory_order_release);
                    Log("engine: model path at scene object +%#x -> +%#x (\"%s\")", a, b, out);
                    found = true;
                    break;
                }
        }
    }
    ReleaseSRWLockExclusive(&g_ScanLock);
    return found;
}

bool ItemModel(uintptr_t item, char* out, size_t cap) {
    void* sobj = nullptr;
    out[0] = 0;
    return ReadPtr(item + kItemSobj, &sobj) && sobj && FindModel(sobj, out, cap);
}

bool IsGloveModel(const char* path) {
    char low[sizeof(kGloveFolder)];
    const size_t n = strlen(path), k = sizeof(kGloveFolder) - 1;
    for (size_t i = 0; i + k <= n; ++i) {
        for (size_t j = 0; j < k; ++j) low[j] = (char)(path[i + j] | (path[i + j] >= 'A' && path[i + j] <= 'Z' ? 0x20 : 0));
        if (memcmp(low, kGloveFolder, k) == 0) return true;
    }
    return false;
}

// Is this csgo_character item a glove? Cached by material, with the verdict in
// the low bit (materials are 2 KB-aligned).
constexpr int kGloveCache = 256;
std::atomic<uintptr_t> g_GloveCache[kGloveCache];
// Distinct csgo_character materials logged so far, so the log says once what
// each arm, sleeve and glove material is called - and does not grow per pass.
constexpr unsigned kMaxLoggedMaterials = 64;
std::atomic<unsigned> g_LoggedMaterials{0};

// The material's logical name through its vtable slot 0 (what the editor's
// world paint reads names with). A call into the engine: guarded, and made
// once per material on a cache miss.
bool MaterialName(void* mat, char* out, size_t cap) {
    out[0] = 0;
    void* vt = nullptr;
    void* fn = nullptr;
    if (!mat || !ReadPtr((uintptr_t)mat, &vt) || !vt || !ReadPtr((uintptr_t)vt, &fn) || !fn) return false;
    const char* name = nullptr;
    __try {
        name = ((const char*(__fastcall*)(void*))fn)(mat);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (!name || !ReadMem((uintptr_t)name, out, 1)) return false;
    size_t i = 0;
    for (; i + 1 < cap; ++i)
        if (!ReadMem((uintptr_t)name + i, out + i, 1) || !out[i]) break;
    out[i] = 0;
    return i > 0;
}

// The first-person arms are one scene object of the agent's own model
// (agents/models/ctm_sas/ctm_sas.vmdl) with a material per part: the bare
// arm, the sleeve, the glove (2026-10-17). So a glove is told by its material,
// not its model: characters/models/shared/arms/glove_<name>/materials/...
// Gloves with a skin on are the other way round: a composite material
// (compmat_1.vmat) on a scene object of their own,
// agents/models/shared/arms/glove_sporty/glove_sporty.vmdl. Either names it.
bool IsGloveItem(uintptr_t item) {
    void* mat = nullptr;
    if (!ReadPtr(item + kItemMat, &mat) || !mat) return false;
    const uintptr_t key = (uintptr_t)mat;
    std::atomic<uintptr_t>& slot = g_GloveCache[SlotOf(key, kGloveCache)];
    const uintptr_t c = slot.load(std::memory_order_relaxed);
    if ((c & ~(uintptr_t)1) == key) return (c & 1) != 0;
    char name[260];
    if (!MaterialName(mat, name, sizeof(name))) return false;   // not cached: a material mid-teardown
    char model[260];
    const bool haveModel = ItemModel(item, model, sizeof(model));
    const bool glove = IsGloveModel(name) || (haveModel && IsGloveModel(model));
    if (!(key & 1)) slot.store(key | (glove ? 1 : 0), std::memory_order_relaxed);
    if (g_LoggedMaterials.fetch_add(1, std::memory_order_relaxed) < kMaxLoggedMaterials)
        Log("engine: character material %s (%p, model %s)%s", name, mat, haveModel ? model : "?", glove ? " - gloves" : "");
    return glove;
}

// Whose the item is: kWeapon, kGloves or kNone. The glove check reads a model
// path, so it is only made when the gloves are wanted (`gloves`).
int ItemTarget(uintptr_t item, bool gloves) {
    void* mat = nullptr;
    if (!ReadPtr(item + kItemMat, &mat)) return WeaponFx::kNone;
    const uintptr_t cls = ShaderClassOf(mat);
    if (cls == kWeaponShaderClass) return kWeapon;
    if (cls == kCharacterShaderClass && gloves && IsGloveItem(item)) return kGloves;
    return WeaponFx::kNone;
}

// --- the model in hands, per target -----------------------------------------
char g_Model[kTargets][260] = {};
std::atomic<unsigned> g_ModelSeq[kTargets] = {};      // odd while g_Model is written
std::atomic<unsigned> g_ModelSerial[kTargets] = {};
SRWLOCK g_ModelWrite = SRWLOCK_INIT;                  // one writer at a time (several workers)

// `path` cut after the glove's own folder: ".../arms/glove_hardknuckle".
bool CutGloveFolder(char* path) {
    for (char* p = path; *p; ++p) *p = *p == '\\' ? '/' : *p;
    char* at = nullptr;
    for (char* p = path; *p && !at; ++p)
        if (_strnicmp(p, kGloveFolder, sizeof(kGloveFolder) - 1) == 0) at = p;
    if (!at) return false;
    char* end = strchr(at + sizeof(kGloveFolder) - 1, '/');   // the end of glove_<name>
    if (end) *end = 0;
    return true;
}

// The gloves' "model" for the settings: the glove's folder,
// "characters/models/shared/arms/glove_hardknuckle". From the material for
// stock gloves (their scene object is the agent's, the same for every glove),
// from the model for skinned ones (their material is a composite).
bool GloveFolder(uintptr_t item, char* out, size_t cap) {
    void* mat = nullptr;
    if (ReadPtr(item + kItemMat, &mat) && MaterialName(mat, out, cap) && CutGloveFolder(out)) return true;
    return ItemModel(item, out, cap) && CutGloveFolder(out);
}

void NoteModel(int target, uintptr_t item) {
    char path[260];
    if (!(target == kGloves ? GloveFolder(item, path, sizeof(path)) : ItemModel(item, path, sizeof(path)))) return;
    AcquireSRWLockExclusive(&g_ModelWrite);
    if (strcmp(path, g_Model[target]) != 0) {
        g_ModelSeq[target].fetch_add(1, std::memory_order_acq_rel);
        strcpy_s(g_Model[target], path);
        g_ModelSeq[target].fetch_add(1, std::memory_order_acq_rel);
        g_ModelSerial[target].fetch_add(1, std::memory_order_relaxed);
    }
    ReleaseSRWLockExclusive(&g_ModelWrite);
}

// Every distinct model among the marked items, logged once: the weapon itself,
// its attachments, the gloves, and anything else that draws with those shaders
// in the viewmodel pass. Scene objects are remembered by pointer, so a model is
// read once per object, not per frame.
constexpr int kSeenObjects = 64;
void* g_SeenObj[kSeenObjects] = {};
int g_SeenCount = 0;

void NoteItemModels(uintptr_t base, int count, const int8_t* targets) {
    if (g_ModelO2.load(std::memory_order_relaxed) == -2 || g_SeenCount >= kSeenObjects) return;
    AcquireSRWLockExclusive(&g_ModelWrite);
    for (int i = 0; i < count && g_SeenCount < kSeenObjects; ++i) {
        if (targets[i] < 0) continue;
        const uintptr_t e = base + (size_t)i * g_Stride;
        void* sobj = nullptr;
        if (!ReadPtr(e + kItemSobj, &sobj) || !sobj) continue;
        bool seen = false;
        for (int k = 0; k < g_SeenCount && !seen; ++k) seen = g_SeenObj[k] == sobj;
        if (seen) continue;
        g_SeenObj[g_SeenCount++] = sobj;
        if (targets[i] == kGloves) continue;   // one scene object for arms and gloves; the glove's material is logged
        char path[260];
        void* mat = nullptr;
        ReadPtr(e + kItemMat, &mat);
        Log("engine: weapon item %d/%d model %s (scene object %p, material %p)", i, count,
            ItemModel(e, path, sizeof(path)) ? path : "?", sobj, mat);
    }
    ReleaseSRWLockExclusive(&g_ModelWrite);
}

// --- the passes ---------------------------------------------------------------

// Per call: whose each item is. The list is walked several times; reading the
// verdict once keeps the glove model reads to one per item.
struct Classified {
    int8_t target[kMaxItems];
    int count[kTargets];
};

// The viewmodel's depth pass without the marked items. True when it issued
// the call itself (or skipped it: nothing but them was in it).
bool StripMarked(RenderObjectsFn orig, void* p1, void* p2, void* items, int count, void* p5, void* L, void* p7,
                 const int8_t* strip, void** result) {
    const uintptr_t base = (uintptr_t)items;
    int n0 = 0;
    for (int i = 0; i < count; ++i) n0 += strip[i] ? 1 : 0;
    if (!n0) return false;
    g_Stripped.fetch_add((unsigned)n0, std::memory_order_relaxed);
    if (n0 == count) { *result = nullptr; return true; }
    if ((size_t)count * g_Stride > kScratchBytes) { g_NoRoom.fetch_add(1); return false; }
    const int slot = ClaimScratch();
    if (slot < 0) { g_NoRoom.fetch_add(1); return false; }
    uint8_t* out = g_Scratch[slot];
    size_t n = 0;
    for (int i = 0; i < count; ++i) {
        if (strip[i]) continue;
        if (!ReadMem(base + (size_t)i * g_Stride, out + n * g_Stride, g_Stride)) { ReleaseScratch(slot); return false; }
        ++n;
    }
    *result = orig(p1, p2, out, (int)n, p5, L, p7);
    ReleaseScratch(slot);
    return true;
}

// Copies `count` items into `out`, putting `marker[target]` on the ones whose
// target is in `which` and leaving the rest (unless `onlyWhich`). Returns the
// item count written, -1 on a failed read.
int BuildList(uintptr_t base, int count, const int8_t* targets, void* const* marker, unsigned which, bool onlyWhich,
              uint8_t* out) {
    int n = 0;
    for (int i = 0; i < count; ++i) {
        const int t = targets[i];
        const bool mark = t >= 0 && (which >> t & 1u);
        if (onlyWhich && !mark) continue;
        uint8_t* dst = out + (size_t)n * g_Stride;
        if (!ReadMem(base + (size_t)i * g_Stride, dst, g_Stride)) return -1;
        if (mark) {
            // Both material slots: +0x28 is the variant the draw takes its
            // render state from; the item's own would draw the marker with its
            // depth and blend.
            *(void**)(dst + kItemMat) = marker[t];
            *(void**)(dst + kItemMat2) = marker[t];
            *(uint32_t*)(dst + kItemColor) = kMarkerColor;
        }
        ++n;
    }
    return n;
}

// The viewmodel's colour pass with the markers on: the replace targets' items
// swapped for their marker in the engine's list, the overlay targets' items
// drawn again with theirs in a second call after it.
bool MarkItems(RenderObjectsFn orig, void* p1, void* p2, void* items, int count, void* p5, void* L, void* p7,
               const int8_t* targets, void* const* marker, unsigned replace, unsigned overlay, void** result) {
    const uintptr_t base = (uintptr_t)items;
    if (!replace && !overlay) return false;
    if ((size_t)count * g_Stride > kScratchBytes) { g_NoRoom.fetch_add(1); return false; }
    int slotA = -1, slotB = -1;
    int nA = count, nB = 0;
    void* listA = items;
    if (replace) {
        slotA = ClaimScratch();
        if (slotA < 0) { g_NoRoom.fetch_add(1); return false; }
        nA = BuildList(base, count, targets, marker, replace, false, g_Scratch[slotA]);
        listA = g_Scratch[slotA];
    }
    if (overlay && nA >= 0) {
        slotB = ClaimScratch();
        if (slotB < 0) { g_NoRoom.fetch_add(1); ReleaseScratch(slotA); return false; }
        nB = BuildList(base, count, targets, marker, overlay, true, g_Scratch[slotB]);
    }
    if (nA < 0 || nB < 0) { ReleaseScratch(slotA); ReleaseScratch(slotB); return false; }
    *result = orig(p1, p2, listA, nA, p5, L, p7);   // the skin first, as the engine handed it over
    if (nB > 0) orig(p1, p2, g_Scratch[slotB], nB, p5, L, p7);
    ReleaseScratch(slotA);
    ReleaseScratch(slotB);
    return true;
}

bool HandleImpl(RenderObjectsFn orig, void* p1, void* p2, void* items, int count, void* p5, void* L, void* p7,
                void** result) {
    uint64_t view = 0;
    uint32_t lflags = 0;
    if (!items || !L || count <= 0 || count > kMaxItems || !ReadMem((uintptr_t)L + kPassViewBits, &view, 8) ||
        !(view & kViewIsViewmodel) || !ReadMem((uintptr_t)L + kPassLflags, &lflags, 4))
        return false;
    void* marker[kTargets] = {};
    Mode mode[kTargets] = {};
    unsigned on = 0;
    for (int t = 0; t < kTargets; ++t) {
        mode[t] = (Mode)g_Mode[t].load(std::memory_order_relaxed);
        marker[t] = mode[t] == Mode::Off ? nullptr : Material::MarkerIfReady(t);
        if (marker[t]) on |= 1u << t;
    }
    // The gloves' model is followed whatever their mode: the settings are found
    // by it (a [glove_sporty] section can turn them on while [gloves] is off),
    // and the first glove seen is what lets the host create their marker. The
    // model read is cached per scene object, so this is one lookup per item.
    const bool wantGloves = true;
    g_Calls.fetch_add(1, std::memory_order_relaxed);
    // On the stack: 4 KB per list, far inside a worker's stack, and
    // nothing here recurses.
    Classified cl;
    cl.count[kWeapon] = cl.count[kGloves] = 0;
    const uintptr_t base = (uintptr_t)items;
    int first[kTargets] = { -1, -1 };
    for (int i = 0; i < count; ++i) {
        const int t = ItemTarget(base + (size_t)i * g_Stride, wantGloves);
        cl.target[i] = (int8_t)t;
        if (t >= 0) {
            ++cl.count[t];
            if (first[t] < 0) first[t] = i;
        }
    }
    if (!cl.count[kWeapon] && !cl.count[kGloves]) return false;
    if (lflags & kLflagDepth) {
        if (!on) return false;
        int8_t strip[kMaxItems];
        for (int i = 0; i < count; ++i) strip[i] = (int8_t)(cl.target[i] >= 0 && (on >> cl.target[i] & 1u));
        return StripMarked(orig, p1, p2, items, count, p5, L, p7, strip, result);
    }
    for (int t = 0; t < kTargets; ++t)
        if (first[t] >= 0) NoteModel(t, base + (size_t)first[t] * g_Stride);
    NoteItemModels(base, count, cl.target);
    // Off, or the marker not made yet: the models are still followed, for the
    // next settings.
    unsigned replace = 0, overlay = 0;
    for (int t = 0; t < kTargets; ++t) {
        if (!(on >> t & 1u) || !cl.count[t]) continue;
        (mode[t] == Mode::Replace ? replace : overlay) |= 1u << t;
    }
    if (!replace && !overlay) return false;
    if ((replace | overlay) & (1u << kWeapon)) g_WeaponItems.fetch_add((unsigned)cl.count[kWeapon], std::memory_order_relaxed);
    if ((replace | overlay) & (1u << kGloves)) g_GloveItems.fetch_add((unsigned)cl.count[kGloves], std::memory_order_relaxed);
    return MarkItems(orig, p1, p2, items, count, p5, L, p7, cl.target, marker, replace, overlay, result);
}

void* __fastcall HkRenderObjects(void* p1, void* p2, void* items, int count, void* p5, void* L, void* p7) {
    struct Guard {
        Guard() { g_InHook.fetch_add(1, std::memory_order_acquire); }
        ~Guard() { g_InHook.fetch_sub(1, std::memory_order_release); }
    } guard;
    const RenderObjectsFn orig = (RenderObjectsFn)g_Det.orig;
    void* r = nullptr;
    if (HandleImpl(orig, p1, p2, items, count, p5, L, p7, &r)) return r;
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

bool Attach(size_t itemStride) {
    if (itemStride >= 0x40 && itemStride <= 0x100 && itemStride % 8 == 0) g_Stride = itemStride;
    if (!Material::Resolve()) return false;
    g_Attached = true;
    Log("engine: attached to the host's RenderObjects, item stride %#zx", g_Stride);
    return true;
}

bool Handle(RenderObjectsFn orig, void* p1, void* p2, void* items, int count, void* p5, void* L, void* p7,
            void** result) {
    if (!g_Attached || !orig) return false;
    return HandleImpl(orig, p1, p2, items, count, p5, L, p7, result);
}

bool Wanted() {
    for (int t = 0; t < kTargets; ++t)
        if (g_Mode[t].load(std::memory_order_relaxed) != (int)Mode::Off) return true;
    return false;
}

void SetMode(int target, Mode m) {
    if (target >= 0 && target < kTargets) g_Mode[target].store((int)m, std::memory_order_relaxed);
}
Mode GetMode(int target) {
    return target >= 0 && target < kTargets ? (Mode)g_Mode[target].load(std::memory_order_relaxed) : Mode::Off;
}

void Model(int target, char* out, unsigned cap, unsigned* serial) {
    if (!out || !cap) return;
    out[0] = 0;
    if (target < 0 || target >= kTargets) return;
    for (int tries = 0; tries < 100; ++tries) {
        const unsigned s0 = g_ModelSeq[target].load(std::memory_order_acquire);
        if (s0 & 1) { YieldProcessor(); continue; }
        strncpy_s(out, cap, g_Model[target], _TRUNCATE);
        if (g_ModelSeq[target].load(std::memory_order_acquire) == s0) {
            if (serial) *serial = g_ModelSerial[target].load(std::memory_order_relaxed);
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

unsigned GloveItems() { return g_GloveItems.load(); }

} // namespace Engine
} // namespace Wfx
