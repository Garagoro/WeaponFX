# WeaponFX: how CS2 draws the weapon in hands, and where it breaks

Notes for WeaponFX, a custom HLSL pixel shader on the weapon in hands (the
viewmodel) in CS2 under DX11. They record what is known about the game's
frame, why the code is shaped the way it is rather than simpler, and every
breakage so far with its symptom, cause and fix. The point is that after a
game update or a new breakage nobody has to find all of this again.

State as of 2026-10-16. Everything below was measured on DX11 and Windows;
Vulkan was not looked at. Addresses and indices that move with updates are
marked **[fragile]**.

---

## 1. The game's frame as D3D11 sees it

### 1.1. Who issues the draws, and when

- `scenesystem.dll` describes the scene (`RenderObjects`, one call per item
  and pass), but **it does not call D3D11**. It records commands into its own
  buffer, and `rendersystemdx11.dll` plays them back later on the render
  thread, on the **immediate context**. A thread mark set inside
  `RenderObjects` is never visible at D3D draw time. Everything WeaponFX
  decides, it decides from the D3D11 state at the draw.
- `ExecuteCommandList` and indirect draws do not occur in the frame.
- Meshes are drawn with `DrawIndexedInstanced`; `DrawIndexed` is never
  called. Full-screen passes use `Draw` / `DrawInstanced`.

### 1.2. Frame order on the main target

The main target is HDR at screen size (2560x1440 here), R11G11B10_FLOAT.
Order, as recorded draw by draw:

1. Clear of the main depth.
2. **Depth prepass**: the viewmodel (hands and weapon), legs, a stencil fill
   by a full-screen `Draw`.
3. Enemies and other models, depth test `EQUAL` against the prepass.
4. **The world**, `LESS_EQUAL`, about 130-150 draws.
5. A stencil-only clear of the main depth, then passes into other targets.
6. **The viewmodel**, two draws per mesh: no depth test, then `LESS_EQUAL`
   without a write. It can come as several runs (see 1.3): the weapon with
   its sleeves, another depth clear, the gloves.
7. Post effects, tonemapping, UI.

The same weapon is also drawn into other targets: an R32 of the same size,
half- and quarter-size targets, shadow maps (up to 5760²).

The key consequence: **the world under the hands is never drawn**. The hands
are in the depth prepass, so the world fails the depth test there. A copy of
the screen holds pure black in those pixels (or last frame, if the target is
not cleared).

### 1.3. Runs by the view buffer (VS `b1`)

Within a frame, draws come in groups under one view constant buffer, VS `b1`
(640 bytes: matrices, camera position, projection). WeaponFX calls
consecutive draws with the same `b1` pointer a **run**. The viewmodel is a
run of its own (or several in a row): it has its own projection (its own
FOV and near plane).

- The `b1` pointer comes from a pool each frame. It is not constant in
  itself, but at the same place in the frame it usually repeats.
- **The number of runs before the viewmodel changes**: a shot adds runs
  (muzzle flash, casings); turning the camera right adds two runs right
  before the viewmodel; a weapon switch changes the layout for a while. The
  run index is not a signal, and neither is the `b1` pointer (it is never
  the same two frames running).

### 1.4. Interception

- Every `ID3D11DeviceContext` has its own copy of the method table on the
  heap. Patching the table catches that one object only, and a deferred
  context's table dies with it (writing into it after `Release` crashes the
  game).
- The `DrawIndexedInstanced` implementation the table points to goes
  straight into the driver (`nvwgf2umx.dll`); we do not detour there. The
  detours go on the thunks inside `d3d11.dll`, shared by all table copies
  **[fragile: the addresses move with the Windows version]**.
- **d3d11 switches the immediate context's table to a second set of
  implementations at run time** (all Draw* calls, within one game session).
  Both sets have to be hooked, and the prologue bytes checked before
  patching. Symptom of a miss: the effect is gone or the weapon looks
  "broken" and the log shows no draws. Reinject first.

---

## 2. How WeaponFX finds the weapon in hands

### 2.1. The marker material

The WeaponFX host replaces the weapon-in-hands material with a marker
material carrying a rare texture,
`materials/dev/black_grid_8_color_tga_dd8b6813.vtex`. A draw with that
texture among the pixel shader's SRVs is the weapon. The P2000 has about 7
such draws per frame, one per mesh.

