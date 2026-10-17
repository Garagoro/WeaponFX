# WeaponFX guide

Everything about installing, using and extending WeaponFX. For what it is and
what it looks like, see the [README](README.md).

- [Quick start](#quick-start)
- [Using it](#using-it)
  - [The menu (Insert)](#the-menu-insert)
  - [weaponfx.ini](#weaponfxini)
  - [Masks](#masks)
- [Writing your own effect](#writing-your-own-effect)
- [Building](#building)
- [Limits](#limits)
- [When a game update breaks it](#when-a-game-update-breaks-it)
- [How it works](#how-it-works)

## Quick start

1. **Build** `WeaponFX.dll` (see [Building](#building)).
2. **Copy the `shaders` folder** to `%LOCALAPPDATA%\WeaponFX\`.
3. **Start CS2 offline**: with `-insecure`, on a local server or on a workshop
   map.
4. **Load `WeaponFX.dll` into `cs2.exe`** with any DLL loader. Loading at the
   main menu is fine: it waits for the game to finish starting.
5. **Press Insert** to open the menu, set the mode to `replace` and pick a
   shader.

**End** unloads it and puts the game back the way it was.

> WeaponFX is a tool for people who write shaders and know how to load a DLL.
> It does not ship a loader. It only changes how your own weapon is drawn, but
> it is still foreign code inside the game process. Use it offline.

## Using it

Only your own first-person weapon and gloves change, and only on your screen. Other
players, spectators and demos see the normal skin. Your fingers hide the
weapon and the weapon hides your fingers, as in the real game.

### The menu (Insert)

The menu has two tabs: **Weapon** edits the weapon currently in your hands,
**Gloves** your gloves. Each has its own shader, textures and parameters:

- **Mode**: `replace` draws the weapon with your shader instead of its skin.
  `overlay` draws the skin first and your shader on top. `off` leaves the
  weapon alone.
- **Shader**: every `.hlsl` in the shader folder.
- **Textures and parameters**: values your shader can read. See the top of
  each shader file for what it understands.

Changes show up immediately. **Save to [weapon]** keeps them for this weapon
only, **Save to [default]** for every weapon (on the Gloves tab: this pair, or
**[gloves]** for every pair). While the menu is open the game gets no mouse or
keyboard input.

### weaponfx.ini

The menu writes `weaponfx.ini`, and you can edit it by hand. It lives in
`%LOCALAPPDATA%\WeaponFX\`, next to `weaponfx.log`. Saved changes apply at once.

```ini
[default]                 ; every weapon without a section of its own
mode    = replace         ; replace | overlay | off
shader  = shaders\seethrough.hlsl
tex0    = shaders\masks\stripes.png
param0  = 1 0.04 0 0

[weapon_snip_awp]         ; the AWP gets glass
shader  = shaders\glass.hlsl

[weapon_knife_karambit]   ; the karambit gets chrome
shader  = shaders\chrome.hlsl

[gloves]                  ; every pair of gloves without a section of its own
mode    = replace
shader  = shaders\galaxy.hlsl

[glove_sporty]            ; Sport Gloves: a mask of their own
tex0    = masks\sporty.png
```

A section is named after the weapon's model. The log prints it every time you
switch weapons:

```
host: weapon in hands weapons/models/awp/weapon_snip_awp.vmdl
```

A weapon's section overrides `[default]` one key at a time. Skins do not
change the model name, so one section covers every skin of that weapon.

The gloves work the same way, with `[gloves]` instead of `[default]`. A pair's
section is named after the glove's folder in the game, and the log prints it
when the gloves in your hands change:

```
host: gloves in hands characters/models/shared/arms/glove_sporty
```

Your arms and sleeves are left alone. A shader can tell which of the two it is
drawing with `IsGloves()`, so one file can serve both.

<details>
<summary>All keys</summary>

| Key | Meaning |
|---|---|
| `mode` | `replace`, `overlay` or `off` |
| `shader` | path to the `.hlsl`, relative to the data folder or absolute |
| `tex0` .. `tex3` | images the shader samples by the weapon's UV (`g_fxTex0..3`). On gloves whose right hand reads the texture mirrored (Sport, Slick, Specialist), a mask twice as wide as tall holds the left hand in its left half and the right hand in its right half; `Tex0(i)` picks the half |
| `param0` .. `param3` | four numbers each, read as `g_fxParam[0..3]` |
| `vmdepth` | `1` (default): hands and weapon hide each other correctly. Leave it on. Read from the weapon's sections only |

The data folder can be moved by setting the `WEAPONFX_DIR` environment
variable for the game. Paths may contain any characters. Save the file as
UTF-8.
</details>

### Masks

A mask is an image painted on the weapon's texture that splits it between two
effects. `masks/awp_stock.png` is a ready one for the AWP: galaxy on the
stock, chrome on the metal.

Any effect can also go over the skin instead of replacing it (`overlay`
mode), so a mask can cut a window into the skin and leave the rest as it is.

## Writing your own effect

A minimal effect, a blue metal with the map's reflections at the edges:

```hlsl
#include "weaponfx.hlsli"

float4 main(PSIn i) : SV_Target {
    float3 n = Normal(i);
    float  f = Fresnel(i, 3.0);
    float3 c = lerp(float3(0.1, 0.2, 0.4), Environment(reflect(ViewDir(i), n), 0, 2), f);
    return float4(c, 1);
}
```

Save it next to the other shaders, pick it in the menu, and keep editing. Every
save recompiles it. If it does not compile, the weapon stays as it was and the
compiler's error is in `weaponfx.log`.

The output colour is **premultiplied alpha** over whatever is under the
weapon: alpha 1 covers it, alpha 0 leaves it. In `overlay` mode what is under
is the skin. In `replace` mode, draw the world yourself with `SceneAt`.

What your shader can use (all in [`shaders/weaponfx.hlsli`](shaders/weaponfx.hlsli)):

| | |
|---|---|
| `PSIn i` | position, normal, two UV sets, vertex colour, screen position |
| `Time()` | seconds since the shader was loaded |
| `WorldPos(i)`, `CameraPos()`, `ViewDir(i)`, `Normal(i)`, `ScreenUV(i)` | positions and directions |
| `CameraRight()`, `CameraUp()`, `ScreenNormal(i)` | the camera's axes, the normal on screen |
| `Environment(dir, slice, mip)`, `Fresnel(i, power)` | the map's reflections |
| `SceneAt(i, offsetPx)` | the world behind the weapon, optionally shifted |
| `SceneDistance(px)`, `HasDepth()` | how far away the world is at a pixel |
| `g_fxParam[0..3]` | the four parameters from the menu |
| `g_fxTex0..3`, `Tex0(i)` .. | your images, sampled by the weapon's UV |

The copies of the world and of its depth are only made for shaders that read
them, so an effect that only uses UVs costs nothing extra.

The shipped effects are good starting points; each one explains its parameters
at the top of the file. `debug.hlsl` shows the spaces the helpers use. The
`_probe_*` files are diagnostics and are hidden from the menu.

## Building

Visual Studio 2019 or newer (or the Build Tools) with the C++ workload, x64:

```
build.bat
```

The result is `build\WeaponFX.dll`. No developer prompt is needed: the script
finds the compiler itself. The project builds with warnings as errors at
`/W4`, and needs nothing beyond the Windows SDK. Dear ImGui is included under
`third_party/imgui`.

## Limits

- DirectX 11 only, the game's default on Windows. Vulkan is not supported.
- Only the weapon in your hands and your gloves: not other players' weapons,
  not weapons on the ground, not your bare arms or sleeves.
- One shader per target (the weapon, the gloves). Switching to a weapon with a
  different shader recompiles, and the first frames of the new weapon may
  still show the old effect.
- After changing the display mode, reload the DLL.

## When a game update breaks it

Look at `weaponfx.log` first. Every step that touches the game logs what it
found or what it could not find. [RENDERING.md](RENDERING.md), section 6,
matches those lines and the visible symptoms to what changed in the game. The
values that move between updates are named constants at the top of
`host/engine.cpp`, `host/material.cpp` and `host/menu.cpp`.

## How it works

In short: WeaponFX gives your weapon a marker material so it can recognise the
weapon's draws inside the game's DirectX 11 calls, takes the weapon out of the
game's depth passes so the world behind it gets drawn, copies that world, and
then draws the weapon again with your shader. The weapon and your hands get a
depth buffer of their own so they still hide each other.

The full story, with the game's frame step by step, every breakage met so far
and what did not work, is in [RENDERING.md](RENDERING.md). Read it before
changing the core.

<details>
<summary>Source layout</summary>

| Path | What |
|---|---|
| `core/weapon_fx.{h,cpp}` | the effect itself: compiling and watching shaders, finding the viewmodel, the world and depth copies, our draw. DirectX 11 and Win32 only, no knowledge of the game |
| `host/patch` | signature scans, detours, code writes with other threads paused |
| `host/d3d_hooks` | `Present`, the DirectX 11 draw calls, `ClearDepthStencilView`; recognising the weapon's draws by the marker |
| `host/engine` | the game's `RenderObjects`: the weapon and the gloves out of the depth passes and drawn with their markers; which weapon and gloves are in hands |
| `host/material` | creating the marker materials, one per target |
| `host/config` | reading and watching `weaponfx.ini` |
| `host/panel` | the settings panel, drawn into any ImGui window |
| `host/menu` | the in-game menu: input, the ImGui context, the window around the panel |
| `host/dllmain.cpp` | start-up, the per-frame driver, unloading in order |
| `shaders/` | the shared headers and the effects |
</details>

<details>
<summary>Embedding WeaponFX in another tool</summary>

A host that already hooks `Present` and the game's `RenderObjects` (a second
detour on the same prologue would be refused by the signature check) does not
use `host/dllmain.cpp` and `host/menu.cpp`. It compiles the core and the other
host parts, and:

- once: `Wfx::SetLogSink(fn)` and `Wfx::SetDataDir(dir)` for its own log and
  folder, then a control thread that calls `Wfx::Config::Tick()` (under the
  same lock as `WeaponFx::Init/Shutdown`);
- in its `Present`: `WeaponFx::Init(device, log)` on a new device,
  `Wfx::D3d::HookDraws(device)`, the markers (`Material::Marker(target)` once
  `Engine::Model(target)` is known, `Material::MarkerViews` into
  `D3d::SetMarkerViews`), then `WeaponFx::NewFrame()`;
- in its `RenderObjects` detour, first: `Engine::Attach(itemStride)` once, then
  `if (Engine::Handle(orig, ..., &r)) return r;`;
- `Panel::Draw()` inside one of its ImGui windows, with `Panel::SetTranslator`
  for its UI language and `Panel::SetTexExtra` for a button next to each
  texture slot;
- at unload: the `Handle` calls stop, `D3d::ClearMarkerViews()`,
  `D3d::RemoveDraws()`, the control thread ends, `WeaponFx::Shutdown()`.
</details>
