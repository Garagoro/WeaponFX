#include "panel.h"
#include "config.h"
#include "d3d_hooks.h"
#include "engine.h"
#include "log.h"
#include "../core/weapon_fx.h"

#include "imgui.h"

#include <Windows.h>
#include <cstdio>
#include <cstring>

namespace Wfx {
namespace Panel {
namespace {

using WeaponFx::kTargets;

TrFn g_Tr = nullptr;
TexExtraFn g_TexExtra = nullptr;

const char* T(const char* english) { return g_Tr ? g_Tr(english) : english; }

constexpr int kMaxShaders = 64;
char g_Shaders[kMaxShaders][Config::kPath] = {};
int g_ShaderCount = 0;
char g_ShaderDir[Config::kPath] = {};

// The tab being edited, and per target what it edits.
int g_Tab = WeaponFx::kWeapon;
struct Edit {
    char section[Config::kPath] = {};
    unsigned gen = ~0u;
    Config::Settings s;
    bool dirty = false;
};
Edit g_Edit[kTargets];
char g_Note[160] = {};
char g_Error[512] = {};

// The .hlsl files next to the current shader (or in shaders\), for the list.
void ScanShaders(const Config::Settings& s) {
    char dir[Config::kPath] = {};
    char cur[Config::kPath];
    DataPath(s.shader[0] ? s.shader : "shaders\\x", cur, sizeof(cur));
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

void Sync(int t) {
    Edit& e = g_Edit[t];
    char section[Config::kPath];
    Config::CurrentSection(t, section, sizeof(section));
    if (!section[0]) strcpy_s(section, Config::DefaultSection(t));
    const unsigned gen = Config::Generation();
    if (gen == e.gen && strcmp(section, e.section) == 0) return;
    e.gen = gen;
    strcpy_s(e.section, section);
    Config::Read(e.section, &e.s);
    e.dirty = false;
    if (t == g_Tab) ScanShaders(e.s);
}

void Body(int t) {
    Edit& e = g_Edit[t];
    Config::Settings& s = e.s;
    bool changed = false;
    ImGui::Text(t == WeaponFx::kGloves ? T("Gloves in hands: [%s]%s") : T("Weapon in hands: [%s]%s"), e.section,
                e.dirty ? T("  (not saved)") : "");

    int mode = (int)s.mode;
    const char* modes[] = { T("off"), T("replace"), T("overlay") };
    if (ImGui::Combo(T("Mode"), &mode, modes, 3)) {
        s.mode = (Engine::Mode)mode;
        changed = true;
    }
    if (ImGui::BeginCombo(T("Shader"), s.shader[0] ? FileName(s.shader) : T("(none)"))) {
        for (int i = 0; i < g_ShaderCount; ++i) {
            const bool sel = _stricmp(g_Shaders[i], s.shader) == 0;
            if (ImGui::Selectable(FileName(g_Shaders[i]), sel)) {
                strcpy_s(s.shader, g_Shaders[i]);
                changed = true;
            }
        }
        if (!g_ShaderCount) ImGui::TextDisabled(T("no .hlsl in %s"), g_ShaderDir);
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(T("Rescan"))) ScanShaders(s);
    if (ImGui::InputText(T("Shader file"), s.shader, sizeof(s.shader), ImGuiInputTextFlags_EnterReturnsTrue))
        changed = true;
    WeaponFx::LastError(t, g_Error, sizeof(g_Error));
    if (g_Error[0]) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", g_Error);

    ImGui::Separator();
    for (int i = 0; i < Config::kSlots; ++i) {
        ImGui::PushID(i);
        char label[32];
        _snprintf_s(label, sizeof(label), _TRUNCATE, "tex%d", i);
        if (ImGui::InputText(label, s.tex[i], sizeof(s.tex[i]), ImGuiInputTextFlags_EnterReturnsTrue)) changed = true;
        ImGui::SameLine();
        if (ImGui::SmallButton("x") && s.tex[i][0]) {
            s.tex[i][0] = 0;
            changed = true;
        }
        if (g_TexExtra) {
            ImGui::SameLine();
            if (g_TexExtra(t, i, s.tex[i], sizeof(s.tex[i]))) changed = true;
        }
        ImGui::PopID();
    }
    for (int i = 0; i < Config::kSlots; ++i) {
        char label[32];
        _snprintf_s(label, sizeof(label), _TRUNCATE, "param%d", i);
        if (ImGui::DragFloat4(label, s.param[i], 0.01f)) changed = true;
    }
    if (t == WeaponFx::kWeapon && ImGui::Checkbox(T("Viewmodel depth"), &s.vmdepth)) changed = true;

    if (changed) {
        e.dirty = true;
        Config::Preview(e.section, s);
    }

    ImGui::Separator();
    char buf[160];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, T("Save to [%s]"), e.section);
    if (ImGui::Button(buf)) {
        const bool ok = Config::Write(e.section, s);
        _snprintf_s(g_Note, sizeof(g_Note), _TRUNCATE, ok ? T("saved to [%s]") : T("saving [%s] failed - see the log"),
                    e.section);
    }
    const char* def = Config::DefaultSection(t);
    if (_stricmp(def, e.section) != 0) {
        ImGui::SameLine();
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, T("Save to [%s]"), def);
        if (ImGui::Button(buf)) {
            const bool ok = Config::Write(def, s);
            _snprintf_s(g_Note, sizeof(g_Note), _TRUNCATE, ok ? T("saved to [%s]") : T("saving [%s] failed - see the log"),
                        def);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Revert"))) {
        e.gen = ~0u;   // read the file again
        Config::Invalidate();
        g_Note[0] = 0;
    }
    if (g_Note[0]) ImGui::TextDisabled("%s", g_Note);
}

} // namespace

void SetTranslator(TrFn fn) { g_Tr = fn; }
void SetTexExtra(TexExtraFn fn) { g_TexExtra = fn; }

void Draw() {
    for (int t = 0; t < kTargets; ++t) Sync(t);
    if (ImGui::BeginTabBar("wfx_targets")) {
        const char* names[kTargets] = { T("Weapon"), T("Gloves") };
        for (int t = 0; t < kTargets; ++t) {
            if (!ImGui::BeginTabItem(names[t])) continue;
            if (g_Tab != t) {
                g_Tab = t;
                ScanShaders(g_Edit[t].s);
            }
            Body(t);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    unsigned draws = 0, marked = 0, passes = 0, items = 0, stripped = 0, noRoom = 0;
    int hooks = 0;
    D3d::Stats(&draws, &marked, &hooks);
    Engine::Stats(&passes, &items, &stripped, &noRoom);
    ImGui::TextDisabled(T("draw hooks %d, marked draws %u + %u gloves / %u; viewmodel passes %u, items %u + %u gloves, no room %u"),
                        hooks, marked, D3d::MarkedGloves(), draws, passes, items, Engine::GloveItems(), noRoom);
}

} // namespace Panel
} // namespace Wfx