**Two slots, not one.** The marker material binds the texture twice
(`g_tColor`, `g_tColor2`), and its draws carry the view in PS slots 13 and 14
**[fragile: slot numbers]**. D3D11 keeps a binding until something rebinds the
slot, so a draw right after the weapon can inherit the view in one slot - the
muzzle flash and smoke quads do (slot 14 only). A draw is the weapon's only
with the marker in two slots or more. `d3d: marked draw - PS ..., marker in
slots ...` in the log lists every distinct case once.

The host makes the marker itself (`host/material.cpp`): KV3 text through the
engine's loader (tier0) and `CreateMaterial` (materialsystem2). Nothing in a
match uses that texture, so nothing loads it either: the host asks the
resource system for it first (`ResourceSystem013`, a non-blocking preload)
and creates the material half a second later, and only once a weapon has
been seen in hands. Created earlier, or without the preload, the material
comes out with the engine's error texture (a pink and black checker on the
weapon), and the "marker" views are then shared with every other missing
texture.

The views are read from the material's own `g_tColor` parameter every frame
(record `+0x10` -> binding, `+0` -> texture object, `+0x10`/`+0x18` -> the
two SRVs) **[fragile]**: the engine may swap them while it streams.

Which items are the weapon, in `RenderObjects`: the ones whose material's
shader is `csgo_weapon.vfx`. Arms, sleeves and gloves are
`csgo_character.vfx`. Which weapon it is comes from the item's scene object,
not its material - with a skin on, the material is a composite
(`compmat_27.vmat`): scene object `+0x110` -> `+0xB8` -> the model path
(`weapons/models/awp/weapon_snip_awp.vmdl`) **[fragile; found by scanning,
logged as "engine: model path at scene object ..."]**.

The same marker also reaches other models of that weapon: a pistol
holstered on your own body, a knife on the ground, the half-size passes,
enemies (if the replacement reaches them). So "a marked draw" does not yet
mean "the viewmodel".

### 2.2. The material setup see-through cannot work without

For the world to show through the weapon, the world has to be drawn behind
it. Both conditions are needed:

1. The weapon is taken out of the viewmodel depth passes at the engine
   level (the weapon's items are dropped from the `RenderObjects` call of
   the pass with the depth bit).
2. The original skin does not draw into depth: the marker material has
   `F_DISABLE_Z_BUFFERING`, `F_DISABLE_Z_PREPASS`, `F_DISABLE_Z_WRITE` (`kMarkerKv3` in
   `host/material.cpp`).

Neither works alone. With only (1), the world is still hidden. With only
(2), the weapon's place is black. Our pass has no depth test: with one, it
falls behind the world.

Two modes:
- **full replace**: our shader is all you see on the weapon;
- **overlay**: the engine draws its own skin and our pass goes on top by a
  mask. The scene copy is taken before the skin, so under the mask you see
  the world, outside it the skin.

Dropping draws at the D3D level (the prepass) does not replace this: it was
tried, and the world behind the weapon did not appear.

### 2.3. Which run is the viewmodel

The choice is made from last frame's tallies (`RollFrame`):

- only weapon draws **with a colour target** count (otherwise a shadow pass
  won);
