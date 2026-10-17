#pragma once

// The settings panel: what the in-game menu shows inside its window, and what
// a host with an ImGui UI of its own (the skin editor's dock) draws in a panel
// of its own. Draws into the current ImGui window - no Begin/End here - with
// whatever ImGui context is current, so it is compiled against the host's
// Dear ImGui (the same version, 1.91.9b).
//
// Two tabs, the weapon in hands and the gloves; each edits the section its
// target reads (the model's file name), previews live, saves to that section
// or to the target's default one.

namespace Wfx {
namespace Panel {

// UI text through the host's translation: English in, the host's language
// out. Null (the default) shows the English.
using TrFn = const char* (*)(const char* english);
void SetTranslator(TrFn fn);

// Drawn right after a texture slot's path field, on the same line: a host's
// own way to fill it (the editor: "from the layer"). Returns true when it
// wrote a new path into `path` (UTF-8, `cap` bytes). Null: nothing extra.
using TexExtraFn = bool (*)(int target, int slot, char* path, unsigned cap);
void SetTexExtra(TexExtraFn fn);

// Render thread, inside an ImGui window.
void Draw();

} // namespace Panel
} // namespace Wfx
