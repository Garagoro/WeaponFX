#pragma once

// Engine side of the WeaponFX host: scenesystem's RenderObjects, where the
// weapon in hands and the gloves are handed to the renderer one pass at a time.
//
// In the viewmodel's passes it (1) leaves them out of the depth passes -
// otherwise the world behind them is never drawn and a see-through effect has
// nothing to show - and (2) draws them with their marker material, instead of
// their skin (replace) or on top of it (overlay), so their D3D11 draws carry
// the marker and WeaponFX recognises them. Each target has its own marker and
// its own mode.
//
// The weapon is told from the hands by the item's own shader (csgo_weapon;
// arms, sleeves and gloves are csgo_character); the gloves among the
// csgo_character items by their model (agents/models/shared/arms/glove_*), and
// which weapon or glove it is by the scene object's model path - the material
// says nothing once a skin is on (a composite, "compmat_27.vmat").
//
// Two ways in. Standalone, Install() puts its own detour on RenderObjects. A
// host that already has one there (the skin editor: a second detour on the
// same prologue would be refused by the signature check) calls Attach() once
// and Handle() from its own hook instead.

#include <cstddef>

namespace Wfx {
namespace Engine {

enum class Mode { Off, Replace, Overlay };

bool Install();      // any thread; false leaves the game untouched
void Remove();       // restores the prologue, waits (bounded) for calls inside

using RenderObjectsFn = void*(__fastcall*)(void*, void*, void*, int, void*, void*, void*);
// For a host with its own RenderObjects detour: what Install does short of the
// patch. `itemStride` is the host's item size (0: the default).
bool Attach(size_t itemStride);
// From the host's detour, before anything of its own. True when this call was
// a viewmodel pass WeaponFX took: *result is what RenderObjects returns, and
// the host returns it as is. False: not ours, the host goes on.
bool Handle(RenderObjectsFn orig, void* p1, void* p2, void* items, int count, void* p5, void* L, void* p7,
            void** result);
// Whether Handle may take any pass right now: some target's mode is on and its
// marker exists. Lets a host skip the call (and its reads) when idle.
bool Wanted();

// `target` is a WeaponFx::Target.
void SetMode(int target, Mode m);
Mode GetMode(int target);

// The model in hands as of the last viewmodel pass, per target: the weapon
// ("weapons/models/awp/weapon_snip_awp.vmdl"), the gloves
// ("agents/models/shared/arms/glove_sporty/glove_sporty.vmdl"), or "" before
// the first one. `serial` changes whenever the path does.
void Model(int target, char* out, unsigned cap, unsigned* serial);
inline void WeaponModel(char* out, unsigned cap, unsigned* serial) { Model(0, out, cap, serial); }

// For the status line.
void Stats(unsigned* calls, unsigned* weaponItems, unsigned* stripped, unsigned* noRoom);
unsigned GloveItems();

} // namespace Engine
} // namespace Wfx
