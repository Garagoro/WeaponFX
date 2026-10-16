#pragma once

// The marker material: the one the weapon in hands is drawn with instead of
// (or over) its skin, so that its D3D11 draws can be told apart - its colour
// texture's view is bound to their pixel shader and to nothing else's.
//
// Built from KV3 text through the engine's own loader (tier0) and
// CreateMaterial (materialsystem2), on the render thread.

struct ID3D11ShaderResourceView;

namespace Wfx {
namespace Material {

// Resolves CreateMaterial and the KV3 loader. Any thread; idempotent.
bool Resolve();

// Creates the marker material once (render thread). Null until it exists or
// when creation failed (logged once). The pointer is the engine's CMaterial2.
void* Marker();
// The marker if it already exists, never creating it: for the scenesystem
// workers, which must not race the render thread into CreateMaterial.
void* MarkerIfReady();

// The marker texture's two D3D11 views (linear, sRGB), read through the
// material's own g_tColor parameter each call - the engine can swap a view
// (streaming), and a stale pointer would lose the weapon. Null when not
// readable. Compared, never called through.
void MarkerViews(ID3D11ShaderResourceView** a, ID3D11ShaderResourceView** b);

} // namespace Material
} // namespace Wfx
