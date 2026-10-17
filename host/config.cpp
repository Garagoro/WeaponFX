#include "config.h"
#include "ini.h"
#include "log.h"
#include "../core/weapon_fx.h"

#include <Windows.h>
#include <atomic>
#include <cstdio>
#include <cstring>

namespace Wfx {
namespace Config {
namespace {

using WeaponFx::kTargets;
using WeaponFx::kWeapon;
using WeaponFx::kGloves;

constexpr char kFile[] = "weaponfx.ini";
const char* const kDefaultOf[kTargets] = { "default", "gloves" };
constexpr char kGlovePrefix[] = "glove_";
constexpr ULONGLONG kFileCheckMs = 300;
constexpr int kMaxWriteKeys = 16;

// Written when there is no file, so the first run shows the format instead of
// doing nothing silently. Off: no shader is shipped at a known path.
constexpr char kTemplate[] =
    "; WeaponFX settings. Saved changes apply at once.\r\n"
    "; [default] is every weapon; a section named after the weapon's model file\r\n"
    "; (weapon_snip_awp, weapon_knife_karambit, ...) overrides it key by key.\r\n"
    "; [gloves] is every pair of gloves, a glove model's section (glove_sporty,\r\n"
    "; glove_slick, ...) overrides it. The two have shaders of their own.\r\n"
    "; What is in hands is logged as \"host: weapon in hands ...\" / \"gloves ...\".\r\n"
    "; Relative paths are against this folder.\r\n"
    "\r\n"
    "[default]\r\n"
    "; replace: our shader instead of the skin; overlay: over it; off: untouched\r\n"
    "mode = off\r\n"
    "shader = shaders\\seethrough.hlsl\r\n"
    "; tex0..tex3: images for g_fxTex0..3; param0..param3: four floats for g_fxParam[i]\r\n"
    "tex0 =\r\n"
    "param0 = 0 0 0 0\r\n"
    "vmdepth = 1\r\n"
    "\r\n"
    "[gloves]\r\n"
    "mode = off\r\n"
    "shader = shaders\\galaxy.hlsl\r\n";

char g_Path[kPath] = {};
FILETIME g_Stamp = {};
ULONGLONG g_NextCheck = 0;
std::atomic<bool> g_Force{true};

// One per target. Control thread, except the pending preview (its own lock).
struct State {
    unsigned appliedSerial = ~0u;
    char appliedSection[kPath] = {};
    Settings applied;          // what WeaponFX holds now (paths resolved)
    bool appliedValid = false;
    // The mode the file asks for while the shader has not compiled yet: WeaponFX
    // keeps watching the file, and the target goes on once a save compiles.
    Engine::Mode waiting = Engine::Mode::Off;
    bool hasPending = false;
    char pendingSection[kPath] = {};
    Settings pending;
};
State g_State[kTargets];

std::atomic<unsigned> g_Generation{0};
HANDLE g_WakeEvent = nullptr;
SRWLOCK g_PendingLock = SRWLOCK_INIT;   // the menu (render thread) -> Tick (control thread)

// SetHostSection, under g_PendingLock.
char g_HostSection[kPath] = {};
Settings g_HostSettings;

// The host's settings for `section`, if it holds that one.
bool HostSettings(const char* section, Settings* out) {
    AcquireSRWLockExclusive(&g_PendingLock);
    const bool have = g_HostSection[0] && _stricmp(g_HostSection, section) == 0;
    if (have) *out = g_HostSettings;
    ReleaseSRWLockExclusive(&g_PendingLock);
    return have;
}

void Wake() {
    if (g_WakeEvent) SetEvent(g_WakeEvent);
}

const char* IniPath() {
    if (!g_Path[0]) DataPath(kFile, g_Path, sizeof(g_Path));
    return g_Path;
}

bool FileStamp(FILETIME* out) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    wchar_t w[MAX_PATH];
    if (!Widen(IniPath(), w, MAX_PATH) || !GetFileAttributesExW(w, GetFileExInfoStandard, &a)) return false;
    *out = a.ftLastWriteTime;
    return true;
}

void WriteTemplate() {
    wchar_t w[MAX_PATH];
    if (!Widen(IniPath(), w, MAX_PATH)) return;
    HANDLE f = CreateFileW(w, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;   // exists already, or the folder is not writable
    DWORD n = 0;
    WriteFile(f, kTemplate, (DWORD)(sizeof(kTemplate) - 1), &n, nullptr);
    CloseHandle(f);
    Log("config: wrote a template %s", IniPath());
}

// The section's value, else its target's default section's, else `fallback`.
void Get(const Ini::File& f, const char* section, const char* key, const char* fallback, char* out, size_t cap) {
    const char* def = kDefaultOf[TargetOf(section)];
    if (section && section[0] && _stricmp(section, def) != 0 && Ini::Get(f, section, key, out, cap)) return;
    if (Ini::Get(f, def, key, out, cap)) return;
    strncpy_s(out, cap, fallback, _TRUNCATE);
}

Engine::Mode ParseMode(const char* v) {
    if (!_stricmp(v, "replace")) return Engine::Mode::Replace;
    if (!_stricmp(v, "overlay")) return Engine::Mode::Overlay;
    return Engine::Mode::Off;
}
const char* ModeName(Engine::Mode m) {
    return m == Engine::Mode::Replace ? "replace" : m == Engine::Mode::Overlay ? "overlay" : "off";
}

// Relative to the data folder; "" stays "" (the slot is off).
void Resolve(const char* in, char* out, size_t cap) {
    out[0] = 0;
    if (!in[0]) return;
    DataPath(in, out, (unsigned)cap);
}

void Apply(int t, const char* section, const Settings& s, bool force) {
    State& st = g_State[t];
    Settings r = s;   // paths resolved
    Resolve(s.shader, r.shader, sizeof(r.shader));
    for (int i = 0; i < kSlots; ++i) Resolve(s.tex[i], r.tex[i], sizeof(r.tex[i]));
    Engine::Mode mode = r.shader[0] ? r.mode : Engine::Mode::Off;

    // Off before the shader goes, on after it is in: the marker must never be
    // on the target with no shader to draw it (it would show as the marker).
    if (mode == Engine::Mode::Off) Engine::SetMode(t, mode);
    if (force || !st.appliedValid || strcmp(r.shader, st.applied.shader) != 0) {
        if (mode != Engine::Mode::Off && !WeaponFx::Load(t, r.shader)) Log("config: [%s] shader %s did not load", section, r.shader);
        if (mode == Engine::Mode::Off) WeaponFx::Load(t, nullptr);
    }
    for (int i = 0; i < kSlots; ++i) {
        if (force || !st.appliedValid || strcmp(r.tex[i], st.applied.tex[i]) != 0)
            if (!WeaponFx::LoadTexture(t, i, r.tex[i]) && r.tex[i][0])
                Log("config: [%s] tex%d %s did not load", section, i, r.tex[i]);
        WeaponFx::SetParam(t, i, r.param[i][0], r.param[i][1], r.param[i][2], r.param[i][3]);
    }
    const bool fresh = force || !st.appliedValid;
    // One viewmodel depth for both targets: the weapon's settings own it.
    if (t == kWeapon && (fresh || r.vmdepth != st.applied.vmdepth)) WeaponFx::SetViewmodelDepth(r.vmdepth);
    if (mode != Engine::Mode::Off && !WeaponFx::Active(t)) {
        Log("config: [%s] no shader compiled - left untouched until it does", section);
        st.waiting = mode;
        mode = Engine::Mode::Off;
    } else {
        st.waiting = Engine::Mode::Off;
    }
    // A slider in the menu applies every frame it moves: the log says what
    // changed what WeaponFX draws, not every parameter value.
    const bool quiet = !fresh && mode == Engine::GetMode(t) && strcmp(r.shader, st.applied.shader) == 0;
    Engine::SetMode(t, mode);
    st.applied = r;
    if (mode == Engine::Mode::Off && st.waiting == Engine::Mode::Off) st.applied.shader[0] = 0;   // unloaded: the next "on" loads it
    st.appliedValid = true;
    if (!quiet) Log("config: [%s] mode %s, shader %s", section, ModeName(mode), r.shader[0] ? r.shader : "-");
}

void TickTarget(int t, bool force) {
    State& st = g_State[t];
    if (st.waiting != Engine::Mode::Off && WeaponFx::Active(t)) {
        Log("config: [%s] shader compiled - mode %s", st.appliedSection[0] ? st.appliedSection : kDefaultOf[t],
            ModeName(st.waiting));
        Engine::SetMode(t, st.waiting);
        st.waiting = Engine::Mode::Off;
    }
    char model[kPath];
    unsigned serial = 0;
    Engine::Model(t, model, sizeof(model), &serial);
    const char* current = st.appliedSection[0] ? st.appliedSection : kDefaultOf[t];
    if (!force && serial == st.appliedSerial) {
        // Same model, same file: only the menu can have something to apply.
        Settings p;
        bool have = false;
        AcquireSRWLockExclusive(&g_PendingLock);
        if (st.hasPending && _stricmp(st.pendingSection, current) == 0) {
            p = st.pending;
            have = true;
        }
        st.hasPending = false;
        ReleaseSRWLockExclusive(&g_PendingLock);
        if (have) Apply(t, current, p, false);
        return;
    }
    AcquireSRWLockExclusive(&g_PendingLock);
    st.hasPending = false;   // another model or a re-read file: the preview was for what is gone
    ReleaseSRWLockExclusive(&g_PendingLock);
    char section[kPath];
    SectionOf(model, section, sizeof(section));
    // A model change re-applies only what differs; the shader on disk is
    // watched by WeaponFX itself.
    const bool sameSection = strcmp(section, st.appliedSection) == 0;
    st.appliedSerial = serial;
    strcpy_s(st.appliedSection, section);
    if (!force && sameSection && st.appliedValid) return;
    const char* name = section[0] ? section : kDefaultOf[t];
    Settings s;
    if (HostSettings(name, &s)) Log("config: [%s] held by the host", name);
    else Read(name, &s);
    Apply(t, name, s, force);
}

} // namespace

const char* DefaultSection(int target) { return kDefaultOf[target == kGloves ? kGloves : kWeapon]; }

int TargetOf(const char* section) {
    if (!section) return kWeapon;
    if (!_stricmp(section, kDefaultOf[kGloves]) || !_strnicmp(section, kGlovePrefix, sizeof(kGlovePrefix) - 1))
        return kGloves;
    return kWeapon;
}

void SectionOf(const char* model, char* out, unsigned cap) {
    if (!out || !cap) return;
    const char* name = model ? model : "";
    for (const char* p = name; *p; ++p)
        if (*p == '/' || *p == '\\') name = p + 1;
    strncpy_s(out, cap, name, _TRUNCATE);
    if (char* dot = strrchr(out, '.')) *dot = 0;
}

void Read(const char* section, Settings* out) {
    Settings s;
    Ini::File f;
    Ini::Load(IniPath(), &f);   // missing: every key takes its fallback
    char v[kPath];
    Get(f, section, "mode", "off", v, sizeof(v));
    s.mode = ParseMode(v);
    Get(f, section, "shader", "", s.shader, sizeof(s.shader));
    for (int i = 0; i < kSlots; ++i) {
        char key[16];
        _snprintf_s(key, sizeof(key), _TRUNCATE, "tex%d", i);
        Get(f, section, key, "", s.tex[i], sizeof(s.tex[i]));
        _snprintf_s(key, sizeof(key), _TRUNCATE, "param%d", i);
        Get(f, section, key, "", v, sizeof(v));
        for (char* p = v; *p; ++p)
            if (*p == ',') *p = ' ';
        float* q = s.param[i];
        if (sscanf_s(v, "%f %f %f %f", &q[0], &q[1], &q[2], &q[3]) < 1) q[0] = q[1] = q[2] = q[3] = 0.f;
    }
    Get(f, section, "vmdepth", "1", v, sizeof(v));
    s.vmdepth = v[0] != '0';
    Ini::Free(&f);
    *out = s;
}

bool Write(const char* section, const Settings& s) {
    if (!section || !section[0]) return false;
    char texKey[kSlots][8], paramKey[kSlots][8], paramVal[kSlots][96];
    Ini::Pair pairs[3 + 2 * kSlots];
    int n = 0;
    pairs[n++] = { "mode", ModeName(s.mode) };
    pairs[n++] = { "shader", s.shader };
    for (int i = 0; i < kSlots; ++i) {
        _snprintf_s(texKey[i], sizeof(texKey[i]), _TRUNCATE, "tex%d", i);
        _snprintf_s(paramKey[i], sizeof(paramKey[i]), _TRUNCATE, "param%d", i);
        _snprintf_s(paramVal[i], sizeof(paramVal[i]), _TRUNCATE, "%g %g %g %g", s.param[i][0], s.param[i][1],
                    s.param[i][2], s.param[i][3]);
        pairs[n++] = { texKey[i], s.tex[i] };
        pairs[n++] = { paramKey[i], paramVal[i] };
    }
    pairs[n++] = { "vmdepth", s.vmdepth ? "1" : "0" };
    const bool ok = Ini::Put(IniPath(), section, pairs, n);
    if (!ok) Log("config: writing [%s] to %s failed (%lu)", section, IniPath(), GetLastError());
    g_Force.store(true);
    Wake();
    return ok;
}

bool WriteKeys(const char* section, const char* const* keys, const char* const* values, int n) {
    if (!section || !section[0] || n <= 0 || n > kMaxWriteKeys) return false;
    Ini::Pair pairs[kMaxWriteKeys];
    for (int i = 0; i < n; ++i) pairs[i] = { keys[i], values[i] ? values[i] : "" };
    const bool ok = Ini::Put(IniPath(), section, pairs, n);
    if (!ok) Log("config: writing [%s] to %s failed (%lu)", section, IniPath(), GetLastError());
    g_Force.store(true);
    Wake();
    return ok;
}

void CurrentSection(int target, char* out, unsigned cap) {
    if (!out || !cap) return;
    char model[kPath];
    Engine::Model(target, model, sizeof(model), nullptr);
    SectionOf(model, out, cap);
}

void Invalidate() { g_Force.store(true); }

void Preview(const char* section, const Settings& s) {
    if (!section) return;
    State& st = g_State[TargetOf(section)];
    AcquireSRWLockExclusive(&g_PendingLock);
    strncpy_s(st.pendingSection, section, _TRUNCATE);
    st.pending = s;
    st.hasPending = true;
    ReleaseSRWLockExclusive(&g_PendingLock);
    Wake();
}

void SetHostSection(const char* section, const Settings* s) {
    AcquireSRWLockExclusive(&g_PendingLock);
    if (s && section && section[0]) {
        strncpy_s(g_HostSection, section, _TRUNCATE);
        g_HostSettings = *s;
    } else {
        g_HostSection[0] = 0;
    }
    ReleaseSRWLockExclusive(&g_PendingLock);
    // Everything re-applied: the section may be in hands right now, and a
    // cleared one has to fall back to the file.
    g_Force.store(true);
    Wake();
}

unsigned Generation() { return g_Generation.load(std::memory_order_acquire); }
void SetWake(void* event) { g_WakeEvent = (HANDLE)event; }
const char* FilePath() { return IniPath(); }

void Tick() {
    bool force = g_Force.exchange(false);
    const ULONGLONG now = GetTickCount64();
    if (now >= g_NextCheck) {
        g_NextCheck = now + kFileCheckMs;
        FILETIME t = {};
        if (!FileStamp(&t)) {
            WriteTemplate();
            FileStamp(&t);
        }
        if (CompareFileTime(&t, &g_Stamp) != 0) {
            if (g_Stamp.dwLowDateTime || g_Stamp.dwHighDateTime) Log("config: %s changed, re-read", IniPath());
            g_Stamp = t;
            force = true;   // re-reads the textures too: a saved ini is the "reload" button
        }
    }
    const unsigned gen0 = g_Generation.load(std::memory_order_relaxed);
    for (int t = 0; t < kTargets; ++t) TickTarget(t, force);
    if (force) g_Generation.store(gen0 + 1, std::memory_order_release);
}

} // namespace Config
} // namespace Wfx
