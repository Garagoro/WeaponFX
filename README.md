# WeaponFX

Your own HLSL pixel shader on the weapon in hands in Counter-Strike 2 (DX11):
see-through glass, chrome, plasma, holograms, a window into space - anything a
pixel shader can draw, with the world behind the weapon, its depth, the
weapon's UV, normals and world position available to it. Different weapons can
carry different shaders, masks and parameters. Shaders reload on save.

It is a DLL for developers, not a finished mod: there is no injector, and the
engine offsets it relies on are documented rather than hidden. Use it offline
(`-insecure`, a local server or the workshop maps); it changes only how your
own weapon is drawn, but it is still foreign code in the game process.

How CS2 draws the weapon, how WeaponFX finds it, and every breakage met so far
with its cause: [RENDERING.md](RENDERING.md). Read it before changing the core.

## Build

Visual Studio 2019 or newer (or the Build Tools) with the C++ workload, x64.

    build.bat

gives `build\WeaponFX.dll` (and its `.pdb`). The script finds the
compiler through `vswhere`, so no developer prompt is needed. Our code builds at
`/W4 /WX`; Dear ImGui (`third_party/imgui`, 1.91.9b, unmodified) at its own
level.

Links only system libraries: `d3d11`, `dxgi`, `dxguid`, `windowscodecs`,
`ole32`, `user32`, `d3dcompiler` (ImGui's backend). `d3dcompiler_47.dll` for
the effects themselves is loaded at run time; it ships with Windows 10 and
later.

## Load

Load `WeaponFX.dll` into `cs2.exe` with any loader: `LoadLibrary` from a remote
thread or a manual map both work (the code has no `thread_local` and no
function-local statics with constructors, the two things a manual map breaks).
It can go in at the main menu: it waits for `d3d11`, `dxgi`, `scenesystem`,
`materialsystem2`, `client` and `inputsystem`, then hooks.

**End** unloads it: every hook goes back, in order (input, engine, draws,
present, then WeaponFX's own resources). A `LoadLibrary`-loaded copy frees
itself; a manual-mapped one stays mapped and inert.

## Data folder

`weaponfx.ini` and `weaponfx.log` live in one folder, the same however the DLL
was loaded:

- `%LOCALAPPDATA%\WeaponFX\` (created on the first run), e.g.
  `C:\Users\<you>\AppData\Local\WeaponFX\`;
- or `%WEAPONFX_DIR%`, if that environment variable is set for the game.

The log's first line names the folder, and the menu shows it. Relative paths
in the ini are taken against it, so copy `shaders\` there (or write absolute
paths).

Paths may hold any characters (a Cyrillic or Chinese user name included): they
are UTF-8 in the ini and in the code, and UTF-16 at every Windows call. Save the
ini as UTF-8 (with or without BOM) - any current editor does by default.

## weaponfx.ini

Written as a commented template on the first run (with `mode = off`). Saved
changes apply at once.

    [default]                 ; every weapon without a section of its own
    mode    = replace         ; replace | overlay | off
    shader  = shaders\seethrough.hlsl
    tex0    = shaders\masks\stripes.png   ; tex0..tex3: g_fxTex0..3
    param0  = 1 0.04 0 0      ; param0..param3: g_fxParam[0..3]
    vmdepth = 1               ; the viewmodel's own depth buffer

    [weapon_snip_awp]         ; the weapon's model file name, without .vmdl
    shader  = shaders\glass.hlsl

    [weapon_knife_karambit]
    shader  = shaders\chrome.hlsl
    tex0    =

- A weapon's section overrides `[default]` key by key.
- The section name is the model of the weapon in hands, which the log prints
  on every switch: `host: weapon in hands weapons/models/awp/weapon_snip_awp.vmdl`
  -> `[weapon_snip_awp]`. A skin does not change it.
- `replace`: the weapon is drawn with our shader instead of its skin.
  `overlay`: the skin is drawn first and our shader on top of it, so an effect
  returning alpha 0 leaves the skin and alpha 1 covers it (a mask picks where).
  `off`: the weapon is left to the game.
- A shader that fails to compile leaves the weapon untouched; the compiler's
  message is in the log, and the next save that compiles turns it on.
- `vmdepth`: hands and weapon get a depth buffer of their own, so the fingers
  hide the weapon and the weapon hides the fingers correctly even though the
  weapon is drawn without the engine's depth. Leave it on.

## Menu

**Insert** opens it (Esc or Insert closes). It edits the weapon in hands:
mode, shader (a list of the `.hlsl` next to the current one, files starting
with `_` left out), textures, parameters, viewmodel depth. Changes apply live;
**Save to [weapon]** / **Save to [default]** write them to the ini, **Revert**
reads the file again. A weapon switch drops unsaved changes.

While it is open the game gets no mouse and no key presses (key releases still
go through, so nothing stays held).

## Writing an effect

    #include "weaponfx.hlsli"

    float4 main(PSIn i) : SV_Target {
        float3 n = Normal(i);
        float  f = Fresnel(i, 3.0);
        float3 c = lerp(float3(0.1, 0.2, 0.4), Environment(reflect(ViewDir(i), n), 0, 2), f);
        return float4(c, 1);   // premultiplied alpha over what is under the weapon
    }

The output is premultiplied alpha: `out = ours.rgb + under * (1 - ours.a)`.
What is "under" is the skin in `overlay` mode and last frame's pixels in
`replace` mode, so an effect that does not cover everything should draw the
world itself (`SceneAt`).

What a shader gets (all in `shaders/weaponfx.hlsli`):

| Name | What |
|---|---|
| `PSIn i` | `pos` (camera-relative), `nrm` (world), `uv.xy` / `uv.zw`, `col`, `svpos`, `front`. Its layout must stay as it is |
| `Time()`, `g_fxTime` | seconds since load, delta, draw counter |
| `WorldPos(i)`, `CameraPos()`, `ViewDir(i)`, `Normal(i)`, `ScreenUV(i)` | spaces |
| `CameraRight()`, `CameraUp()`, `ScreenNormal(i)` | camera axes, the normal on screen |
| `Environment(dir, slice, mip)`, `Fresnel(i, power)` | the map's reflection probes |
| `SceneAt(i, offsetPx)` | the world behind the weapon (copied before the viewmodel), shifted by pixels |
| `SceneDistance(px)`, `HasDepth()` | distance to what the world has at a pixel |
| `g_fxParam[0..3]` | `param0..param3` from the ini / the menu |
| `g_fxTex0..3`, `HasTex0()`, `Tex0(i)`... | `tex0..tex3`, images sampled by the weapon's UV (masks: red channel) |

The world copy and the depth copy are taken only for a shader that reads them
(found by reflection), so a UV-only effect costs nothing extra.

The shipped effects and their parameters are described at the top of each
file: `seethrough`, `glass`, `chrome`, `galaxy`, `plasma`, `hologram`,
`masked` (two effects split by a mask). `debug.hlsl` shows the spaces the
helpers rely on; the `_probe_*` files are diagnostics.

`#include` is resolved next to the shader. Only the shader file itself is
watched: after editing an included file, save the shader (or the ini) to
recompile.

## Layout

| Path | What |
|---|---|
| `core/weapon_fx.{h,cpp}` | WeaponFX itself: shader compile and watch, the viewmodel detection, scene and depth copies, our draw. D3D11 and Win32 only, no game knowledge |
| `host/patch` | signature scans, 14-byte detours with a trampoline, code writes with other threads parked |
| `host/d3d_hooks` | `Present`; the `d3d11.dll` draw thunks of both implementation sets; `ClearDepthStencilView`; a draw is the weapon's when the marker's view is among its PS views |
| `host/engine` | `scenesystem!RenderObjects`: the weapon (items with the `csgo_weapon` shader) out of the viewmodel's depth passes and drawn with the marker; which weapon, by model path |
| `host/material` | the marker material: its texture preloaded, KV3 through the engine's loader, `CreateMaterial` |
| `host/config` | `weaponfx.ini`: read, watch, apply on a weapon switch |
| `host/menu` | ImGui on Insert, the game's input taken over while open |
| `host/dllmain.cpp` | the start thread, the per-frame driver, End and the ordered teardown |
| `shaders/` | `weaponfx.hlsli`, `effects.hlsli` and the effects |

## When it stops working

After a game update read the log first: every engine-side step logs what it
found or that it did not, and [RENDERING.md, section 6](RENDERING.md) maps
those lines and the visible symptoms to what moved. The offsets that move are
named constants at the top of `host/engine.cpp`, `host/material.cpp` and
`host/menu.cpp`, each with a comment on what it is.

## Limits

- DX11 only (the game's default on Windows). Vulkan was not looked at.
- The weapon in your hands only: not other players' weapons, not the world
  model, not the hands or gloves.
- One shader at a time: a weapon switch recompiles if the new weapon uses
  another file, and the first frames of the new weapon may still show the old
  effect.
- The menu belongs to the device it was created on; after a device reset
  (changing the display mode) reload the DLL.

## License

MIT, see [LICENSE](LICENSE). Dear ImGui under `third_party/imgui` is MIT as well,
with its own notice in `third_party/imgui/LICENSE.txt`.
