#pragma once

// weaponfx.ini in the data folder: what WeaponFX does to each weapon.
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
// A weapon's section overrides [default] key by key. Relative paths are
// against the data folder. The file is UTF-8 (host/ini.h). The file is re-read when it is saved; the settings
// are applied when the weapon in hands changes.

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
// call: a new weapon in hands, or a saved file.
void Tick();
// Everything is re-applied at the next Tick (WeaponFX was re-created).
void Invalidate();

// The section the weapon in hands reads (its model's file name), or "" before
// the first viewmodel pass.
void CurrentSection(char* out, unsigned cap);
// The settings a section resolves to ([default] under it).
void Read(const char* section, Settings* out);
// Writes every key of `s` into `section` and applies it at the next Tick.
bool Write(const char* section, const Settings& s);
// Applies `s` to the weapon in hands without touching the file (the menu,
// while a slider moves). Dropped when the weapon changes or the file is read
// again; Write keeps it.
void Preview(const char* section, const Settings& s);
// Changes whenever the file was read again: the menu's copy is stale.
unsigned Generation();
// Signalled by Preview and Write, so the control thread applies at once.
void SetWake(void* event);

} // namespace Config
} // namespace Wfx
