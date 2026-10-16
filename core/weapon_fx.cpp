#include "weapon_fx.h"

#include <Windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>   // pD3DCompile, flags and D3D_COMPILE_STANDARD_FILE_INCLUDE; the DLL is loaded at run time
#include <d3d11shader.h>   // ID3D11ShaderReflection, to see whether a shader reads the scene copy
#include <wincodec.h>      // WIC, for mask textures (windowscodecs.lib)
#include <psapi.h>         // PROCESS_MEMORY_COUNTERS_EX; K32GetProcessMemoryInfo is in kernel32, no psapi.lib
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace WeaponFx {
namespace {

// A shader source past this is not a shader, it is the wrong file.
constexpr DWORD kMaxSourceBytes = 1u << 20;
// How often the watcher looks at the file's write time.
constexpr DWORD kWatchMs = 250;
// A replaced shader may still be in a draw the render thread is building:
// it loaded the pointer, we swap, it calls PSSetShader. Held this long before
// the last Release - far above one frame, and no fence is needed for it.
constexpr ULONGLONG kRetireMs = 2000;
constexpr int kGraveSlots = 32;
// Memory guard: the effect turns itself off when the game's private commit
// grows this far past what it was when the shader was loaded. The 2026-10-15
// leak added ~28 GB in about two minutes before Windows fell over; a map load
// adds well under this, so a false trip costs only "effect off" and a log line.
constexpr ULONGLONG kMaxGrowthBytes = 4ull << 30;
// Mask textures larger than this are refused rather than uploaded.
constexpr UINT kMaxTexSide = 8192;

// d3dcompiler_47's ID3D11ShaderReflection. Spelled out here because the header
// declares it with DEFINE_GUID, which would otherwise need dxguid.lib.
constexpr GUID kIidShaderReflection = { 0x8d536ca1, 0x0cca, 0x4956, { 0xa8, 0x37, 0x78, 0x69, 0x63, 0x75, 0x55, 0x84 } };
using D3DReflectFn = HRESULT(WINAPI*)(const void*, SIZE_T, REFIID, void**);

// Mirrors the cbuffer in shaders/weaponfx.hlsli; the static_assert
// below and the HLSL packing rules (float4 rows) keep them in step.
struct FxConstants {
    float time[4];     // seconds since Load, delta to the previous draw, draw counter, 0
    float screen[4];   // viewport width, height, 1/width, 1/height
    float param[kParams][4];
};
static_assert(sizeof(FxConstants) % 16 == 0, "constant buffers are float4 rows");

ID3D11Device* g_Dev = nullptr;
LogFn g_Log = nullptr;
pD3DCompile g_Compile = nullptr;
D3DReflectFn g_Reflect = nullptr;
ID3D11Buffer* g_Cb = nullptr;

std::atomic<ID3D11PixelShader*> g_Ps{nullptr};
// Whether the bound shader samples g_fxScene / g_fxDepth; each copy costs a
// full-screen blit, so effects that do not look through the weapon do not pay.
std::atomic<bool> g_PsUsesScene{false};
std::atomic<bool> g_PsUsesDepth{false};
// out = src + dst * (1 - src.a): premultiplied over, so a shader's alpha is how
// much of what is under the weapon's pixel it covers.
ID3D11BlendState* g_Blend = nullptr;

// Mask textures, published for the render thread. Replaced ones are retired.
std::atomic<ID3D11ShaderResourceView*> g_Tex[kTextures] = {};

// The scene behind the weapon and its depth, copied together. Render thread only.
//
// When: before the viewmodel pass, so the copy holds the world and none of the
// viewmodel - neither the hands nor the weapon's own first meshes. Copied at
// the weapon's first draw instead, the glass refracted the world but also
// whatever of the viewmodel was drawn before it (fingers, the blade's first
// meshes), and the knife never fully disappeared. On the main target the
// frame goes: depth prepass, enemies (the marker is on them too), world, then
// the viewmodel - one run of ~34 draws under one VS b1 (per-view constants).
// b1 is a different buffer every frame (a pool), so the run is found by its
// ordinal among b1 changes: the run with the most weapon draws last frame is
// copied at its first draw this frame. A wrong guess (the frame's structure
// changed) falls back to copying at the weapon draw, as before, for a frame.
//
// Where to: a few copies kept per kind, matched by the source's shape. The
// weapon is drawn into more than one target a frame (the main 2560x1440 one
// and a half-size 1280x720 effects pass), and with one texture per kind each
// switch recreated it - twice a frame. The replaced one went to the retire
// list, which filled in a fraction of a second and then leaked every one:
// 2026-10-15 cs2.exe reached 35.9 GB of commit in about two minutes, Windows
// ran out of virtual memory, DWM and every other app fell over and the PC
// rebooted. Slots are owned by the render thread alone, so a replaced slot is
// released on the spot: D3D11 holds its own reference to anything still bound,
// and the copies are never published to another thread.
struct CopySlot {
    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    UINT w = 0, h = 0;
    DXGI_FORMAT src = DXGI_FORMAT_UNKNOWN, view = DXGI_FORMAT_UNKNOWN;
    uint32_t used = 0;             // g_Frame when last copied into
};
constexpr int kCopySlots = 3;      // targets the weapon is seen on per frame: main, half-size, one spare
// Two sets, one per frame parity: this frame's copies go into one, so the
// other still holds last frame's - the fallback a broken frame falls back to
// (g_PrevSceneSrv, t38). One extra screen-size texture.
CopySlot g_SceneSlots[2][kCopySlots];
int g_SceneParity = 0;             // the set this frame copies into; flipped in RollFrame
CopySlot g_DepthSlots[kCopySlots];
// The bisect's own (DebugDepthAt): in the shared slots the run-start copy
// of the same shape overwrote it before the weapon was drawn, and every index
// read as the run start.
CopySlot g_DbgDepthSlots[kCopySlots];
CopySlot g_DbgSceneSlots[kCopySlots];
ID3D11ShaderResourceView* g_DbgSceneSrv = nullptr;
// The copies the next weapon draw binds (point into the slots, not owning).
ID3D11Texture2D* g_SceneTex = nullptr;
ID3D11ShaderResourceView* g_SceneSrv = nullptr;
ID3D11Texture2D* g_DepthTex = nullptr;
ID3D11ShaderResourceView* g_DepthSrv = nullptr;
bool g_DepthReady = false;         // the last copy included depth
uint32_t g_DepthClearFrame = 0xFFFFFFFFu;
uint32_t g_WritesSinceClear = 0;
// The first viewmodel run's copies, held (own reference) for the later
// viewmodel runs of the frame: copies of other targets are taken in between
// and move g_SceneSrv, and the gloves run then read a half-size copy - black
// in the P2000's grip (2026-10-15). Render thread only.
ID3D11ShaderResourceView* g_VmSceneSrv = nullptr;
ID3D11ShaderResourceView* g_VmDepthSrv = nullptr;
uint32_t g_VmSceneFrame = ~0u;     // frame g_VmSceneSrv was copied in
// Last frame's good copy (held), bound as t38: SceneAt() reads it where this
// frame's copy is pure black - whatever made it black.
ID3D11ShaderResourceView* g_PrevSceneSrv = nullptr;
// A frame whose viewmodel was not found (no copy, the weapon past the
// predicted run) draws with the last good copy instead of none - black was
// what every past miss looked like (the blinks of 2026-10-16). Older than
// this, it is too far behind the camera and the frame stays without
// (fail closed: a weapon switch, a respawn). ~25 ms at 300 fps, 130 at 60.
constexpr uint32_t kStaleCopyFrames = 8;
bool g_EnsureStale = false;        // the last EnsureCopies fell back to an old copy
uint32_t g_CoveredFrame = ~0u;
std::atomic<uint32_t> g_CoveredFrames{0};   // frames drawn with the fallback, all time
uint32_t g_CoveredWindow = 0;                // the same, since the last blink-watch line
bool g_VmDepthCopied = false;   // depth-writing draws on the main depth since its last clear   // frame whose depth was copied at a clear (BeforeClearDepth)
// Fuse: a copy texture created more often than this per second means targets
// change shape faster than the slots cover. Copies then stop (effects read
// black) until the next Load, with one log line - never a creation per frame.
constexpr uint32_t kMaxCreatesPerSecond = 8;
ULONGLONG g_CreateWindowAt = 0;
uint32_t g_CreatesInWindow = 0;
std::atomic<bool> g_CopiesTripped{false};
std::atomic<uint32_t> g_CopyCreates{0};
uint32_t g_SceneFrame = 0xFFFFFFFFu;
uint32_t g_OtherDraws = 0;
std::atomic<uint32_t> g_Frame{0};
std::atomic<uint32_t> g_SceneCopies{0}, g_SceneSkips{0}, g_DepthCopies{0}, g_DepthSkips{0};

// Runs of draws under one VS b1, per frame. Render thread only.
constexpr int kMaxRuns = 64;       // ~10 on the main target, a few hundred draws in all
ID3D11DeviceContext* g_TrackCtx = nullptr;   // the context the weapon is drawn on; compared, never called
const void* g_RunB1 = nullptr;     // compared, never dereferenced
int g_Run = -1;
uint16_t g_RunWeapon[kMaxRuns] = {};
uint16_t g_RunWrites[kMaxRuns] = {};   // other draws with a colour target that write depth: the hands
uint16_t g_RunNoZ[kMaxRuns] = {};      // weapon draws without a depth test (other overlay passes test EQUAL)
// Width of the colour target the run's weapon draws went to. The weapon is
// drawn again into half-size passes; with one draw each, the half-size run
// tied the real one and won every other frame - the prepass drop and the
// copies then followed the wrong target and the see-through blinked black
// (2026-10-16).
uint32_t g_RunRtW[kMaxRuns] = {};
// The viewmodel runs predicted from last frame (bit per run) - there can be
// two: the weapon with its sleeves, then a clear of the main depth, then the
// gloves (and, with some weapons, part of the weapon). The copy is taken at the
// first one; our depth is prepared afresh at each.
uint64_t g_VmRuns = 0;   // relative to the copied run: bit 0 is it, bit k the k-th run after it
// Viewmodel runs whose scene was copied at their first draw of the weapon's
// buffers this frame. That is the moment that has the hands (through the
// P2000's grip the palm shows - at the run's start its colour is not drawn
// yet and the world behind it never is: black) and not yet the engine's own
// passes of the weapon, which with replace on are the marker's black. Once
// per run: a hand drawn between the weapon's meshes would bring the marker in.
// Frame whose scene copy is taken: once, before the first draw of the weapon
// in hands on the main colour target - the engine's own passes of it included.
uint32_t g_SceneDoneFrame = ~0u;
// Later viewmodel runs that took a fresh copy this frame. A marked piece drawn
// among the gloves, after them (one draw in a run after the knife's), read the
// copy taken before the gloves and painted the black behind them over the
// glove (2026-10-16). Each later run copies again before its first marked draw.
uint64_t g_LateRunsCopied = 0;
// The marker material's pixel shader, as bound on our marked draws (Begin).
// In replace mode the engine also draws the weapon's own passes with
// it - the marker's black, no depth test - and once our pass stopped covering
// everything (it now hides behind the fingers) that black showed in the
// P2000's grip. Those passes are dropped in viewmodel runs; with replace off
// they carry the skin's own shader and stay. Compared, never called.
const void* g_MarkerPs = nullptr;
std::atomic<uint32_t> g_MarkerPassSkips{0};
static_assert(kMaxRuns <= 64, "g_VmRuns is one bit per run");
// Debug (DebugDepthAt(n)): the main depth copied right before the n-th
// draw of the frame (counted on the weapon's context) and shown as t37 instead
// of the run copy - a bisect for whoever writes depth under the weapon.
std::atomic<int> g_DbgDepthAt{-1};
int g_DbgDrawIdx = 0;
// DebugDepthAt(-2): one frame's scene copies and our draws, in order.
// 1 armed (next frame), 2 recording, 0 off.
int g_Trace = 0;
std::atomic<bool> g_TraceWanted{false};
int g_DbgDrawsLastFrame = 0;
ID3D11ShaderResourceView* g_DbgDepthSrv = nullptr;
const void* g_DbgDepthDsv = nullptr;
int g_PredictedRun = -1;
int g_CopiedRun = -1;              // the run whose start was copied this frame
uint32_t g_TrackFrame = 0xFFFFFFFFu;

// The viewmodel weapon's vertex buffers, learnt from its marked draws in the
// copied run; last frame's set picks its draws out of this frame's depth
// prepass (which comes first). Double-buffered so a weapon switch cannot leave
// a stale buffer in the set for more than a frame. Compared, never called.
constexpr int kMaxVmVbs = 16;      // a weapon has 5..11 meshes
const void* g_VmVb[2][kMaxVmVbs] = {};
int g_VmVbCount[2] = {};
int g_VmVbCur = 0;                 // the set being collected; the other one is used
// The same, from the copied run only: what fires the scene copy. The later
// viewmodel runs hold a weapon also drawn with the world (its buffer fired the
// copy mid-world), but they also hold parts of the weapon in hands in some
// holds (the knife's handle in the palm), so the prepass still drops them all.
const void* g_HandVb[2][kMaxVmVbs] = {};
int g_HandVbCount[2] = {};
const void* g_MainDsv = nullptr;   // depth target bound at the viewmodel run's start
// Colour target bound there. The weapon's buffers are drawn into other targets
// inside the viewmodel runs too (a 2560x1440 R32, half- and quarter-size
// passes); a copy of one of those replaced the scene and the see-through
// weapon read black - "the shadow" (2026-10-15).
const void* g_MainRt = nullptr;
// Frames in a row without a viewmodel start. The start is only taken on
// g_MainRt, so a wrong one learnt once (another program's view drawing a
// marked weapon - a 3D preview window of another tool) locked it out for good. Past
// kRelearnFrames the targets are forgotten and learnt again.
constexpr uint32_t kRelearnFrames = 30;

// How the viewmodel's start is found: the first draw on the screen target
// that tests and writes depth after a stencil-only clear of the main depth.
// The engine clears that stencil after the world, and the viewmodel's first
// hand is the next such draw (a dump of the frame's targets and draws, five frames, 2026-10-16). It is an
// event, not a guess. What it replaced - the run index or the b1 pointer
// predicted from last frame - landed in the world when a weapon switch, a
// turn or a shot changed the run layout; the whole world then went to our
// viewmodel depth and the screen blinked. Measured before the switch: 13274
// frames of switches, knife, shots and turns, the event's run matched the
// run of the first marked draw without a depth test every time; the guess
// missed 253 times in the same span.
//
// Still compared every frame with that marked draw ("event start" counters,
// a WRONG line in the log): a game update that moves the clear shows there
// first. Frames without the event (the targets not learnt yet, the first
// frame) start at the first marked draw instead - the fallback in BeforeDraw.
uint32_t g_StencilClearFrame = ~0u;
int g_ShadowRun = -1, g_ShadowDraw = -1;   // this frame's event start
int g_TruthRun = -1, g_TruthDraw = -1;     // this frame's first marked no-Z draw on the screen target
uint32_t g_ShadowAgree = 0, g_ShadowWrong = 0, g_ShadowNone = 0;
std::atomic<uint32_t> g_ShadowAgreeAll{0}, g_ShadowWrongAll{0}, g_ShadowNoneAll{0};
ULONGLONG g_ShadowLogAt = 0, g_ShadowWrongLogAt = 0;
// The world first. A stencil-only clear of the main depth also comes early in
// some frames (2 of 7929: the start fired at #147 with the weapon at #555,
// and the world after it would have gone to our viewmodel depth). The event
// is taken only once at least half as many draws went to the screen target
// as last frame had before its start.
uint32_t g_MainDraws = 0;          // draws on the screen target this frame, before the start
uint32_t g_MainDrawsAtStart = 0;   // the same count at last frame's start
std::atomic<uint32_t> g_EventsEarly{0};
ULONGLONG g_EarlyLogAt = 0;
uint32_t g_FramesNoStart = 0;
// How the screen target is learnt (and learnt again): over a frame, the widest
// target a marked draw without a depth test went to, with its depth target.
// The weapon is drawn into half- and quarter-size passes too; the screen is
// the widest. Taken over at the frame's end when no target is known.
const void* g_CandRt = nullptr;
const void* g_CandDsv = nullptr;
uint32_t g_CandW = 0;
std::atomic<uint32_t> g_Relearns{0};
std::atomic<uint32_t> g_PrepassSkips{0};

// Viewmodel depth (see SetViewmodelDepth). Render thread only, except the
// switch. The main depth is cleared by the engine right after the viewmodel
// run, so leaving the hands out of it costs nothing later in the frame.
std::atomic<bool> g_VmDepthOn{true};
ID3D11Texture2D* g_VmDepthTex = nullptr;
ID3D11DepthStencilView* g_VmDsv = nullptr;
D3D11_TEXTURE2D_DESC g_VmDepthDesc = {};
DXGI_FORMAT g_VmDsvFormat = DXGI_FORMAT_UNKNOWN;
uint32_t g_VmDepthFrame = 0xFFFFFFFFu;
int g_VmDepthRun = -1;             // the run g_VmDepthFrame's preparation was for
bool g_VmDepthReady = false;       // prepared for g_VmDepthFrame
ID3D11DepthStencilState* g_DsPre = nullptr;   // depth-only pass: LESS_EQUAL, write, no stencil
ID3D11DepthStencilState* g_DsFx = nullptr;    // our colour pass: LESS_EQUAL, no write, no stencil
std::atomic<uint32_t> g_VmDepthDraws{0}, g_VmDepthPre{0}, g_VmDepthFails{0};

SRWLOCK g_CompileLock = SRWLOCK_INIT;   // Load and the watcher both compile
char g_Path[1024] = {};   // UTF-8
FILETIME g_PathTime = {};

struct Grave { IUnknown* obj; ULONGLONG at; };
Grave g_Grave[kGraveSlots] = {};
SRWLOCK g_GraveLock = SRWLOCK_INIT;   // the watcher, Load and the render thread all retire

HANDLE g_Stop = nullptr;
HANDLE g_Watcher = nullptr;

std::atomic<float> g_Param[kParams][4] = {};
LARGE_INTEGER g_T0 = {}, g_Freq = {};
std::atomic<int64_t> g_LastTick{0};
std::atomic<uint32_t> g_Draws{0};
std::atomic<uint32_t> g_Compiles{0}, g_Failures{0};

void Logf(const char* fmt, ...) {
    if (!g_Log) return;
    char line[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(line, sizeof(line), _TRUNCATE, fmt, ap);
    va_end(ap);
    g_Log(line);
}

// Releases graves older than kRetireMs; `all` at shutdown, when no draw runs.
void BuryLocked(bool all) {
    const ULONGLONG now = GetTickCount64();
    for (Grave& g : g_Grave) {
        if (g.obj && (all || now - g.at >= kRetireMs)) { g.obj->Release(); g.obj = nullptr; }
    }
}

void Bury(bool all) {
    AcquireSRWLockExclusive(&g_GraveLock);
    BuryLocked(all);
    ReleaseSRWLockExclusive(&g_GraveLock);
}

std::atomic<uint32_t> g_GraveEvictions{0};

void Retire(IUnknown* obj) {
    if (!obj) return;
    AcquireSRWLockExclusive(&g_GraveLock);
    // Release happens off the render thread (the watcher buries): a Release that
    // frees a texture is not something to do in the middle of a draw.
    Grave* oldest = nullptr;
    for (Grave& g : g_Grave) {
        if (!g.obj) { g = { obj, GetTickCount64() }; ReleaseSRWLockExclusive(&g_GraveLock); return; }
        if (!oldest || g.at < oldest->at) oldest = &g;
    }
    // Full: the oldest goes now. Leaking instead is what took the whole PC
    // down on 2026-10-15 (see CopySlot); an early Release of something a frame
    // or more old is the far smaller risk.
    oldest->obj->Release();
    *oldest = { obj, GetTickCount64() };
    ReleaseSRWLockExclusive(&g_GraveLock);
    if (g_GraveEvictions.fetch_add(1, std::memory_order_relaxed) == 0)
        Logf("WeaponFX: retire list full - the oldest object released early (counted in the report)");
}

// Does the compiled shader read `name`? Unknown (no reflection) counts as yes:
// a needless copy costs time, a missing one costs a wrong picture.
bool ShaderReads(const void* code, SIZE_T len, const char* name) {
    if (!g_Reflect) return true;
    ID3D11ShaderReflection* r = nullptr;
    if (FAILED(g_Reflect(code, len, kIidShaderReflection, (void**)&r)) || !r) return true;
    D3D11_SHADER_INPUT_BIND_DESC d = {};
    const bool used = SUCCEEDED(r->GetResourceBindingDescByName(name, &d));
    r->Release();
    return used;
}

// Paths come in as UTF-8 (weapon_fx.h) and go to Windows as UTF-16: the
// ANSI calls would read them through the system code page.
bool WidePath(const char* utf8, wchar_t* out, int cap) {
    out[0] = 0;
    if (!utf8 || !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, out, cap)) {
        out[0] = 0;
        return false;
    }
    return true;
}

// Zero on failure; `out` holds a NUL-terminated copy the caller frees.
bool ReadSource(const char* path, char** out, DWORD* len, FILETIME* when) {
    *out = nullptr;
    *len = 0;
    wchar_t wpath[MAX_PATH];
    if (!WidePath(path, wpath, MAX_PATH)) return false;
    HANDLE f = CreateFileW(wpath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size = {};
    bool ok = GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart <= kMaxSourceBytes &&
              GetFileTime(f, nullptr, nullptr, when);
    char* buf = ok ? (char*)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)size.QuadPart + 1) : nullptr;
    DWORD got = 0;
    ok = buf && ReadFile(f, buf, (DWORD)size.QuadPart, &got, nullptr) && got == (DWORD)size.QuadPart;
    CloseHandle(f);
    if (!ok) {
        if (buf) HeapFree(GetProcessHeap(), 0, buf);
        return false;
    }
    buf[got] = 0;
    *out = buf;
    *len = got;
    return true;
}

// #include "x.hlsli" from the shader's own folder, through ReadSource. The
// compiler's standard handler opens files by ANSI path, which loses a folder
// with letters outside the system code page. Nested includes resolve against
// the shader's folder too, not the including file's.
class FolderInclude : public ID3DInclude {
public:
    explicit FolderInclude(const char* shaderPath) {
        strncpy_s(m_Dir, shaderPath, _TRUNCATE);
        char* cut = nullptr;
        for (char* p = m_Dir; *p; ++p)
            if (*p == '\\' || *p == '/') cut = p;
        if (cut) cut[1] = 0;
        else m_Dir[0] = 0;
    }
    HRESULT __stdcall Open(D3D_INCLUDE_TYPE, LPCSTR name, LPCVOID, LPCVOID* data, UINT* bytes) override {
        *data = nullptr;
        *bytes = 0;
        if (!name || !*name) return E_FAIL;
        char path[sizeof(g_Path)];
        const bool absolute = (name[0] && name[1] == ':') || (name[0] == '\\' && name[1] == '\\');
        _snprintf_s(path, sizeof(path), _TRUNCATE, "%s%s", absolute ? "" : m_Dir, name);
        char* src = nullptr;
        DWORD len = 0;
        FILETIME when = {};
        if (!ReadSource(path, &src, &len, &when)) return E_FAIL;
        *data = src;
        *bytes = len;
        return S_OK;
    }
    HRESULT __stdcall Close(LPCVOID data) override {
        if (data) HeapFree(GetProcessHeap(), 0, (void*)data);
        return S_OK;
    }

private:
    char m_Dir[sizeof(g_Path)];
};

// Caller holds g_CompileLock exclusively. A failed compile leaves the current
// shader in place: a typo mid-edit must not blank the weapon.
bool CompileLocked() {
    char* src = nullptr;
    DWORD len = 0;
    FILETIME when = {};
    if (!ReadSource(g_Path, &src, &len, &when)) {
        Logf("WeaponFX: cannot read %s", g_Path);
        return false;
    }
    g_PathTime = when;   // a broken save is not retried until the next save

    ID3DBlob* code = nullptr;
    ID3DBlob* err = nullptr;
    FolderInclude includes(g_Path);
    const HRESULT hr = g_Compile(src, len, g_Path, nullptr, &includes, "main", "ps_5_0",
                                 D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    HeapFree(GetProcessHeap(), 0, src);
    if (FAILED(hr) || !code) {
        g_Failures.fetch_add(1, std::memory_order_relaxed);
        Logf("WeaponFX: %s did not compile, keeping the previous shader:\n%s", g_Path,
             err ? (const char*)err->GetBufferPointer() : "(no message)");
        if (err) err->Release();
        if (code) code->Release();
        return false;
    }
    if (err) err->Release();
    ID3D11PixelShader* ps = nullptr;
    const HRESULT hc = g_Dev->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &ps);
    const bool usesScene = ShaderReads(code->GetBufferPointer(), code->GetBufferSize(), "g_fxScene");
    const bool usesDepth = ShaderReads(code->GetBufferPointer(), code->GetBufferSize(), "g_fxDepth");
    code->Release();
    if (FAILED(hc) || !ps) {
        g_Failures.fetch_add(1, std::memory_order_relaxed);
        Logf("WeaponFX: CreatePixelShader failed (0x%08X)", (unsigned)hc);
        return false;
    }
    // The flag before the shader: a draw that sees the new shader must not see
    // the old "no scene" and skip the copy it reads.
    g_PsUsesScene.store(usesScene, std::memory_order_release);
    g_PsUsesDepth.store(usesDepth, std::memory_order_release);
    Retire(g_Ps.exchange(ps, std::memory_order_acq_rel));
    g_Compiles.fetch_add(1, std::memory_order_relaxed);
    Logf("WeaponFX: %s compiled%s%s", g_Path, usesScene ? " (reads the scene)" : "",
         usesDepth ? " (reads depth)" : "");
    return true;
}

// Zero when the counters cannot be read - the guard then stays quiet rather
// than tripping on garbage.
ULONGLONG PrivateBytes() {
    PROCESS_MEMORY_COUNTERS_EX pm = {};
    pm.cb = sizeof(pm);
    if (!K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pm, sizeof(pm))) return 0;
    return pm.PrivateUsage;
}

