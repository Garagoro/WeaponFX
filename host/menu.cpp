#include "menu.h"
#include "config.h"
#include "d3d_hooks.h"
#include "engine.h"
#include "log.h"
#include "patch.h"

#include "../third_party/imgui/imgui.h"
#include "../third_party/imgui/imgui_impl_dx11.h"
#include "../third_party/imgui/imgui_impl_win32.h"

#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <atomic>
#include <cstdio>
#include <cstring>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace Wfx {
namespace Menu {
namespace {

constexpr int kToggleKey = VK_INSERT;

// --- the game's input ---------------------------------------------------------
// Virtual slots (2026-10 builds). A wrong slot is caught by the executable-
// pointer check below and the menu still opens, only without a free cursor.
constexpr int kMouseInputEnabledSlot = 23;   // CCSGOInput: false = the camera ignores the mouse
constexpr int kEnableCursorSlot = 76;        // CInputSystem: relative mode on/off
constexpr char kInputSystemName[] = "InputSystemVersion001";
// client.dll: `mov [rip+disp32], rax; xorps xmm0,xmm0; movups [rip+disp32], xmm0`
// - the store right after CCSGOInput is constructed; the target is the object.
constexpr short kSigCSGOInput[] = { 0x48, 0x89, 0x05, -1, -1, -1, -1, 0x0F, 0x57, 0xC0, 0x0F, 0x11, 0x05 };

using MouseInputEnabledFn = bool(__fastcall*)(void*);
using EnableCursorFn = void*(__fastcall*)(void*, bool);
using CreateInterfaceFn = void*(__cdecl*)(const char*, int*);

void** g_SlotInput = nullptr;
void** g_SlotCursor = nullptr;
MouseInputEnabledFn g_OrigMouseInput = nullptr;
EnableCursorFn g_OrigCursor = nullptr;
void* g_InputSystem = nullptr;
std::atomic<bool> g_LastGameCursor{true};
std::atomic<bool> g_Internal{false};
bool g_SavedGameCursor = true;
bool g_SavedOsCursor = true;

std::atomic<bool> g_Open{false};
std::atomic<int> g_InWndProc{0};
HWND g_Hwnd = nullptr;
WNDPROC g_OrigWndProc = nullptr;
bool g_ImGuiReady = false;
bool g_InitFailed = false;
SRWLOCK g_ImGuiLock = SRWLOCK_INIT;   // ImGui's event queue: window thread vs render thread
std::atomic<DWORD> g_ImGuiOwner{0};

// Re-entrant on one thread. The Win32 backend calls SetCapture/ReleaseCapture
// on a mouse button, and Windows delivers WM_CAPTURECHANGED to this same
// window procedure synchronously, inside that call: a plain SRW lock taken
// again there waits for itself, and the game's window thread hangs for good
// (it did, on the release after dragging a slider).
struct ImGuiLock {
    bool held = false;
    ImGuiLock() {
        const DWORD me = GetCurrentThreadId();
        if (g_ImGuiOwner.load(std::memory_order_acquire) == me) return;
        AcquireSRWLockExclusive(&g_ImGuiLock);
        g_ImGuiOwner.store(me, std::memory_order_release);
        held = true;
    }
    ~ImGuiLock() {
        if (!held) return;
        g_ImGuiOwner.store(0, std::memory_order_release);
        ReleaseSRWLockExclusive(&g_ImGuiLock);
    }
    ImGuiLock(const ImGuiLock&) = delete;
    ImGuiLock& operator=(const ImGuiLock&) = delete;
};

bool IsExecutable(void* p) {
    MEMORY_BASIC_INFORMATION m = {};
    if (!p || !VirtualQuery(p, &m, sizeof(m))) return false;
    const DWORD x = m.Protect & 0xFF;
    return x == PAGE_EXECUTE || x == PAGE_EXECUTE_READ || x == PAGE_EXECUTE_READWRITE || x == PAGE_EXECUTE_WRITECOPY;
}

bool SetSlot(void** slot, void* value, void** old) {
    DWORD prot = 0;
    if (!slot || !VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &prot)) return false;
    if (old) *old = *slot;
    *slot = value;
    VirtualProtect(slot, sizeof(void*), prot, &prot);
    return true;
}

bool __fastcall HkMouseInputEnabled(void* self) {
    if (g_Open.load(std::memory_order_relaxed)) return false;
    return g_OrigMouseInput ? g_OrigMouseInput(self) : true;
}

void* __fastcall HkEnableCursor(void* self, bool relative) {
    if (!g_Internal.load()) g_LastGameCursor.store(relative);
    if (g_Open.load()) relative = false;
    return g_OrigCursor ? g_OrigCursor(self, relative) : nullptr;
}

void CallGameCursor(bool relative) {
    if (!g_OrigCursor || !g_InputSystem) return;
    g_Internal.store(true);
    __try {
        g_OrigCursor(g_InputSystem, relative);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    g_Internal.store(false);
}

void InstallInput() {
    int hits = 0;
    const uintptr_t at = FindUnique("client.dll", kSigCSGOInput, sizeof(kSigCSGOInput) / sizeof(kSigCSGOInput[0]), &hits);
    int32_t disp = 0;
    void* csgoInput = nullptr;
    if (at && ReadMem(at + 3, &disp, sizeof(disp))) csgoInput = (void*)(at + 7 + (intptr_t)disp);
    HMODULE is = GetModuleHandleW(L"inputsystem.dll");
    const CreateInterfaceFn create = is ? (CreateInterfaceFn)GetProcAddress(is, "CreateInterface") : nullptr;
    __try {
        g_InputSystem = create ? create(kInputSystemName, nullptr) : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_InputSystem = nullptr;
    }
    void** vtInput = nullptr;
    void** vtSystem = nullptr;
    if (!csgoInput || !g_InputSystem || !ReadMem((uintptr_t)csgoInput, &vtInput, sizeof(vtInput)) ||
        !ReadMem((uintptr_t)g_InputSystem, &vtSystem, sizeof(vtSystem)) || !vtInput || !vtSystem) {
        Log("menu: game input not found (CCSGOInput signature %d hits, %s %p) - the menu opens without a free cursor", hits,
            kInputSystemName, g_InputSystem);
        return;
    }
    void* a = nullptr;
    void* b = nullptr;
    if (!ReadMem((uintptr_t)&vtInput[kMouseInputEnabledSlot], &a, sizeof(a)) ||
        !ReadMem((uintptr_t)&vtSystem[kEnableCursorSlot], &b, sizeof(b)) || !IsExecutable(a) || !IsExecutable(b)) {
        Log("menu: input slots %d/%d are not code - a game update moved them; no free cursor", kMouseInputEnabledSlot,
            kEnableCursorSlot);
        return;
    }
    void* oa = nullptr;
    void* ob = nullptr;
    if (!SetSlot(&vtInput[kMouseInputEnabledSlot], (void*)&HkMouseInputEnabled, &oa)) return;
    if (!SetSlot(&vtSystem[kEnableCursorSlot], (void*)&HkEnableCursor, &ob)) {
        SetSlot(&vtInput[kMouseInputEnabledSlot], oa, nullptr);
        return;
    }
    g_OrigMouseInput = (MouseInputEnabledFn)oa;
    g_OrigCursor = (EnableCursorFn)ob;
    g_SlotInput = &vtInput[kMouseInputEnabledSlot];
    g_SlotCursor = &vtSystem[kEnableCursorSlot];
    Log("menu: game input taken over (CCSGOInput %p, input system %p)", csgoInput, g_InputSystem);
}

void ShowOsCursor(bool show) {
    if (show)
        for (int i = 0; i < 64 && ShowCursor(TRUE) < 0; ++i) {}
    else
        for (int i = 0; i < 64 && ShowCursor(FALSE) >= 0; ++i) {}
}

void SetOpen(bool open) {
    if (g_Open.exchange(open) == open) return;
    if (open) {
        g_SavedGameCursor = g_LastGameCursor.load();
        CURSORINFO ci = { sizeof(ci) };
        g_SavedOsCursor = GetCursorInfo(&ci) ? (ci.flags & CURSOR_SHOWING) != 0 : g_SavedGameCursor;
        ShowOsCursor(true);
        SetCursor(LoadCursorA(nullptr, IDC_ARROW));
        ClipCursor(nullptr);
        CallGameCursor(false);
    } else {
        ClipCursor(nullptr);
        ShowOsCursor(g_SavedOsCursor);
        CallGameCursor(g_SavedGameCursor);
    }
}

bool IsMouseMessage(UINT msg) {
    return (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) || msg == WM_INPUT;
}

LRESULT CALLBACK HkWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    struct Guard {
        Guard() { g_InWndProc.fetch_add(1); }
        ~Guard() { g_InWndProc.fetch_sub(1); }
    } guard;
    const WNDPROC orig = g_OrigWndProc;
    if (msg == WM_KEYDOWN && wp == kToggleKey && !(lp & (1LL << 30))) {   // not auto-repeat
        SetOpen(!g_Open.load());
        return 0;
    }
    if (g_Open.load()) {
        if (msg == WM_KEYDOWN && wp == VK_ESCAPE) {
            SetOpen(false);
            return 0;
        }
        bool handled = false;
        {
            ImGuiLock lock;
            handled = g_ImGuiReady && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp) != 0;
        }
        if (msg == WM_INPUT) {
            // Raw mouse would still turn the camera: drained, not passed on.
            UINT size = 0;
            GetRawInputData((HRAWINPUT)lp, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER));
            BYTE buf[256];
            if (size && size <= sizeof(buf)) GetRawInputData((HRAWINPUT)lp, RID_INPUT, buf, &size, sizeof(RAWINPUTHEADER));
            return 0;
        }
        if (IsMouseMessage(msg)) return 0;
        // Releases go through: a key held when the menu opened must not stay
        // pressed in the game.
        if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN || msg == WM_CHAR) return 0;
        if (msg == WM_SETCURSOR) {
            SetCursor(LoadCursorA(nullptr, IDC_ARROW));
            return 1;
        }
        if (handled && msg != WM_KEYUP && msg != WM_SYSKEYUP) return 1;
    }
    return orig ? CallWindowProcW(orig, hwnd, msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

// --- the window -----------------------------------------------------------------

constexpr int kMaxShaders = 64;
char g_Shaders[kMaxShaders][Config::kPath] = {};
int g_ShaderCount = 0;
char g_ShaderDir[Config::kPath] = {};

char g_Section[Config::kPath] = {};
unsigned g_Gen = ~0u;
Config::Settings g_Edit;
bool g_Dirty = false;
char g_Note[160] = {};

// The .hlsl files next to the current shader (or in shaders\), for the list.
void ScanShaders() {
    char dir[Config::kPath] = {};
    char cur[Config::kPath];
    DataPath(g_Edit.shader[0] ? g_Edit.shader : "shaders\\x", cur, sizeof(cur));
    strncpy_s(dir, cur, _TRUNCATE);
    char* cut = nullptr;
    for (char* p = dir; *p; ++p)
        if (*p == '\\' || *p == '/') cut = p;
    if (cut) cut[1] = 0;
    strncpy_s(g_ShaderDir, dir, _TRUNCATE);
    g_ShaderCount = 0;
    char pattern[Config::kPath];
    _snprintf_s(pattern, sizeof(pattern), _TRUNCATE, "%s*.hlsl", dir);
    wchar_t wpattern[MAX_PATH];
    if (!Widen(pattern, wpattern, MAX_PATH)) return;
    WIN32_FIND_DATAW f;
    HANDLE h = FindFirstFileW(wpattern, &f);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        char name[Config::kPath];
        if (f.cFileName[0] == L'_' || !Narrow(f.cFileName, name, sizeof(name))) continue;   // '_': probes, not effects
        _snprintf_s(g_Shaders[g_ShaderCount], Config::kPath, _TRUNCATE, "%s%s", dir, name);
        ++g_ShaderCount;
    } while (g_ShaderCount < kMaxShaders && FindNextFileW(h, &f));
    FindClose(h);
}

