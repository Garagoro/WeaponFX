#pragma once

// WeaponFX's own log and data folder, and the one rule for paths.
//
// Paths are UTF-8 everywhere inside WeaponFX (the ini, the menu, the core's
// Load/LoadTexture) and become UTF-16 only at the Windows call. The ANSI
// functions would read a user folder like C:\Users\Иван through the system
// code page, which cannot hold every name and does not match what an editor
// saves in the ini.
//
// The data folder holds weaponfx.ini and weaponfx.log, and relative shader
// and mask paths in the ini resolve against it. One place however the DLL was
// loaded (a manual map has no path of its own, so "next to the DLL" could not
// be the rule):
//   %WEAPONFX_DIR%, if set, else %LOCALAPPDATA%\WeaponFX\ (created).
namespace Wfx {

constexpr unsigned kPathBytes = 1024;      // UTF-8: a Cyrillic character takes two

const char* DataDir();                      // UTF-8, with a trailing backslash
// `name` inside DataDir, or `name` itself when it is already absolute.
void DataPath(const char* name, char* out, unsigned cap);

// UTF-8 <-> UTF-16. False (and an empty `out`) when it does not fit or is not
// valid.
bool Widen(const char* utf8, wchar_t* out, int cap);
bool Narrow(const wchar_t* wide, char* out, int cap);

// One line, timestamped, appended to weaponfx.log. Any thread. A line longer
// than the buffer is cut, never overrun.
void Log(const char* fmt, ...);

// For a host that embeds WeaponFX (the skin editor) and keeps its own folder
// and log. Both before anything else here runs - DataDir() resolves once.
// `dir`: UTF-8, created when missing (one level), a trailing backslash added.
void SetDataDir(const char* dir);
// Lines go to `sink` (untimestamped: the host stamps its own) instead of
// weaponfx.log. Null puts the file back.
using LogSink = void (*)(const char* line);
void SetLogSink(LogSink sink);

// The module this code lives in when the loader knows it (LoadLibrary), or
// null (manual map). For FreeLibraryAndExitThread.
void* SelfModule();

} // namespace Wfx