ULONGLONG g_MemBase = 0;           // private bytes at the last Load (under g_CompileLock)
std::atomic<bool> g_MemTripped{false};

// Under g_CompileLock. Watches the whole process, not just our objects: a leak
// anywhere while the effect runs is reason enough to stop it, and it is the
// one guard that also covers a churn path nobody has thought of yet.
void MemGuardLocked() {
    if (!g_Path[0] || !g_MemBase) return;
    const ULONGLONG now = PrivateBytes();
    if (!now || now <= g_MemBase + kMaxGrowthBytes) return;
    g_Path[0] = 0;
    Retire(g_Ps.exchange(nullptr, std::memory_order_acq_rel));
    g_CopiesTripped.store(true, std::memory_order_relaxed);
    g_MemTripped.store(true, std::memory_order_relaxed);
    Logf("WeaponFX: OFF by the memory guard - game private memory %.2f GB, %.2f GB when loaded (limit +%.0f GB)",
         now / 1073741824.0, g_MemBase / 1073741824.0, kMaxGrowthBytes / 1073741824.0);
}

DWORD WINAPI WatchThread(void*) {
    while (WaitForSingleObject(g_Stop, kWatchMs) == WAIT_TIMEOUT) {
        AcquireSRWLockExclusive(&g_CompileLock);
        Bury(false);
        MemGuardLocked();
        if (g_Path[0]) {
            WIN32_FILE_ATTRIBUTE_DATA a = {};
            wchar_t w[MAX_PATH];
            if (WidePath(g_Path, w, MAX_PATH) && GetFileAttributesExW(w, GetFileExInfoStandard, &a) &&
                CompareFileTime(&a.ftLastWriteTime, &g_PathTime) != 0)
                CompileLocked();
        }
        ReleaseSRWLockExclusive(&g_CompileLock);
    }
    return 0;
}