- candidates are runs with weapon draws **without a depth test** (our pass
  and the marker's passes; enemies are tested `EQUAL`);
- among them, the run on the **widest target** comes first (the screen, not
  a half-size pass), then the run with the most such draws;
- that run and **every run after it** are the viewmodel.

That tally only says which runs to treat as the viewmodel's and feeds the
statistics. **The start in the current frame is an event, not a guess**:

1. the engine issues a **stencil-only clear of the main depth** after the
   world; the **first draw on the screen target that tests and writes depth
   after it** is the viewmodel's first hand;
2. the event is refused if fewer than half as many draws reached the screen
   target as last frame had before its start (an early stencil clear also
   happens, 2 frames in ~8000; taking it put the world into our viewmodel
   depth);
3. the fallback, when there is no event (first frames, targets not learnt),
   is the first **marked** draw without a depth test on the screen target.
   A match on the weapon's vertex buffer alone is never enough: that buffer
   is a pool the world draws from too.

The "screen target" (colour and depth) is learnt as the widest target a
marked draw without a depth test went to during a frame, and forgotten after
30 frames without a start. Without the forgetting, a marked weapon drawn in
another program's view (a 3D preview window of another tool, 2139x1286) was learnt
once and locked the start out for good.

Before this, the start was guessed from last frame by run index and `b1`.
Measured side by side over 13274 frames of switches, knife, shots and turns,
the event matched the run of the first marked draw every time; the guess
missed 253 times. Every frame is still checked against that marked draw
(section 5), so a game update that moves the clear shows up in the log.

### 2.4. When the scene copy (`t32 g_fxScene`) is taken

Only when the shader reads it (reflection, by the name `g_fxScene`). The copy
is taken:

- **before the first draw of the in-hand weapon's vertex buffers on the
  screen target**, the engine's own passes included, not at the run's
  start: otherwise the pieces of the hands drawn in the run before the
  weapon are missing from it;
- not before the viewmodel starts: the weapon's vertex buffer is a pool
  shared with world models, and the same model is drawn in the middle of
  the world;
- not on an `EQUAL`-tested draw (the same model in the world pass);
- again in **each later viewmodel run**, before its first weapon draw
  (otherwise a piece among the gloves reads a copy without the gloves).

Other targets are never copied: 5-6 target shapes per frame against 3 slots
evicted each other every frame.

### 2.5. Depth

- `t37 g_fxDepth` is the world's depth. It is copied at the
  `ClearDepthStencilView` of the main depth before the viewmodel: at the
  run's start it already holds the far plane. The depth is plain (not
  reversed), near ~6.6; the projection is `b1[12..15]` **[fragile]**.
- **Our own viewmodel depth**: the run's draws to the main depth go into our
  buffer of the same shape, and draws without a depth write get a depth-only
  pass first. That way the hands and the weapon hide each other in any draw
  order, and the world does not hide them.
- For an effect that reads the scene, **the weapon's own depth pass is
  skipped**: with the weapon's depth in our buffer the see-through turned
  black, and the cause was never fully pinned down. The fingers still hide
  the weapon: they are drawn first and write depth.
- Under an effect that reads the scene, the engine's own passes of the
  weapon write no depth: otherwise a hand behind the weapon did not land,
  and the window showed black.

### 2.6. The fallback for a miss

If the viewmodel is still not found in a frame, the weapon gets **the last
good copy**, not nothing. The copy may be at most 8 frames old; older than
that, the effect draws without a background that frame (a weapon switch,
for example). The copy slot sets alternate by frame parity, so last frame's
copy is always intact.

Also, `t38 g_fxScenePrev` is last frame's copy. `SceneAt()` reads it where
the current copy is pure black. Pure black in an HDR frame is not a lit
world; it means "nothing was drawn here".

The miss is not hidden: it shows in the log (section 5).

---

## 3. Breakages so far

