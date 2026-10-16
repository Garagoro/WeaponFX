#include "material.h"
#include "log.h"
#include "patch.h"

#include <Windows.h>
#include <atomic>
#include <cstdio>
#include <cstring>

namespace Wfx {
namespace Material {
namespace {

// The marker: csgo_unlitgeneric with every depth feature off (the weapon has
// to stay out of the depth passes, or the world behind it is never drawn) and
// a texture nothing else in a match uses.
const char kMarkerKv3[] =
    "<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} "
    "format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->\n"
    "{\n"
    "\tshader = \"csgo_unlitgeneric.vfx\"\n"
    "\tF_TWOTEXTURE = 1\n"
    "\tF_DISABLE_Z_BUFFERING = 1\n"
    "\tF_DISABLE_Z_PREPASS = 1\n"
    "\tF_DISABLE_Z_WRITE = 1\n"
    "\tg_vColorTint = [1.0, 1.0, 1.0, 0.0]\n"
    "\tg_tColor = resource:\"materials/dev/black_grid_8_color_tga_dd8b6813.vtex\"\n"
    "\tg_tColor2 = resource:\"materials/dev/black_grid_8_color_tga_dd8b6813.vtex\"\n"
    "}\n";
// The same texture, as the resource system names it. Nothing in a match loads
// it, so it is asked for before the material is made; without that the
// material comes out with the engine's error texture.
constexpr char kMarkerTexture[] = "materials/dev/black_grid_8_color_tga_dd8b6813.vtex";
constexpr ULONGLONG kLoadWaitMs = 500;   // the request is not blocking; give it a moment
constexpr char kMarkerParam[] = "g_tColor";

// materialsystem2!CreateMaterial(this-less, CStrongHandle* out, name, kv3, flags, flags).
constexpr short kSigCreateMaterial[] = {
    0x48, 0x89, 0x5C, 0x24, -1, 0x48, 0x89, 0x6C, 0x24, -1, 0x48, 0x89, 0x74, 0x24, -1,
    0x48, 0x89, 0x7C, 0x24, -1, 0x41, 0x56, 0x48, 0x81, 0xEC, -1, -1, -1, -1, 0x48, 0x8B, 0x05,
    -1, -1, -1, -1, 0x48, 0x8B, 0xF2 };
// tier0: a call to the KV3 text loader, followed by `mov ecx,[rsp+34h]; movzx ebx,al`.
constexpr short kSigLoadKv3Call[] = { 0xE8, -1, -1, -1, -1, 0x8B, 0x4C, 0x24, 0x34, 0x0F, 0xB6, 0xD8 };

using CreateMaterialFn = long long(__fastcall*)(void*, void*, const char*, void*, unsigned, unsigned);
using LoadKv3Fn = bool(__fastcall*)(void* kv, void* err, void* buf, void* id, void*, void*, void*, void*, const char*);
using UtlBufferInitFn = void(__fastcall*)(void* self, int growSize, int initSize, int flags);
using UtlBufferPutStringFn = void(__fastcall*)(void* self, const char* s);
using UtlBufferEnsureFn = void(__fastcall*)(void* self, int size);

CreateMaterialFn g_Create = nullptr;
LoadKv3Fn g_LoadKv3 = nullptr;
UtlBufferInitFn g_BufInit = nullptr;
UtlBufferPutStringFn g_BufPut = nullptr;
UtlBufferEnsureFn g_BufEnsure = nullptr;

// Layouts (engine builds of 2026-10). Each read is guarded; a moved field
// reads as "no marker", which leaves the weapon to the engine - never a wrong
// draw.
constexpr size_t kUtlBufferBytes = 0x80;       // CUtlBuffer, opaque, on our stack
constexpr size_t kKv3Bytes = 0x1100;           // KeyValues3 root, zeroed, type set below
constexpr uint64_t kKv3TableType = 0x1805;     // KV3 type "table" in the root's first word
constexpr size_t kMatParamCount = 0x18;        // CMaterial2 -> u32 parameter count
constexpr size_t kMatParamBlock = 0x20;        // CMaterial2 -> parameter records
constexpr size_t kParamStride = 0x40;
constexpr size_t kParamName = 0x28;            // record -> const char*
constexpr size_t kParamTexBinding = 0x10;      // texture record -> resource binding
constexpr size_t kTexObjViewA = 0x10;          // texture object -> SRV (linear)
constexpr size_t kTexObjViewB = 0x18;          // texture object -> SRV (sRGB)
constexpr size_t kBindingRefCount = 0x20;      // ResourceBinding -> int32 owners
constexpr uint32_t kRefCountSane = 0x1000000;
constexpr uint32_t kMaxParams = 512;

struct Kv3Id { const char* name; uint64_t a, b; };

std::atomic<void*> g_Marker{nullptr};
bool g_Tried = false;
ULONGLONG g_CreateAt = 0;   // 0: the texture not requested yet

// IResourceSystem (resourcesystem.dll, by interface name), slot 22: preload a
// list of resources (ResourceLoadRequest_t).
constexpr char kResVersion[] = "ResourceSystem013";
constexpr size_t kResPreloadSlot = 0xB0;
using ResPreloadFn = void(__fastcall*)(void*, void*);
using CreateInterfaceFn = void*(__cdecl*)(const char*, int*);

bool CallPreload(void* rs, ResPreloadFn fn, const char* path) {
    __try {
        const char* paths[1] = { path };
        uint8_t req[0x40] = {};
        *(uint32_t*)(req + 0x00) = 1;              // count
        *(const char***)(req + 0x08) = paths;      // paths
        req[0x10] = 0;                             // no blocking: this runs inside Present
        req[0x11] = 1;                             // load flag
        req[0x20] = 0xFF;                          // behaviour
        fn(rs, req);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void RequestTexture() {
    HMODULE m = GetModuleHandleW(L"resourcesystem.dll");
    const CreateInterfaceFn create = m ? (CreateInterfaceFn)GetProcAddress(m, "CreateInterface") : nullptr;
    void* rs = nullptr;
    __try {
        rs = create ? create(kResVersion, nullptr) : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        rs = nullptr;
    }
    void* vt = nullptr;
    void* fn = nullptr;
    if (!rs || !ReadMem((uintptr_t)rs, &vt, sizeof(vt)) || !vt || !ReadMem((uintptr_t)vt + kResPreloadSlot, &fn, sizeof(fn)) || !fn) {
        Log("material: %s not found - the marker may come out with the error texture", kResVersion);
        return;
    }
    if (!CallPreload(rs, (ResPreloadFn)fn, kMarkerTexture)) Log("material: preload of %s faulted", kMarkerTexture);
}

bool ReadPtr(uintptr_t at, void** out) { return ReadMem(at, out, sizeof(*out)); }

void* LoadKv3(const char* text) {
    void* kv = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, kKv3Bytes);
    if (!kv) return nullptr;
    *(uint64_t*)kv = kKv3TableType;
    uint8_t buf[kUtlBufferBytes] = {};
    const int len = (int)strlen(text) + 16;
    g_BufInit(buf, 0, len, 1);
    if (g_BufEnsure) g_BufEnsure(buf, len);
    g_BufPut(buf, text);
    Kv3Id id = { "generic", 0x41B818518343427EULL, 0xB5F447C23C0CDF8CULL };
    if (!g_LoadKv3(kv, nullptr, buf, &id, nullptr, nullptr, nullptr, nullptr, "")) {
        // The root stays allocated: the engine may already point into it.
        Log("material: KV3 text rejected");
        return nullptr;
    }
    // Owned by the material from here on; never freed (one per load).
    return kv;
}

// CreateMaterial does not take a reference for the handle it writes; with
// zero owners the resource system may purge a material we keep drawing with.
// One reference of ours, never released: the material lives for the process.
void TakeReference(void* binding) {
    uint32_t n = 0;
    if (!ReadMem((uintptr_t)binding + kBindingRefCount, &n, sizeof(n)) || n >= kRefCountSane) return;
    InterlockedIncrement((volatile LONG*)((uintptr_t)binding + kBindingRefCount));
}

} // namespace

bool Resolve() {
    if (g_Create && g_LoadKv3) return true;
    int hits = 0;
    const uintptr_t create = FindUnique("materialsystem2.dll", kSigCreateMaterial,
                                        sizeof(kSigCreateMaterial) / sizeof(kSigCreateMaterial[0]), &hits);
    if (!create) { Log("material: CreateMaterial signature matched %d times (need 1)", hits); return false; }
    const uintptr_t call = FindUnique("tier0.dll", kSigLoadKv3Call, sizeof(kSigLoadKv3Call) / sizeof(kSigLoadKv3Call[0]),
                                      &hits);
    if (!call) { Log("material: KV3 loader call matched %d times (need 1)", hits); return false; }
    int32_t rel = 0;
    memcpy(&rel, (const void*)(call + 1), sizeof(rel));
    HMODULE t0 = GetModuleHandleW(L"tier0.dll");
    g_BufInit = (UtlBufferInitFn)GetProcAddress(t0, "??0CUtlBuffer@@QEAA@HHW4BufferFlags_t@0@@Z");
    g_BufPut = (UtlBufferPutStringFn)GetProcAddress(t0, "?PutString@CUtlBuffer@@QEAAXPEBD@Z");
    g_BufEnsure = (UtlBufferEnsureFn)GetProcAddress(t0, "?EnsureCapacity@CUtlBuffer@@QEAA_NH@Z");
    if (!g_BufEnsure) g_BufEnsure = (UtlBufferEnsureFn)GetProcAddress(t0, "?EnsureCapacity@CUtlBuffer@@QEAAXH@Z");
    if (!g_BufInit || !g_BufPut) { Log("material: tier0 CUtlBuffer exports missing"); return false; }
    g_LoadKv3 = (LoadKv3Fn)(call + 5 + (intptr_t)rel);
    g_Create = (CreateMaterialFn)create;
    Log("material: CreateMaterial %p, KV3 loader %p", (void*)create, (void*)g_LoadKv3);
    return true;
}

void* Marker() {
    if (void* m = g_Marker.load(std::memory_order_acquire)) return m;
    if (g_Tried) return nullptr;
    if (!g_CreateAt) {
        RequestTexture();
        g_CreateAt = GetTickCount64() + kLoadWaitMs;
        return nullptr;
    }
    if (GetTickCount64() < g_CreateAt) return nullptr;
    g_Tried = true;   // one attempt per load: a failure is logged, not retried every frame
    if (!Resolve()) return nullptr;
    void* kv = LoadKv3(kMarkerKv3);
    if (!kv) return nullptr;
    // A name per load: a second load in the same game session must not get
    // the first one's (possibly purged) resource back.
    char name[96];
    _snprintf_s(name, sizeof(name), _TRUNCATE, "materials/weaponfx/marker_%llx.vmat",
                (unsigned long long)GetTickCount64());
    void* binding = nullptr;   // CStrongHandle: one pointer, to the resource binding
    __try {
        g_Create(nullptr, &binding, name, kv, 0, 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("material: CreateMaterial faulted");
        return nullptr;
    }
    void* mat = nullptr;
    if (!binding || !ReadPtr((uintptr_t)binding, &mat) || !mat) { Log("material: CreateMaterial returned nothing"); return nullptr; }
    TakeReference(binding);
    g_Marker.store(mat, std::memory_order_release);
    Log("material: marker %s = %p", name, mat);
    return mat;
}

void* MarkerIfReady() { return g_Marker.load(std::memory_order_acquire); }

void MarkerViews(ID3D11ShaderResourceView** a, ID3D11ShaderResourceView** b) {
    *a = *b = nullptr;
    void* mat = g_Marker.load(std::memory_order_acquire);
    if (!mat) return;
    uint32_t count = 0;
    void* block = nullptr;
    if (!ReadMem((uintptr_t)mat + kMatParamCount, &count, sizeof(count)) || !ReadPtr((uintptr_t)mat + kMatParamBlock, &block) ||
        !block)
        return;
    if (count > kMaxParams) count = kMaxParams;
    for (uint32_t i = 0; i < count; ++i) {
        const uintptr_t rec = (uintptr_t)block + (size_t)i * kParamStride;
        void* np = nullptr;
        char name[sizeof(kMarkerParam) + 1] = {};
        if (!ReadPtr(rec + kParamName, &np) || !np || !ReadMem((uintptr_t)np, name, sizeof(kMarkerParam))) continue;
        if (memcmp(name, kMarkerParam, sizeof(kMarkerParam)) != 0) continue;   // includes the terminator
        void* binding = nullptr;
        void* tex = nullptr;
        void* va = nullptr;
        void* vb = nullptr;
        if (!ReadPtr(rec + kParamTexBinding, &binding) || !binding || !ReadPtr((uintptr_t)binding, &tex) || !tex) return;
        ReadPtr((uintptr_t)tex + kTexObjViewA, &va);
        ReadPtr((uintptr_t)tex + kTexObjViewB, &vb);
        *a = (ID3D11ShaderResourceView*)va;
        *b = (ID3D11ShaderResourceView*)vb;
        return;
    }
}

} // namespace Material
} // namespace Wfx