// Every texture we make per target goes through here. False once the fuse is
// out: past kMaxCreatesPerSecond the targets change shape faster than our
// caches cover, and creating on regardless is the 2026-10-15 leak.
bool CreationAllowed(const char* what) {
    if (g_CopiesTripped.load(std::memory_order_relaxed)) return false;
    const ULONGLONG now = GetTickCount64();
    if (now - g_CreateWindowAt >= 1000) { g_CreateWindowAt = now; g_CreatesInWindow = 0; }
    if (++g_CreatesInWindow > kMaxCreatesPerSecond) {
        g_CopiesTripped.store(true, std::memory_order_relaxed);
        Logf("WeaponFX: copies OFF - %s textures recreated %u times in a second (targets change shape); "
             "reload the shader to retry", what, g_CreatesInWindow);
        return false;
    }
    return true;
}

// The slot holding copies of a source of this shape; creates one (in a free or
// the least recently used slot) when none does. Null when creation failed or
// the fuse is out.
CopySlot* SlotFor(CopySlot* slots, UINT w, UINT h, DXGI_FORMAT src, DXGI_FORMAT texFmt, DXGI_FORMAT view, UINT bind,
                  const char* what) {
    const uint32_t frame = g_Frame.load(std::memory_order_relaxed);
    for (int k = 0; k < kCopySlots; ++k) {
        CopySlot& sl = slots[k];
        if (sl.tex && sl.w == w && sl.h == h && sl.src == src && sl.view == view) { sl.used = frame; return &sl; }
    }
    if (!CreationAllowed(what)) return nullptr;
    CopySlot* pick = nullptr;
    for (int k = 0; k < kCopySlots; ++k)
        if (!slots[k].tex) { pick = &slots[k]; break; }
    if (!pick) {
        pick = &slots[0];
        for (int k = 1; k < kCopySlots; ++k)
            if (slots[k].used < pick->used) pick = &slots[k];
    }
    // Render thread only, never published: released directly (see CopySlot).
    if (pick->srv) pick->srv->Release();
    if (pick->tex) pick->tex->Release();
    *pick = CopySlot{};
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = texFmt;
    td.SampleDesc = { 1, 0 };
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = bind;
    D3D11_SHADER_RESOURCE_VIEW_DESC srd = {};
    srd.Format = view;
    srd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srd.Texture2D.MipLevels = 1;
    if (FAILED(g_Dev->CreateTexture2D(&td, nullptr, &pick->tex)) || !pick->tex ||
        FAILED(g_Dev->CreateShaderResourceView(pick->tex, &srd, &pick->srv)) || !pick->srv) {
        if (pick->tex) { pick->tex->Release(); pick->tex = nullptr; }
        return nullptr;
    }
    pick->w = w;
    pick->h = h;
    pick->src = src;
    pick->view = view;
    pick->used = frame;
    g_CopyCreates.fetch_add(1, std::memory_order_relaxed);
    Logf("WeaponFX: %s copy %ux%u format %d (view %d) in slot %d", what, w, h, (int)src, (int)view,
         (int)(pick - slots));
    return pick;
}

void ReleaseSlots(CopySlot* slots) {
    for (int k = 0; k < kCopySlots; ++k) {
        if (slots[k].srv) slots[k].srv->Release();
        if (slots[k].tex) slots[k].tex->Release();
        slots[k] = CopySlot{};
    }
}

// Copies the bound render target into a scene slot. False (and the old copy
// kept) when the target is of a shape we do not copy - the effect then reads
// black or stale.
// `from`: a target to copy instead of the bound one (the bisect's, by address).
bool CopyScene(ID3D11DeviceContext* c, CopySlot* slots = nullptr, ID3D11Resource* from = nullptr) {
    if (!slots) slots = g_SceneSlots[g_SceneParity];
    D3D11_RENDER_TARGET_VIEW_DESC vd = {};
    ID3D11Resource* res = nullptr;
    if (from) {
        from->AddRef();
        res = from;
    } else {
        ID3D11RenderTargetView* rtv = nullptr;
        c->OMGetRenderTargets(1, &rtv, nullptr);
        if (!rtv) { g_SceneSkips.fetch_add(1, std::memory_order_relaxed); return false; }
        rtv->GetDesc(&vd);
        rtv->GetResource(&res);
        rtv->Release();
    }
    ID3D11Texture2D* src = nullptr;
    if (res) { res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&src); res->Release(); }
    if (!src) { g_SceneSkips.fetch_add(1, std::memory_order_relaxed); return false; }
    D3D11_TEXTURE2D_DESC sd = {};
    src->GetDesc(&sd);
    if (from) vd.Format = sd.Format;
    // One mip, one slice: what a screen target is. Anything else is a target we
    // do not understand, and copying part of it would be the wrong picture.
    if (sd.MipLevels != 1 || sd.ArraySize != 1 || vd.Format == DXGI_FORMAT_UNKNOWN) {
        src->Release();
        g_SceneSkips.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    CopySlot* sl = SlotFor(slots, sd.Width, sd.Height, sd.Format, sd.Format, vd.Format,
                           D3D11_BIND_SHADER_RESOURCE, "scene");
    if (!sl) {
        src->Release();
        g_SceneSkips.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (sd.SampleDesc.Count > 1) c->ResolveSubresource(sl->tex, 0, src, 0, vd.Format);
    else                         c->CopyResource(sl->tex, src);
    src->Release();
    g_SceneTex = sl->tex;
    g_SceneSrv = sl->srv;
    g_SceneCopies.fetch_add(1, std::memory_order_relaxed);
    if (g_Trace == 2)
        Logf("WeaponFX: trace #%d run %d scene copy of %p %ux%u fmt %d into view %p", g_DbgDrawIdx - 1, g_Run, (void*)src, sd.Width,
             sd.Height, (int)sd.Format, (void*)sl->srv);
    return true;
}

// A depth format's typeless family (what our copy is made as - CopyResource
// needs the same family, and a depth format cannot be a shader resource) and
// the view that reads its depth. UNKNOWN for anything else.
void DepthFormats(DXGI_FORMAT f, DXGI_FORMAT* tex, DXGI_FORMAT* view) {
    switch (f) {
    case DXGI_FORMAT_D24_UNORM_S8_UINT: case DXGI_FORMAT_R24G8_TYPELESS:
        *tex = DXGI_FORMAT_R24G8_TYPELESS; *view = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; return;
    case DXGI_FORMAT_D32_FLOAT: case DXGI_FORMAT_R32_TYPELESS:
        *tex = DXGI_FORMAT_R32_TYPELESS; *view = DXGI_FORMAT_R32_FLOAT; return;
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: case DXGI_FORMAT_R32G8X24_TYPELESS:
        *tex = DXGI_FORMAT_R32G8X24_TYPELESS; *view = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS; return;
    case DXGI_FORMAT_D16_UNORM: case DXGI_FORMAT_R16_TYPELESS:
        *tex = DXGI_FORMAT_R16_TYPELESS; *view = DXGI_FORMAT_R16_UNORM; return;
    default:
        *tex = *view = DXGI_FORMAT_UNKNOWN; return;
    }
}

// Copies the bound depth target into g_DepthTex. Multisampled depth cannot be
// resolved by the API and is skipped - the effect then sees no depth.
bool CopyDepthFrom(ID3D11DepthStencilView* dsv, ID3D11DeviceContext* c, CopySlot* slots = g_DepthSlots) {
    if (!dsv) { g_DepthSkips.fetch_add(1, std::memory_order_relaxed); return false; }
    ID3D11Resource* res = nullptr;
    dsv->GetResource(&res);
    ID3D11Texture2D* src = nullptr;
    if (res) { res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&src); res->Release(); }
    if (!src) { g_DepthSkips.fetch_add(1, std::memory_order_relaxed); return false; }
    D3D11_TEXTURE2D_DESC sd = {};
    src->GetDesc(&sd);
    DXGI_FORMAT texFmt, viewFmt;
    DepthFormats(sd.Format, &texFmt, &viewFmt);
    if (sd.MipLevels != 1 || sd.ArraySize != 1 || sd.SampleDesc.Count != 1 || texFmt == DXGI_FORMAT_UNKNOWN) {
        src->Release();
        g_DepthSkips.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    // A depth-stencil binding as well, though it is never bound as one:
    // copying out of a depth target into a plain texture is not something
    // every driver does.
    CopySlot* sl = SlotFor(slots, sd.Width, sd.Height, sd.Format, texFmt, viewFmt,
                           D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_DEPTH_STENCIL, "depth");
    if (!sl) {
        src->Release();
        g_DepthSkips.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    c->CopyResource(sl->tex, src);
    src->Release();
    g_DepthTex = sl->tex;
    g_DepthSrv = sl->srv;
    g_DepthCopies.fetch_add(1, std::memory_order_relaxed);
    return true;
}

const void* BoundVb0(ID3D11DeviceContext* c) {
    ID3D11Buffer* vb = nullptr;
    UINT stride = 0, offset = 0;
    c->IAGetVertexBuffers(0, 1, &vb, &stride, &offset);
    if (vb) vb->Release();   // only the address is kept
    return vb;
}

const void* BoundDsvResource(ID3D11DeviceContext* c) {
    ID3D11DepthStencilView* dsv = nullptr;
    c->OMGetRenderTargets(0, nullptr, &dsv);
    if (!dsv) return nullptr;
    ID3D11Resource* r = nullptr;
    dsv->GetResource(&r);
    dsv->Release();
    if (r) r->Release();
    return r;
}

const void* BoundRtResource(ID3D11DeviceContext* c) {
    ID3D11RenderTargetView* rtv = nullptr;
    c->OMGetRenderTargets(1, &rtv, nullptr);
    if (!rtv) return nullptr;
    ID3D11Resource* r = nullptr;
    rtv->GetResource(&r);
    rtv->Release();
    if (r) r->Release();
    return r;
}

uint32_t RtWidth(ID3D11DeviceContext* c) {
    ID3D11RenderTargetView* rtv = nullptr;
    c->OMGetRenderTargets(1, &rtv, nullptr);
    if (!rtv) return 0;
    ID3D11Resource* r = nullptr;
    rtv->GetResource(&r);
    rtv->Release();
    ID3D11Texture2D* t = nullptr;
    if (r) { r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t); r->Release(); }
    if (!t) return 0;
    D3D11_TEXTURE2D_DESC d = {};
    t->GetDesc(&d);
    t->Release();
    return d.Width;
}

bool InSet(const void* const* set, int n, const void* p) {
    for (int k = 0; k < n; ++k)
        if (set[k] == p) return true;
    return false;
}

bool CopyDepth(ID3D11DeviceContext* c) {
    ID3D11DepthStencilView* dsv = nullptr;
    c->OMGetRenderTargets(0, nullptr, &dsv);
    const bool ok = CopyDepthFrom(dsv, c);
    if (dsv) dsv->Release();
    return ok;
}

// Both copies, whichever the shader reads, and the bookkeeping after.
std::atomic<uint32_t> g_CopyEvents{0};

void Hold(ID3D11ShaderResourceView*& held, ID3D11ShaderResourceView* v) {
    if (held == v) return;
    if (v) v->AddRef();
    if (held) held->Release();
    held = v;
}

void CopyNow(ID3D11DeviceContext* c, uint32_t frame) {
    g_CopyEvents.fetch_add(1, std::memory_order_relaxed);
    // The scene copy is not taken here: the hands' pieces drawn in this run
    // before the weapon would be missing from it, and the see-through weapon
    // in front of them showed the black behind them (the world is never drawn
    // there - the hands are in the depth prepass). CopyBeforeWeapon takes it.
    // At the run's start, unless the main depth was cleared and nothing wrote
    // it since: then it holds only the far plane, and the copy taken at that
    // clear (the world's depth) stays.
    if (g_DepthClearFrame != frame || g_WritesSinceClear)
        g_DepthReady = g_PsUsesDepth.load(std::memory_order_relaxed) && CopyDepth(c);
    g_SceneFrame = frame;
}

// No per-run list from last frame here: turning right adds two runs before
// the viewmodel and moves the later ones, and in that one frame last frame's
// list missed the weapon - 3 black frames in 5 s of turning, all at the
// switch (ring, 2026-10-16). Everything from the copied run on is the
// viewmodel's.
bool InVmRun(uint32_t frame) {
    return g_TrackFrame == frame && g_CopiedRun >= 0 && g_Run >= g_CopiedRun && g_Run < kMaxRuns &&
           g_SceneFrame == frame;
}

// Blink watch: frames where a screen-size weapon draw under a scene reader
// had no copy of this frame. "outside" - not in a viewmodel run (the
// prediction missed); "stale" - in one, but no copy was taken yet this frame,
// so the texture still held last frame's late copy. One line per second when
// either is non-zero. Render thread.
uint32_t g_BlinkFrame = ~0u, g_BlinkFrames = 0, g_BlinkOutside = 0, g_BlinkStale = 0;
// Frames where the weapon in hands reached the depth prepass and we dropped
// it - the engine side missed taking it out of the depth pass that frame. A miss we do not catch leaves the
// world undrawn under the weapon: a black frame.
uint32_t g_FramePrepassDrops = 0, g_PrepassFrames = 0;
int g_BlinkLastRun = -1, g_BlinkLastPredicted = -1;
ULONGLONG g_BlinkNext = 0;

// covered: the viewmodel was missed, and the draw got the last good copy
// (EnsureCopies) - still counted as "outside", so a miss stays visible in
// the log even when the picture no longer shows it.
void NoteWeaponDraw(ID3D11DeviceContext* c, bool copies, bool covered) {
    if (!g_MainRt || BoundRtResource(c) != g_MainRt) return;
    // Marked draws before the viewmodel are other weapons; they read no copy by design.
    if (g_PredictedRun < 0 || g_Run < g_PredictedRun) return;
    const uint32_t frame = g_Frame.load(std::memory_order_relaxed);
    const bool fresh = g_BlinkFrame != frame;
    if (fresh) { g_BlinkFrame = frame; ++g_BlinkFrames; }
    if (covered && g_CoveredFrame != frame) {
        g_CoveredFrame = frame;
        ++g_CoveredWindow;
        g_CoveredFrames.fetch_add(1, std::memory_order_relaxed);
    }
    static uint32_t s_markedFrame = ~0u;
    if (s_markedFrame != frame) {
        if (!copies || covered) { ++g_BlinkOutside; s_markedFrame = frame; g_BlinkLastRun = g_Run; g_BlinkLastPredicted = g_PredictedRun; }
        else if (g_SceneDoneFrame != frame) { ++g_BlinkStale; s_markedFrame = frame; g_BlinkLastRun = g_Run; g_BlinkLastPredicted = g_PredictedRun; }
    }
    const ULONGLONG now = GetTickCount64();
    if (now < g_BlinkNext) return;
    g_BlinkNext = now + 1000;
    if (g_BlinkOutside || g_BlinkStale || g_PrepassFrames)
        Logf("WeaponFX: blink watch - %u frames, outside the viewmodel run %u (covered by the last good copy %u), stale copy %u, weapon in the prepass %u (last: run %d, predicted %d)",
             g_BlinkFrames, g_BlinkOutside, g_CoveredWindow, g_BlinkStale, g_PrepassFrames, g_BlinkLastRun, g_BlinkLastPredicted);
    g_BlinkFrames = g_BlinkOutside = g_BlinkStale = g_PrepassFrames = g_CoveredWindow = 0;
}


// --- frame ring (debug: FrameRing on|off|dump) -------------------------------
// The last ~5 s of the scene copy the weapon read, 160x90 each, on the GPU:
// the full copy goes into mip 0 of a mipped texture, GenerateMips, and mip 4
// lands in one slice of an array. Made once on "on", freed on "off" and in
// Shutdown - nothing is created per frame. "dump" (done at the next present,
// on the render thread) maps the ring once, writes the frames whose weapon
// area turns black and their neighbours as .ppm, and a .csv of every frame,
// to the folder given by SetDumpDir.
constexpr UINT kRingW = 160, kRingH = 90, kRingSlices = 1536;   // 5 s at 300 fps
constexpr UINT kRingMip = 4;                                     // 2560 / 16 = 160
struct RingMeta { uint32_t frame; int run, predicted, copiedRun; uint32_t copyDraw, ourDraw; bool copies, fresh; };
ID3D11Texture2D* g_RingTex = nullptr;    // kRingSlices x kRingW x kRingH
ID3D11Texture2D* g_RingMipTex = nullptr; // the source size with kRingMip+1 levels
ID3D11ShaderResourceView* g_RingMipSrv = nullptr;
UINT g_RingSrcW = 0, g_RingSrcH = 0;
DXGI_FORMAT g_RingFmt = DXGI_FORMAT_UNKNOWN;
RingMeta g_RingMeta[kRingSlices] = {};
uint32_t g_RingHead = 0, g_RingCount = 0, g_RingLastFrame = ~0u;
std::atomic<int> g_RingState{0};
std::atomic<uint32_t> g_LateStarts{0};
std::atomic<uint32_t> g_VbOnlyRefused{0};   // fallback starts refused: the weapon's vertex buffer, no mark
uint32_t g_VbOnlyFrame = ~0u;               // counted once per frame
ULONGLONG g_VbOnlyLogAt = 0;                // logged once per second
ULONGLONG g_LateLogAt = 0;   // viewmodel starts by the fallback (see BeforeDraw)   // 0 off, 1 on, 2 dump wanted
uint32_t g_LastCopyDraw = 0;       // draw index of this frame's scene copy

void RingRelease() {
    if (g_RingMipSrv) { g_RingMipSrv->Release(); g_RingMipSrv = nullptr; }
    if (g_RingMipTex) { g_RingMipTex->Release(); g_RingMipTex = nullptr; }
    if (g_RingTex) { g_RingTex->Release(); g_RingTex = nullptr; }
    g_RingSrcW = g_RingSrcH = 0;
    g_RingFmt = DXGI_FORMAT_UNKNOWN;
    g_RingHead = g_RingCount = 0;
}

// The mip texture for one source shape; the ring keeps the format of the
// first source. False (and nothing recorded) for anything else.
bool RingPrepare(const D3D11_TEXTURE2D_DESC& sd) {
    if (g_RingMipTex && sd.Width == g_RingSrcW && sd.Height == g_RingSrcH && sd.Format == g_RingFmt) return true;
    if (g_RingTex && sd.Format != g_RingFmt) return false;
    if (sd.Width < (kRingW << kRingMip) || sd.Height < (kRingH << kRingMip)) return false;
    if (g_RingMipSrv) { g_RingMipSrv->Release(); g_RingMipSrv = nullptr; }
    if (g_RingMipTex) { g_RingMipTex->Release(); g_RingMipTex = nullptr; }
    D3D11_TEXTURE2D_DESC md = {};
    md.Width = sd.Width;
    md.Height = sd.Height;
    md.MipLevels = kRingMip + 1;
    md.ArraySize = 1;
    md.Format = sd.Format;
    md.SampleDesc.Count = 1;
    md.Usage = D3D11_USAGE_DEFAULT;
    md.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    md.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    if (FAILED(g_Dev->CreateTexture2D(&md, nullptr, &g_RingMipTex)) || !g_RingMipTex ||
        FAILED(g_Dev->CreateShaderResourceView(g_RingMipTex, nullptr, &g_RingMipSrv)) || !g_RingMipSrv) {
        Logf("WeaponFX: ring - mip texture %ux%u format %d failed", sd.Width, sd.Height, (int)sd.Format);
        RingRelease();
        g_RingState.store(0);
        return false;
    }
    if (!g_RingTex) {
        D3D11_TEXTURE2D_DESC rd = {};
        rd.Width = kRingW;
        rd.Height = kRingH;
        rd.MipLevels = 1;
        rd.ArraySize = kRingSlices;
        rd.Format = sd.Format;
        rd.SampleDesc.Count = 1;
        rd.Usage = D3D11_USAGE_DEFAULT;
        rd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(g_Dev->CreateTexture2D(&rd, nullptr, &g_RingTex)) || !g_RingTex) {
            Logf("WeaponFX: ring - %u slices failed", kRingSlices);
            RingRelease();
            g_RingState.store(0);
            return false;
        }
        Logf("WeaponFX: ring - %u frames of %ux%u, format %d", kRingSlices, kRingW, kRingH, (int)sd.Format);
    }
    g_RingSrcW = sd.Width;
    g_RingSrcH = sd.Height;
    g_RingFmt = sd.Format;
    return true;
}

