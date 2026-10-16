#include "ini.h"
#include "log.h"

#include <Windows.h>
#include <cstdint>
#include <cstring>
#include <string>

namespace Wfx {
namespace Ini {
namespace {

constexpr int kMaxPairs = 32;

bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r'; }

// [b, e) without spaces around it.
void Trim(const char*& b, const char*& e) {
    while (b < e && IsSpace(*b)) ++b;
    while (e > b && IsSpace(e[-1])) --e;
}

bool Equal(const char* b, const char* e, const char* name) {
    const size_t n = strlen(name);
    return (size_t)(e - b) == n && _strnicmp(b, name, n) == 0;
}

// One line, parsed. kind: 0 blank/comment, 1 section header, 2 key = value.
struct Line {
    int kind = 0;
    const char *nameB = nullptr, *nameE = nullptr;
    const char *valB = nullptr, *valE = nullptr;
};

Line Parse(const char* b, const char* e) {
    Line l;
    Trim(b, e);
    if (b == e || *b == ';' || *b == '#') return l;
    if (*b == '[') {
        const char* close = (const char*)memchr(b, ']', (size_t)(e - b));
        if (!close) return l;
        l.kind = 1;
        l.nameB = b + 1;
        l.nameE = close;
        Trim(l.nameB, l.nameE);
        return l;
    }
    const char* eq = (const char*)memchr(b, '=', (size_t)(e - b));
    if (!eq) return l;
    l.kind = 2;
    l.nameB = b;
    l.nameE = eq;
    Trim(l.nameB, l.nameE);
    l.valB = eq + 1;
    l.valE = e;
    // A comment after the value needs a space before it: a path may hold ';'.
    for (const char* p = l.valB; p + 1 < l.valE; ++p)
        if (IsSpace(*p) && (p[1] == ';' || p[1] == '#')) { l.valE = p; break; }
    Trim(l.valB, l.valE);
    return l;
}

// Calls fn(lineBegin, lineEnd) for each line; lineEnd excludes the '\n'.
template <class Fn>
void ForEachLine(const char* text, size_t len, Fn fn) {
    const char* p = text;
    const char* end = text + len;
    while (p < end) {
        const char* nl = (const char*)memchr(p, '\n', (size_t)(end - p));
        const char* le = nl ? nl : end;
        fn(p, le);
        p = nl ? nl + 1 : end;
    }
}

bool WriteAll(const wchar_t* path, const std::string& data) {
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD n = 0;
    const bool ok = WriteFile(f, data.data(), (DWORD)data.size(), &n, nullptr) && n == data.size();
    CloseHandle(f);
    return ok;
}

} // namespace

bool Load(const char* pathUtf8, File* f) {
    *f = File();
    wchar_t w[MAX_PATH];
    if (!Widen(pathUtf8, w, MAX_PATH)) return false;
    // Shared for writing: an editor may be saving it right now; a torn read
    // is followed by another change notice and another read.
    HANDLE h = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size = {};
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart > 0 && size.QuadPart <= (LONGLONG)kMaxFileBytes;
    char* buf = ok ? (char*)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)size.QuadPart + 1) : nullptr;
    DWORD got = 0;
    ok = buf && ReadFile(h, buf, (DWORD)size.QuadPart, &got, nullptr);
    CloseHandle(h);
    if (!ok) {
        if (buf) HeapFree(GetProcessHeap(), 0, buf);
        return false;
    }
    buf[got] = 0;
    size_t skip = (got >= 3 && (uint8_t)buf[0] == 0xEF && (uint8_t)buf[1] == 0xBB && (uint8_t)buf[2] == 0xBF) ? 3 : 0;
    if (skip) memmove(buf, buf + skip, got - skip + 1);
    f->text = buf;
    f->len = got - skip;
    return true;
}

void Free(File* f) {
    if (f->text) HeapFree(GetProcessHeap(), 0, f->text);
    *f = File();
}

bool Get(const File& f, const char* section, const char* key, char* out, size_t cap) {
    if (!out || !cap) return false;
    out[0] = 0;
    if (!f.text) return false;
    bool inSection = false;
    bool found = false;
    ForEachLine(f.text, f.len, [&](const char* b, const char* e) {
        if (found) return;
        const Line l = Parse(b, e);
        if (l.kind == 1) inSection = Equal(l.nameB, l.nameE, section);
        else if (l.kind == 2 && inSection && Equal(l.nameB, l.nameE, key)) {
            const size_t n = (size_t)(l.valE - l.valB);
            if (n < cap) {
                memcpy(out, l.valB, n);
                out[n] = 0;
            } else {
                out[0] = 0;   // too long for the caller: as if absent, never cut
            }
            found = true;
        }
    });
    return found;
}

bool Put(const char* pathUtf8, const char* section, const Pair* pairs, int count) {
    if (!section || !section[0] || !pairs || count <= 0 || count > kMaxPairs) return false;
    File f;
    Load(pathUtf8, &f);   // a missing file is an empty one
    bool written[kMaxPairs] = {};
    std::string out;
    out.reserve(f.len + 1024);
    bool inSection = false, seenSection = false;

    auto emitMissing = [&]() {
        for (int k = 0; k < count; ++k) {
            if (written[k]) continue;
            out += pairs[k].key;
            out += " = ";
            out += pairs[k].value;
            out += "\r\n";
            written[k] = true;
        }
    };

    if (f.text) {
        ForEachLine(f.text, f.len, [&](const char* b, const char* e) {
            const Line l = Parse(b, e);
            if (l.kind == 1) {
                if (inSection) emitMissing();   // the section ends here
                inSection = Equal(l.nameB, l.nameE, section);
                seenSection = seenSection || inSection;
            } else if (l.kind == 2 && inSection) {
                for (int k = 0; k < count; ++k) {
                    if (!Equal(l.nameB, l.nameE, pairs[k].key)) continue;
                    // The key keeps its own spelling and spacing; only the value changes.
                    out.append(b, l.valB);
                    out += pairs[k].value;
                    out.append(l.valE, e[-1] == '\r' ? e - 1 : e);
                    out += "\r\n";
                    written[k] = true;
                    return;
                }
            }
            out.append(b, e);
            if (e == b || e[-1] != '\r') out += '\r';
            out += '\n';
        });
    }
    if (inSection) emitMissing();
    if (!seenSection) {
        if (!out.empty() && out.back() != '\n') out += "\r\n";
        out += "\r\n[";
        out += section;
        out += "]\r\n";
        emitMissing();
    }
    Free(&f);

    wchar_t w[MAX_PATH], tmp[MAX_PATH];
    if (!Widen(pathUtf8, w, MAX_PATH) || wcslen(w) + 5 >= MAX_PATH) return false;
    wcscpy_s(tmp, w);
    wcscat_s(tmp, L".tmp");
    if (!WriteAll(tmp, out)) return false;
    if (!MoveFileExW(tmp, w, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp);
        return false;
    }
    return true;
}

} // namespace Ini
} // namespace Wfx
