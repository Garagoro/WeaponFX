#include "log.h"

#include <Windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace Wfx {
namespace {

char g_Dir[kPathBytes] = {};
SRWLOCK g_DirLock = SRWLOCK_INIT;
SRWLOCK g_LogLock = SRWLOCK_INIT;
LogSink g_Sink = nullptr;

// Any address inside this image will do; GetModuleHandleEx with FROM_ADDRESS
// finds the module only when the loader mapped it.
void Anchor() {}

// %NAME% as UTF-16, or empty when unset or too long.
bool EnvW(const wchar_t* name, wchar_t* out, DWORD cap) {
    const DWORD n = GetEnvironmentVariableW(name, out, cap);
    if (n == 0 || n >= cap) {
        out[0] = 0;
        return false;
    }
    return true;
}

void ResolveDir() {
    wchar_t w[MAX_PATH] = {};
    if (!EnvW(L"WEAPONFX_DIR", w, MAX_PATH)) {
        // LOCALAPPDATA is set for every interactive user; the temp folder is
        // only a last resort so the log still has somewhere to go.
        if (!EnvW(L"LOCALAPPDATA", w, MAX_PATH - 12)) {
            const DWORD n = GetTempPathW(MAX_PATH - 12, w);
            if (n == 0 || n >= MAX_PATH - 12) w[0] = 0;
        }
        if (w[0]) {
            const size_t len = wcslen(w);
            if (w[len - 1] != L'\\' && w[len - 1] != L'/') wcscat_s(w, L"\\");
            wcscat_s(w, L"WeaponFX");
        }
    }
    if (!w[0]) return;   // nowhere to write: DataDir stays "" and the log is silent
    CreateDirectoryW(w, nullptr);   // exists already: fine; a deeper WEAPONFX_DIR must exist
    const size_t len = wcslen(w);
    if (w[len - 1] != L'\\' && w[len - 1] != L'/' && len + 1 < MAX_PATH) wcscat_s(w, L"\\");
    if (!Narrow(w, g_Dir, sizeof(g_Dir))) g_Dir[0] = 0;
}

} // namespace

bool Widen(const char* utf8, wchar_t* out, int cap) {
    if (!out || cap <= 0) return false;
    out[0] = 0;
    if (!utf8) return false;
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, out, cap)) {
        out[0] = 0;
        return false;
    }
    return true;
}

bool Narrow(const wchar_t* wide, char* out, int cap) {
    if (!out || cap <= 0) return false;
    out[0] = 0;
    if (!wide) return false;
    if (!WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, cap, nullptr, nullptr)) {
        out[0] = 0;
        return false;
    }
    return true;
}

void* SelfModule() {
    HMODULE m = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)&Anchor, &m))
        return nullptr;
    return m;
}

const char* DataDir() {
    AcquireSRWLockExclusive(&g_DirLock);
    if (!g_Dir[0]) ResolveDir();
    ReleaseSRWLockExclusive(&g_DirLock);
    return g_Dir;
}

void DataPath(const char* name, char* out, unsigned cap) {
    if (!out || !cap) return;
    out[0] = 0;
    if (!name) return;
    const bool absolute = (name[0] && name[1] == ':') || (name[0] == '\\' && name[1] == '\\');
    if (absolute) strncpy_s(out, cap, name, _TRUNCATE);
    else _snprintf_s(out, cap, _TRUNCATE, "%s%s", DataDir(), name);
}

void SetDataDir(const char* dir) {
    wchar_t w[MAX_PATH] = {};
    if (!dir || !Widen(dir, w, MAX_PATH - 2) || !w[0]) return;
    CreateDirectoryW(w, nullptr);
    const size_t len = wcslen(w);
    if (w[len - 1] != L'\\' && w[len - 1] != L'/') wcscat_s(w, L"\\");
    AcquireSRWLockExclusive(&g_DirLock);
    if (!Narrow(w, g_Dir, sizeof(g_Dir))) g_Dir[0] = 0;
    ReleaseSRWLockExclusive(&g_DirLock);
}

void SetLogSink(LogSink sink) { g_Sink = sink; }

void Log(const char* fmt, ...) {
    char line[1024];
    if (const LogSink sink = g_Sink) {
        va_list ap;
        va_start(ap, fmt);
        _vsnprintf_s(line, sizeof(line), _TRUNCATE, fmt, ap);
        va_end(ap);
        sink(line);
        return;
    }
    SYSTEMTIME t;
    GetLocalTime(&t);
    int o = _snprintf_s(line, sizeof(line), _TRUNCATE, "%02u:%02u:%02u.%03u ", t.wHour, t.wMinute, t.wSecond,
                        t.wMilliseconds);
    if (o < 0) o = 0;
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(line + o, sizeof(line) - o, _TRUNCATE, fmt, ap);
    va_end(ap);
    if (!DataDir()[0]) return;
    char path[kPathBytes];
    wchar_t wpath[MAX_PATH];
    DataPath("weaponfx.log", path, sizeof(path));
    if (!Widen(path, wpath, MAX_PATH)) return;
    AcquireSRWLockExclusive(&g_LogLock);
    FILE* f = nullptr;
    if (_wfopen_s(&f, wpath, L"ab") == 0 && f) {
        fputs(line, f);   // UTF-8 bytes as they are
        fputs("\r\n", f);
        fclose(f);
    }
    ReleaseSRWLockExclusive(&g_LogLock);
}

} // namespace Wfx
