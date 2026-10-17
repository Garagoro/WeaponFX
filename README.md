<div align="center">

# WeaponFX

**Your own pixel shaders on the weapon in your hands in Counter-Strike 2.**

Glass that bends the world behind it · a window into space · liquid metal ·
holograms · lava under a glass shell

[**Guide**](GUIDE.md) · [How it works](RENDERING.md) · [License](LICENSE)

</div>

<br>

![Galaxy](media/galaxy.webp)

| ![Chrome](media/chrome.webp) | ![Plasma](media/plasma.webp) |
|:---:|:---:|
| **Chrome**: the map's reflections on polished metal | **Plasma**: molten energy flowing along the weapon |
| ![Black hole](media/blackhole.webp) | ![Masked](media/masked.webp) |
| **Black hole**: the world bends around a point of nothing | **Masked**: two effects on one weapon, split by a mask |

<br>

<div align="center">

**Write HLSL, save, see it in game** — every save recompiles live

**Only your weapon, only your screen** — others and demos see the normal skin

**Replace the skin or draw over it** — with masks, textures and parameters

**In-game menu** — per-weapon settings, saved to an `.ini`

<br>

### [Read the guide →](GUIDE.md)

Install, menu, config, writing your own effect, building.

</div>

<br>

> WeaponFX is foreign code inside the game process, made for people who write
> shaders and know how to load a DLL. Use it offline.

<sub>MIT licensed. Dear ImGui under <code>third_party/imgui</code> is MIT as well.</sub>
