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

constexpr char kFile[] = "weaponfx.ini";
constexpr char kDefault[] = "default";
constexpr ULONGLONG kFileCheckMs = 300;

// Written when there is no file, so the first run shows the format instead of
// doing nothing silently. Off: no shader is shipped at a known path.
constexpr char kTemplate[] =
    "; WeaponFX settings. Saved changes apply at once.\r\n"
    "; [default] is every weapon; a section named after the weapon's model file\r\n"
    "; (weapon_snip_awp, weapon_knife_karambit, ...) overrides it key by key.\r\n"
    "; The weapon in hands is logged as \"host: weapon in hands ...\".\r\n"
    "; Relative paths are against this folder.\r\n"
    "\r\n"
    "[default]\r\n"
    "; replace: our shader instead of the skin; overlay: over it; off: untouched\r\n"
    "mode = off\r\n"
    "shader = shaders\\seethrough.hlsl\r\n"
    "; tex0..tex3: images for g_fxTex0..3; param0..param3: four floats for g_fxParam[i]\r\n"
    "tex0 =\r\n"
    "param0 = 0 0 0 0\r\n"
    "vmdepth = 1\r\n";

char g_Path[kPath] = {};
FILETIME g_Stamp = {};
ULONGLONG g_NextCheck = 0;
std::atomic<bool> g_Force{true};

unsigned g_AppliedSerial = ~0u;
char g_AppliedSection[kPath] = {};
Settings g_Applied;          // what WeaponFX holds now (paths resolved)
bool g_AppliedValid = false;
// The mode the file asks for while the shader has not compiled yet: WeaponFX
// keeps watching the file, and the weapon goes on once a save compiles.
Engine::Mode g_Waiting = Engine::Mode::Off;

std::atomic<unsigned> g_Generation{0};
HANDLE g_WakeEvent = nullptr;
SRWLOCK g_PendingLock = SRWLOCK_INIT;   // the menu (render thread) -> Tick (control thread)
bool g_HasPending = false;
char g_PendingSection[kPath] = {};
Settings g_Pending;

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

// The section's value, else [default]'s, else `fallback`.
void Get(const Ini::File& f, const char* section, const char* key, const char* fallback, char* out, size_t cap) {
    if (section && section[0] && _stricmp(section, kDefault) != 0 && Ini::Get(f, section, key, out, cap)) return;
    if (Ini::Get(f, kDefault, key, out, cap)) return;
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

// "weapons/models/awp/weapon_snip_awp.vmdl" -> "weapon_snip_awp".
void SectionOf(const char* model, char* out, size_t cap) {
    const char* name = model;
    for (const char* p = model; *p; ++p)
        if (*p == '/' || *p == '\\') name = p + 1;
    strncpy_s(out, cap, name, _TRUNCATE);
    if (char* dot = strrchr(out, '.')) *dot = 0;
}

void Apply(const char* section, const Settings& s, bool force) {
    Settings r = s;   // paths resolved
    Resolve(s.shader, r.shader, sizeof(r.shader));
    for (int i = 0; i < kSlots; ++i) Resolve(s.tex[i], r.tex[i], sizeof(r.tex[i]));
    Engine::Mode mode = r.shader[0] ? r.mode : Engine::Mode::Off;

    // Off before the shader goes, on after it is in: the marker must never be
    // on the weapon with no shader to draw it (it would show as the marker).
    if (mode == Engine::Mode::Off) Engine::SetMode(mode);
    if (force || !g_AppliedValid || strcmp(r.shader, g_Applied.shader) != 0) {
        if (mode != Engine::Mode::Off && !WeaponFx::Load(r.shader)) Log("config: [%s] shader %s did not load", section, r.shader);
        if (mode == Engine::Mode::Off) WeaponFx::Load(nullptr);
    }
    for (int i = 0; i < kSlots; ++i) {
        if (force || !g_AppliedValid || strcmp(r.tex[i], g_Applied.tex[i]) != 0)
            if (!WeaponFx::LoadTexture(i, r.tex[i]) && r.tex[i][0]) Log("config: [%s] tex%d %s did not load", section, i, r.tex[i]);
        WeaponFx::SetParam(i, r.param[i][0], r.param[i][1], r.param[i][2], r.param[i][3]);
    }
    const bool fresh = force || !g_AppliedValid;
    if (fresh || r.vmdepth != g_Applied.vmdepth) WeaponFx::SetViewmodelDepth(r.vmdepth);
    if (mode != Engine::Mode::Off && !WeaponFx::Active()) {
        Log("config: [%s] no shader compiled - weapon left untouched until it does", section);
        g_Waiting = mode;
        mode = Engine::Mode::Off;
    } else {
        g_Waiting = Engine::Mode::Off;
    }
    // A slider in the menu applies every frame it moves: the log says what
    // changed what WeaponFX draws, not every parameter value.
    const bool quiet = !fresh && mode == Engine::GetMode() && strcmp(r.shader, g_Applied.shader) == 0;
    Engine::SetMode(mode);
    g_Applied = r;
    if (mode == Engine::Mode::Off && g_Waiting == Engine::Mode::Off) g_Applied.shader[0] = 0;   // unloaded: the next "on" loads it
    g_AppliedValid = true;
    if (!quiet) Log("config: [%s] mode %s, shader %s", section, ModeName(mode), r.shader[0] ? r.shader : "-");
}

} // namespace

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

