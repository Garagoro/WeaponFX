#pragma once

// WeaponFX: our own HLSL pixel shader on the weapon in hands.
//
// The core knows nothing of the game: it depends on d3d11 and Win32 only, and
// the host (host/) hands in a log sink and the device. The host owns the draw
// interception and calls Begin/End around the one draw it decided is the
// weapon.
//
// Threads: Init/Shutdown/Load/SetParam on any one control thread; Begin/End
// on whatever thread issues the draw. A watcher thread of our own recompiles
// the file when it changes on disk.

#include <cstdint>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11PixelShader;
struct ID3D11Buffer;
struct ID3D11ShaderResourceView;
struct ID3D11BlendState;
struct ID3D11DepthStencilState;
struct ID3D11DepthStencilView;

namespace WeaponFx {

using LogFn = void (*)(const char* line);

// Our constant buffer's register. The engine's weapon PS uses b0..b5; the
// top slot is the one least likely to collide with a future combo. Restored
// after every draw, so a collision costs a wrong-looking weapon, not state.
constexpr unsigned kCbSlot = 13;
constexpr int kParams = 4;   // float4 g_fxParam[kParams]
// Our textures, far above the engine's (it uses t0..t15 on the weapon):
// t32 g_fxScene - the scene behind the weapon; t33..t36 g_fxTex0..3 - files;
// t37 g_fxDepth - the scene's depth at the same moment; t38 g_fxScenePrev -
// last frame's g_fxScene, the fallback SceneAt() reads. The copies are
// made only for a shader that reads them.
constexpr unsigned kSceneSlot = 32;
constexpr int kTextures = 4;
constexpr int kViews = 1 + kTextures + 2;   // t32..t38

bool Init(ID3D11Device* dev, LogFn log);
// Stops the watcher, unbinds nothing (End always restores), releases all.
// The host must have stopped calling Begin/End first.
void Shutdown();

// Compile `path` (an .hlsl with `float4 main(PSIn) : SV_Target`; #include is
// resolved next to it) and use it from the next draw. Kept and watched: a
// save on disk recompiles; a broken save keeps the last good shader and logs
// the compiler's message. nullptr or "" = off.
// `path` is UTF-8, as every path here.
bool Load(const char* path);
bool Active();

// The host's frame boundary (its present hook).
void NewFrame();
// Every draw the host intercepts, BEFORE it runs, with whether it is the
// weapon's. This is how the scene copy is taken before the viewmodel pass
// (hands and all), not just before the weapon's own draw: the viewmodel is
// one run of draws under one VS per-view buffer (b1), and the run that held
// the weapon last frame is copied at its first draw this frame. Same thread
// as the draws; cheap when no shader reads the scene.
//
// Returns true when the host must NOT issue this draw: the weapon in hands
// drawn into the main depth prepass. The engine puts the viewmodel there, so
// the world behind it fails the depth test and is never drawn - the pixels
// keep last frame's weapon, and a see-through effect has nothing to show.
// Without it the world fills in behind; the viewmodel's own colour draws do
// not need it (they test LESS_EQUAL or not at all). Also true for the
// engine's own passes of the weapon drawn with the marker material inside the
// viewmodel runs (replace mode): only its black, which our pass no longer
// covers where the weapon is behind the fingers.
bool BeforeDraw(ID3D11DeviceContext* c, bool weapon);
// Every ClearDepthStencilView, BEFORE it runs, with its D3D11_CLEAR_* flags.
// The engine clears the main depth between the world and the viewmodel run, so
// the world's depth has to be copied here - at the run's start it is already
// the far plane. A stencil-only clear of the main depth comes right before the
// viewmodel (see the shadow start in weapon_fx.cpp).
void BeforeClearDepth(ID3D11DeviceContext* c, ID3D11DepthStencilView* dsv, unsigned flags);
// Load an image (PNG/JPG/BMP..., via WIC) into g_fxTex<index>, linear RGBA8,
// one mip. nullptr or "" clears it. Control thread.
bool LoadTexture(int index, const char* path);
void SetParam(int index, float x, float y, float z, float w);

// The host's original draw call, with the arguments it intercepted. Draw may
// call it twice (a depth-only pass first), so it must not have side effects
// beyond the draw.
using IssueFn = void (*)(void* ctx);
// Issues a draw BeforeDraw let through: with our shader when it is the
// weapon's (Begin/End around it), and inside the viewmodel run against our
// viewmodel depth (SetViewmodelDepth). True when our shader drew it.
bool Draw(ID3D11DeviceContext* c, bool weapon, IssueFn issue, void* ctx);
// Viewmodel depth (on by default). The see-through setup takes the weapon out
// of the engine's depth passes and draws it without a depth test, so it lands
// on top of the fingers and of its own far side. With this on, every draw of
// the viewmodel run that targets the main depth goes to a depth buffer of our
// own instead - cleared at the run's start, so it holds the hands and the
// weapon only - with a depth-only pass first: hands and weapon hide each
// other in any draw order, and the world (not in it) never hides them. An
// effect that reads the scene gets no pass of the weapon's own (it turned the
// see-through black): the fingers, drawn first, still hide it.
void SetViewmodelDepth(bool on);

// Saved engine state between Begin and End.
struct Bound {
    ID3D11PixelShader* ps = nullptr;
    ID3D11Buffer* cb = nullptr;
    ID3D11ShaderResourceView* srv[kViews] = {};
    ID3D11BlendState* blend = nullptr;
    float blendFactor[4] = {};
    unsigned sampleMask = 0;
    bool on = false;
};
// Binds our PS, constant buffer, textures and a premultiplied-alpha blend
// (out = ours.rgb + under * (1 - ours.a)), remembering theirs: an effect that
// returns alpha 1 replaces the weapon, alpha 0 leaves what is under it.
// Returns false (and touches nothing) when no shader is loaded or no colour
// target is bound (the weapon's shadow-map draws).
bool Begin(ID3D11DeviceContext* c, Bound& saved);
void End(ID3D11DeviceContext* c, Bound& saved);

// How many scene/depth copies were made so far - lets a host mark in its own
// draw log where they happened.
uint32_t Copies();
// The b1 run the last BeforeDraw counted (-1 before the first), for host logs.
int CurrentRun();

// Debug: t37 and t32 show the main depth and colour target as they were
// before the n-th draw or depth clear of the frame on the weapon's context
// (-1 = off, the normal run copies). -2 logs one frame's scene copies and our
// draws, in order.
void DebugDepthAt(int drawIndex);
// Debug: every n-th frame the viewmodel is not found (0 = off) - exercises the
// fallback to the last good scene copy that covers a detection miss.
void DebugMissEvery(int n);
// Debug: a GPU ring of the last ~5 s of the scene copy the weapon read, small.
// 1 = record, 0 = stop (freed at the next frame), 2 = dump at the next
// present: black-spike frames as .ppm plus a .csv of every frame.
void FrameRing(int op);
// Where a dump goes (created if missing, one level). Unset: the dump is refused.
void SetDumpDir(const char* dir);

// One status line: file, compiles, failures, draws.
void Report();

} // namespace WeaponFx
