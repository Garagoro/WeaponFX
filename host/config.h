#pragma once

// weaponfx.ini in the data folder: what WeaponFX does to each weapon and to
// the gloves.
//
//   [default]                 ; every weapon without a section of its own
//   mode    = replace         ; replace | overlay | off
//   shader  = shaders\seethrough.hlsl
//   tex0    = shaders\masks\awp.png   ; tex0..tex3, image files
//   param0  = 1 0.5 0 0       ; param0..param3, four floats
//   vmdepth = 1               ; the viewmodel's own depth buffer
//
//   [weapon_snip_awp]         ; the model's file name, without .vmdl
//   shader  = shaders\glass.hlsl
//
//   [gloves]                  ; every pair of gloves without a section of its own
//   mode    = replace
//   shader  = shaders\galaxy.hlsl
//
//   [glove_sporty]            ; the glove model's file name
//   tex0    = masks\sporty.png
//
// A weapon's section overrides [default] key by key, a glove's section
// overrides [gloves]. The weapon and the gloves are separate targets, each
// with its own shader; vmdepth is the weapon's (one depth for the viewmodel).
// Relative paths are against the data folder. The file is UTF-8
// (host/ini.h). It is re-read when it is saved; the settings are applied when
// the weapon or the gloves in hands change.

#include "engine.h"
#include "log.h"

namespace Wfx {
namespace Config {

constexpr int kSlots = 4;
constexpr unsigned kPath = kPathBytes;   // UTF-8

struct Settings {
    Engine::Mode mode = Engine::Mode::Off;
    char shader[kPath] = {};          // as written in the file
    char tex[kSlots][kPath] = {};
    float param[kSlots][4] = {};
    bool vmdepth = true;
};

// Control thread, while WeaponFX is initialised (the host serialises this
// against WeaponFX's Init/Shutdown). Applies what changed since the last
// call: a new weapon or gloves in hands, or a saved file.
void Tick();
// Everything is re-applied at the next Tick (WeaponFX was re-created).
void Invalidate();

// A target's (WeaponFx::Target) default section: "default" or "gloves".
const char* DefaultSection(int target);
// Which target a section configures: "gloves" and "glove_*" are the gloves',
// everything else the weapon's.
int TargetOf(const char* section);
// The section `target` in hands reads (its model's file name), or "" before
// the first viewmodel pass that had it.
void CurrentSection(int target, char* out, unsigned cap);
// The settings a section resolves to (its target's default section under it).
void Read(const char* section, Settings* out);
// Writes every key of `s` into `section` and applies it at the next Tick.
bool Write(const char* section, const Settings& s);
// Writes only the given keys (key, value pairs, `n` of them) into `section`.
bool WriteKeys(const char* section, const char* const* keys, const char* const* values, int n);
// Applies `s` to `section`'s target in hands without touching the file (the
// menu, while a slider moves). Dropped when that target changes or the file
// is read again; Write keeps it.
void Preview(const char* section, const Settings& s);
// A section the host fills itself, in place of what the file says for it (the
// skin editor's animation on its weapon). One at a time; nullptr `s` clears.
// Any thread; applied at the next Tick.
void SetHostSection(const char* section, const Settings* s);
// Changes whenever the file was read again: the menu's copy is stale.
unsigned Generation();
// Signalled by Preview and Write, so the control thread applies at once.
void SetWake(void* event);
// The ini's full path (UTF-8).
const char* FilePath();

// "weapons/models/awp/weapon_snip_awp.vmdl" -> "weapon_snip_awp".
void SectionOf(const char* model, char* out, unsigned cap);

} // namespace Config
} // namespace Wfx