| Symptom | Cause | Fix |
|---|---|---|
| The weapon shows its normal material for one frame, more often with the console open | The host cleared and rebuilt its list of replaced models every frame, with the weapon added last; the render thread hit that window | The list is built in a spare buffer and published with one write |
| Glass: black behind the weapon | The copy was taken at the first marked draw, on an enemy, before the world | Copies only in the viewmodel (2.4) |
| The weapon shows itself through itself, the tint "accumulates" | The world behind the weapon is not drawn; the target keeps last frame | No-depth material plus the weapon out of the depth passes (2.2) |
| Video memory leak, the PC went down (process at ~36 GB) | The copy was recreated every frame: the weapon is drawn into targets of different sizes | Slots by target shape, a creation fuse (8/s), a memory guard |
| Black blinking, the effect turns itself off | Copies of other targets (shadows, menus) evicted the slots, the fuse tripped | Copies of the viewmodel's target only |
| The holstered pistol gets the copy instead of the knife in hand | "Most draws" vote: 10 draws holstered against one for the knife | Runs without a depth test, widest target (2.3) |
| "Shadow": a black patch under the weapon in see-through | The weapon's own depth in our viewmodel depth buffer | No weapon depth pass for effects that read the scene (2.5) |
| Black in the palm and by the fist (knife) | Blade and hands in different runs; a copy at the run's start is taken before the hands | Copy before the first weapon draw; a fresh copy in every later run |
| Black streaks by the fingers (AWP, Scout) | The bent read lands behind a hand drawn after the copy, where it is pure black | In `seethrough.hlsl` a black result is replaced by the unbent read |
| See-through black with no bots | The weapon's vertex buffer is a shared pool; a model in the world fired the copy mid-world | No copy before the viewmodel |
| One black frame on shots | A shot adds runs and shifts the run index | Found by the `b1` pointer and occurrence |
| One black frame when turning the camera right | Turning adds two runs before the viewmodel; the guess landed on a run of another target and the frame had no copy at all | The viewmodel starts only on the screen target; fallback start at a weapon draw without a depth test (2.3) |
| The whole screen blinks on a weapon switch: the world is gone for a frame, only fog and a few props | The switch changes the run layout; the guessed start landed in the world, and every world draw after it went to our viewmodel depth | The start is the stencil-clear event (2.3) |
| The effect stops working after another tool's 3D preview was open | That preview (a marked weapon in another view) was learnt as the screen target, and starts were only accepted there | The screen target is forgotten after 30 frames without a start (2.3) |
| The weapon in a pink and black checker in a fresh game | The marker's texture was never loaded: nothing in a match uses it | Preload through the resource system, then the material (2.1) |
| Muzzle flash and smoke painted with the effect | The quads drawn right after the weapon rebind PS slot 13 but not 14; the marker view left in 14 made them "marked" | A weapon draw carries the marker in two slots (the material binds it as `g_tColor` and `g_tColor2`); one slot is a stale binding (2.1) |
| An opaque effect turns the weapon inside out (far side over the near side), for a frame, or for good after a shot | A weapon draw outside the detected viewmodel run gets the marker's own state: no depth test. Then: the flash quads taken for the weapon rebuilt the viewmodel depth at 640x360 against 2560x1440 until the creation fuse tripped | Such a draw gets our viewmodel depth (a depth pass, then ours), only on the screen target and its depth |
| (for the future) any new miss | - | The last good copy instead of nothing (2.6) |

Not ours: the right glove is semi-transparent even without the replacement
and the effect.

---

## 4. What was tried and did not work

- **Valve's shaders for a "window"**: `csgo_simple_3layer_parallax` is static
  on the viewmodel (its depth is in surface space, and the weapon moves with
  the camera); `F_OPAQUE_REFRACT` cannot be switched on either through the
  material replacement or in a skin recipe. Hence our own HLSL.
- **A thread mark in `RenderObjects`** as the viewmodel signal: it is gone
  by D3D draw time (1.1).
- **Copying by mesh key**: the same mesh is drawn mid-world too, and the
  frame was black almost every time.
- **PIX events as an exact pass signal.** `rendersystemdx11.dll` is built
  with PIX instrumentation. Its command-list player has Begin/End/Marker
  opcodes; the event names travel inside the command stream and are handed
  to `d3d9!D3DPERF_BeginEvent/EndEvent/SetMarker` at execution time. It does
  that only if a startup test (`D3DPERF_BeginEvent(-1, "TestActive")`
  returning non-negative) remembered "PIX is attached". The probe swaps those
  three imports and sets the remembered byte (found by the test's
  signature). It works, but **only the engine's own compute passes** arrive
  (skinning, the luminance histogram): scene passes carry no events.
  `sc_show_gpu_profiler` is not available in the release game. Dead end.
- **Not tried yet**: telling the viewmodel by the contents of `b1`. It has
  its own projection (`viewmodel_fov`). That is a signal by meaning rather
  than by draw order, and it does not depend on how many runs there are.

---

## 5. Debugging tools

Everything above was found with a debug build that is not part of this
project. What it measured, and how much of it is here:

