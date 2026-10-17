// WeaponFX.dll: entry, the per-frame driver, and ordered teardown.
//
// Works loaded either way: LoadLibrary (the folder and FreeLibrary come from
// the loader) or a manual map (no module entry: no thread_local, no static
// with a constructor inside a function, the data folder falls back to
// %LOCALAPPDATA%\WeaponFX). Unload: the End key.

#include "config.h"
#include "d3d_hooks.h"
#include "engine.h"
#include "log.h"
#include "material.h"
#include "menu.h"
#include "../core/weapon_fx.h"

#include <Windows.h>
#include <d3d11.h>
#include <atomic>

namespace {

// The game modules everything here hooks into; nothing is touched until all
// of them are mapped (the DLL may be injected at the main menu or earlier).
const wchar_t* const kNeedModules[] = { L"d3d11.dll", L"dxgi.dll", L"scenesystem.dll", L"materialsystem2.dll",
                                           L"client.dll", L"inputsystem.dll" };
constexpr DWORD kModuleWaitMs = 60000;
constexpr int kUnloadKey = VK_END;

std::atomic<bool> g_Running{true};
std::atomic<bool> g_FxReady{false};
std::atomic<ID3D11Device*> g_Device{nullptr};   // compared, the swap chain owns it
HMODULE g_Self = nullptr;
std::atomic<bool> g_EngineOn{false};
unsigned g_ModelSerial[WeaponFx::kTargets] = {};   // render thread only
// WeaponFX's Init/Shutdown (render thread, on a device change) against the
// config applying shaders and textures (control thread).
SRWLOCK g_FxLock = SRWLOCK_INIT;
HANDLE g_Wake = nullptr;      // the render thread saw another weapon: apply its settings now

void CoreLog(const char* line) { Wfx::Log("%s", line); }

// Render thread, inside Present.
void OnFrame(IDXGISwapChain* sc, ID3D11Device* dev) {
    if (!g_Running.load(std::memory_order_relaxed)) return;
    if (g_Device.load(std::memory_order_relaxed) != dev) {
        // First frame, or a device reset: WeaponFX lives on one device.
        AcquireSRWLockExclusive(&g_FxLock);
        if (g_FxReady.load()) {
            Wfx::Log("host: device changed %p -> %p", (void*)g_Device.load(), (void*)dev);
            WeaponFx::Shutdown();
            g_FxReady.store(false);
        }
        g_Device.store(dev);
        if (WeaponFx::Init(dev, &CoreLog)) {
            g_FxReady.store(true);
            Wfx::Config::Invalidate();
        } else {
            Wfx::Log("host: WeaponFX init failed");
        }
        ReleaseSRWLockExclusive(&g_FxLock);
        if (g_Wake) SetEvent(g_Wake);
    }
    Wfx::D3d::HookDraws(dev);
    if (g_EngineOn.load(std::memory_order_acquire)) {
        for (int t = 0; t < WeaponFx::kTargets; ++t) {
            char model[260];
            unsigned serial = 0;
            Wfx::Engine::Model(t, model, sizeof(model), &serial);
            if (serial != g_ModelSerial[t]) {
                g_ModelSerial[t] = serial;
                Wfx::Log("host: %s in hands %s", t == WeaponFx::kGloves ? "gloves" : "weapon", model);
                if (g_Wake) SetEvent(g_Wake);
            }
            // Created here, not on the scenesystem workers that use it: one
            // thread owns the attempt. Not before the target was seen in
            // hands: in the main menu the marker's texture does not resolve and
            // the material came out with the engine's error texture (on the
            // weapon, and its views shared with every other missing texture).
            if (serial && Wfx::Engine::GetMode(t) != Wfx::Engine::Mode::Off) Wfx::Material::Marker(t);
            // Re-read every frame: the engine may swap the views when it streams.
            ID3D11ShaderResourceView* a = nullptr;
            ID3D11ShaderResourceView* b = nullptr;
            Wfx::Material::MarkerViews(t, &a, &b);
            Wfx::D3d::SetMarkerViews(t, a, b);
        }
    }
    if (g_FxReady.load(std::memory_order_relaxed)) WeaponFx::NewFrame();
    // Last: drawn over the finished frame, right before the game presents it.
    Wfx::Menu::Frame(sc, dev);
}

bool WaitForModules() {
    const ULONGLONG until = GetTickCount64() + kModuleWaitMs;
    for (;;) {
        bool all = true;
        for (const wchar_t* m : kNeedModules) all = all && GetModuleHandleW(m);
        if (all) return true;
        if (GetTickCount64() > until || !g_Running.load()) return false;
        Sleep(200);
    }
}

// Producers first (the hooks that call into WeaponFX), then WeaponFX itself:
// it must not be released under a draw.
void Teardown() {
    g_Running.store(false);
    // Input first: the window procedure and the game's input calls point here.
    Wfx::Menu::Detach();
    // The engine hook first: it is what puts the marker on the weapon, and a
    // marked draw with WeaponFX gone would be drawn with the bare marker.
    if (g_EngineOn.exchange(false)) {
        unsigned calls = 0, items = 0, stripped = 0, noRoom = 0;
        Wfx::Engine::Stats(&calls, &items, &stripped, &noRoom);
        Wfx::Log("host: engine passes %u, weapon items %u, stripped from depth %u, no room %u", calls, items, stripped,
                 noRoom);
        Wfx::Engine::Remove();
    }
    Wfx::D3d::ClearMarkerViews();
    Wfx::D3d::RemoveDraws();
    Wfx::D3d::RemovePresent();
    Wfx::Menu::Shutdown();
    AcquireSRWLockExclusive(&g_FxLock);
    if (g_FxReady.load()) WeaponFx::Report();   // the session's counters, for the log
    if (g_FxReady.exchange(false)) WeaponFx::Shutdown();
    ReleaseSRWLockExclusive(&g_FxLock);
    Wfx::Log("host: unloaded");
}

DWORD WINAPI MainThread(LPVOID) {
    Wfx::Log("host: WeaponFX built " __DATE__ " " __TIME__ ", data folder %s, %s load", Wfx::DataDir(),
             Wfx::SelfModule() ? "LoadLibrary" : "manual-mapped");
    if (!WaitForModules()) {
        Wfx::Log("host: game modules not loaded within %lu ms - nothing installed", kModuleWaitMs);
        return 0;
    }
    g_Wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Wfx::Config::SetWake(g_Wake);
    if (!Wfx::D3d::InstallPresent(&OnFrame)) {
        Wfx::Log("host: Present not hooked - nothing installed");
        return 0;
    }
    // Without it the D3D side stays in and never sees a marked draw: inert.
    if (Wfx::Engine::Install()) g_EngineOn.store(true, std::memory_order_release);
    else Wfx::Log("host: engine side not installed - the weapon keeps its own material");
    while (g_Running.load()) {
        if (GetAsyncKeyState(kUnloadKey) & 0x8000) break;
        if (g_Wake) WaitForSingleObject(g_Wake, 50);
        else Sleep(50);
        AcquireSRWLockExclusive(&g_FxLock);
        if (g_FxReady.load()) Wfx::Config::Tick();
        ReleaseSRWLockExclusive(&g_FxLock);
    }
    Teardown();
    if (g_Wake) CloseHandle(g_Wake);   // after the Present hook is gone: nothing signals it now
    // Only a loader-mapped image can be freed; a manual map stays mapped (it
    // is inert now: every hook points back at the game).
    if (g_Self) FreeLibraryAndExitThread(g_Self, 0);
    return 0;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_Self = (HMODULE)Wfx::SelfModule();
        if (g_Self) DisableThreadLibraryCalls(g_Self);
        if (HANDLE t = CreateThread(nullptr, 0, &MainThread, nullptr, 0, nullptr)) CloseHandle(t);
    }
    (void)module;
    return TRUE;
}