// Once a frame, at the first screen-size weapon draw: what the copy holds.
void RingRecord(ID3D11DeviceContext* c, ID3D11ShaderResourceView* scene, const RingMeta& meta) {
    if (g_RingState.load(std::memory_order_relaxed) != 1 || g_RingLastFrame == meta.frame) return;
    g_RingLastFrame = meta.frame;
    const uint32_t slot = g_RingHead;
    g_RingMeta[slot] = meta;
    g_RingHead = (g_RingHead + 1) % kRingSlices;
    if (g_RingCount < kRingSlices) ++g_RingCount;
    if (!scene) return;   // no copy bound: the meta says so, the slice keeps old pixels
    ID3D11Resource* r = nullptr;
    scene->GetResource(&r);
    ID3D11Texture2D* t = nullptr;
    if (r) { r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t); r->Release(); }
    if (!t) return;
    D3D11_TEXTURE2D_DESC sd = {};
    t->GetDesc(&sd);
    if (sd.SampleDesc.Count == 1 && sd.MipLevels == 1 && RingPrepare(sd)) {
        c->CopySubresourceRegion(g_RingMipTex, 0, 0, 0, 0, t, 0, nullptr);
        c->GenerateMips(g_RingMipSrv);
        c->CopySubresourceRegion(g_RingTex, D3D11CalcSubresource(0, slot, 1), 0, 0, 0, g_RingMipTex, kRingMip, nullptr);
    }
    t->Release();
}

// R11G11B10_FLOAT channel: no sign, 5-bit exponent, `mant`-bit mantissa.
float UnpackSmallFloat(uint32_t v, int mant) {
    const uint32_t e = (v >> mant) & 31, m = v & ((1u << mant) - 1);
    if (e == 0) return (float)m / (float)(1u << mant) / 16384.0f;
    if (e == 31) return 65504.0f;
    return ldexpf(1.0f + (float)m / (float)(1u << mant), (int)e - 15);
}

uint8_t ToByte(float x) {
    x = x / (1.0f + x);   // a plain tone curve: the copy is HDR
    x = x < 0 ? 0 : (x > 1 ? 1 : x);
    return (uint8_t)(powf(x, 1.0f / 2.2f) * 255.0f + 0.5f);
}

char g_DumpDir[MAX_PATH] = {};   // control thread writes it before a dump is asked for

void RingDump(ID3D11DeviceContext* c) {
    if (!g_RingTex || !g_RingCount) { Logf("WeaponFX: ring - nothing recorded"); return; }
    if (g_RingFmt != DXGI_FORMAT_R11G11B10_FLOAT && g_RingFmt != DXGI_FORMAT_R8G8B8A8_UNORM) {
        Logf("WeaponFX: ring - format %d not decoded", (int)g_RingFmt);
        return;
    }
    D3D11_TEXTURE2D_DESC sd = {};
    g_RingTex->GetDesc(&sd);
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* st = nullptr;
    if (FAILED(g_Dev->CreateTexture2D(&sd, nullptr, &st)) || !st) { Logf("WeaponFX: ring - staging failed"); return; }
    c->CopyResource(st, g_RingTex);
    // The host's folder: a manual-mapped module has no path of its own to go by.
    const char* dir = g_DumpDir;
    wchar_t wdir[MAX_PATH];
    if (WidePath(dir, wdir, MAX_PATH)) CreateDirectoryW(wdir, nullptr);
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\frames.csv", dir);
    FILE* csv = nullptr;
    wchar_t wcsv[MAX_PATH];
    if (!WidePath(path, wcsv, MAX_PATH) || _wfopen_s(&csv, wcsv, L"w") != 0) csv = nullptr;
    if (csv) fprintf(csv, "order,frame,run,predicted,copiedRun,copyDraw,ourDraw,copies,fresh,blackPct\n");
    static float s_black[kRingSlices];
    static uint8_t s_rgb[kRingW * kRingH * 3];
    const uint32_t first = (g_RingHead + kRingSlices - g_RingCount) % kRingSlices;
    // Pass 1: how black the weapon's part of the picture (right half, lower
    // two thirds) is in each frame.
    for (uint32_t i = 0; i < g_RingCount; ++i) {
        const uint32_t slot = (first + i) % kRingSlices;
        D3D11_MAPPED_SUBRESOURCE m = {};
        s_black[i] = -1.0f;
        if (FAILED(c->Map(st, D3D11CalcSubresource(0, slot, 1), D3D11_MAP_READ, 0, &m)) || !m.pData) continue;
        uint32_t black = 0, n = 0;
        for (UINT y = kRingH / 3; y < kRingH; ++y) {
            const uint32_t* row = (const uint32_t*)((const uint8_t*)m.pData + (size_t)y * m.RowPitch);
            for (UINT x = kRingW / 2; x < kRingW; ++x, ++n) {
                const uint32_t v = g_RingFmt == DXGI_FORMAT_R11G11B10_FLOAT ? row[x] : (row[x] & 0x00FFFFFFu);
                if (v == 0) ++black;
            }
        }
        c->Unmap(st, D3D11CalcSubresource(0, slot, 1));
        s_black[i] = n ? 100.0f * (float)black / (float)n : 0.0f;
        const RingMeta& mt = g_RingMeta[slot];
        if (csv)
            fprintf(csv, "%u,%u,%d,%d,%d,%u,%u,%d,%d,%.1f\n", i, mt.frame, mt.run, mt.predicted, mt.copiedRun, mt.copyDraw,
                    mt.ourDraw, (int)mt.copies, (int)mt.fresh, s_black[i]);
    }
    if (csv) fclose(csv);
    // Pass 2: frames clearly blacker than both neighbours, two frames around
    // each; at most 40 files.
    int written = 0, spikes = 0;
    for (uint32_t i = 0; i < g_RingCount && written < 40; ++i) {
        const float b = s_black[i];
        const float prev = i ? s_black[i - 1] : b, next = i + 1 < g_RingCount ? s_black[i + 1] : b;
        if (b < 0 || b < prev + 10.0f || b < next + 10.0f) continue;
        ++spikes;
        for (uint32_t j = i >= 2 ? i - 2 : 0; j <= i + 2 && j < g_RingCount && written < 40; ++j) {
            const uint32_t slot = (first + j) % kRingSlices;
            D3D11_MAPPED_SUBRESOURCE m = {};
            if (FAILED(c->Map(st, D3D11CalcSubresource(0, slot, 1), D3D11_MAP_READ, 0, &m)) || !m.pData) continue;
            for (UINT y = 0; y < kRingH; ++y) {
                const uint32_t* row = (const uint32_t*)((const uint8_t*)m.pData + (size_t)y * m.RowPitch);
                for (UINT x = 0; x < kRingW; ++x) {
                    const uint32_t v = row[x];
                    uint8_t* o = s_rgb + (y * kRingW + x) * 3;
                    if (g_RingFmt == DXGI_FORMAT_R11G11B10_FLOAT) {
                        o[0] = ToByte(UnpackSmallFloat(v & 0x7FF, 6));
                        o[1] = ToByte(UnpackSmallFloat((v >> 11) & 0x7FF, 6));
                        o[2] = ToByte(UnpackSmallFloat((v >> 22) & 0x3FF, 5));
                    } else {
                        o[0] = (uint8_t)v;
                        o[1] = (uint8_t)(v >> 8);
                        o[2] = (uint8_t)(v >> 16);
                    }
                }
            }
            c->Unmap(st, D3D11CalcSubresource(0, slot, 1));
            snprintf(path, sizeof(path), "%s\\f%04u_%s_b%.0f.ppm", dir, j, j == i ? "SPIKE" : "near", s_black[j]);
            FILE* f = nullptr;
            wchar_t wppm[MAX_PATH];
            if (WidePath(path, wppm, MAX_PATH) && _wfopen_s(&f, wppm, L"wb") == 0 && f) {
                fprintf(f, "P6\n%u %u\n255\n", kRingW, kRingH);
                fwrite(s_rgb, 1, sizeof(s_rgb), f);
                fclose(f);
                ++written;
            }
        }
    }
    st->Release();
    Logf("WeaponFX: ring dumped - %u frames, %d black spikes, %d images, %s", g_RingCount, spikes, written, dir);
}