| Tool | What it does | Here |
|---|---|---|
| Report | Shader, compiles, copies, misses, covered frames, dropped passes - three lines | `WeaponFx::Report()`; written to `weaponfx.log` at unload |
| Viewmodel miss | Loses the viewmodel on purpose every n-th frame, to test the fallback (2.6) | `WeaponFx::DebugMissEvery(n)` |
| Depth bisect | Depth and colour copied right before the n-th draw of the frame and shown instead of the normal copies; `-2` traces one frame's copies | `WeaponFx::DebugDepthAt(n)` |
| Frame ring | The last ~5 s of the scene copy (160x90, on the GPU), dumped as `.ppm` plus a per-frame `.csv`; gaps in the frame numbers = frames without a copy (the black-fraction column is broken) | `WeaponFx::FrameRing`, `SetDumpDir` |
| Marked draws | Every distinct pixel shader and marker slot mask of a marked draw, and every model among the weapon items | logged once each by the host |
| Render targets | The frame's targets and the draw sequence on the main one (kind, depth test, `b1` changes, copies) | not included |
| PIX events | The engine's own PIX markers (section 4) | not included |
| Constant buffers | The VS/PS constant buffers at the weapon draw, to a file | not included |

The report's second line is the viewmodel start:

```
viewmodel start - by the stencil event: agree <a>, wrong <w>, none <n>, refused early <e> |
late (at a marked draw) <l>, refused on the buffer alone <b> | frames covered by the last good copy <c> |
screen target relearnt <r> | viewmodel run <run>
```

`wrong` or a steadily growing `late` after a game update means the stencil
event moved; `WeaponFX: event start WRONG` lines give the draw numbers.

The **blink watch** log line (once a second, only when there is something
to report):

```
blink watch - <frames>, outside the viewmodel run <N> (covered by the last good copy <M>),
stale copy <K>, weapon in the prepass <P> (last: run <r>, predicted <p>)
```

- `outside`: the weapon on screen outside the found viewmodel (a search miss);
- `covered`: how many of those the fallback covered;
- `stale`: in the viewmodel, but this frame's copy is not taken yet;
- `prepass`: the weapon reached the depth prepass (the material replacement
  missed a frame; the world under the weapon will not be drawn).

If `outside` grows after a game update, the viewmodel search is broken. The
picture may still look fine, because the fallback holds.

Probe shaders: `_probe_bind.hlsl` (magenta: scene unbound, red: copy black,
blue: no depth), `_probe_depth.hlsl` (what `t37` holds). Test with a mask of
diagonal stripes, not a solid fill: shifts and seams only show on a pattern.

---

## 6. What to check after a game update

| Symptom | What most likely moved |
|---|---|
| The effect is frozen or black while UV-only effects work | Indices into the engine's buffers: `b1[26]` (camera), `b3[35]`, `b1[8..10]` (axes), `b1[12..15]` (projection) **[fragile]**; compare with a dump of the constant buffers at the weapon draw |
| The weapon is filled with garbage | The input signature of the marker material's PS (`PSIn` in `weaponfx.hlsli`): field order and widths must match the game's VS |
| The depth-aware bend goes flat or wild | Depth became reversed; see `SceneDistance()` |
| No draws in the log, no effect | The `d3d11.dll` thunks / the switched table (1.4) |
| `outside` grows in the blink watch | The run layout changed; section 2.3; dump the frame's draws |
| `wrong` grows in the report, or every start is `late` | The stencil-only clear before the viewmodel moved or is gone; find the new event in a dump of the frame's draws (2.3) |
| `engine: RenderObjects signature matched 0 times` | `scenesystem.dll` rebuilt that function's prologue (`host/engine.cpp`, `kSigRenderObjects`); 2 or more hits - another module's detour on it |
| `material: CreateMaterial signature ...` / `KV3 loader call ...` | `materialsystem2.dll` / `tier0.dll` changed (`host/material.cpp`) |
| `host: weapon in hands` never appears | The pass layout (`kPassViewBits`, `kViewIsViewmodel`), the item layout (`kItemMat`, the stride) or the shader-name read (`kMatParamBlock`, `kParamShaderName`) in `host/engine.cpp` |
| The weapon is found but the model path never is | The scene object layout; the scan covers `+0..0x400 -> +0..0x100` |
| The menu opens but the camera still turns / no cursor | `menu: input slots ... are not code`: `kMouseInputEnabledSlot` / `kEnableCursorSlot` in `host/menu.cpp` |
