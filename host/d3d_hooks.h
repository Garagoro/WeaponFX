#pragma once

// D3D11 side of the WeaponFX host: the present hook (the frame boundary and
// where everything else is driven from), the context's draw entry points and
// ClearDepthStencilView, handed to WeaponFX.
//
// A draw is "the weapon" when one of the marker texture's views is bound to
// the pixel shader - the views come from the engine side (SetMarkerViews).

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

// The marker's views (either may be null). Compared, never called through.
void SetMarkerViews(ID3D11ShaderResourceView* a, ID3D11ShaderResourceView* b);

// Counters for the status line: draws seen, marked draws, hooks installed.
void Stats(unsigned* draws, unsigned* marked, int* hooks);

} // namespace D3d
} // namespace Wfx
