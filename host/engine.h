#pragma once

// Engine side of the WeaponFX host: scenesystem's RenderObjects, where the
// weapon in hands is handed to the renderer one pass at a time.
//
// In the viewmodel's passes it (1) leaves the weapon out of the depth passes -
// otherwise the world behind it is never drawn and a see-through effect has
// nothing to show - and (2) draws the weapon with the marker material, instead
// of its skin (replace) or on top of it (overlay), so its D3D11 draws carry
// the marker and WeaponFX recognises them.
//
// The weapon is told from the hands by the item's own shader (csgo_weapon;
// arms, sleeves and gloves are csgo_character), and which weapon it is by the
// scene object's model path - the material says nothing once a skin is on
// (a composite, "compmat_27.vmat").

namespace Wfx {
namespace Engine {

enum class Mode { Off, Replace, Overlay };

bool Install();      // any thread; false leaves the game untouched
void Remove();       // restores the prologue, waits (bounded) for calls inside

void SetMode(Mode m);
Mode GetMode();

// The weapon in hands as of the last viewmodel pass: its model path
// ("weapons/models/awp/weapon_snip_awp.vmdl"), or "" before the first one.
// `serial` changes whenever the path does.
void WeaponModel(char* out, unsigned cap, unsigned* serial);

// For the status line.
void Stats(unsigned* calls, unsigned* weaponItems, unsigned* stripped, unsigned* noRoom);

} // namespace Engine
} // namespace Wfx
