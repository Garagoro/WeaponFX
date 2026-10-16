#pragma once

// The in-game menu (Insert): the settings of the weapon in hands - mode,
// shader, textures, parameters - applied live and saved to weaponfx.ini.
//
// While it is open the game gets no mouse and no key presses (releases still
// go through, so no movement key sticks): CS2 keeps the cursor in relative
// mode through its own input system, so two of its virtual calls are taken
// over for that time (CCSGOInput::MouseInputEnabled, CInputSystem's cursor
// switch).

struct IDXGISwapChain;
struct ID3D11Device;

namespace Wfx {
namespace Menu {

// Render thread (Present), every frame: initialises on the first call, draws
// when open.
void Frame(IDXGISwapChain* sc, ID3D11Device* dev);

// Control thread, teardown step one: the window procedure and the input calls
// go back to the game, the cursor to the state the game asked for.
void Detach();
// Teardown, after the Present hook is gone: ImGui itself.
void Shutdown();

} // namespace Menu
} // namespace Wfx