// A later viewmodel run's own copy, before its first draw from the marked
// buffers - the engine's draw of the same mesh included: with the weapon kept
// (overlay mode) the engine draws its skin right before our pass, and a
// copy taken between them showed the skin through the see-through window.
void CopyLateRun(ID3D11DeviceContext* c, uint32_t frame) {
    if (g_CopiedRun < 0 || g_Run <= g_CopiedRun || g_Run >= kMaxRuns || !InVmRun(frame)) return;
    if ((g_LateRunsCopied >> g_Run & 1) || !g_PsUsesScene.load(std::memory_order_relaxed)) return;
    if (BoundRtResource(c) != g_MainRt) return;
    g_LateRunsCopied |= 1ull << g_Run;
    if (CopyScene(c)) { Hold(g_VmSceneSrv, g_SceneSrv); g_VmSceneFrame = frame; }
}

// Before a draw from the weapon's buffers (ours or the engine's): the frame's
// scene copy, if this is the first such draw on the main colour target. The
// engine's own passes of the knife can come in a run before the one with our
// marked draws (the inspect-like hold, 2026-10-15); a copy at our run's start
// then held the engine's black knife and the see-through showed it.
void CopyBeforeWeapon(ID3D11DeviceContext* c, uint32_t frame) {
    if (g_SceneDoneFrame == frame || !g_PsUsesScene.load(std::memory_order_relaxed)) return;
    if (!g_MainRt || BoundRtResource(c) != g_MainRt) return;
    if (g_PredictedRun >= 0 && g_Run > g_PredictedRun && !InVmRun(frame)) return;
    // Not before the viewmodel run: the in-hand weapon's vertex buffer is a
    // pool shared with models drawn with the world (with the bots kicked, a
    // third-person model in the world's run fired the copy at #150, before
    // the world under the weapon was drawn - the window showed black).
    if (g_PredictedRun < 0 || g_Run < g_PredictedRun) return;
    // And not before an EQUAL-tested draw. The run guess can still land a
    // step early on a sharp camera turn (runs appear and go), and the same
    // weapon mesh is drawn with the world too - tested EQUAL against the
    // prepass (2026-10-16: a black frame every few seconds while turning).
    // The weapon in hands is drawn without a test (our pass, the engine's
    // marker passes) or LESS_EQUAL (its skin with the weapon kept, which the
    // copy must come before).
    {
        ID3D11DepthStencilState* ds = nullptr;
        UINT ref = 0;
        c->OMGetDepthStencilState(&ds, &ref);
        D3D11_DEPTH_STENCIL_DESC dd = {};
        if (ds) { ds->GetDesc(&dd); ds->Release(); }
        if (!ds || (dd.DepthEnable && dd.DepthFunc == D3D11_COMPARISON_EQUAL)) return;
    }
    if (!CopyScene(c)) return;
    g_LastCopyDraw = (uint32_t)(g_DbgDrawIdx - 1);
    g_SceneDoneFrame = frame;
    Hold(g_VmSceneSrv, g_SceneSrv);
    g_VmSceneFrame = frame;
}

// At a weapon draw: the viewmodel run's copies, or false - no copy at all.
// Marked draws outside the viewmodel (a knife on the ground, the holstered
// pistol on our body, the half- and quarter-size passes) used to copy their
// own target. That is five or six target shapes a frame against three slots:
// the slots evicted each other every frame, the fuse went out within a second
// and the effect blinked (2026-10-15). Those draws read no scene now - a
// see-through effect there shows black, which nobody looks at.
bool EnsureCopies(ID3D11DeviceContext* c) {
    g_EnsureStale = false;
    const uint32_t frame = g_Frame.load(std::memory_order_relaxed);
    if (!g_MainRt || BoundRtResource(c) != g_MainRt) return false;
    if (!InVmRun(frame)) {
        // The weapon in hands (past the predicted run) with no viewmodel found
        // this frame: the last good copy, if it is recent (kStaleCopyFrames).
        // It sits intact in the other parity's slots or, when older, in slots
        // nothing copied into this frame - the main-shape copy is only ever
        // taken by CopyBeforeWeapon/CopyLateRun, which would have refreshed it.
        if (g_PredictedRun < 0 || g_Run < g_PredictedRun || !g_VmSceneSrv) return false;
        if (frame - g_VmSceneFrame > kStaleCopyFrames) return false;
        g_EnsureStale = true;
    }
    g_SceneSrv = g_VmSceneSrv;
    g_DepthSrv = g_VmDepthSrv;
    g_DepthReady = g_VmDepthCopied;
    return true;
}

// Closes the previous frame's run table. Viewmodel runs: weapon draws without a
// depth test (our no-Z marker; other overlay passes test EQUAL). The one
// with the most of them is the weapon in hands, and it and every no-Z run
// after it are the viewmodel: the knife is drawn in one run and the hands in
// a later one (2026-10-15 - "no-Z and hand depth writes in one run" picked a
// one-draw run after the knife, and the knife read no scene: black). What did
// not hold up before: "most weapon draws" counting the EQUAL passes too, and
// "last run with weapon draws" (a knife on the ground is marked as well).
void RollFrame(uint32_t frame) {
    if (g_FramePrepassDrops) ++g_PrepassFrames;
    g_FramePrepassDrops = 0;
    int best = -1, lastWeapon = -1;
    uint64_t runs = 0;
    for (int r = 0; r <= g_Run && r < kMaxRuns; ++r) {
        if (!g_RunWeapon[r]) continue;
        lastWeapon = r;
        if (!g_RunNoZ[r]) continue;
        // The widest target first (the screen, not a half-size pass), then
        // the most no-Z draws.
        if (best < 0 || g_RunRtW[r] > g_RunRtW[best] ||
            (g_RunRtW[r] == g_RunRtW[best] && g_RunNoZ[r] > g_RunNoZ[best]))
            best = r;
    }
    if (best >= 0)
        for (int r = best; r <= g_Run && r < kMaxRuns; ++r)
            if (g_RunNoZ[r]) runs |= 1ull << r;
    if (best < 0 && lastWeapon >= 0) { best = lastWeapon; runs = 1ull << best; }
    if (g_Trace == 2)
        for (int r = 0; r <= g_Run && r < kMaxRuns; ++r)
            if (g_RunWeapon[r])
                Logf("WeaponFX: trace run %d: weapon draws %u, no-Z %u, depth writes %u, width %u%s", r, g_RunWeapon[r], g_RunNoZ[r],
                     g_RunWrites[r], g_RunRtW[r], (runs >> r & 1) ? (r == best ? " <- viewmodel, copied" : " <- viewmodel") : "");
    if (best >= 0) g_VmRuns = runs >> best;
    if (best >= 0) g_PredictedRun = best;
    if (g_TruthRun >= 0) {
        if (g_ShadowRun == g_TruthRun) { ++g_ShadowAgree; g_ShadowAgreeAll.fetch_add(1, std::memory_order_relaxed); }
        else if (g_ShadowRun < 0)      { ++g_ShadowNone;  g_ShadowNoneAll.fetch_add(1, std::memory_order_relaxed); }
        else {
            ++g_ShadowWrong;
            g_ShadowWrongAll.fetch_add(1, std::memory_order_relaxed);
            const ULONGLONG now = GetTickCount64();
            if (now >= g_ShadowWrongLogAt) {
                g_ShadowWrongLogAt = now + 250;
                Logf("WeaponFX: event start WRONG - event run %d at #%d, first marked draw run %d at #%d (of %d), stencil clear %s",
                     g_ShadowRun, g_ShadowDraw, g_TruthRun, g_TruthDraw, g_DbgDrawIdx,
                     g_StencilClearFrame == g_TrackFrame ? "seen" : "not seen");
            }
        }
    }
    {
        const ULONGLONG now = GetTickCount64();
        if (now >= g_ShadowLogAt && (g_ShadowAgree || g_ShadowWrong || g_ShadowNone)) {
            g_ShadowLogAt = now + 1000;
            if (g_ShadowWrong || g_ShadowNone)
                Logf("WeaponFX: event start - agree %u, wrong %u, none %u", g_ShadowAgree, g_ShadowWrong, g_ShadowNone);
            g_ShadowAgree = g_ShadowWrong = g_ShadowNone = 0;
        }
    }
    g_ShadowRun = g_ShadowDraw = g_TruthRun = g_TruthDraw = -1;
    g_DbgDrawsLastFrame = g_DbgDrawIdx;
    g_DbgDrawIdx = 0;
    if (!g_MainRt && g_CandRt) {
        g_MainRt = g_CandRt;
        g_MainDsv = g_CandDsv;
        Logf("WeaponFX: screen target learnt - %p, %u wide, depth %p", g_MainRt, g_CandW, g_MainDsv);
    }
    g_CandRt = g_CandDsv = nullptr;
    g_CandW = 0;
    g_MainDraws = 0;
    if (g_CopiedRun >= 0) g_FramesNoStart = 0;
    else if (++g_FramesNoStart > kRelearnFrames && (g_MainRt || g_MainDsv)) {
        g_MainRt = nullptr;
        g_MainDsv = nullptr;
        g_FramesNoStart = 0;
        g_Relearns.fetch_add(1, std::memory_order_relaxed);
        Logf("WeaponFX: no viewmodel for %u frames - the screen target is learnt again", kRelearnFrames);
    }
    g_LateRunsCopied = 0;
    // The frame that ends here is "last frame" from now on: its copy stays in
    // its slots, and this frame copies into the other set.
    Hold(g_PrevSceneSrv, g_VmSceneFrame == g_TrackFrame ? g_VmSceneSrv : nullptr);
    g_SceneParity ^= 1;
    g_Trace = g_Trace == 1 ? 2 : 0;
    if (g_TraceWanted.exchange(false)) g_Trace = 1;
    memset(g_RunWeapon, 0, sizeof(g_RunWeapon));
    memset(g_RunWrites, 0, sizeof(g_RunWrites));
    memset(g_RunNoZ, 0, sizeof(g_RunNoZ));
    memset(g_RunRtW, 0, sizeof(g_RunRtW));
    // The set collected last frame becomes the one this frame's prepass uses.
    // An empty one (no viewmodel run copied) keeps the old: a frame without
    // the copy must not let the weapon back into the prepass for a frame.
    if (g_VmVbCount[g_VmVbCur]) {
        g_VmVbCur ^= 1;
        g_VmVbCount[g_VmVbCur] = 0;
        g_HandVbCount[g_VmVbCur] = 0;
    }
    g_Run = -1;
    g_RunB1 = nullptr;
    g_CopiedRun = -1;
    g_TrackFrame = frame;
}

// Decodes an image file to RGBA8 with WIC. Zero on failure; the caller frees
// `*pixels` with HeapFree.
bool DecodeImage(const char* path, uint8_t** pixels, UINT* w, UINT* h) {
    *pixels = nullptr;
    *w = *h = 0;
    wchar_t wpath[MAX_PATH];
    if (!WidePath(path, wpath, MAX_PATH)) return false;
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* f = nullptr;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* fr = nullptr;
    IWICFormatConverter* cv = nullptr;
    bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                         __uuidof(IWICImagingFactory), (void**)&f)) && f &&
              SUCCEEDED(f->CreateDecoderFromFilename(wpath, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                                     &dec)) &&
              SUCCEEDED(dec->GetFrame(0, &fr)) && SUCCEEDED(fr->GetSize(w, h)) && *w && *h &&
              *w <= kMaxTexSide && *h <= kMaxTexSide && SUCCEEDED(f->CreateFormatConverter(&cv)) &&
              SUCCEEDED(cv->Initialize(fr, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                       WICBitmapPaletteTypeCustom));
    if (ok) {
        const UINT stride = *w * 4;
        *pixels = (uint8_t*)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)stride * *h);
        ok = *pixels && SUCCEEDED(cv->CopyPixels(nullptr, stride, stride * *h, *pixels));
    }
    if (cv) cv->Release();
    if (fr) fr->Release();
    if (dec) dec->Release();
    if (f) f->Release();
    if (SUCCEEDED(co)) CoUninitialize();
    if (!ok) {
        if (*pixels) HeapFree(GetProcessHeap(), 0, *pixels);
        *pixels = nullptr;
        *w = *h = 0;
    }
    return ok;
}