void CurrentSection(char* out, unsigned cap) {
    char model[kPath];
    Engine::WeaponModel(model, sizeof(model), nullptr);
    SectionOf(model, out, cap);
}

void Invalidate() { g_Force.store(true); }

void Preview(const char* section, const Settings& s) {
    if (!section) return;
    AcquireSRWLockExclusive(&g_PendingLock);
    strncpy_s(g_PendingSection, section, _TRUNCATE);
    g_Pending = s;
    g_HasPending = true;
    ReleaseSRWLockExclusive(&g_PendingLock);
    Wake();
}

unsigned Generation() { return g_Generation.load(std::memory_order_acquire); }
void SetWake(void* event) { g_WakeEvent = (HANDLE)event; }

void Tick() {
    if (g_Waiting != Engine::Mode::Off && WeaponFx::Active()) {
        Log("config: [%s] shader compiled - mode %s", g_AppliedSection, ModeName(g_Waiting));
        Engine::SetMode(g_Waiting);
        g_Waiting = Engine::Mode::Off;
    }
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
    char model[kPath];
    unsigned serial = 0;
    Engine::WeaponModel(model, sizeof(model), &serial);
    if (!force && serial == g_AppliedSerial) {
        // Same weapon, same file: only the menu can have something to apply.
        Settings p;
        bool have = false;
        AcquireSRWLockExclusive(&g_PendingLock);
        if (g_HasPending && strcmp(g_PendingSection, g_AppliedSection[0] ? g_AppliedSection : kDefault) == 0) {
            p = g_Pending;
            have = true;
        }
        g_HasPending = false;
        ReleaseSRWLockExclusive(&g_PendingLock);
        if (have) Apply(g_AppliedSection[0] ? g_AppliedSection : kDefault, p, false);
        return;
    }
    AcquireSRWLockExclusive(&g_PendingLock);
    g_HasPending = false;   // another weapon or a re-read file: the preview was for what is gone
    ReleaseSRWLockExclusive(&g_PendingLock);
    char section[kPath];
    SectionOf(model, section, sizeof(section));
    // A weapon change re-applies only what differs; the shader on disk is
    // watched by WeaponFX itself.
    const bool sameSection = strcmp(section, g_AppliedSection) == 0;
    g_AppliedSerial = serial;
    strcpy_s(g_AppliedSection, section);
    if (!force && sameSection) return;
    Settings s;
    Read(section, &s);
    Apply(section[0] ? section : kDefault, s, force);
    g_Generation.fetch_add(1, std::memory_order_release);
}

} // namespace Config
} // namespace Wfx