const char* FileName(const char* path) {
    const char* n = path;
    for (const char* p = path; *p; ++p)
        if (*p == '\\' || *p == '/') n = p + 1;
    return n;
}

void Sync() {
    char section[Config::kPath];
    Config::CurrentSection(section, sizeof(section));
    if (!section[0]) strcpy_s(section, "default");
    const unsigned gen = Config::Generation();
    if (gen == g_Gen && strcmp(section, g_Section) == 0) return;
    g_Gen = gen;
    strcpy_s(g_Section, section);
    Config::Read(g_Section, &g_Edit);
    g_Dirty = false;
    ScanShaders();
}

void Window() {
    Sync();
    bool changed = false;
    ImGui::SetNextWindowSize(ImVec2(520, 0), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(60, 60), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("WeaponFX", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }
    ImGui::Text("Weapon in hands: [%s]%s", g_Section, g_Dirty ? "  (not saved)" : "");
    ImGui::Separator();

    int mode = (int)g_Edit.mode;
    const char* modes[] = { "off", "replace", "overlay" };
    if (ImGui::Combo("Mode", &mode, modes, 3)) {
        g_Edit.mode = (Engine::Mode)mode;
        changed = true;
    }
    if (ImGui::BeginCombo("Shader", g_Edit.shader[0] ? FileName(g_Edit.shader) : "(none)")) {
        for (int i = 0; i < g_ShaderCount; ++i) {
            const bool sel = _stricmp(g_Shaders[i], g_Edit.shader) == 0;
            if (ImGui::Selectable(FileName(g_Shaders[i]), sel)) {
                strcpy_s(g_Edit.shader, g_Shaders[i]);
                changed = true;
            }
        }
        if (!g_ShaderCount) ImGui::TextDisabled("no .hlsl in %s", g_ShaderDir);
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Rescan")) ScanShaders();
    if (ImGui::InputText("Shader file", g_Edit.shader, sizeof(g_Edit.shader), ImGuiInputTextFlags_EnterReturnsTrue))
        changed = true;

    ImGui::Separator();
    for (int i = 0; i < Config::kSlots; ++i) {
        ImGui::PushID(i);
        char label[32];
        _snprintf_s(label, sizeof(label), _TRUNCATE, "tex%d", i);
        if (ImGui::InputText(label, g_Edit.tex[i], sizeof(g_Edit.tex[i]), ImGuiInputTextFlags_EnterReturnsTrue)) changed = true;
        ImGui::SameLine();
        if (ImGui::SmallButton("x") && g_Edit.tex[i][0]) {
            g_Edit.tex[i][0] = 0;
            changed = true;
        }
        ImGui::PopID();
    }
    for (int i = 0; i < Config::kSlots; ++i) {
        char label[32];
        _snprintf_s(label, sizeof(label), _TRUNCATE, "param%d", i);
        if (ImGui::DragFloat4(label, g_Edit.param[i], 0.01f)) changed = true;
    }
    if (ImGui::Checkbox("Viewmodel depth", &g_Edit.vmdepth)) changed = true;

    if (changed) {
        g_Dirty = true;
        Config::Preview(g_Section, g_Edit);
    }

    ImGui::Separator();
    char buf[96];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "Save to [%s]", g_Section);
    if (ImGui::Button(buf)) {
        const bool ok = Config::Write(g_Section, g_Edit);
        _snprintf_s(g_Note, sizeof(g_Note), _TRUNCATE, ok ? "saved to [%s]" : "saving [%s] failed - see weaponfx.log", g_Section);
    }
    ImGui::SameLine();
    if (ImGui::Button("Save to [default]")) {
        const bool ok = Config::Write("default", g_Edit);
        _snprintf_s(g_Note, sizeof(g_Note), _TRUNCATE, ok ? "saved to [default]" : "saving [default] failed - see weaponfx.log");
    }
    ImGui::SameLine();
    if (ImGui::Button("Revert")) {
        g_Gen = ~0u;   // read the file again
        Config::Invalidate();
        g_Note[0] = 0;
    }
    if (g_Note[0]) ImGui::TextDisabled("%s", g_Note);

    unsigned draws = 0, marked = 0, passes = 0, items = 0, stripped = 0, noRoom = 0;
    int hooks = 0;
    D3d::Stats(&draws, &marked, &hooks);
    Engine::Stats(&passes, &items, &stripped, &noRoom);
    ImGui::TextDisabled("draw hooks %d, marked draws %u / %u; viewmodel passes %u, weapon items %u, no room %u", hooks,
                        marked, draws, passes, items, noRoom);
    ImGui::TextDisabled("Insert / Esc closes. weaponfx.log in %s", DataDir());
    ImGui::End();
}

bool Init(IDXGISwapChain* sc, ID3D11Device* dev) {
    DXGI_SWAP_CHAIN_DESC d = {};
    if (FAILED(sc->GetDesc(&d)) || !d.OutputWindow) return false;
    ID3D11DeviceContext* ctx = nullptr;
    dev->GetImmediateContext(&ctx);
    if (!ctx) return false;
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;   // the window has a fixed place; nothing of the menu's is written
    io.LogFilename = nullptr;
    // Cyrillic too: paths in the ini are UTF-8 and may hold it. ImGui opens
    // the font through a UTF-8 path of its own.
    wchar_t wfont[MAX_PATH];
    char font[Config::kPath];
    const UINT n = GetWindowsDirectoryW(wfont, MAX_PATH);
    if (n && n + 20 < MAX_PATH) {
        wcscat_s(wfont, L"\\Fonts\\segoeui.ttf");
        if (GetFileAttributesW(wfont) != INVALID_FILE_ATTRIBUTES && Narrow(wfont, font, sizeof(font)))
            io.Fonts->AddFontFromFileTTF(font, 18.0f, nullptr, io.Fonts->GetGlyphRangesCyrillic());
    }
    if (io.Fonts->Fonts.empty()) io.Fonts->AddFontDefault();
    ImGui::StyleColorsDark();
    const bool ok = ImGui_ImplWin32_Init(d.OutputWindow) && ImGui_ImplDX11_Init(dev, ctx);
    ctx->Release();   // the backend took its own reference
    if (!ok) {
        ImGui::DestroyContext();
        return false;
    }
    g_Hwnd = d.OutputWindow;
    g_ImGuiReady = true;
    InstallInput();
    g_OrigWndProc = (WNDPROC)SetWindowLongPtrW(g_Hwnd, GWLP_WNDPROC, (LONG_PTR)&HkWndProc);
    if (!g_OrigWndProc) Log("menu: window procedure not replaced (%lu) - no menu key", GetLastError());
    else Log("menu: ready, %s toggles", "Insert");
    return true;
}

void Draw(IDXGISwapChain* sc, ID3D11Device* dev) {
    ID3D11Texture2D* back = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back)) || !back) return;
    ID3D11RenderTargetView* rtv = nullptr;
    dev->CreateRenderTargetView(back, nullptr, &rtv);
    back->Release();
    if (!rtv) return;
    ID3D11DeviceContext* ctx = nullptr;
    dev->GetImmediateContext(&ctx);
    // The backend restores most state but not the targets: the game's stay.
    ID3D11RenderTargetView* oldRtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* oldDsv = nullptr;
    ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldRtv, &oldDsv);

    {
        ImGuiLock lock;
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        ImGui::GetIO().MouseDrawCursor = false;
        Window();
        ImGui::Render();
    }
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldRtv, oldDsv);
    for (ID3D11RenderTargetView* v : oldRtv)
        if (v) v->Release();
    if (oldDsv) oldDsv->Release();
    rtv->Release();
    ctx->Release();
}

} // namespace