// Whether a colour target is bound. The weapon is also drawn into the shadow
// maps (depth only, with the marker material); those draws are left alone:
// our pixel shader would change nothing there, and counted as the weapon they
// won the "most weapon draws" vote - cascades draw it many times - so the
// copy went to a shadow pass and the viewmodel fell back to copying at its own
// draw, the pistol already in the picture.
bool HasColorTarget(ID3D11DeviceContext* c) {
    ID3D11RenderTargetView* rtv = nullptr;
    c->OMGetRenderTargets(1, &rtv, nullptr);
    if (!rtv) return false;
    rtv->Release();
    return true;
}

void ReleaseVmDepth() {
    if (g_VmDsv) { g_VmDsv->Release(); g_VmDsv = nullptr; }
    if (g_VmDepthTex) { g_VmDepthTex->Release(); g_VmDepthTex = nullptr; }
    g_VmDepthDesc = {};
    g_VmDsvFormat = DXGI_FORMAT_UNKNOWN;
    g_VmDepthReady = false;
}

// Once per frame, at the viewmodel run's first draw on the main depth: our
// depth buffer of the same shape, with the main one's stencil (the hands test
// the stencil the prepass marked) and depth cleared to the far plane. False
// leaves the frame to the engine's own depth.
bool PrepareVmDepth(ID3D11DeviceContext* c, ID3D11DepthStencilView* mainDsv, uint32_t frame) {
    if (g_VmDepthFrame == frame && g_VmDepthRun == g_Run) return g_VmDepthReady;
    g_VmDepthFrame = frame;
    g_VmDepthRun = g_Run;
    g_VmDepthReady = false;
    ID3D11Resource* r = nullptr;
    mainDsv->GetResource(&r);
    ID3D11Texture2D* src = nullptr;
    if (r) { r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&src); r->Release(); }
    if (!src) return false;
    D3D11_TEXTURE2D_DESC sd = {};
    src->GetDesc(&sd);
    D3D11_DEPTH_STENCIL_VIEW_DESC vd = {};
    mainDsv->GetDesc(&vd);
    if (!g_VmDsv || g_VmDepthDesc.Width != sd.Width || g_VmDepthDesc.Height != sd.Height ||
        g_VmDepthDesc.Format != sd.Format || g_VmDepthDesc.SampleDesc.Count != sd.SampleDesc.Count ||
        g_VmDsvFormat != vd.Format) {
        ReleaseVmDepth();
        if (sd.ArraySize != 1 || sd.MipLevels != 1 || !CreationAllowed("viewmodel depth")) { src->Release(); return false; }
        D3D11_TEXTURE2D_DESC td = sd;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_DEPTH_STENCIL;   // CopyResource needs the format, not the bindings
        td.CPUAccessFlags = 0;
        td.MiscFlags = 0;
        vd.Flags = 0;
        if (FAILED(g_Dev->CreateTexture2D(&td, nullptr, &g_VmDepthTex)) || !g_VmDepthTex ||
            FAILED(g_Dev->CreateDepthStencilView(g_VmDepthTex, &vd, &g_VmDsv)) || !g_VmDsv) {
            ReleaseVmDepth();
            src->Release();
            g_VmDepthFails.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        g_VmDepthDesc = sd;
        g_VmDsvFormat = vd.Format;
        g_CopyCreates.fetch_add(1, std::memory_order_relaxed);
        Logf("WeaponFX: viewmodel depth %ux%u format %d (view %d)", sd.Width, sd.Height, (int)sd.Format, (int)vd.Format);
    }
    c->CopyResource(g_VmDepthTex, src);
    src->Release();
    c->ClearDepthStencilView(g_VmDsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
    g_VmDepthReady = true;
    return true;
}

// The draw belongs to the viewmodel run copied this frame and targets the
// main depth. *mainDsv gets a reference when true.
bool InViewmodelRun(ID3D11DeviceContext* c, ID3D11DepthStencilView** mainDsv) {
    *mainDsv = nullptr;
    if (!g_VmDepthOn.load(std::memory_order_relaxed) || !g_DsPre || !g_DsFx) return false;
    const uint32_t frame = g_Frame.load(std::memory_order_relaxed);
    if (c != g_TrackCtx || !InVmRun(frame) || !g_MainDsv) return false;
    ID3D11DepthStencilView* dsv = nullptr;
    c->OMGetRenderTargets(0, nullptr, &dsv);
    if (!dsv) return false;
    ID3D11Resource* r = nullptr;
    dsv->GetResource(&r);
    if (r) r->Release();
    if (r != g_MainDsv || !PrepareVmDepth(c, dsv, frame)) { dsv->Release(); return false; }
    *mainDsv = dsv;
    return true;
}

std::atomic<uint32_t> g_MissDepthDraws{0};

// A weapon draw outside the viewmodel run this frame (the start was not found)
// with an opaque effect: the marker material has no depth test, so without
// this its far side lands on top of its near side - the weapon "turns inside
// out" for that frame (2026-10-16, plasma and others, now and then). It gets
// our viewmodel depth for that frame instead: the main depth's stencil, depth
// cleared, a depth-only pass, then our colour pass against it. The hands are
// not in it, so for that one frame the weapon is drawn over the fingers - far
// less visible than inside out. False: nothing done, the caller draws as before.
bool DrawMissedWeapon(ID3D11DeviceContext* c, IssueFn issue, void* ctx, bool* on) {
    *on = false;
    if (!g_VmDepthOn.load(std::memory_order_relaxed) || !g_DsPre || !g_DsFx || g_PsUsesScene.load(std::memory_order_relaxed) ||
        !g_Ps.load(std::memory_order_acquire))
        return false;
    ID3D11RenderTargetView* rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, &dsv);
    auto release = [&]() {
        for (ID3D11RenderTargetView* v : rtv)
            if (v) v->Release();
        if (dsv) dsv->Release();
    };
    // The screen and its depth only. Anything else that reaches here (a
    // smaller target, a shadow map) would make PrepareVmDepth rebuild the
    // buffer in its shape, and the next viewmodel run rebuild it back: a
    // muzzle flash taken for the weapon did exactly that at 640x360 against
    // 2560x1440, tripped the creation fuse, and left the weapon without depth
    // until the next shader load (2026-10-16).
    ID3D11Resource* rt = nullptr;
    ID3D11Resource* ds0 = nullptr;
    if (rtv[0]) rtv[0]->GetResource(&rt);
    if (dsv) dsv->GetResource(&ds0);
    if (rt) rt->Release();     // compared only
    if (ds0) ds0->Release();
    if (!rt || !ds0 || !g_MainRt || rt != g_MainRt || ds0 != g_MainDsv ||
        !PrepareVmDepth(c, dsv, g_Frame.load(std::memory_order_relaxed))) {
        release();
        return false;
    }
    ID3D11DepthStencilState* ds = nullptr;
    UINT ref = 0;
    c->OMGetDepthStencilState(&ds, &ref);
    c->OMSetRenderTargets(0, nullptr, g_VmDsv);
    c->OMSetDepthStencilState(g_DsPre, 0);
    issue(ctx);
    c->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, g_VmDsv);
    Bound fx;
    *on = Begin(c, fx);
    c->OMSetDepthStencilState(g_DsFx, 0);
    issue(ctx);
    End(c, fx);
    c->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, dsv);
    c->OMSetDepthStencilState(ds, ref);
    if (ds) ds->Release();
    release();
    g_MissDepthDraws.fetch_add(1, std::memory_order_relaxed);
    return true;
}

} // namespace

bool Init(ID3D11Device* dev, LogFn log) {
    if (g_Dev) return true;
    if (!dev) return false;
    g_Log = log;
    // d3dcompiler_47 ships with Windows 10+; loaded rather than linked so the
    // module carries no import a machine might lack.
    HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    g_Compile = dc ? (pD3DCompile)GetProcAddress(dc, "D3DCompile") : nullptr;
    g_Reflect = dc ? (D3DReflectFn)GetProcAddress(dc, "D3DReflect") : nullptr;
    if (!g_Compile) { Logf("WeaponFX: d3dcompiler_47.dll / D3DCompile not available"); return false; }

    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(FxConstants);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(dev->CreateBuffer(&bd, nullptr, &g_Cb)) || !g_Cb) { Logf("WeaponFX: constant buffer failed"); return false; }
    D3D11_BLEND_DESC bl = {};
    D3D11_RENDER_TARGET_BLEND_DESC& rt = bl.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D11_BLEND_ONE;
    rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ONE;
    rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(dev->CreateBlendState(&bl, &g_Blend)) || !g_Blend) {
        Logf("WeaponFX: blend state failed");
        g_Cb->Release(); g_Cb = nullptr;
        return false;
    }

    D3D11_DEPTH_STENCIL_DESC dsd = {};
    dsd.DepthEnable = TRUE;
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dsd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    // No depth states = no viewmodel depth; the effect itself still works.
    if (FAILED(dev->CreateDepthStencilState(&dsd, &g_DsPre))) g_DsPre = nullptr;
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    if (FAILED(dev->CreateDepthStencilState(&dsd, &g_DsFx))) g_DsFx = nullptr;
    if (!g_DsPre || !g_DsFx) Logf("WeaponFX: depth states failed - viewmodel depth off");

    g_Stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_Watcher = g_Stop ? CreateThread(nullptr, 0, WatchThread, nullptr, 0, nullptr) : nullptr;
    if (!g_Watcher) {
        Logf("WeaponFX: watcher thread failed");
        if (g_Stop) { CloseHandle(g_Stop); g_Stop = nullptr; }
        g_Cb->Release(); g_Cb = nullptr;
        g_Blend->Release(); g_Blend = nullptr;
        if (g_DsPre) { g_DsPre->Release(); g_DsPre = nullptr; }
        if (g_DsFx) { g_DsFx->Release(); g_DsFx = nullptr; }
        return false;
    }
    QueryPerformanceFrequency(&g_Freq);
    QueryPerformanceCounter(&g_T0);
    dev->AddRef();
    g_Dev = dev;
    return true;
}

void Shutdown() {
    if (!g_Dev) return;
    SetEvent(g_Stop);
    WaitForSingleObject(g_Watcher, INFINITE);
    CloseHandle(g_Watcher); g_Watcher = nullptr;
    CloseHandle(g_Stop); g_Stop = nullptr;
    if (ID3D11PixelShader* ps = g_Ps.exchange(nullptr)) ps->Release();
    for (auto& t : g_Tex)
        if (ID3D11ShaderResourceView* v = t.exchange(nullptr)) v->Release();
    Hold(g_VmSceneSrv, nullptr);
    Hold(g_VmDepthSrv, nullptr);
    Hold(g_PrevSceneSrv, nullptr);
    g_VmSceneFrame = ~0u;
    Hold(g_DbgDepthSrv, nullptr);
    g_VmDepthCopied = false;
    ReleaseSlots(g_SceneSlots[0]);
    ReleaseSlots(g_SceneSlots[1]);
    ReleaseSlots(g_DepthSlots);
    ReleaseSlots(g_DbgDepthSlots);
    ReleaseSlots(g_DbgSceneSlots);
    Hold(g_DbgSceneSrv, nullptr);
    ReleaseVmDepth();
    RingRelease();
    g_RingState.store(0);
    g_VmDepthFrame = 0xFFFFFFFFu;
    if (g_DsPre) { g_DsPre->Release(); g_DsPre = nullptr; }
    if (g_DsFx) { g_DsFx->Release(); g_DsFx = nullptr; }
    g_SceneTex = nullptr;
    g_SceneSrv = nullptr;
    g_DepthTex = nullptr;
    g_DepthSrv = nullptr;
    g_DepthReady = false;
    g_SceneFrame = 0xFFFFFFFFu;
    g_TrackCtx = nullptr;
    g_TrackFrame = 0xFFFFFFFFu;
    g_PredictedRun = -1;
    g_VmVbCount[0] = g_VmVbCount[1] = 0;
    g_HandVbCount[0] = g_HandVbCount[1] = 0;
    g_MainDsv = nullptr;
    g_MainRt = nullptr;
    Bury(true);
    if (g_Cb) { g_Cb->Release(); g_Cb = nullptr; }
    if (g_Blend) { g_Blend->Release(); g_Blend = nullptr; }
    g_Dev->Release();
    g_Dev = nullptr;
    g_Path[0] = 0;
}

bool Load(const char* path) {
    if (!g_Dev) return false;
    AcquireSRWLockExclusive(&g_CompileLock);
    bool ok = true;
    if (!path || !*path) {
        g_Path[0] = 0;
        Retire(g_Ps.exchange(nullptr, std::memory_order_acq_rel));
        Logf("WeaponFX: off");
    } else if (strlen(path) >= sizeof(g_Path)) {
        Logf("WeaponFX: path too long");
        ok = false;
    } else {
        strcpy_s(g_Path, path);
        g_CopiesTripped.store(false, std::memory_order_relaxed);
        g_MemTripped.store(false, std::memory_order_relaxed);
        g_MemBase = PrivateBytes();
        ok = CompileLocked();
        if (ok) { QueryPerformanceCounter(&g_T0); g_Draws = 0; }
    }
    ReleaseSRWLockExclusive(&g_CompileLock);
    return ok;
}

bool Active() { return g_Ps.load(std::memory_order_acquire) != nullptr; }

void NewFrame() {
    g_Frame.fetch_add(1, std::memory_order_relaxed);
    // The present is on the render thread, the one that owns the context.
    const int ring = g_RingState.load(std::memory_order_relaxed);
    if (ring == 2 && g_TrackCtx) {
        RingDump(g_TrackCtx);
        g_RingState.store(1);
    } else if (ring == 0 && g_RingTex) {
        RingRelease();
        Logf("WeaponFX: ring freed");
    }
}

void FrameRing(int op) {
    if (op == 0) { g_RingState.store(0); Logf("WeaponFX: ring off"); return; }
    if (op == 1) { g_RingState.store(1); Logf("WeaponFX: ring recording"); return; }
    if (g_RingState.load() == 0) { Logf("WeaponFX: ring is off - nothing to dump"); return; }
    if (!g_DumpDir[0]) { Logf("WeaponFX: ring - no dump folder set (SetDumpDir)"); return; }
    g_RingState.store(2);
}

