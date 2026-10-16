#pragma once

// A small INI reader/writer for weaponfx.ini, in UTF-8.
//
// Not GetPrivateProfileString: that reads a file without a UTF-16 BOM in the
// system code page, so a path saved by any editor in UTF-8 (the default
// everywhere now) comes back garbled the moment it holds a non-Latin letter.
//
// Format: [section], key = value, comments on their own line or after a value
// behind " ;" / " #". Section and key names compare case-insensitively.

#include <cstddef>

namespace Wfx {
namespace Ini {

constexpr size_t kMaxFileBytes = 256 * 1024;   // a corrupt or wrong file is refused, not walked

struct File {
    char* text = nullptr;   // NUL-terminated, BOM stripped
    size_t len = 0;
};

// False when the file is missing, empty, unreadable or too big; `f` is then
// empty and Get finds nothing.
bool Load(const char* pathUtf8, File* f);
void Free(File* f);

// The value of `key` in `section`; false (and "" in `out`) when absent.
bool Get(const File& f, const char* section, const char* key, char* out, size_t cap);

struct Pair {
    const char* key;
    const char* value;
};
// Sets every pair in `section`, keeping the rest of the file - other
// sections, comments, key order - as it was; a missing key goes at the end of
// its section, a missing section at the end of the file. Written to a
// temporary file first and moved over the old one.
bool Put(const char* pathUtf8, const char* section, const Pair* pairs, int count);

} // namespace Ini
} // namespace Wfx
