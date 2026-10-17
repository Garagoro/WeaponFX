#pragma once

// The marker materials: what the weapon in hands and the gloves are drawn with
// instead of (or over) their skin, so that their D3D11 draws can be told apart
// - each marker's colour texture view is bound to their pixel shader and to
// nothing else's. One per WeaponFx::Target, each with its own texture.
//
// Built from KV3 text through the engine's own loader (tier0) and
// CreateMaterial (materialsystem2), on the render thread.

struct ID3D11ShaderResourceView;

namespace Wfx {
namespace Material {

// Resolves CreateMaterial and the KV3 loader. Any thread; idempotent.
bool Resolve();

// Creates `target`'s marker material once (render thread). Null until it
// exists or when creation failed (logged once). The engine's CMaterial2.
void* Marker(int target);
// The marker if it already exists, never creating it: for the scenesystem
// workers, which must not race the render thread into CreateMaterial.
void* MarkerIfReady(int target);

// The marker texture's two D3D11 views (linear, sRGB), read through the
// material's own g_tColor parameter each call - the engine can swap a view
// (streaming), and a stale pointer would lose the weapon. Null when not
// readable. Compared, never called through.
void MarkerViews(int target, ID3D11ShaderResourceView** a, ID3D11ShaderResourceView** b);

} // namespace Material
} // namespace Wfx