void SetDumpDir(const char* dir) {
    strncpy_s(g_DumpDir, dir ? dir : "", _TRUNCATE);
}
// The bisect copy: the main depth as it stands now (before the draw or clear
// numbered g_DbgDepthAt). One log line per second says what sat there.
void DbgCopyAt(ID3D11DeviceContext* c, ID3D11DepthStencilView* clearing) {
    ID3D11DepthStencilView* dsv = clearing;
    if (!dsv) c->OMGetRenderTargets(0, nullptr, &dsv);
    else dsv->AddRef();
    if (!dsv) return;
    ID3D11Resource* r = nullptr;
    dsv->GetResource(&r);
    if (r) r->Release();
    if (CopyDepthFrom(dsv, c, g_DbgDepthSlots)) { Hold(g_DbgDepthSrv, g_DepthSrv); g_DbgDepthDsv = r; }
    if (g_MainRt && CopyScene(c, g_DbgSceneSlots, (ID3D11Resource*)g_MainRt)) Hold(g_DbgSceneSrv, g_SceneSrv);
    dsv->Release();
    static ULONGLONG s_next = 0;
    const ULONGLONG now = GetTickCount64();
    if (now >= s_next) {
        s_next = now + 1000;
        ID3D11Buffer* vb = nullptr;
        UINT st = 0, of = 0;
        c->IAGetVertexBuffers(0, 1, &vb, &st, &of);
        if (vb) vb->Release();
        Logf("WeaponFX: debug depth copy at #%d: %s dsv %p vb %p run %d", g_DbgDrawIdx - 1,
             clearing ? "before a clear" : "before a draw", r, (void*)vb, g_Run);
    }
}

// The viewmodel run starts at the current draw: its copies and targets.
void StartViewmodel(ID3D11DeviceContext* c, uint32_t frame) {
    g_MainDrawsAtStart = g_MainDraws;
    g_PredictedRun = g_Run;
    CopyNow(c, frame);
    g_CopiedRun = g_Run;
    g_MainDsv = BoundDsvResource(c);
    g_MainRt = BoundRtResource(c);
    Hold(g_VmDepthSrv, g_DepthReady ? g_DepthSrv : nullptr);
    g_VmDepthCopied = g_DepthReady;
}

// Debug (DebugMissEvery(n)): every n-th frame the viewmodel is not found -
// what a detection that broke after a game update would do - to see what the
// fallback (EnsureCopies) makes of it. 0 = off.
std::atomic<int> g_DbgMissEvery{0};
bool DbgMiss(uint32_t frame) {
    const int n = g_DbgMissEvery.load(std::memory_order_relaxed);
    return n > 0 && frame % (uint32_t)n == 0;
}

bool DepthTested(ID3D11DeviceContext* c) {
    ID3D11DepthStencilState* ds = nullptr;
    UINT ref = 0;
    c->OMGetDepthStencilState(&ds, &ref);
    D3D11_DEPTH_STENCIL_DESC dd = {};
    if (ds) { ds->GetDesc(&dd); ds->Release(); }
    return ds && dd.DepthEnable;
}

bool BeforeDraw(ID3D11DeviceContext* c, bool weapon) {
    if (!g_Ps.load(std::memory_order_relaxed)) return false;
    if (!g_PsUsesScene.load(std::memory_order_relaxed) && !g_PsUsesDepth.load(std::memory_order_relaxed) &&
        !g_VmDepthOn.load(std::memory_order_relaxed))
        return false;
    if (weapon && !HasColorTarget(c)) weapon = false;
    if (weapon) g_TrackCtx = c;
    // Deferred contexts record out of order with the immediate one; their
    // draws would only scramble the run count.
    if (c != g_TrackCtx) return false;
    const uint32_t frame = g_Frame.load(std::memory_order_relaxed);
    if (g_TrackFrame != frame) RollFrame(frame);
    if (g_DbgDepthAt.load(std::memory_order_relaxed) == g_DbgDrawIdx++) DbgCopyAt(c, nullptr);
    ID3D11Buffer* b1 = nullptr;
    c->VSGetConstantBuffers(1, 1, &b1);
    if (b1) b1->Release();   // only the address is kept, for comparison
    if (g_Run < 0 || b1 != g_RunB1) {
        g_RunB1 = b1;
        if (g_Run < kMaxRuns - 1) ++g_Run;
    }
    if (g_MainRt && BoundRtResource(c) == g_MainRt) {
        if (g_ShadowRun < 0 && g_StencilClearFrame == frame && g_MainDsv && BoundDsvResource(c) == g_MainDsv) {
            ID3D11DepthStencilState* ds = nullptr;
            UINT ref = 0;
            c->OMGetDepthStencilState(&ds, &ref);
            D3D11_DEPTH_STENCIL_DESC dd = {};
            if (ds) { ds->GetDesc(&dd); ds->Release(); }
            if (ds && dd.DepthEnable && dd.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ALL) {
                if (g_MainDraws * 2 < g_MainDrawsAtStart) {
                    // Too early: the world is still to come. The next stencil
                    // clear (or the marked draw, below) starts it instead.
                    g_StencilClearFrame = ~0u;
                    g_EventsEarly.fetch_add(1, std::memory_order_relaxed);
                    const ULONGLONG now = GetTickCount64();
                    if (now >= g_EarlyLogAt) {
                        g_EarlyLogAt = now + 1000;
                        Logf("WeaponFX: stencil event refused at #%d - %u screen draws so far, %u last frame",
                             g_DbgDrawIdx - 1, g_MainDraws, g_MainDrawsAtStart);
                    }
                } else {
                    g_ShadowRun = g_Run;
                    g_ShadowDraw = g_DbgDrawIdx - 1;
                    if (g_CopiedRun < 0 && !DbgMiss(frame)) StartViewmodel(c, frame);
                }
            }
        }
        if (g_CopiedRun < 0) ++g_MainDraws;
        if (weapon && g_TruthRun < 0 && !DepthTested(c)) {
            g_TruthRun = g_Run;
            g_TruthDraw = g_DbgDrawIdx - 1;
        }
    }
    if (weapon && !DepthTested(c)) {
        const uint32_t w = RtWidth(c);
        if (w > g_CandW) {
            g_CandW = w;
            g_CandRt = BoundRtResource(c);
            g_CandDsv = BoundDsvResource(c);
        }
    }
    // The fallback: no start this frame (no stencil event, or the targets
    // not learnt yet), yet here is the weapon in hands - drawn without a depth
    // test on the screen target, ours or the engine's pass of it. Without it
    // such a frame has no copy at all: the black blink while turning
    // (2026-10-16). It starts late: the hand pieces drawn before the weapon
    // are not in our viewmodel depth that frame.
    //
    // A marked draw only. The weapon's vertex buffer is a pool the world is
    // drawn from too (the set holds one buffer), and a match on it alone
    // started the viewmodel at a world draw on a weapon switch: everything
    // after it went to our viewmodel depth and the whole screen blinked
    // (2026-10-16). Such matches are counted and logged, never started on.
    // The cost: with the weapon kept (replace off) a fallback frame copies
    // after the engine's skin pass, and that one frame shows the skin.
    if (g_CopiedRun < 0 && !DbgMiss(frame) && g_MainRt && BoundRtResource(c) == g_MainRt && !DepthTested(c)) {
        const int set = g_VmVbCur ^ 1;
        if (weapon) {
            const ULONGLONG now = GetTickCount64();
            if (now >= g_LateLogAt) {
                g_LateLogAt = now + 1000;
                Logf("WeaponFX: late viewmodel start at a marked draw #%d of ~%d, run %d (stencil event this frame: %s)",
                     g_DbgDrawIdx - 1, g_DbgDrawsLastFrame, g_Run, g_StencilClearFrame == frame ? "seen" : "none");
            }
            StartViewmodel(c, frame);
            g_LateStarts.fetch_add(1, std::memory_order_relaxed);
        } else if (g_VmVbCount[set] && InSet(g_VmVb[set], g_VmVbCount[set], BoundVb0(c)) &&
                   g_VbOnlyFrame != frame) {
            g_VbOnlyFrame = frame;
            g_VbOnlyRefused.fetch_add(1, std::memory_order_relaxed);
            const ULONGLONG now = GetTickCount64();
            if (now >= g_VbOnlyLogAt) {
                g_VbOnlyLogAt = now + 1000;
                Logf("WeaponFX: refused a viewmodel start on the weapon's vertex buffer alone at #%d of ~%d, run %d",
                     g_DbgDrawIdx - 1, g_DbgDrawsLastFrame, g_Run);
            }
        }
    }
    if (weapon) {
        // Marked draws before the viewmodel are other weapons (enemies',
        // ours on the body) drawn with the world - too early to copy.
        if (g_Run >= g_PredictedRun) CopyBeforeWeapon(c, frame);
        CopyLateRun(c, frame);
        if (g_RunWeapon[g_Run] < 0xFFFF) ++g_RunWeapon[g_Run];
        {
            ID3D11DepthStencilState* ds = nullptr;
            UINT ref = 0;
            c->OMGetDepthStencilState(&ds, &ref);
            D3D11_DEPTH_STENCIL_DESC dd = {};
            if (ds) { ds->GetDesc(&dd); ds->Release(); }
            if (ds && !dd.DepthEnable && g_RunNoZ[g_Run] < 0xFFFF) ++g_RunNoZ[g_Run];
            if (!g_RunRtW[g_Run]) g_RunRtW[g_Run] = RtWidth(c);
        }
        if (g_CopiedRun >= 0 && g_Run >= g_CopiedRun && (g_VmRuns >> (g_Run - g_CopiedRun) & 1)) {
            const void* vb = BoundVb0(c);
            int& n = g_VmVbCount[g_VmVbCur];
            if (vb && n < kMaxVmVbs && !InSet(g_VmVb[g_VmVbCur], n, vb)) g_VmVb[g_VmVbCur][n++] = vb;
            int& h = g_HandVbCount[g_VmVbCur];
            if (vb && g_Run == g_CopiedRun && h < kMaxVmVbs && !InSet(g_HandVb[g_VmVbCur], h, vb)) g_HandVb[g_VmVbCur][h++] = vb;
        }
        return false;
    }
    ++g_OtherDraws;
    {
        const int set = g_VmVbCur ^ 1;
        if (g_VmVbCount[set] && InSet(g_VmVb[set], g_VmVbCount[set], BoundVb0(c))) {
            if (InSet(g_HandVb[set], g_HandVbCount[set], BoundVb0(c))) CopyBeforeWeapon(c, frame);
            CopyLateRun(c, frame);
            if (g_MarkerPs && InVmRun(frame)) {
                ID3D11PixelShader* ps = nullptr;
                c->PSGetShader(&ps, nullptr, nullptr);
                if (ps) ps->Release();
                if (ps == g_MarkerPs) { g_MarkerPassSkips.fetch_add(1, std::memory_order_relaxed); return true; }
            }
        }
    }
    {
        ID3D11DepthStencilState* ds = nullptr;
        UINT ref = 0;
        c->OMGetDepthStencilState(&ds, &ref);
        if (ds) {
            D3D11_DEPTH_STENCIL_DESC dd = {};
            ds->GetDesc(&dd);
            ds->Release();
            if (dd.DepthEnable && dd.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ALL) {
                if (g_RunWrites[g_Run] < 0xFFFF && HasColorTarget(c)) ++g_RunWrites[g_Run];
                if (g_MainDsv && BoundDsvResource(c) == g_MainDsv) ++g_WritesSinceClear;
            }
        }
    }
    // The prepass draw of the weapon in hands: its vertex buffer, no colour
    // target, the main depth. Shadow maps are other depth targets and keep it.
    const int used = g_VmVbCur ^ 1;
    if (!g_VmVbCount[used] || !g_MainDsv) return false;
    if (!InSet(g_VmVb[used], g_VmVbCount[used], BoundVb0(c))) return false;
    if (HasColorTarget(c) || BoundDsvResource(c) != g_MainDsv) return false;
    g_PrepassSkips.fetch_add(1, std::memory_order_relaxed);
    ++g_FramePrepassDrops;
    return true;
}

void BeforeClearDepth(ID3D11DeviceContext* c, ID3D11DepthStencilView* dsv, unsigned flags) {
    // Clears count in the bisect, as they do in a dump of the frame's draws.
    if (dsv && c == g_TrackCtx && g_DbgDepthAt.load(std::memory_order_relaxed) == g_DbgDrawIdx++ &&
        (flags & D3D11_CLEAR_DEPTH))
        DbgCopyAt(c, dsv);
    if (!(flags & D3D11_CLEAR_DEPTH)) {
        // Stencil only: the shadow start's event.
        if (dsv && c == g_TrackCtx && g_MainDsv && g_Ps.load(std::memory_order_relaxed)) {
            ID3D11Resource* r = nullptr;
            dsv->GetResource(&r);
            if (r) r->Release();
            const uint32_t frame = g_Frame.load(std::memory_order_relaxed);
            if (g_TrackFrame != frame) RollFrame(frame);
            if (r == g_MainDsv) g_StencilClearFrame = frame;
        }
        return;
    }
    if (!dsv || !g_Ps.load(std::memory_order_relaxed) || !g_PsUsesDepth.load(std::memory_order_relaxed)) return;
    if (c != g_TrackCtx || !g_MainDsv) return;
    const uint32_t frame = g_Frame.load(std::memory_order_relaxed);
    if (g_TrackFrame != frame) RollFrame(frame);
    // Only before the viewmodel run: each such clear overwrites the copy, so
    // the last one before the run - the one after the world - is what stays.
    if (g_PredictedRun < 0 || g_Run >= g_PredictedRun) return;
    ID3D11Resource* r = nullptr;
    dsv->GetResource(&r);
    if (r) r->Release();
    if (r != g_MainDsv) return;
    g_WritesSinceClear = 0;
    if (CopyDepthFrom(dsv, c)) {
        g_DepthReady = true;
        g_DepthClearFrame = frame;
    }
}