void Frame(IDXGISwapChain* sc, ID3D11Device* dev) {
    if (!g_ImGuiReady) {
        if (g_InitFailed) return;
        if (!Init(sc, dev)) {
            g_InitFailed = true;
            Log("menu: ImGui init failed - no menu");
            return;
        }
    }
    if (g_Open.load(std::memory_order_relaxed)) {
        ClipCursor(nullptr);   // the game clips it again on focus changes
        Draw(sc, dev);
    }
}

void Detach() {
    if (g_Hwnd && g_OrigWndProc) {
        SetWindowLongPtrW(g_Hwnd, GWLP_WNDPROC, (LONG_PTR)g_OrigWndProc);
        // A message already inside ours finishes before the code can go.
        for (int i = 0; i < 500 && g_InWndProc.load() > 0; ++i) Sleep(1);
    }
    const bool wasOpen = g_Open.exchange(false);
    if (wasOpen) {
        ClipCursor(nullptr);
        ShowOsCursor(g_SavedOsCursor);
        CallGameCursor(g_SavedGameCursor);
    }
    if (g_SlotInput) SetSlot(g_SlotInput, (void*)g_OrigMouseInput, nullptr);
    if (g_SlotCursor) SetSlot(g_SlotCursor, (void*)g_OrigCursor, nullptr);
    g_SlotInput = g_SlotCursor = nullptr;
}

void Shutdown() {
    if (!g_ImGuiReady) return;
    g_ImGuiReady = false;
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
}

} // namespace Menu
} // namespace Wfx
