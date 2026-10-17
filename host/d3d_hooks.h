#pragma once

#include <cstdint>

// D3D11 side of the WeaponFX host: the present hook (the frame boundary and
// where everything else is driven from), the context's draw entry points and
// ClearDepthStencilView, handed to WeaponFX.
//
// A draw is the weapon's (or the gloves') when that target's marker texture
// is bound to the pixel shader - the views come from the engine side
// (SetMarkerViews).

struct ID3D11Device;
struct ID3D11ShaderResourceView;
struct IDXGISwapChain;

namespace Wfx {
namespace D3d {

// Called on the render thread inside Present, before the game's frame is
// presented. The first call is where the device becomes known.
using FrameFn = void (*)(IDXGISwapChain* sc, ID3D11Device* dev);

// Points the swap chain vtable's Present at ours (a throwaway device and
// window find the vtable; every swap chain of the class shares it).
bool InstallPresent(FrameFn onFrame);
void RemovePresent();

// Render thread. Hooks what the immediate context's table points at for
// every draw kind and ClearDepthStencilView. Call again each frame: d3d11
// switches implementations at run time, and a new one gets hooked then.
void HookDraws(ID3D11Device* dev);
// Removes every draw hook and waits (bounded) for draws inside our code.
void RemoveDraws();

// A target's marker views (either may be null). Compared, never called through.
void SetMarkerViews(int target, ID3D11ShaderResourceView* a, ID3D11ShaderResourceView* b);
void ClearMarkerViews();
// Any draw hook in place.
bool Hooked();

// Counters for the status line: draws seen, marked draws (the weapon's),
// hooks installed; the gloves' marked draws.
void Stats(unsigned* draws, unsigned* marked, int* hooks);
unsigned MarkedGloves();
// The host's frame boundary for the CPU cost line (logged every 5 s): what
// the draw hooks added this frame, and `frameTicks` (QPC) the host spent on
// WeaponFX's per-frame work. The standalone present hook calls it itself.
// Render thread.
void FrameCost(int64_t frameTicks);

} // namespace D3d
} // namespace Wfx