bool LoadTexture(int index, const char* path) {
    if (!g_Dev || index < 0 || index >= kTextures) return false;
    if (!path || !*path) {
        Retire(g_Tex[index].exchange(nullptr, std::memory_order_acq_rel));
        Logf("WeaponFX: texture %d off", index);
        return true;
    }
    uint8_t* px = nullptr;
    UINT w = 0, h = 0;
    if (!DecodeImage(path, &px, &w, &h)) { Logf("WeaponFX: texture %d - cannot decode %s", index, path); return false; }
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    // Linear, not sRGB: these are masks and data first. A colour picture read
    // through one comes out a little light, which is the cheaper mistake.
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc = { 1, 0 };
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init = { px, w * 4, 0 };
    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    const bool ok = SUCCEEDED(g_Dev->CreateTexture2D(&td, &init, &tex)) && tex &&
                    SUCCEEDED(g_Dev->CreateShaderResourceView(tex, nullptr, &srv)) && srv;
    HeapFree(GetProcessHeap(), 0, px);
    if (tex) tex->Release();   // the view holds it
    if (!ok) { Logf("WeaponFX: texture %d - upload failed (%ux%u)", index, w, h); return false; }
    Retire(g_Tex[index].exchange(srv, std::memory_order_acq_rel));
    Logf("WeaponFX: texture %d = %s (%ux%u)", index, path, w, h);
    return true;
}

void SetParam(int index, float x, float y, float z, float w) {
    if (index < 0 || index >= kParams) return;
    g_Param[index][0].store(x, std::memory_order_relaxed);
    g_Param[index][1].store(y, std::memory_order_relaxed);
    g_Param[index][2].store(z, std::memory_order_relaxed);
    g_Param[index][3].store(w, std::memory_order_relaxed);
}

bool Begin(ID3D11DeviceContext* c, Bound& saved) {
    saved = {};
    ID3D11PixelShader* ours = g_Ps.load(std::memory_order_acquire);
    if (!ours || !g_Cb || !HasColorTarget(c)) return false;

    FxConstants k = {};
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const int64_t prev = g_LastTick.exchange(now.QuadPart, std::memory_order_relaxed);
    const double freq = g_Freq.QuadPart ? (double)g_Freq.QuadPart : 1.0;
    k.time[0] = (float)((double)(now.QuadPart - g_T0.QuadPart) / freq);
    k.time[1] = prev ? (float)((double)(now.QuadPart - prev) / freq) : 0.0f;
    k.time[2] = (float)g_Draws.fetch_add(1, std::memory_order_relaxed);
    D3D11_VIEWPORT vp = {};
    UINT nvp = 1;
    c->RSGetViewports(&nvp, &vp);
    if (nvp && vp.Width > 0.0f && vp.Height > 0.0f) {
        k.screen[0] = vp.Width;
        k.screen[1] = vp.Height;
        k.screen[2] = 1.0f / vp.Width;
        k.screen[3] = 1.0f / vp.Height;
    }
    for (int i = 0; i < kParams; ++i)
        for (int j = 0; j < 4; ++j) k.param[i][j] = g_Param[i][j].load(std::memory_order_relaxed);

    D3D11_MAPPED_SUBRESOURCE m = {};
    if (FAILED(c->Map(g_Cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)) || !m.pData) return false;
    memcpy(m.pData, &k, sizeof(k));
    c->Unmap(g_Cb, 0);

    // Before our shader is bound: the copy has to hold what was under the
    // weapon, not the weapon.
    ID3D11ShaderResourceView* views[kViews] = {};
    const bool scene = g_PsUsesScene.load(std::memory_order_acquire);
    const bool depth = g_PsUsesDepth.load(std::memory_order_acquire);
    const bool copies = (scene || depth) && EnsureCopies(c);
    if (scene && copies) views[0] = g_SceneSrv;
    if (scene && copies) views[2 + kTextures] = g_PrevSceneSrv;
    if (scene) NoteWeaponDraw(c, copies, copies && g_EnsureStale);
    if (scene && g_RingState.load(std::memory_order_relaxed) == 1 && g_MainRt && BoundRtResource(c) == g_MainRt &&
        g_PredictedRun >= 0 && g_Run >= g_PredictedRun) {
        const uint32_t frame = g_Frame.load(std::memory_order_relaxed);
        const RingMeta mt = { frame, g_Run, g_PredictedRun, g_CopiedRun, g_LastCopyDraw, (uint32_t)(g_DbgDrawIdx - 1),
                              copies, g_SceneDoneFrame == frame };
        RingRecord(c, copies ? g_SceneSrv : nullptr, mt);
    }
    if (g_Trace == 2)
        Logf("WeaponFX: trace #%d run %d our draw, copies %d, rt %p, scene view %p", g_DbgDrawIdx - 1, g_Run, (int)copies,
             BoundRtResource(c), (void*)(copies ? g_SceneSrv : nullptr));
    for (int i = 0; i < kTextures; ++i) views[1 + i] = g_Tex[i].load(std::memory_order_acquire);
    if (depth && copies && g_DepthReady) views[1 + kTextures] = g_DepthSrv;
    if (depth && g_DbgDepthAt.load(std::memory_order_relaxed) >= 0 && g_DbgDepthSrv) views[1 + kTextures] = g_DbgDepthSrv;
    if (scene && g_DbgDepthAt.load(std::memory_order_relaxed) >= 0 && g_DbgSceneSrv) views[0] = g_DbgSceneSrv;

    c->PSGetShader(&saved.ps, nullptr, nullptr);
    g_MarkerPs = saved.ps;
    c->PSGetConstantBuffers(kCbSlot, 1, &saved.cb);
    c->PSGetShaderResources(kSceneSlot, kViews, saved.srv);
    c->OMGetBlendState(&saved.blend, saved.blendFactor, &saved.sampleMask);
    c->PSSetShader(ours, nullptr, 0);
    c->PSSetConstantBuffers(kCbSlot, 1, &g_Cb);
    c->PSSetShaderResources(kSceneSlot, kViews, views);
    c->OMSetBlendState(g_Blend, nullptr, 0xFFFFFFFFu);
    saved.on = true;
    return true;
}

void End(ID3D11DeviceContext* c, Bound& saved) {
    if (!saved.on) return;
    c->PSSetShader(saved.ps, nullptr, 0);
    c->PSSetConstantBuffers(kCbSlot, 1, &saved.cb);
    c->PSSetShaderResources(kSceneSlot, kViews, saved.srv);
    c->OMSetBlendState(saved.blend, saved.blendFactor, saved.sampleMask);
    if (saved.ps) saved.ps->Release();
    if (saved.cb) saved.cb->Release();
    if (saved.blend) saved.blend->Release();
    for (ID3D11ShaderResourceView*& v : saved.srv)
        if (v) v->Release();
    saved = {};
}

bool Draw(ID3D11DeviceContext* c, bool weapon, IssueFn issue, void* ctx) {
    Bound fx;
    ID3D11DepthStencilView* mainDsv = nullptr;
    if (!InViewmodelRun(c, &mainDsv)) {
        bool drawn = false;
        if (weapon && DrawMissedWeapon(c, issue, ctx, &drawn)) return drawn;
        const bool on = weapon && Begin(c, fx);
        issue(ctx);
        End(c, fx);
        return on;
    }
    ID3D11RenderTargetView* rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* none = nullptr;
    c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, &none);
    if (none) none->Release();   // mainDsv already holds it
    ID3D11DepthStencilState* ds = nullptr;
    UINT ref = 0;
    c->OMGetDepthStencilState(&ds, &ref);
    D3D11_DEPTH_STENCIL_DESC dd = {};
    if (ds) ds->GetDesc(&dd);
    // A draw that tests and writes depth itself (the hands) lays its own depth
    // down; the rest - the weapon's passes, ours, anything without a write -
    // get a depth-only pass first, so what is drawn later still finds them.
    const bool writes = ds && dd.DepthEnable && dd.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ALL;
    // Not the engine's own passes of the weapon (its buffers, unmarked): they
    // write no depth in the game either, and their depth lands a hair in front
    // of ours - our draw then failed LESS_EQUAL on its own surface and the
    // marker's black showed through in the P2000's grip (2026-10-15).
    const int used = g_VmVbCur ^ 1;
    const bool weaponPass = !weapon && InSet(g_VmVb[used], g_VmVbCount[used], BoundVb0(c));
    // Not for the weapon under an effect that reads the scene. With the
    // weapon's own depth in our buffer the see-through came out black under
    // the weapon (2026-10-15: the separate pass and a depth write in our
    // colour pass both did it, the engine's passes under a depth test did not
    // help; the cause was not pinned down). A see-through weapon shows the
    // scene on its far side anyway; the fingers still hide it - they are
    // drawn first and write depth. Opaque effects keep the pass: without it
    // their far side shows through the near one.
    const bool selfDepth = weapon && !g_PsUsesScene.load(std::memory_order_relaxed);
    if (selfDepth || (!weapon && !writes && !weaponPass)) {
        c->OMSetRenderTargets(0, nullptr, g_VmDsv);
        c->OMSetDepthStencilState(g_DsPre, 0);
        issue(ctx);
        g_VmDepthPre.fetch_add(1, std::memory_order_relaxed);
    }
    c->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, g_VmDsv);
    const bool on = weapon && Begin(c, fx);
    // Ours tests against the depth pass it just laid down (equal passes), or
    // only against the hands for a scene reader; theirs keep their own state,
    // stencil included.
    // The engine's own passes of the weapon under a scene reader write no
    // depth either. With the weapon kept (overlay mode) its skin pass
    // writes it, the hand drawn after it then failed behind the weapon, and
    // the see-through window showed the black the copy holds there (no world
    // behind a hand, no hand yet).
    const bool quiet = weaponPass && writes && g_PsUsesScene.load(std::memory_order_relaxed);
    c->OMSetDepthStencilState(on || quiet ? g_DsFx : ds, on || quiet ? 0 : ref);
    issue(ctx);
    End(c, fx);
    c->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, mainDsv);
    c->OMSetDepthStencilState(ds, ref);
    for (ID3D11RenderTargetView* v : rtv)
        if (v) v->Release();
    if (ds) ds->Release();
    mainDsv->Release();
    g_VmDepthDraws.fetch_add(1, std::memory_order_relaxed);
    return on;
}

void SetViewmodelDepth(bool on) {
    g_VmDepthOn.store(on, std::memory_order_relaxed);
    Logf("WeaponFX: viewmodel depth %s", on ? "on" : "off");
}

uint32_t Copies() { return g_CopyEvents.load(std::memory_order_relaxed); }

void DebugDepthAt(int drawIndex) {
    if (drawIndex == -2) { g_TraceWanted.store(true); Logf("WeaponFX: tracing one frame"); return; }
    g_DbgDepthAt.store(drawIndex, std::memory_order_relaxed);
    Logf("WeaponFX: debug depth at draw %d of %d last frame (dsv %p)", drawIndex, g_DbgDrawsLastFrame, g_DbgDepthDsv);
}
int CurrentRun() { return g_Run; }

void DebugMissEvery(int n) {
    g_DbgMissEvery.store(n > 0 ? n : 0, std::memory_order_relaxed);
    Logf("WeaponFX: viewmodel missed on purpose every %d frames%s", n, n > 0 ? "" : " (off)");
}

// Three lines: the host's log cuts a line at 512 characters, and one line
// lost its tail - the detection counters - exactly when they were needed.
void Report() {
    int texs = 0;
    for (const auto& t : g_Tex) texs += t.load() ? 1 : 0;
    Logf("WeaponFX: %s | shader %s%s%s | compiles %u, failures %u | draws %u | textures %d | "
         "scene copies %u, skipped %u | depth copies %u, skipped %u | copy textures made %u%s, retire evictions %u",
         g_Path[0] ? g_Path : "(no file)", Active() ? "bound" : "none",
         g_PsUsesScene.load() ? " (reads the scene)" : "", g_PsUsesDepth.load() ? " (reads depth)" : "",
         g_Compiles.load(), g_Failures.load(), g_Draws.load(), texs, g_SceneCopies.load(), g_SceneSkips.load(),
         g_DepthCopies.load(), g_DepthSkips.load(), g_CopyCreates.load(),
         g_MemTripped.load() ? " (MEMORY GUARD OFF)" : g_CopiesTripped.load() ? " (FUSE OUT)" : "", g_GraveEvictions.load());
    Logf("WeaponFX: viewmodel start - by the stencil event: agree %u, wrong %u, none %u, refused early %u | "
         "late (at a marked draw) %u, refused on the buffer alone %u | frames covered by the last good copy %u | "
         "screen target relearnt %u | viewmodel run %d",
         g_ShadowAgreeAll.load(), g_ShadowWrongAll.load(), g_ShadowNoneAll.load(), g_EventsEarly.load(),
         g_LateStarts.load(), g_VbOnlyRefused.load(), g_CoveredFrames.load(), g_Relearns.load(), g_PredictedRun);
    Logf("WeaponFX: prepass draws dropped %u (weapon buffers %d), marker passes dropped %u | "
         "viewmodel depth %s: draws %u, depth passes %u, failures %u, weapon draws outside the run %u",
         g_PrepassSkips.load(), g_VmVbCount[g_VmVbCur ^ 1], g_MarkerPassSkips.load(),
         g_VmDepthOn.load() ? "on" : "off", g_VmDepthDraws.load(), g_VmDepthPre.load(), g_VmDepthFails.load(),
         g_MissDepthDraws.load());
}

} // namespace WeaponFx
